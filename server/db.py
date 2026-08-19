"""
SQLite qatlami. Tashqi ORM yo'q - jadvallar uchta, so'rovlar oddiy.

Vaqt hamma joyda UTC epoch (butun son) sifatida saqlanadi va faqat ko'rsatishda
Toshkent vaqtiga o'giriladi. Baza ichida mahalliy vaqt saqlansa, server boshqa
zonaga ko'chirilganda eski yozuvlar siljib ketardi.
"""

import os
import sqlite3
import hashlib
import secrets
import time
from contextlib import contextmanager
from datetime import datetime, timedelta, timezone
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

DB_PATH = os.environ.get("DB_PATH", "data/qulf.db")


def _load_tz(name: str):
    """
    Windows da va python:slim obrazida IANA bazasi tizimda yo'q - u `tzdata`
    paketidan keladi. Paket ham bo'lmasa dastur ishdan chiqmasin: O'zbekiston
    yozgi vaqtga o'tmaydi, shuning uchun qat'iy UTC+5 to'g'ri javob beradi.
    """
    try:
        return ZoneInfo(name)
    except (ZoneInfoNotFoundError, KeyError, ValueError):
        print(f"OGOHLANTIRISH: '{name}' zonasi topilmadi, UTC+5 ishlatiladi. "
              f"Tuzatish uchun: pip install tzdata")
        return timezone(timedelta(hours=5), "UTC+5")


TZ = _load_tz(os.environ.get("TZ_NAME", "Asia/Tashkent"))

SCHEMA = """
CREATE TABLE IF NOT EXISTS users (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    username      TEXT    NOT NULL UNIQUE,
    password_hash TEXT    NOT NULL,
    created_at    INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS cards (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    uid        TEXT    NOT NULL UNIQUE,
    name       TEXT    NOT NULL,
    note       TEXT    NOT NULL DEFAULT '',
    active     INTEGER NOT NULL DEFAULT 1,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS events (
    id      INTEGER PRIMARY KEY AUTOINCREMENT,
    uid     TEXT    NOT NULL,
    name    TEXT    NOT NULL DEFAULT '',
    reader  TEXT    NOT NULL DEFAULT '',
    result  TEXT    NOT NULL,
    at      INTEGER NOT NULL,
    device  TEXT    NOT NULL DEFAULT ''
);

CREATE INDEX IF NOT EXISTS idx_events_at  ON events (at DESC);
CREATE INDEX IF NOT EXISTS idx_events_uid ON events (uid, at DESC);
"""

# Hodisa turlari. Proshivka shu qiymatlarni yuboradi.
RESULT_LABELS = {
    "allowed": "Ruxsat berildi",
    "denied":  "Rad etildi",
    "open":    "Ochildi",
    "close":   "Yopildi",
    "learn":   "Ro'yxatga qo'shildi",
    "boot":    "Plata ishga tushdi",
}


# ================= Ulanish =================

@contextmanager
def connect():
    os.makedirs(os.path.dirname(DB_PATH) or ".", exist_ok=True)
    conn = sqlite3.connect(DB_PATH, timeout=10)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA foreign_keys = ON")
    conn.execute("PRAGMA journal_mode = WAL")
    try:
        yield conn
        conn.commit()
    finally:
        conn.close()


def init_db():
    with connect() as conn:
        conn.executescript(SCHEMA)


# ================= Vaqt =================

def now() -> int:
    return int(time.time())


def fmt(epoch: int | None) -> str:
    """UTC epoch -> "19.08.2026 14:32" (Toshkent vaqti)."""
    if not epoch:
        return "—"
    return datetime.fromtimestamp(epoch, TZ).strftime("%d.%m.%Y %H:%M")


def ago(epoch: int | None) -> str:
    """Insonga tushunarli "12 daqiqa oldin"."""
    if not epoch:
        return "hech qachon"
    d = now() - epoch
    if d < 60:
        return "hozirgina"
    if d < 3600:
        return f"{d // 60} daqiqa oldin"
    if d < 86400:
        return f"{d // 3600} soat oldin"
    if d < 86400 * 30:
        return f"{d // 86400} kun oldin"
    return fmt(epoch)


# ================= UID =================

def normalize_uid(raw: str) -> str | None:
    """
    "80 96 03 9e", "80:96:03:9E", "8096039e" -> "8096039E".
    4 baytdan farq qilsa None qaytaradi.
    """
    if not raw:
        return None
    hexchars = "".join(c for c in raw if c in "0123456789abcdefABCDEF")
    if len(hexchars) != 8:
        return None
    return hexchars.upper()


# ================= Foydalanuvchilar =================

def hash_password(password: str, salt: bytes | None = None) -> str:
    salt = salt or secrets.token_bytes(16)
    dk = hashlib.scrypt(password.encode(), salt=salt, n=2**14, r=8, p=1, dklen=32)
    return f"scrypt${salt.hex()}${dk.hex()}"


def verify_password(password: str, stored: str) -> bool:
    try:
        algo, salt_hex, want = stored.split("$")
        if algo != "scrypt":
            return False
    except ValueError:
        return False
    got = hashlib.scrypt(
        password.encode(), salt=bytes.fromhex(salt_hex), n=2**14, r=8, p=1, dklen=32
    )
    return secrets.compare_digest(got.hex(), want)


def ensure_admin(username: str, password: str):
    """Birinchi ishga tushirishda admin yaratadi. Mavjud bo'lsa parolni yangilaydi."""
    with connect() as conn:
        row = conn.execute(
            "SELECT id FROM users WHERE username = ?", (username,)
        ).fetchone()
        if row:
            conn.execute(
                "UPDATE users SET password_hash = ? WHERE id = ?",
                (hash_password(password), row["id"]),
            )
        else:
            conn.execute(
                "INSERT INTO users (username, password_hash, created_at) VALUES (?, ?, ?)",
                (username, hash_password(password), now()),
            )


def check_login(username: str, password: str) -> bool:
    with connect() as conn:
        row = conn.execute(
            "SELECT password_hash FROM users WHERE username = ?", (username,)
        ).fetchone()
    if not row:
        # Foydalanuvchi yo'q bo'lsa ham parolni hisoblaymiz - javob vaqti bo'yicha
        # "bunday login bor mi" degan ma'lumot sizib chiqmasin.
        hash_password(password)
        return False
    return verify_password(password, row["password_hash"])


# ================= Kartalar =================

def list_cards() -> list[sqlite3.Row]:
    """
    Har bir karta yoniga oxirgi ishlatilgan vaqti va jami urinishlar soni.
    LEFT JOIN - hali ishlatilmagan karta ham ro'yxatda qoladi.
    """
    with connect() as conn:
        return conn.execute(
            """
            SELECT c.*,
                   (SELECT MAX(at) FROM events e
                     WHERE e.uid = c.uid AND e.result = 'allowed')  AS last_used,
                   (SELECT COUNT(*) FROM events e
                     WHERE e.uid = c.uid AND e.result = 'allowed')  AS use_count
              FROM cards c
             ORDER BY c.active DESC, c.name COLLATE NOCASE
            """
        ).fetchall()


def active_cards() -> list[sqlite3.Row]:
    with connect() as conn:
        return conn.execute(
            "SELECT uid, name FROM cards WHERE active = 1 ORDER BY id"
        ).fetchall()


def get_card_by_uid(uid: str) -> sqlite3.Row | None:
    with connect() as conn:
        return conn.execute("SELECT * FROM cards WHERE uid = ?", (uid,)).fetchone()


def add_card(uid: str, name: str, note: str = "") -> tuple[bool, str]:
    """(muvaffaqiyat, xabar)."""
    t = now()
    with connect() as conn:
        exists = conn.execute("SELECT name FROM cards WHERE uid = ?", (uid,)).fetchone()
        if exists:
            return False, f"Bu karta allaqachon ro'yxatda: {exists['name']}"
        conn.execute(
            """INSERT INTO cards (uid, name, note, active, created_at, updated_at)
               VALUES (?, ?, ?, 1, ?, ?)""",
            (uid, name, note, t, t),
        )
    return True, f"{name} qo'shildi"


def update_card(card_id: int, name: str, note: str):
    with connect() as conn:
        conn.execute(
            "UPDATE cards SET name = ?, note = ?, updated_at = ? WHERE id = ?",
            (name, note, now(), card_id),
        )


def toggle_card(card_id: int) -> str | None:
    with connect() as conn:
        row = conn.execute(
            "SELECT active, name FROM cards WHERE id = ?", (card_id,)
        ).fetchone()
        if not row:
            return None
        new = 0 if row["active"] else 1
        conn.execute(
            "UPDATE cards SET active = ?, updated_at = ? WHERE id = ?",
            (new, now(), card_id),
        )
    return f"{row['name']} " + ("yoqildi" if new else "o'chirildi")


def delete_card(card_id: int) -> str | None:
    with connect() as conn:
        row = conn.execute("SELECT name FROM cards WHERE id = ?", (card_id,)).fetchone()
        if not row:
            return None
        conn.execute("DELETE FROM cards WHERE id = ?", (card_id,))
    return f"{row['name']} o'chirildi"


# ================= Hodisalar =================

def add_event(uid: str, reader: str, result: str, at: int, device: str = "") -> int:
    """Hodisani yozadi va kartaning nomini o'sha paytdagi holicha saqlab qoladi."""
    card = get_card_by_uid(uid) if uid else None
    with connect() as conn:
        cur = conn.execute(
            """INSERT INTO events (uid, name, reader, result, at, device)
               VALUES (?, ?, ?, ?, ?, ?)""",
            (uid, card["name"] if card else "", reader, result, at, device),
        )
        return cur.lastrowid


def list_events(limit: int = 200, uid: str = "", result: str = "") -> list[sqlite3.Row]:
    sql = "SELECT * FROM events WHERE 1 = 1"
    args: list = []
    if uid:
        sql += " AND uid = ?"
        args.append(uid)
    if result:
        sql += " AND result = ?"
        args.append(result)
    sql += " ORDER BY at DESC, id DESC LIMIT ?"
    args.append(limit)
    with connect() as conn:
        return conn.execute(sql, args).fetchall()


def unknown_cards(limit: int = 10) -> list[sqlite3.Row]:
    """
    Rad etilgan, lekin hali ro'yxatga qo'shilmagan kartalar.
    CRM da "shunchaki nomini yozib qo'shish" uchun shu ro'yxat ishlatiladi.
    """
    with connect() as conn:
        return conn.execute(
            """
            SELECT e.uid,
                   MAX(e.at)  AS last_at,
                   COUNT(*)   AS tries,
                   MAX(e.reader) AS reader
              FROM events e
             WHERE e.result = 'denied'
               AND e.uid <> ''
               AND e.uid NOT IN (SELECT uid FROM cards)
             GROUP BY e.uid
             ORDER BY last_at DESC
             LIMIT ?
            """,
            (limit,),
        ).fetchall()


def stats() -> dict:
    with connect() as conn:
        day_ago = now() - 86400
        return {
            "cards_total":  conn.execute("SELECT COUNT(*) c FROM cards").fetchone()["c"],
            "cards_active": conn.execute("SELECT COUNT(*) c FROM cards WHERE active = 1").fetchone()["c"],
            "opens_today":  conn.execute(
                "SELECT COUNT(*) c FROM events WHERE result = 'allowed' AND at >= ?",
                (day_ago,),
            ).fetchone()["c"],
            "denied_today": conn.execute(
                "SELECT COUNT(*) c FROM events WHERE result = 'denied' AND at >= ?",
                (day_ago,),
            ).fetchone()["c"],
            "last_seen": conn.execute("SELECT MAX(at) m FROM events").fetchone()["m"],
        }
