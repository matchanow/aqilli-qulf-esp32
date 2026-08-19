"""
Aqilli qulf - CRM va qurilma API si.

Ikki xil mijoz bor va ular butunlay boshqacha autentifikatsiya ishlatadi:

  1. Odam  -> /login, sessiya cookie si
  2. ESP32 -> /api/*, Authorization: Bearer <DEVICE_TOKEN>

Qurilma hech qachon sessiya cookie sini olmaydi, odam esa DEVICE_TOKEN ni
bilishi shart emas - shuning uchun birini o'g'irlash ikkinchisini ochmaydi.

Ishga tushirish:
    uvicorn app:app --host 0.0.0.0 --port 8000
"""

import os
import secrets

from fastapi import Depends, FastAPI, Form, HTTPException, Request
from fastapi.responses import HTMLResponse, JSONResponse, RedirectResponse
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates
from starlette.middleware.sessions import SessionMiddleware
from starlette.status import HTTP_303_SEE_OTHER

import db

# ================= Sozlamalar =================
ADMIN_USER    = os.environ.get("ADMIN_USER", "admin")
ADMIN_PASS    = os.environ.get("ADMIN_PASS", "")
DEVICE_TOKEN  = os.environ.get("DEVICE_TOKEN", "")
SECRET_KEY    = os.environ.get("SECRET_KEY", "")
MAX_EVENTS_PER_POST = 50

if not ADMIN_PASS:
    raise RuntimeError("ADMIN_PASS o'rnatilmagan. .env faylini to'ldiring.")
if not DEVICE_TOKEN:
    raise RuntimeError("DEVICE_TOKEN o'rnatilmagan. .env faylini to'ldiring.")
if not SECRET_KEY:
    # Tasodifiy kalit ham ishlaydi, lekin server qayta yuklanganda hamma
    # sessiya uzilib qoladi. Shuning uchun ogohlantiramiz.
    SECRET_KEY = secrets.token_hex(32)
    print("OGOHLANTIRISH: SECRET_KEY yo'q, vaqtinchalik kalit yaratildi. "
          "Server qayta yuklansa hamma tizimdan chiqib ketadi.")

BASE_DIR  = os.path.dirname(os.path.abspath(__file__))
templates = Jinja2Templates(directory=os.path.join(BASE_DIR, "templates"))
templates.env.filters["fmt"] = db.fmt
templates.env.filters["ago"] = db.ago

app = FastAPI(title="Aqilli qulf CRM", docs_url=None, redoc_url=None)
app.add_middleware(SessionMiddleware, secret_key=SECRET_KEY, https_only=False,
                   same_site="lax", max_age=14 * 24 * 3600)
app.mount("/static", StaticFiles(directory=os.path.join(BASE_DIR, "static")), name="static")


@app.on_event("startup")
def startup():
    db.init_db()
    db.ensure_admin(ADMIN_USER, ADMIN_PASS)
    print(f"CRM tayyor. Login: {ADMIN_USER}")


# ================= Yordamchilar =================

def current_user(request: Request) -> str | None:
    return request.session.get("user")


def require_login(request: Request) -> str:
    user = current_user(request)
    if not user:
        # Dependency ichidan redirect qaytarib bo'lmaydi, shuning uchun
        # 303 ni istisno sifatida tashlaymiz va handler da ushlaymiz.
        raise HTTPException(status_code=303, headers={"Location": "/login"})
    return user


def require_device(request: Request):
    header = request.headers.get("authorization", "")
    if not header.startswith("Bearer "):
        raise HTTPException(status_code=401, detail="Bearer token kerak")
    token = header[7:].strip()
    if not secrets.compare_digest(token, DEVICE_TOKEN):
        raise HTTPException(status_code=403, detail="Token noto'g'ri")


def flash(request: Request, message: str, kind: str = "ok"):
    request.session["flash"] = {"message": message, "kind": kind}


def pop_flash(request: Request):
    return request.session.pop("flash", None)


def page(request: Request, name: str, **ctx) -> HTMLResponse:
    return templates.TemplateResponse(
        name,
        {"request": request, "user": current_user(request), "flash": pop_flash(request), **ctx},
    )


@app.exception_handler(HTTPException)
async def redirect_or_error(request: Request, exc: HTTPException):
    if exc.status_code == 303 and "Location" in (exc.headers or {}):
        return RedirectResponse(exc.headers["Location"], status_code=HTTP_303_SEE_OTHER)
    if request.url.path.startswith("/api/"):
        return JSONResponse({"error": exc.detail}, status_code=exc.status_code)
    return HTMLResponse(f"<h1>{exc.status_code}</h1><p>{exc.detail}</p>",
                        status_code=exc.status_code)


# ================= Kirish =================

@app.get("/login", response_class=HTMLResponse)
def login_form(request: Request):
    if current_user(request):
        return RedirectResponse("/", status_code=HTTP_303_SEE_OTHER)
    return page(request, "login.html")


@app.post("/login")
def login(request: Request, username: str = Form(...), password: str = Form(...)):
    if db.check_login(username.strip(), password):
        request.session["user"] = username.strip()
        return RedirectResponse("/", status_code=HTTP_303_SEE_OTHER)
    flash(request, "Login yoki parol noto'g'ri", "err")
    return RedirectResponse("/login", status_code=HTTP_303_SEE_OTHER)


@app.post("/logout")
def logout(request: Request):
    request.session.clear()
    return RedirectResponse("/login", status_code=HTTP_303_SEE_OTHER)


# ================= Kartalar =================

@app.get("/", response_class=HTMLResponse)
def dashboard(request: Request, user: str = Depends(require_login)):
    return page(
        request, "cards.html",
        cards=db.list_cards(),
        unknown=db.unknown_cards(),
        stats=db.stats(),
        recent=db.list_events(limit=8),
        labels=db.RESULT_LABELS,
    )


@app.post("/cards")
def cards_add(request: Request, uid: str = Form(...), name: str = Form(...),
              note: str = Form(""), user: str = Depends(require_login)):
    norm = db.normalize_uid(uid)
    if not norm:
        flash(request, f"UID noto'g'ri: {uid} — 4 baytli (8 ta hex belgi) bo'lishi kerak", "err")
    elif not name.strip():
        flash(request, "Ism kiritilmadi", "err")
    else:
        ok, msg = db.add_card(norm, name.strip(), note.strip())
        flash(request, msg, "ok" if ok else "err")
    return RedirectResponse("/", status_code=HTTP_303_SEE_OTHER)


@app.post("/cards/{card_id}/edit")
def cards_edit(request: Request, card_id: int, name: str = Form(...),
               note: str = Form(""), user: str = Depends(require_login)):
    if not name.strip():
        flash(request, "Ism bo'sh bo'lishi mumkin emas", "err")
    else:
        db.update_card(card_id, name.strip(), note.strip())
        flash(request, "Saqlandi")
    return RedirectResponse("/", status_code=HTTP_303_SEE_OTHER)


@app.post("/cards/{card_id}/toggle")
def cards_toggle(request: Request, card_id: int, user: str = Depends(require_login)):
    msg = db.toggle_card(card_id)
    flash(request, msg or "Karta topilmadi", "ok" if msg else "err")
    return RedirectResponse("/", status_code=HTTP_303_SEE_OTHER)


@app.post("/cards/{card_id}/delete")
def cards_delete(request: Request, card_id: int, user: str = Depends(require_login)):
    msg = db.delete_card(card_id)
    flash(request, msg or "Karta topilmadi", "ok" if msg else "err")
    return RedirectResponse("/", status_code=HTTP_303_SEE_OTHER)


# ================= Tarix =================

@app.get("/logs", response_class=HTMLResponse)
def logs(request: Request, uid: str = "", result: str = "",
         user: str = Depends(require_login)):
    return page(
        request, "logs.html",
        events=db.list_events(limit=300, uid=uid, result=result),
        labels=db.RESULT_LABELS,
        f_uid=uid, f_result=result,
    )


# ================= Qurilma API si =================

@app.get("/api/cards")
def api_cards(_: None = Depends(require_device)):
    """Proshivka shu ro'yxatni tortadi. Faqat faol kartalar yuboriladi."""
    rows = db.active_cards()
    return {"cards": [{"uid": r["uid"], "name": r["name"]} for r in rows]}


@app.post("/api/events")
async def api_events(request: Request, _: None = Depends(require_device)):
    """
    Proshivka hodisalarni to'plab yuboradi:
      {"device":"eshik-1","events":[{"uid":"8096039E","reader":"TASHQI",
                                     "result":"allowed","ts":1755600000}]}

    ts - qurilmadagi UTC epoch. Qurilma NTP ga ulgurmagan bo'lsa 0 yuboradi;
    u holda serverga yetib kelgan vaqtni qo'yamiz. Aks holda 1970-yil chiqardi.
    """
    try:
        body = await request.json()
    except Exception:
        raise HTTPException(status_code=400, detail="JSON noto'g'ri")

    items = body.get("events") or []
    if not isinstance(items, list):
        raise HTTPException(status_code=400, detail="events ro'yxat bo'lishi kerak")
    if len(items) > MAX_EVENTS_PER_POST:
        raise HTTPException(status_code=413, detail=f"Ko'pi {MAX_EVENTS_PER_POST} ta hodisa")

    device = str(body.get("device", ""))[:32]
    saved, skipped = 0, 0
    for it in items:
        if not isinstance(it, dict):
            skipped += 1
            continue
        result = str(it.get("result", ""))[:16]
        if result not in db.RESULT_LABELS:
            skipped += 1
            continue
        uid = db.normalize_uid(str(it.get("uid", ""))) or ""
        # Kartaga oid hodisa UID siz ma'nosiz - uni bo'sh UID bilan yozib qo'ysak,
        # tarixda kimligi noma'lum qator paydo bo'lardi. open/close/boot esa
        # qurilmaning o'z hodisasi, ularda UID bo'lmasligi normal.
        if not uid and result in ("allowed", "denied", "learn"):
            skipped += 1
            continue
        ts = it.get("ts") or 0
        try:
            ts = int(ts)
        except (TypeError, ValueError):
            ts = 0
        # Ishonchsiz vaqtni rad etamiz: 2020-yildan oldingi yoki kelajakdagi
        # qiymat qurilmaning soati sozlanmaganini bildiradi.
        if ts < 1577836800 or ts > db.now() + 86400:
            ts = db.now()
        db.add_event(uid, str(it.get("reader", ""))[:16], result, ts, device)
        saved += 1

    return {"saved": saved, "skipped": skipped}


@app.get("/healthz")
def healthz():
    return {"ok": True}
