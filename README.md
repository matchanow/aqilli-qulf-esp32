# Aqilli qulf — ESP32

RFID karta va Telegram bot orqali boshqariladigan eshik/qulf tizimi.

**Ikkita RC522 o'quvchi** ishlatiladi — biri tashqarida (eshik oldida), biri
ichkarida. Ikkalasi ham bir xil ro'yxatdagi kartalarni qabul qiladi va eshikni
ochadi; Telegramga kelgan xabarda qaysi o'quvchi ishlagani ko'rsatiladi.

Karta tegizilganda eshik ochiladi va belgilangan vaqtdan keyin o'zi yopiladi.
Shu bilan birga Telegram botdagi tugmalar orqali istalgan paytda ochish, yopish
va holatni ko'rish mumkin.

## Imkoniyatlari

- **RFID karta bilan ochish** — faqat ro'yxatdagi kartalar ishlaydi
- **Ikkita o'quvchi** — tashqi va ichki, bitta SPI shinasida, bir-birini bloklamaydi
- **Kartalar kodda emas** — ESP32 xotirasida (NVS) saqlanadi, elektr o'chsa ham yo'qolmaydi
- **Telegramdan karta qo'shish** — `/add Aziz` yuborib kartani tegizasiz, tamom
- **Serverdan yuklash** — ro'yxat sizning API dan avtomatik tortib olinadi
- **Telegram boshqaruvi** — `Ochish` / `Yopish` / `Holat` tugmalari
- **Avtomatik yopilish** — belgilangan vaqtdan keyin
- **Holat ko'rsatkichi** — ochiqmi yopiqmi, necha soniya qolgani
- **WiFi ni telefondan sozlash** — parol kodda saqlanmaydi, kompyuter kerak emas
- **Ikki yadroli ishlash** — karta WiFi/Telegram dan mustaqil, hech qachon to'xtamaydi

## Kerakli qismlar

| Qism | Izoh |
|---|---|
| ESP32 DevKit | ESP32-WROOM-32 asosidagi |
| RC522 (MFRC522) × **2** | RFID o'quvchi, **3.3V** |
| L298N | motor drayveri |
| DC motor | 5–6V kichik motor |
| Manba | motor uchun alohida, 5–7.5V |

## Ulash

### RC522 × 2 → ESP32

Ikkala modul bitta SPI shinasini bo'lishadi — `SCK`, `MOSI`, `MISO` **umumiy**,
ya'ni har ikkala modulning shu uchta pini ESP32 ning bir xil oyoqchasiga boradi.
Farqi faqat `SS` va `RST` da: shu ikki chiziq orqali ESP32 qaysi modul bilan
gaplashayotganini tanlaydi.

| RC522 pini | #1 — TASHQI | #2 — ICHKI |
|---|---|---|
| SDA / SS | GPIO **5** | GPIO **21** |
| RST | GPIO **22** | GPIO **4** |
| SCK | GPIO 18 | GPIO 18 *(umumiy)* |
| MOSI | GPIO 23 | GPIO 23 *(umumiy)* |
| MISO | GPIO 19 | GPIO 19 *(umumiy)* |
| IRQ | ulanmaydi | ulanmaydi |
| VCC | **3.3V** | **3.3V** |
| GND | GND | GND |

> **Modullarni bir-biridan kamida 10–15 sm uzoqroq joylashtiring.** Ikkalasi
> 13.56 MHz da ishlaydi — yaqin tursa antennalari bir-biriga xalaqit beradi va
> karta goho o'qilmay qoladi. Eshikning ikki tomoniga o'rnatilsa bu muammo
> o'z-o'zidan hal bo'ladi.

> **Nega aynan GPIO 21 va 4?** Ikkalasi ham bo'sh, strapping pin emas va chiqish
> sifatida xavfsiz. GPIO 12 va 15 dan qoching — ular ESP32 ning boot rejimiga
> ta'sir qiladi va plata umuman ishga tushmay qolishi mumkin.

### L298N → ESP32

| L298N | ESP32 |
|---|---|
| IN1 | GPIO 27 |
| IN2 | GPIO 26 |
| ENA | GPIO 14 |
| OUT1 / OUT2 | motor |
| GND | GND (umumiy) |

```
                              ESP32
                           ┌─────────┐
      #1 TASHQI  SS   ─────┤  5   27 ├───── IN1   L298N
      #2 ICHKI   SS   ─────┤ 21   26 ├───── IN2
      #1 TASHQI  RST  ─────┤ 22   14 ├───── ENA
      #2 ICHKI   RST  ─────┤  4      │
                           │         │
      ikkalasi   SCK  ─────┤ 18      │
      ikkalasi   MOSI ─────┤ 23      │
      ikkalasi   MISO ─────┤ 19  3V3 ├───── ikkala RC522 VCC
                           │     GND ├───── umumiy GND
                           └─────────┘
```

### Jumperlar — diqqat

**ENA jumperi olinishi shart.** Turgan bo'lsa ENA doimo 5V ga ulanadi,
GPIO 14 dagi PWM ishlamaydi va ESP32 pini 5V bilan urishadi.

**5V-EN jumperi manba kuchlanishiga bog'liq:**

| Motor manbai | 5V-EN jumperi | `+5V` klemma |
|---|---|---|
| 5V | oling | 5V bering |
| 7V dan yuqori | qo'ying | tegmang |

## Kutubxonalar

Arduino IDE → Library Manager:

| Kutubxona | Versiya |
|---|---|
| MFRC522 | 1.4.12 |
| UniversalTelegramBot | 1.3.0 |
| ArduinoJson | **6.21.5** |
| WiFiManager | 2.0.17 |

> **ArduinoJson 7.x ISHLAMAYDI.** `UniversalTelegramBot` o'rnatilganda 7.x
> avtomatik tortiladi — uni qo'lda 6.21.5 ga tushiring.

Plata: **ESP32 Dev Module**

## Ishga tushirish

1. [@BotFather](https://t.me/BotFather) dan bot yarating va tokenni oling
2. `gate_bot/gate_bot.ino` da tokenni yozing:

   ```cpp
   const char* BOT_TOKEN = "BU_YERGA_BOTFATHER_TOKENINI_YOZING";
   ```

3. Kodni platagizga yuklang

   > Standart bo'lim sxemasi bilan sketch **sig'adi**, lekin flash ning 90% ini
   > egallaydi. Zaxira joy qoldirish uchun **Tools → Partition Scheme →
   > `Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)`** ni tanlashni tavsiya
   > qilaman — u holda 60% atrofida qoladi va keyin kod qo'shsangiz sig'maslik
   > muammosiga duch kelmaysiz.

   > Kartalarni kodga yozish **shart emas** — ular keyin Telegramdan qo'shiladi.
   > Pastdagi [Kartalarni boshqarish](#kartalarni-boshqarish) bo'limiga qarang.

4. Telefondan `Eshik-Sozlash` WiFi ga ulaning (parol: `eshik1234`)
5. Ochilgan sahifada o'z WiFi ingizni tanlab, parolini kiriting
6. Telegramda botga `/start` yuboring
7. Javobdagi `chat_id` ni kodga yozing va qayta yuklang:

   ```cpp
   String ALLOWED_CHAT_ID = "123456789";
   ```

   Bu qadam **muhim** — aks holda botni topgan har kim eshikni ocha oladi.

8. Endi kartalarni qo'shing — [Kartalarni boshqarish](#kartalarni-boshqarish) bo'limiga qarang

## Telegram buyruqlari

| Buyruq | Vazifasi |
|---|---|
| `/start` | tugmalar va holat |
| `/status` | joriy holat |
| `/open` | ochish |
| `/close` | yopish |
| `/cards` | kartalar ro'yxati |
| `/add <ism>` | yangi karta o'qitish (30 soniya ichida tegizing) |
| `/addlast <ism>` | oxirgi rad etilgan kartani ro'yxatga qo'shish |
| `/del <raqam>` | kartani o'chirish (`/cards` dagi raqam bo'yicha) |
| `/sync` | ro'yxatni serverdan yangilash |
| `/wifi` | WiFi sozlash oynasini ochish |
| `/wifireset` | saqlangan WiFi ni o'chirish |

## Kartalarni boshqarish

Kartalar **kodda saqlanmaydi**. Ular ESP32 ning ichki xotirasida (NVS) turadi —
elektr o'chsa ham, plata qayta yuklansa ham yo'qolmaydi. Yangi karta qo'shish
uchun kodni qayta yuklash kerak emas.

### Karta qo'shish — usul 1: o'qitish

1. Telegramda `/add Aziz` yuboring
2. 30 soniya ichida kartani **istalgan** o'quvchiga tegizing
3. Bot tasdiqlaydi: `✅ Yangi karta qo'shildi: 8096039E (Aziz)`

> O'qitish paytida tegizilgan karta eshikni **ochmaydi** — u faqat ro'yxatga
> yozib olinadi. 30 soniya ichida tegizmasangiz rejim o'zi bekor bo'ladi.

### Karta qo'shish — usul 2: notanish kartadan

Ro'yxatda yo'q karta tegizilsa, bot uning UID sini yuboradi:

```
⛔ TASHQI o'quvchi - notanish karta: A1B2C3D4

Qo'shish uchun: /addlast <ism>
```

`/addlast Dilnoza` yuborsangiz — o'sha karta ro'yxatga qo'shiladi. Kartani
qaytadan tegizish shart emas.

### O'chirish

`/cards` — raqamlangan ro'yxat chiqadi:

```
💳 Kartalar (2/40)

1. 8096039E  Aziz
2. A1B2C3D4  Dilnoza

O'chirish: /del <raqam>
```

`/del 2` — ikkinchisi o'chadi.

> Ro'yxatga eng ko'pi **40 ta** karta sig'adi (`MAX_CARDS`). Ism 16 belgigacha.

## CRM — brauzerdan boshqarish

Kartalarni Telegram o'rniga veb-paneldan boshqarish mumkin. Panel shu
omborning [`server/`](server/) papkasida — FastAPI + SQLite, bitta konteyner.

Nima beradi:

- kartalarni qo'shish, o'chirish, vaqtincha to'xtatish
- **tanilmagan kartalar** alohida ro'yxatda: qurilmaga tegizilgan karta o'zi
  chiqadi, ismini yozib **Qo'shish** ni bosasiz — UID ni qo'lda kiritish shart emas
- har bir karta uchun: qachon qo'shilgan, oxirgi marta qachon ishlatilgan, necha marta ochgan
- to'liq tarix: kim, qachon, qaysi o'quvchida, ruxsat berildimi yoki rad etildimi

O'rnatish va sozlash: [`server/README.md`](server/README.md).

## Serverdan yuklash

`gate_bot.ino` da manzillarni ko'rsating:

```cpp
const char* CARDS_URL   = "http://server-ip:8000/api/cards";
const char* EVENTS_URL  = "http://server-ip:8000/api/events";
const char* CARDS_TOKEN = "maxfiy-kalit";     // server .env dagi DEVICE_TOKEN
const char* DEVICE_NAME = "eshik-1";          // bir nechta qulf bo'lsa ajratish uchun

const unsigned long SYNC_INTERVAL_MS = 5UL * 60UL * 1000UL;   // 5 daqiqa, 0 = o'chirilgan
```

**Kartalar ro'yxati** quyidagi holatlarda tortiladi:

- WiFi ga ulangan zahoti
- har `SYNC_INTERVAL_MS` da bir marta
- Telegramdan `/sync` yuborilganda

**Hodisalar** (kim qachon kirgani) har 15 soniyada yuboriladi. WiFi uzilsa ular
qurilma xotirasida 24 tagacha to'planib turadi va aloqa tiklanganda ketadi —
shuning uchun internet yo'q paytdagi kirishlar ham tarixda qoladi.

Vaqtni qurilma NTP dan oladi. Soat hali sozlanmagan bo'lsa (masalan ishga
tushgan zahoti) `ts=0` yuboriladi va serverning o'zi qabul qilingan vaqtni qo'yadi.

`CARDS_TOKEN` bo'sh bo'lmasa, so'rovlarga `Authorization: Bearer <token>`
sarlavhasi qo'shiladi.

### Server nima qaytarishi kerak

Oddiy `GET` so'roviga JSON. Ikkala shakl ham qabul qilinadi:

```json
{
  "cards": [
    { "uid": "8096039E", "name": "Aziz" },
    { "uid": "A1B2C3D4", "name": "Dilnoza" }
  ]
}
```

```json
[
  { "uid": "8096039E", "name": "Aziz" }
]
```

`uid` — 4 baytli UID ning 8 ta hex belgisi. Ajratgich qo'ysangiz ham bo'ladi
(`80 96 03 9E`, `80:96:03:9E`) — kod ularni o'zi tozalaydi.

### ⚠ Server yagona haqiqat manbai bo'ladi

Sinxronlash ro'yxatni **to'liq almashtiradi**. Ya'ni `CARDS_URL` to'ldirilgan
bo'lsa, `/add` bilan qo'shilgan karta keyingi sinxronlashda **o'chib ketadi**.

Shuning uchun ikki xil ishlash tartibidan birini tanlang:

| Tartib | `CARDS_URL` | Karta qayerdan qo'shiladi |
|---|---|---|
| Faqat Telegram | bo'sh qoldiring | `/add`, `/addlast` |
| Server boshqaradi | to'ldiring | serverning o'z bazasidan |

Server tartibida ham `/add` ishlaydi — lekin u **vaqtinchalik**: mehmonni
darhol kiritish uchun qulay, keyin serverga ham qo'shib qo'ying. Bot `/add`
javobida buni eslatib turadi.

### HTTPS haqida

Kod `setInsecure()` ishlatadi — ya'ni server sertifikati **tekshirilmaydi**.
Bu ESP32 da sertifikat zanjirini yangilab turish muammosini chetlab o'tadi,
lekin o'rtadagi hujumdan himoya qilmaydi. Jiddiy o'rnatmalarda serveringizning
ildiz sertifikatini kodga qo'yib, `setCACert()` ga o'ting.

## WiFi ni qayta sozlash

Sozlash oynasini ochishning to'rt yo'li:

- **BOOT tugmasini 2 soniya bosib turish** (plata ishlab turganda)
- Telegramdan `/wifi`
- Telegramdan `/wifireset` (eski parolni ham o'chiradi)
- Avtomatik — saqlangan tarmoq topilmasa yoki 60 soniya yo'qolsa

> BOOT tugmasini **RESET paytida** bosib turmang — GPIO 0 past bo'lsa ESP32
> proshivka yuklash rejimiga kiradi va dastur umuman ishga tushmaydi.
> Shuning uchun tugma ish jarayonida kuzatiladi.

## Sozlamalar

`gate_bot.ino` boshidagi qiymatlar:

```cpp
const int MOTOR_SPEED = 255;                // 0..255
const unsigned long OPEN_MS       = 700;    // ochish uchun aylanish
const unsigned long CLOSE_MS      = 700;    // yopish uchun aylanish
const unsigned long AUTO_CLOSE_MS = 10000;  // ochiq turish vaqti
```

Motor ko'p aylansa `OPEN_MS` / `CLOSE_MS` ni kamaytiring, yetib bormasa oshiring.

## Yordamchi dasturlar

| Papka | Vazifasi |
|---|---|
| `tools/rfid_test` | RC522 ishlayaptimi, karta UID sini ko'rsatadi |
| `tools/rc522_diag` | SPI ni turli tezliklarda sinaydi (ulanish muammolari uchun) |
| `tools/wifi_scan` | ESP32 ko'rayotgan WiFi tarmoqlarni ro'yxatlaydi |

## Ko'p uchraydigan muammolar

**`Communication failure, is the MFRC522 properly connected?`**

Bu xabar faqat `VersionReg` `0x00` yoki `0xFF` bo'lganda chiqadi — ya'ni SPI
umuman ishlamayapti. Tekshiring:

1. Modul pinlari lehimlanganmi (ko'p modul lehimsiz keladi)
2. VCC **3.3V** ga ulanganmi (5V modulni kuydiradi)
3. GND umumiymi
4. Dupont simlar butunmi

Arduino Uno bilan bu xato tez-tez chiqadi — Uno 5V mantiqda ishlaydi va uning
3.3V liniyasi kuchsiz. ESP32 esa o'zi 3.3V, shuning uchun RC522 unga to'g'ri keladi.

**Versiya `0x82` yoki boshqa notanish qiymat**

Kutubxona faqat `0x91`, `0x92`, `0x88` ni taniydi. Boshqa qiymat klon chipni
bildiradi va odatda **muammo emas** — karta o'qilayotgan bo'lsa, e'tibor bermang.

**Faqat bitta o'quvchi ishlayapti**

Plata ishga tushganda Serial Monitor (115200) da har ikkala modul tekshiriladi:

```
O'quvchi TASHQI VersionReg = 0x92  ok
O'quvchi ICHKI  VersionReg = 0x00  <-- JAVOB YO'Q! ulanishni tekshiring
```

`0x00` yoki `0xFF` — modul umuman javob bermayapti. Tekshiring:

1. `SS` va `RST` **har bir modulda alohida** pinga ulanganmi (5/22 va 21/4)
2. `SCK`, `MOSI`, `MISO` ikkala modulda **bir xil** pinga ulanganmi
3. Ikkala modulga ham 3.3V va GND borganmi

**Kartalar goho o'qilmay qoladi**

Modullar bir-biriga juda yaqin — antennalari xalaqit beryapti. 10–15 sm
uzoqlashtiring yoki eshikning ikki tomoniga o'rnating.

Agar uzoqlashtirish imkoni bo'lmasa, arzon RC522 modullarining MISO chizig'i
tanlanmagan holatda ham to'liq "qo'yib yubormasligi" sabab bo'lishi mumkin —
bu holda MISO ga 3-holatli bufer (masalan 74HC125) qo'yish kerak bo'ladi.

**Motor kuchsiz aylanadi**

L298N kirish kuchlanishining ~2V ini o'zi yeydi. 5V bersangiz motorga ~3V qoladi.

| L298N kirishi | Motorga yetadi |
|---|---|
| 5V | ~3V |
| 6V | ~4V |
| 7.4V | ~5.4V |

**Motor aylanganda plata qayta yuklanadi**

Motorni ESP32 ning 3.3V/5V pinidan quvvatlantirmang. Alohida manba ishlating,
GND larni birlashtiring. Jurnalda `BROWNOUT` chiqsa, sabab aynan shu.

## Litsenziya

MIT
