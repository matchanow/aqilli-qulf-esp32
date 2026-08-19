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

3. Kartangizning UID sini `tools/rfid_test` bilan aniqlang va ro'yxatga qo'shing:

   ```cpp
   const byte ALLOWED[][4] = {
     { 0x80, 0x96, 0x03, 0x9E },
   };
   ```

4. Kodni platagizga yuklang
5. Telefondan `Eshik-Sozlash` WiFi ga ulaning (parol: `eshik1234`)
6. Ochilgan sahifada o'z WiFi ingizni tanlab, parolini kiriting
7. Telegramda botga `/start` yuboring
8. Javobdagi `chat_id` ni kodga yozing va qayta yuklang:

   ```cpp
   String ALLOWED_CHAT_ID = "123456789";
   ```

   Bu qadam **muhim** — aks holda botni topgan har kim eshikni ocha oladi.

## Telegram buyruqlari

| Buyruq | Vazifasi |
|---|---|
| `/start` | tugmalar va holat |
| `/status` | joriy holat |
| `/open` | ochish |
| `/close` | yopish |
| `/wifi` | WiFi sozlash oynasini ochish |
| `/wifireset` | saqlangan WiFi ni o'chirish |

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
