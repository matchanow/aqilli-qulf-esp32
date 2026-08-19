# Aqilli qulf — CRM

Kartalarni brauzerdan boshqarish va har bir kirishni yozib borish uchun server.

Kirasiz, kartani kiritasiz — tamom. Qurilma ro'yxatni o'zi tortib oladi va har
safar karta tegizilganda bu yerga xabar yuboradi, shuning uchun qaysi karta
qachon ishlatilgani ko'rinib turadi.

FastAPI + SQLite. Bitta konteyner, tashqi baza kerak emas.

## Nimalar bor

- **Kartalar ro'yxati** — qo'shish, nomini o'zgartirish, vaqtincha o'chirish, butunlay olib tashlash
- **Tanilmagan kartalar** — qurilmaga tegizilgan, lekin ro'yxatda yo'q kartalar
  alohida chiqadi. Ismini yozib **Qo'shish** ni bosasiz, UID ni qo'lda kiritish shart emas
- **Har bir karta uchun**: qachon qo'shilgan, oxirgi marta qachon ishlatilgan, necha marta ochgan
- **Tarix** — barcha hodisalar, UID va turi bo'yicha filtr bilan
- **Qurilma API si** — proshivka shu yerdan ro'yxatni oladi va hodisalarni yuboradi

## Ishga tushirish

### Docker (tavsiya etiladi)

```bash
cd server
cp .env.example .env
# .env ni to'ldiring — pastdagi jadvalga qarang
docker compose up -d --build
```

`http://server-ip:8000` ochiladi.

### Docker siz

```bash
cd server
python -m venv .venv
.venv/bin/pip install -r requirements.txt     # Windows: .venv\Scripts\pip
cp .env.example .env                          # to'ldiring
set -a && . ./.env && set +a
.venv/bin/uvicorn app:app --host 0.0.0.0 --port 8000
```

### `.env`

| O'zgaruvchi | Nima uchun |
|---|---|
| `ADMIN_USER` | CRM ga kirish logini |
| `ADMIN_PASS` | paroli — **majburiy**, bo'sh bo'lsa server ishga tushmaydi |
| `DEVICE_TOKEN` | ESP32 shu token bilan murojaat qiladi — **majburiy** |
| `SECRET_KEY` | sessiya cookie sini imzolaydi; o'zgarsa hamma tizimdan chiqadi |
| `DB_PATH` | baza fayli, sukut bo'yicha `data/qulf.db` |
| `TZ_NAME` | ko'rsatiladigan vaqt zonasi, sukut bo'yicha `Asia/Tashkent` |

Kalitlarni yaratish:

```bash
python -c "import secrets; print('DEVICE_TOKEN=' + secrets.token_urlsafe(32))"
python -c "import secrets; print('SECRET_KEY='   + secrets.token_hex(32))"
```

> `ADMIN_PASS` server har ishga tushganda qayta o'rnatiladi. Parolni almashtirish
> uchun `.env` ni tahrirlab konteynerni qayta ishga tushiring.

## Proshivkani ulash

`gate_bot/gate_bot.ino` da:

```cpp
const char* CARDS_URL   = "http://server-ip:8000/api/cards";
const char* EVENTS_URL  = "http://server-ip:8000/api/events";
const char* CARDS_TOKEN = "...";     // .env dagi DEVICE_TOKEN bilan bir xil
const char* DEVICE_NAME = "eshik-1";
```

Shundan keyin:

- ESP32 WiFi ga ulangach ro'yxatni tortadi, keyin har 5 daqiqada yangilaydi
- Har bir karta tegizilishi 15 soniya ichida CRM ga yetib boradi
- WiFi uzilsa hodisalar qurilma xotirasida (24 tagacha) to'planib turadi va
  aloqa tiklanganda yuboriladi

## API

Ikkalasi ham `Authorization: Bearer <DEVICE_TOKEN>` talab qiladi.

### `GET /api/cards`

Faqat **faol** kartalar qaytadi:

```json
{"cards": [{"uid": "8096039E", "name": "Aziz"}]}
```

CRM da kartani o'chirsangiz (yoqish/o'chirish tugmasi) u shu ro'yxatdan
chiqadi va keyingi sinxronlashda qurilma uni rad eta boshlaydi.

### `POST /api/events`

```json
{
  "device": "eshik-1",
  "events": [
    {"uid": "8096039E", "reader": "TASHQI", "result": "allowed", "ts": 1755600000}
  ]
}
```

Javob: `{"saved": 1, "skipped": 0}`

| Maydon | Izoh |
|---|---|
| `result` | `allowed`, `denied`, `open`, `close`, `learn`, `boot` — boshqasi qabul qilinmaydi |
| `ts` | UTC epoch. `0` yoki ishonchsiz qiymat bo'lsa serverga yetib kelgan vaqt qo'yiladi |
| `uid` | `allowed`/`denied`/`learn` uchun majburiy; `open`/`close`/`boot` da bo'sh bo'lishi mumkin |

Bir so'rovda ko'pi bilan 50 ta hodisa.

## Xavfsizlik

Odam va qurilma butunlay boshqacha autentifikatsiya ishlatadi: odam sessiya
cookie si bilan, qurilma esa `DEVICE_TOKEN` bilan. Qurilma hech qachon cookie
olmaydi, odam esa tokenni bilishi shart emas — shuning uchun birini bilish
ikkinchisini ochmaydi.

Parollar `scrypt` bilan saqlanadi.

**Serverni internetga ochsangiz, oldiga HTTPS qo'ying** (nginx yoki Caddy).
Aks holda `DEVICE_TOKEN` ham, parolingiz ham ochiq matnda uchadi. Proshivka
HTTPS da `setInsecure()` ishlatadi — ulanish shifrlanadi, lekin sertifikat
tekshirilmaydi.

## Zaxira nusxa

Hammasi bitta faylda:

```bash
cp server/data/qulf.db  qulf-$(date +%F).db
```

Docker da bu `./data/qulf.db` — u konteyner ichida emas, host da yotadi,
shuning uchun obraz qayta qurilsa ham kartalar joyida qoladi.
