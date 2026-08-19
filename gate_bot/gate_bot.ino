/*
  ESP32 + 2x RC522 + L298N + Telegram bot
  ---------------------------------------
  Ikki yadroli ishlash:
    Yadro 0 -> RFID (2 ta o'quvchi) + motor   (WiFi dan mustaqil, hech qachon to'xtamaydi)
    Yadro 1 -> WiFi + Telegram + server + avtomatik yopilish

  Shu sababli karta HAR DOIM ishlaydi:
    - WiFi sozlanmagan bo'lsa ham
    - Sozlash portali ochiq turganda ham
    - Telegram so'rovi ketayotganda ham

  KARTALAR KODDA EMAS
  -------------------
  Kartalar ESP32 ning ichki xotirasida (NVS) saqlanadi va elektr o'chsa ham
  yo'qolmaydi. Yangi karta qo'shish uchun kodni qayta yuklash SHART EMAS:
    1) Telegramdan /add Aziz  -> 30 soniya ichida kartani tegizing
    2) yoki serverdan:  /sync -> ro'yxat CARDS_URL dan yuklab olinadi

  Kutubxonalar: UniversalTelegramBot 1.3.0, ArduinoJson 6.21.5 (7.x EMAS), WiFiManager 2.0.17

  Bo'lim sxemasi: standart sxema bilan ham sig'adi (flash ning 90% i), lekin
  zaxira joy qolishi uchun Arduino IDE da
    Tools -> Partition Scheme -> "Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)"
  ni tanlash tavsiya etiladi - u holda 60% ga tushadi.

  Ulash
  -----
  Ikkala RC522 bitta SPI shinasini BO'LISHADI (SCK/MOSI/MISO umumiy).
  Farqi faqat SS va RST pinlarida.

  RC522 #1 (TASHQI):     RC522 #2 (ICHKI):      L298N:
    SDA/SS -> GPIO 5       SDA/SS -> GPIO 21      IN1 -> GPIO 27
    RST    -> GPIO 22      RST    -> GPIO 4       IN2 -> GPIO 26
    SCK    -> GPIO 18      SCK    -> GPIO 18      ENA -> GPIO 14  (jumper OLINGAN bo'lsin!)
    MOSI   -> GPIO 23      MOSI   -> GPIO 23      OUT1/OUT2 -> motor
    MISO   -> GPIO 19      MISO   -> GPIO 19      GND -> ESP32 GND bilan UMUMIY
    VCC    -> 3.3V         VCC    -> 3.3V

  DIQQAT: ikkita RC522 ni bir-biridan kamida 10-15 sm uzoqroq joylashtiring.
  Ikkalasi 13.56 MHz da ishlaydi - yaqin tursa antennalari bir-biriga xalaqit
  beradi va karta goho o'qilmay qoladi.
*/

#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <SPI.h>
#include <MFRC522.h>
#include <esp_system.h>
#include <time.h>

// ================= SOZLAMALAR =================
// WiFi paroli kodda saqlanmaydi - foydalanuvchi telefondan kiritadi.
const char* SETUP_AP_NAME = "Eshik-Sozlash";
const char* SETUP_AP_PASS = "eshik1234";      // kamida 8 belgi

const char* BOT_TOKEN = "BU_YERGA_BOTFATHER_TOKENINI_YOZING";

// Bo'sh bo'lsa - botni topgan hamma boshqara oladi. Bot /start da chat_id ni ko'rsatadi.
String ALLOWED_CHAT_ID = "";

// ---- Server (ixtiyoriy) ----
// Bo'sh bo'lsa server bilan ishlash butunlay o'chadi va faqat Telegram orqali
// karta qo'shiladi. To'ldirilsa - ro'yxat shu manzildan yuklanadi.
// Kutilayotgan javob (ikkala shakl ham qabul qilinadi):
//   {"cards":[{"uid":"8096039E","name":"Aziz"}]}
//   [{"uid":"8096039E","name":"Aziz"}]
const char* CARDS_URL   = "";                 // masalan "https://sizning-server.uz/api/cards"
const char* CARDS_TOKEN = "";                 // bo'sh bo'lmasa Authorization: Bearer ... yuboriladi

// Hodisalar CRM ga shu manzilga yuboriladi - "qaysi karta qachon ishlatilgani"
// shu yerdan yoziladi. Bo'sh bo'lsa hodisa yuborilmaydi.
// CARDS_TOKEN o'sha token sifatida ishlatiladi.
const char* EVENTS_URL  = "";                 // masalan "https://sizning-server.uz/api/events"
const char* DEVICE_NAME = "eshik-1";          // bir nechta qulf bo'lsa ajratish uchun

// Avtomatik sinxronlash. 0 = faqat qo'lda (/sync).
const unsigned long SYNC_INTERVAL_MS = 5UL * 60UL * 1000UL;   // 5 daqiqa

// DIQQAT: sinxronlash ro'yxatni SERVERDAGISI bilan TO'LIQ ALMASHTIRADI.
// Ya'ni server ishlatilsa - u yagona haqiqat manbai bo'ladi va /add bilan
// qo'shilgan karta keyingi sinxronlashda o'chib ketadi. Serverdan foydalansangiz,
// kartani ham serverga qo'shing.

#define BOOT_BTN 0        // BOOT tugmasi - sozlashni majburan ochish uchun

// ================= Pinlar =================
// SPI shinasi - ikkala o'quvchi uchun umumiy
#define SPI_SCK  18
#define SPI_MISO 19
#define SPI_MOSI 23

// O'quvchi #1 - tashqarida (eshik oldida)
#define SS1_PIN   5
#define RST1_PIN  22

// O'quvchi #2 - ichkarida
// GPIO 21 va 4 tanlandi: ikkalasi ham bo'sh, strapping pin emas va chiqish
// sifatida xavfsiz. (GPIO 12 va 15 dan qoching - ular boot rejimiga ta'sir qiladi.)
#define SS2_PIN   21
#define RST2_PIN  4

const int IN1 = 27;
const int IN2 = 26;
const int ENA = 14;

// ================= Vaqtlar =================
const int MOTOR_SPEED = 255;                // L298N ~2V yeydi, to'liq beramiz

const unsigned long OPEN_MS       = 700;    // ochish uchun aylanish
const unsigned long CLOSE_MS      = 700;    // yopish uchun aylanish
const unsigned long AUTO_CLOSE_MS = 10000;  // ochiq turish vaqti

const int REMOVAL_CHECKS = 5;
const int RFID_LOOP_MS   = 50;              // o'quvchilar necha ms da bir tekshiriladi

const unsigned long LEARN_WINDOW_MS = 30000;  // /add dan keyin kartani kutish vaqti

// ================= Kartalar ro'yxati =================
// Kodda emas - NVS da saqlanadi. Nomlar uchun heap ishlatmaymiz (char massiv),
// chunki ro'yxatga ikkala yadro ham tegadi va String ni yadrolar aro uzatish
// xotirani buzishi mumkin.
const int MAX_CARDS = 40;
const int NAME_LEN  = 17;                   // 16 belgi + NUL

struct Card {
  byte uid[4];
  char name[NAME_LEN];
};

Card cards[MAX_CARDS];
int  cardCount = 0;
SemaphoreHandle_t cardsMutex;
Preferences prefs;

// O'qitish rejimi: /add dan keyin tegizilgan karta eshikni ochmaydi, ro'yxatga qo'shiladi.
volatile bool          learnMode  = false;
volatile unsigned long learnUntil = 0;
char learnName[NAME_LEN] = "";

// Oxirgi notanish karta - /addlast uchun. String emas, oddiy baytlar:
// yadro 0 yozadi, yadro 1 o'qiydi.
volatile bool lastUnknownValid = false;
byte lastUnknownUid[4];

// ================= Hodisalar navbati =================
// Yadro 0 hodisani navbatga qo'yadi, yadro 1 uni serverga yuboradi.
// Bu yerda ham String ishlatmaymiz - yadrolar aro uzatilgan String heap ni buzadi.
// WiFi yo'q bo'lsa hodisalar to'planib turadi va tarmoq tiklanganda ketadi,
// shuning uchun ulanish uzilgan paytdagi kirishlar ham yo'qolmaydi.
struct Ev {
  byte     uid[4];
  bool     hasUid;
  char     reader[8];
  char     result[10];
  uint32_t ts;            // UTC epoch; 0 = qurilma soati sozlanmagan
};

const int EV_QUEUE = 24;
Ev  evQueue[EV_QUEUE];
volatile int evHead = 0, evTail = 0;
SemaphoreHandle_t evMutex;
unsigned long lastFlush = 0;
const unsigned long FLUSH_MS = 15000;   // navbatni shuncha vaqtda bir bo'shatamiz
bool timeReady = false;

// ================= PWM =================
const int PWM_CH = 0, PWM_FREQ = 1000, PWM_RES = 8;

// ================= Global =================
MFRC522 rfid1(SS1_PIN, RST1_PIN);
MFRC522 rfid2(SS2_PIN, RST2_PIN);

// Har bir o'quvchi o'z holatini yuritadi.
// "waiting" - karta o'qildi, endi u olinishini kutyapmiz.
// Bu kutish BLOKLAMAYDI: aks holda bitta o'quvchida unutib qoldirilgan karta
// ikkinchisini butunlay o'lik qilib qo'yardi.
struct ReaderState {
  MFRC522*    dev;
  const char* name;
  int         absent;
  bool        waiting;
};

ReaderState readers[] = {
  { &rfid1, "TASHQI", 0, false },
  { &rfid2, "ICHKI",  0, false },
};
const int READER_COUNT = sizeof(readers) / sizeof(readers[0]);

WiFiClientSecure secured;
UniversalTelegramBot bot(BOT_TOKEN, secured);
WiFiManager wm;

SemaphoreHandle_t gateMutex;      // motorni bir vaqtda faqat bitta yadro boshqarsin
SemaphoreHandle_t notifyMutex;    // xabar navbatini himoya qiladi

volatile bool gateOpen = false;
volatile unsigned long openedAt = 0;

String notifyChat = "";           // karta hodisalari shu chatga yuboriladi

// Xabar navbati (halqa bufer).
// Ilgari bitta String edi - endi ikkita o'quvchi bor, ikki hodisa bir vaqtda
// kelsa bittasi yo'qolib ketardi.
const int NOTIFY_QUEUE_SIZE = 8;
String notifyQueue[NOTIFY_QUEUE_SIZE];
volatile int notifyHead = 0, notifyTail = 0;

bool botReady = false;
const unsigned long POLL_MS = 1500;
unsigned long lastPoll = 0;
unsigned long lastSync = 0;

// WiFi shuncha vaqt yo'q bo'lsa - sozlash portalini qaytadan ochamiz,
// shunda foydalanuvchi qayta yuklamasdan yangi tarmoq kirita oladi.
const unsigned long WIFI_LOST_TIMEOUT = 60000;
unsigned long wifiLostAt = 0;

RTC_DATA_ATTR int bootCount = 0;

const String KEYBOARD =
  "[[{\"text\":\"\xF0\x9F\x94\x93 Ochish\",\"callback_data\":\"/open\"},"
  "{\"text\":\"\xF0\x9F\x94\x92 Yopish\",\"callback_data\":\"/close\"}],"
  "[{\"text\":\"\xE2\x84\xB9 Holat\",\"callback_data\":\"/status\"},"
  "{\"text\":\"\xF0\x9F\x92\xB3 Kartalar\",\"callback_data\":\"/cards\"}]]";

// ================= PWM yordamchilari =================
void pwmSetup() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(ENA, PWM_FREQ, PWM_RES);
#else
  ledcSetup(PWM_CH, PWM_FREQ, PWM_RES);
  ledcAttachPin(ENA, PWM_CH);
#endif
}

void pwmWrite(int duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(ENA, duty);
#else
  ledcWrite(PWM_CH, duty);
#endif
}

// ================= Motor =================
// Yumshoq start - ishga tushishdagi tok zarbasini kamaytiradi
void motorRamp(bool forward) {
  if (forward) { digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW); }
  else         { digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH); }
  for (int d = 90; d < MOTOR_SPEED; d += 10) {
    pwmWrite(d);
    delay(10);
  }
  pwmWrite(MOTOR_SPEED);
}

void motorStop() {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  pwmWrite(0);
}

// ================= Eshik (ikkala yadrodan chaqiriladi) =================
bool doOpen(const char* who) {
  bool changed = false;
  xSemaphoreTake(gateMutex, portMAX_DELAY);
  if (!gateOpen) {
    Serial.printf("[%s] OCHILMOQDA...\n", who);
    motorRamp(true);
    delay(OPEN_MS);
    motorStop();
    // openedAt BIRINCHI - aks holda ikkinchi yadro gateOpen ni ko'rib,
    // eski openedAt bilan "vaqt tugagan" deb hisoblab, darrov yopib yuborishi mumkin.
    openedAt = millis();
    gateOpen = true;
    changed  = true;
    Serial.println("  -> OCHIQ");
  }
  xSemaphoreGive(gateMutex);
  return changed;
}

bool doClose(const char* who) {
  bool changed = false;
  xSemaphoreTake(gateMutex, portMAX_DELAY);
  if (gateOpen) {
    Serial.printf("[%s] YOPILMOQDA...\n", who);
    motorRamp(false);
    delay(CLOSE_MS);
    motorStop();
    gateOpen = false;
    changed  = true;
    Serial.println("  -> YOPIQ");
  }
  xSemaphoreGive(gateMutex);
  return changed;
}

String statusText() {
  if (!gateOpen) return "\xF0\x9F\x94\x92 Eshik: YOPIQ";
  String s = "\xF0\x9F\x94\x93 Eshik: OCHIQ";
  unsigned long passed = millis() - openedAt;
  if (passed < AUTO_CLOSE_MS) {
    s += "\n\xE2\x8F\xB3 " + String((AUTO_CLOSE_MS - passed) / 1000);
    s += " soniyadan keyin avtomatik yopiladi";
  }
  return s;
}

// Yadro 0 dan Telegramga to'g'ridan-to'g'ri yozib bo'lmaydi (tarmoq yadro 1 da).
// Shuning uchun xabarni navbatga qo'yamiz.
void queueNotify(const String &msg) {
  xSemaphoreTake(notifyMutex, portMAX_DELAY);
  int next = (notifyHead + 1) % NOTIFY_QUEUE_SIZE;
  if (next != notifyTail) {            // navbat to'lgan bo'lsa - yangisini tashlab yuboramiz
    notifyQueue[notifyHead] = msg;
    notifyHead = next;
  }
  xSemaphoreGive(notifyMutex);
}

bool popNotify(String &out) {
  bool got = false;
  xSemaphoreTake(notifyMutex, portMAX_DELAY);
  if (notifyTail != notifyHead) {
    out = notifyQueue[notifyTail];
    notifyQueue[notifyTail] = "";      // String xotirasini bo'shatamiz
    notifyTail = (notifyTail + 1) % NOTIFY_QUEUE_SIZE;
    got = true;
  }
  xSemaphoreGive(notifyMutex);
  return got;
}

void clearNotifyQueue() {
  xSemaphoreTake(notifyMutex, portMAX_DELAY);
  while (notifyTail != notifyHead) {
    notifyQueue[notifyTail] = "";
    notifyTail = (notifyTail + 1) % NOTIFY_QUEUE_SIZE;
  }
  xSemaphoreGive(notifyMutex);
}

// ================= UID yordamchilari =================
// Ixcham shakl: "8096039E". Serverga ham, /del ga ham shu shakl ishlatiladi.
String uidToHex(const byte *uid) {
  char buf[9];
  snprintf(buf, sizeof(buf), "%02X%02X%02X%02X", uid[0], uid[1], uid[2], uid[3]);
  return String(buf);
}

// "8096039E" yoki "80 96 03 9E" yoki "80:96:03:9E" - hammasi qabul qilinadi.
bool hexToUid(const String &in, byte *out) {
  String h;
  for (unsigned int i = 0; i < in.length(); i++) {
    char c = in[i];
    if (isHexadecimalDigit(c)) h += c;
  }
  if (h.length() != 8) return false;
  for (int i = 0; i < 4; i++)
    out[i] = (byte) strtoul(h.substring(i * 2, i * 2 + 2).c_str(), NULL, 16);
  return true;
}

// ================= Kartalar: xotira =================
// Barcha funksiyalar mutex ichida ishlaydi.

int findCardLocked(const byte *uid) {
  for (int i = 0; i < cardCount; i++)
    if (memcmp(cards[i].uid, uid, 4) == 0) return i;
  return -1;
}

// NVS ga yozish. Format: 8096039E:Aziz\n8096039F:Dilnoza\n
void saveCards() {
  String blob;
  xSemaphoreTake(cardsMutex, portMAX_DELAY);
  for (int i = 0; i < cardCount; i++) {
    blob += uidToHex(cards[i].uid);
    blob += ':';
    blob += cards[i].name;
    blob += '\n';
  }
  xSemaphoreGive(cardsMutex);
  prefs.putString("cards", blob);
  Serial.printf("NVS: %d ta karta saqlandi (%u bayt)\n", cardCount, blob.length());
}

void loadCards() {
  String blob = prefs.getString("cards", "");
  xSemaphoreTake(cardsMutex, portMAX_DELAY);
  cardCount = 0;
  int start = 0;
  while (start < (int) blob.length() && cardCount < MAX_CARDS) {
    int nl = blob.indexOf('\n', start);
    if (nl < 0) nl = blob.length();
    String line = blob.substring(start, nl);
    start = nl + 1;

    int colon = line.indexOf(':');
    if (colon < 0) continue;
    byte uid[4];
    if (!hexToUid(line.substring(0, colon), uid)) continue;

    memcpy(cards[cardCount].uid, uid, 4);
    strlcpy(cards[cardCount].name, line.substring(colon + 1).c_str(), NAME_LEN);
    cardCount++;
  }
  xSemaphoreGive(cardsMutex);
  Serial.printf("NVS: %d ta karta yuklandi\n", cardCount);
}

// Qo'shish. Qaytaradi: 1 = qo'shildi, 0 = allaqachon bor, -1 = joy tugadi
int addCard(const byte *uid, const char *name) {
  int r;
  xSemaphoreTake(cardsMutex, portMAX_DELAY);
  if (findCardLocked(uid) >= 0)      r = 0;
  else if (cardCount >= MAX_CARDS)   r = -1;
  else {
    memcpy(cards[cardCount].uid, uid, 4);
    strlcpy(cards[cardCount].name, (name && *name) ? name : "nomsiz", NAME_LEN);
    cardCount++;
    r = 1;
  }
  xSemaphoreGive(cardsMutex);
  if (r == 1) saveCards();
  return r;
}

bool removeCardAt(int idx, String &removedName, String &removedUid) {
  bool ok = false;
  xSemaphoreTake(cardsMutex, portMAX_DELAY);
  if (idx >= 0 && idx < cardCount) {
    removedName = cards[idx].name;
    removedUid  = uidToHex(cards[idx].uid);
    for (int i = idx; i < cardCount - 1; i++) cards[i] = cards[i + 1];
    cardCount--;
    ok = true;
  }
  xSemaphoreGive(cardsMutex);
  if (ok) saveCards();
  return ok;
}

// Yadro 0 dan chaqiriladi - qisqa va bloklamaydigan bo'lishi kerak.
bool isAllowed(MFRC522::Uid &uid) {
  if (uid.size != 4) return false;
  xSemaphoreTake(cardsMutex, portMAX_DELAY);
  bool ok = findCardLocked(uid.uidByte) >= 0;
  xSemaphoreGive(cardsMutex);
  return ok;
}

String cardListText() {
  String s = "\xF0\x9F\x92\xB3 Kartalar (" + String(cardCount) + "/" + String(MAX_CARDS) + ")\n\n";
  xSemaphoreTake(cardsMutex, portMAX_DELAY);
  if (cardCount == 0) {
    s += "Ro'yxat bo'sh.\n/add <ism> yuborib karta qo'shing.";
  } else {
    for (int i = 0; i < cardCount; i++) {
      s += String(i + 1) + ". " + uidToHex(cards[i].uid) + "  " + cards[i].name + "\n";
    }
    s += "\nO'chirish: /del <raqam>";
  }
  xSemaphoreGive(cardsMutex);
  return s;
}

// ================= Serverdan sinxronlash =================
// Yadro 1 dan chaqiriladi (tarmoq shu yerda).
bool syncFromServer(String &report) {
  if (!strlen(CARDS_URL))            { report = "CARDS_URL sozlanmagan.";        return false; }
  if (WiFi.status() != WL_CONNECTED) { report = "WiFi yo'q - sinxronlab bo'lmadi."; return false; }

  HTTPClient http;
  WiFiClientSecure *sec = nullptr;
  bool started;

  if (String(CARDS_URL).startsWith("https://")) {
    sec = new WiFiClientSecure;
    if (!sec) { report = "Xotira yetmadi."; return false; }
    sec->setInsecure();                 // sertifikat tekshirilmaydi
    started = http.begin(*sec, CARDS_URL);
  } else {
    started = http.begin(CARDS_URL);
  }

  if (!started) {
    if (sec) delete sec;
    report = "Manzil noto'g'ri: " + String(CARDS_URL);
    return false;
  }

  http.setTimeout(10000);
  if (strlen(CARDS_TOKEN)) http.addHeader("Authorization", String("Bearer ") + CARDS_TOKEN);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    report = "Server javobi: " + String(code);
    http.end();
    if (sec) delete sec;
    return false;
  }

  String body = http.getString();
  http.end();
  if (sec) delete sec;

  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, body);
  if (err) { report = "JSON xato: " + String(err.c_str()); return false; }

  // {"cards":[...]} va [...] - ikkalasi ham qabul qilinadi
  JsonArray arr = doc["cards"].is<JsonArray>() ? doc["cards"].as<JsonArray>()
                                               : doc.as<JsonArray>();
  if (arr.isNull()) { report = "JSON ichida ro'yxat topilmadi."; return false; }

  // Ro'yxat TO'LIQ almashtiriladi - server yagona haqiqat manbai.
  int added = 0, skipped = 0;
  xSemaphoreTake(cardsMutex, portMAX_DELAY);
  cardCount = 0;
  for (JsonObject o : arr) {
    if (cardCount >= MAX_CARDS) { skipped++; continue; }
    const char* u = o["uid"] | "";
    byte uid[4];
    if (!hexToUid(String(u), uid)) { skipped++; continue; }
    memcpy(cards[cardCount].uid, uid, 4);
    strlcpy(cards[cardCount].name, o["name"] | "nomsiz", NAME_LEN);
    cardCount++;
    added++;
  }
  xSemaphoreGive(cardsMutex);
  saveCards();

  report = "\xE2\x9C\x85 Serverdan yuklandi: " + String(added) + " ta karta";
  if (skipped) report += "\n\xE2\x9A\xA0 " + String(skipped) + " tasi o'tkazib yuborildi (noto'g'ri UID yoki joy tugadi)";
  Serial.println(report);
  return true;
}

// ================= Hodisalarni serverga yuborish =================

// Qurilma soati. NTP dan oldin 1970-yilni qaytaradi - bunday vaqtni yuborsak
// CRM da tarix buzilardi, shuning uchun 0 yuboramiz va serverning o'zi
// qabul qilingan vaqtni qo'yadi.
uint32_t nowEpoch() {
  if (!timeReady) return 0;
  time_t t = time(nullptr);
  return (t < 1577836800) ? 0 : (uint32_t) t;
}

// Yadro 0 dan ham, yadro 1 dan ham chaqirilishi mumkin.
void pushEvent(const byte *uid, const char *reader, const char *result) {
  if (!strlen(EVENTS_URL)) return;
  xSemaphoreTake(evMutex, portMAX_DELAY);
  int next = (evHead + 1) % EV_QUEUE;
  if (next != evTail) {                 // navbat to'lgan bo'lsa yangisini tashlaymiz
    Ev &e = evQueue[evHead];
    e.hasUid = (uid != nullptr);
    if (uid) memcpy(e.uid, uid, 4); else memset(e.uid, 0, 4);
    strlcpy(e.reader, reader ? reader : "", sizeof(e.reader));
    strlcpy(e.result, result, sizeof(e.result));
    e.ts = nowEpoch();
    evHead = next;
  }
  xSemaphoreGive(evMutex);
}

// Yadro 1 dan chaqiriladi. Navbatni bir POST da yuboradi.
// Yozuvlar faqat server 2xx qaytargandan KEYIN o'chiriladi - aks holda
// tarmoq uzilganda hodisalar yo'qolib ketardi.
void flushEvents() {
  if (!strlen(EVENTS_URL)) return;
  if (WiFi.status() != WL_CONNECTED) return;

  xSemaphoreTake(evMutex, portMAX_DELAY);
  int head = evHead, tail = evTail;
  xSemaphoreGive(evMutex);
  if (head == tail) return;

  String body = "{\"device\":\"" + String(DEVICE_NAME) + "\",\"events\":[";
  int sent = 0;
  for (int i = tail; i != head; i = (i + 1) % EV_QUEUE) {
    Ev &e = evQueue[i];
    if (sent) body += ",";
    body += "{\"uid\":\"";
    if (e.hasUid) body += uidToHex(e.uid);
    body += "\",\"reader\":\"" + String(e.reader) +
            "\",\"result\":\"" + String(e.result) +
            "\",\"ts\":" + String(e.ts) + "}";
    sent++;
  }
  body += "]}";

  HTTPClient http;
  WiFiClientSecure *sec = nullptr;
  bool started;
  if (String(EVENTS_URL).startsWith("https://")) {
    sec = new WiFiClientSecure;
    if (!sec) return;
    sec->setInsecure();
    started = http.begin(*sec, EVENTS_URL);
  } else {
    started = http.begin(EVENTS_URL);
  }
  if (!started) {
    if (sec) delete sec;
    return;
  }

  http.setTimeout(10000);
  http.addHeader("Content-Type", "application/json");
  if (strlen(CARDS_TOKEN)) http.addHeader("Authorization", String("Bearer ") + CARDS_TOKEN);

  int code = http.POST(body);
  http.end();
  if (sec) delete sec;

  if (code >= 200 && code < 300) {
    xSemaphoreTake(evMutex, portMAX_DELAY);
    evTail = head;                      // faqat yuborilganini o'chiramiz;
    xSemaphoreGive(evMutex);            // shu orada qo'shilgani navbatda qoladi
    Serial.printf("Hodisalar yuborildi: %d ta\n", sent);
  } else {
    Serial.printf("Hodisa yuborilmadi (kod %d), navbatda qoldi: %d ta\n", code, sent);
  }
}

// ================= Karta =================
// HALT holatidagi kartani REQA ko'rmaydi, WUPA esa ko'radi
bool cardPresent(MFRC522 &dev) {
  byte atqa[2];
  byte len = sizeof(atqa);
  MFRC522::StatusCode s = dev.PICC_WakeupA(atqa, &len);
  if (s == MFRC522::STATUS_OK || s == MFRC522::STATUS_COLLISION) {
    dev.PICC_HaltA();
    return true;
  }
  return false;
}

// Bitta o'quvchini bir marta "xizmat qiladi". Hech qachon uzoq bloklamaydi -
// shuning uchun ikkala o'quvchi navbatma-navbat, bir-biriga xalaqit bermay ishlaydi.
void serviceReader(ReaderState &r) {
  // Karta olinishini kutish bosqichi
  if (r.waiting) {
    r.absent = cardPresent(*r.dev) ? 0 : r.absent + 1;
    if (r.absent >= REMOVAL_CHECKS) {
      r.waiting = false;
      r.absent  = 0;
    }
    return;
  }

  if (!r.dev->PICC_IsNewCardPresent()) return;
  if (!r.dev->PICC_ReadCardSerial())   return;

  byte uid[4] = {0};
  int  n = (r.dev->uid.size < 4) ? r.dev->uid.size : 4;
  memcpy(uid, r.dev->uid.uidByte, n);
  bool sized4 = (r.dev->uid.size == 4);
  String hex  = uidToHex(uid);

  r.dev->PICC_HaltA();
  r.dev->PCD_StopCrypto1();
  r.waiting = true;
  r.absent  = 0;

  // ---- O'qitish rejimi: eshikni OCHMAYMIZ, ro'yxatga qo'shamiz ----
  if (learnMode) {
    if (millis() > learnUntil) {
      learnMode = false;                     // vaqt tugagan
    } else if (!sized4) {
      queueNotify("\xE2\x9A\xA0 Bu karta 4 baytli UID bermadi, qo'shilmadi.");
      return;
    } else {
      int res = addCard(uid, learnName);
      learnMode = false;
      Serial.printf("[%s] O'QITISH: %s -> %d\n", r.name, hex.c_str(), res);
      if (res == 1) {
        pushEvent(uid, r.name, "learn");
        queueNotify("\xE2\x9C\x85 Yangi karta qo'shildi: " + hex + "  (" + String(learnName) + ")");
      }
      else if (res == 0)
        queueNotify("\xE2\x84\xB9 Bu karta allaqachon ro'yxatda: " + hex);
      else
        queueNotify("\xE2\x9B\x94 Joy tugadi (" + String(MAX_CARDS) + " ta karta).");
      return;
    }
  }

  // ---- Oddiy rejim ----
  bool ok = sized4 && isAllowed(r.dev->uid);
  Serial.printf("[%s] Karta: %s  -> %s\n", r.name, hex.c_str(),
                ok ? "RUXSAT" : "RAD ETILDI");

  if (ok) {
    pushEvent(uid, r.name, "allowed");
    if (doOpen(r.name))
      queueNotify("\xF0\x9F\x93\xB3 " + String(r.name) + " o'quvchi - karta bilan ochildi: " + hex);
  } else {
    if (sized4) {
      memcpy(lastUnknownUid, uid, 4);
      lastUnknownValid = true;
      pushEvent(uid, r.name, "denied");   // CRM da "tanilmagan kartalar" shu yerdan chiqadi
    }
    queueNotify("\xE2\x9B\x94 " + String(r.name) + " o'quvchi - notanish karta: " + hex +
                "\n\nQo'shish uchun: /addlast <ism>");
  }
}

// ================= YADRO 0: faqat karta va motor =================
void rfidTask(void *arg) {
  Serial.println("RFID vazifasi yadro 0 da ishga tushdi (2 ta o'quvchi)");
  for (;;) {
    for (int i = 0; i < READER_COUNT; i++) serviceReader(readers[i]);
    vTaskDelay(pdMS_TO_TICKS(RFID_LOOP_MS));
  }
}

// ================= Telegram =================
void handleMessages(int count) {
  for (int i = 0; i < count; i++) {
    String chat_id = String(bot.messages[i].chat_id);
    String text    = bot.messages[i].text;

    if (bot.messages[i].type == "callback_query")
      bot.answerCallbackQuery(bot.messages[i].query_id);

    if (ALLOWED_CHAT_ID.length() && chat_id != ALLOWED_CHAT_ID) {
      bot.sendMessage(chat_id, "\xE2\x9B\x94 Ruxsat yo'q.\nSizning chat_id: " + chat_id, "");
      continue;
    }

    notifyChat = chat_id;

    // Buyruq va argumentni ajratamiz: "/add Aziz" -> cmd="/add", arg="Aziz"
    String cmd = text, arg = "";
    int sp = text.indexOf(' ');
    if (sp > 0) {
      cmd = text.substring(0, sp);
      arg = text.substring(sp + 1);
      arg.trim();
    }

    if (cmd == "/open") {
      bool did = doOpen("TELEGRAM");
      if (did) pushEvent(nullptr, "TELEGRAM", "open");
      String m = did ? "\xF0\x9F\x94\x93 Ochildi\n\n" : "Eshik allaqachon ochiq\n\n";
      bot.sendMessageWithInlineKeyboard(chat_id, m + statusText(), "", KEYBOARD);
    }
    else if (cmd == "/close") {
      bool did = doClose("TELEGRAM");
      if (did) pushEvent(nullptr, "TELEGRAM", "close");
      String m = did ? "\xF0\x9F\x94\x92 Yopildi\n\n" : "Eshik allaqachon yopiq\n\n";
      bot.sendMessageWithInlineKeyboard(chat_id, m + statusText(), "", KEYBOARD);
    }
    // ---- Kartalar ----
    else if (cmd == "/cards") {
      bot.sendMessageWithInlineKeyboard(chat_id, cardListText(), "", KEYBOARD);
    }
    else if (cmd == "/add") {
      if (!arg.length()) {
        bot.sendMessage(chat_id, "Ism kerak. Masalan: /add Aziz", "");
      } else {
        strlcpy(learnName, arg.c_str(), NAME_LEN);
        learnUntil = millis() + LEARN_WINDOW_MS;
        learnMode  = true;                    // OXIRIDA - yadro 0 tayyor qiymatlarni ko'rsin
        String m = "\xF0\x9F\x93\xB3 Kartani istalgan o'quvchiga tegizing.\n";
        m += "Ism: " + String(learnName) + "\n";
        m += String(LEARN_WINDOW_MS / 1000) + " soniya vaqtingiz bor.\n\n";
        m += "\xE2\x84\xB9 Shu payt tegizilgan karta eshikni OCHMAYDI.";
        if (strlen(CARDS_URL))
          m += "\n\n\xE2\x9A\xA0 Server yoqilgan: bu karta keyingi sinxronlashda o'chib ketadi. "
               "Uni serverga ham qo'shing.";
        bot.sendMessage(chat_id, m, "");
      }
    }
    else if (cmd == "/addlast") {
      if (!lastUnknownValid) {
        bot.sendMessage(chat_id, "Oxirgi notanish karta yo'q. Avval kartani o'quvchiga tegizing.", "");
      } else {
        byte uid[4];
        memcpy(uid, lastUnknownUid, 4);
        int res = addCard(uid, arg.length() ? arg.c_str() : "nomsiz");
        if (res == 1) {
          lastUnknownValid = false;
          bot.sendMessage(chat_id, "\xE2\x9C\x85 Qo'shildi: " + uidToHex(uid) +
                                   "  (" + (arg.length() ? arg : "nomsiz") + ")", "");
        } else if (res == 0) {
          bot.sendMessage(chat_id, "\xE2\x84\xB9 Bu karta allaqachon ro'yxatda.", "");
        } else {
          bot.sendMessage(chat_id, "\xE2\x9B\x94 Joy tugadi (" + String(MAX_CARDS) + " ta).", "");
        }
      }
    }
    else if (cmd == "/del") {
      int idx = arg.toInt() - 1;              // ro'yxatdagi raqam 1 dan boshlanadi
      String nm, ux;
      if (!arg.length() || idx < 0) {
        bot.sendMessage(chat_id, "Raqam kerak. /cards ni ochib raqamini ko'ring.\nMasalan: /del 2", "");
      } else if (removeCardAt(idx, nm, ux)) {
        bot.sendMessageWithInlineKeyboard(chat_id,
          "\xF0\x9F\x97\x91 O'chirildi: " + ux + "  (" + nm + ")\n\n" + cardListText(), "", KEYBOARD);
      } else {
        bot.sendMessage(chat_id, "Bunday raqam yo'q.", "");
      }
    }
    else if (cmd == "/sync") {
      bot.sendMessage(chat_id, "\xE2\x8F\xB3 Serverdan yuklanmoqda...", "");
      String report;
      syncFromServer(report);
      lastSync = millis();
      bot.sendMessageWithInlineKeyboard(chat_id, report + "\n\n" + cardListText(), "", KEYBOARD);
    }
    // ---- WiFi ----
    else if (cmd == "/wifi") {
      // Saqlangan parolni o'chirmaydi - shunchaki sozlash oynasini ochadi
      bot.sendMessage(chat_id,
        "\xF0\x9F\x93\xB6 Sozlash oynasi ochildi.\n\n"
        "Telefondan WiFi ga ulaning:\n"
        "  Nomi  : " + String(SETUP_AP_NAME) + "\n"
        "  Parol : " + String(SETUP_AP_PASS) + "\n\n"
        "Sahifa o'zi ochiladi. Ochilmasa: 192.168.4.1", "");
      delay(500);
      wm.startConfigPortal(SETUP_AP_NAME, SETUP_AP_PASS);
    }
    else if (cmd == "/wifireset") {
      bot.sendMessage(chat_id, "WiFi sozlamalari o'chirildi. Plata qayta yuklanmoqda.\n"
                               "\xE2\x84\xB9 Kartalar ro'yxati saqlanib qoladi.", "");
      wm.resetSettings();
      delay(1500);
      ESP.restart();
    }
    else if (cmd == "/status" || cmd == "/start") {
      String head = "\xF0\x9F\x9A\xAA Eshik boshqaruvi\n\n";
      if (!ALLOWED_CHAT_ID.length())
        head += "\xE2\x9A\xA0 Himoya yoqilmagan!\nchat_id: " + chat_id + "\n\n";
      String tail = "\n\n\xF0\x9F\x92\xB3 Kartalar: " + String(cardCount) + " ta";
      if (cmd == "/start")
        tail += "\n\nKarta qo'shish: /add <ism>\nRo'yxat: /cards";
      bot.sendMessageWithInlineKeyboard(chat_id, head + statusText() + tail, "", KEYBOARD);
    }
    else {
      bot.sendMessageWithInlineKeyboard(chat_id, statusText(), "", KEYBOARD);
    }
  }
}

// ================= O'quvchini ishga tushirish =================
// VersionReg - modul javob berayotganini bilishning eng ishonchli usuli.
// 0x00 yoki 0xFF => sim ulanmagan, noto'g'ri pin yoki modul kuygan.
// Ikkita o'quvchi bilan bu tekshiruv ayniqsa kerak: qaysi biri ulanmaganini
// darrov ko'rsatadi.
void initReader(ReaderState &r) {
  r.dev->PCD_Init();
  delay(50);
  byte v = r.dev->PCD_ReadRegister(MFRC522::VersionReg);
  Serial.printf("O'quvchi %-6s VersionReg = 0x%02X  %s\n", r.name, v,
                (v == 0x00 || v == 0xFF) ? "<-- JAVOB YO'Q! ulanishni tekshiring"
                                         : "ok");
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(500);
  bootCount++;

  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pwmSetup();
  motorStop();

  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, SS1_PIN);

  Serial.println("\n=== Eshik boshqaruvi (2x RFID + Telegram) ===");
  for (int i = 0; i < READER_COUNT; i++) initReader(readers[i]);

  Serial.printf("Ishga tushishlar: %d\n", bootCount);
  Serial.printf("Reset sababi    : %s\n",
                esp_reset_reason() == ESP_RST_BROWNOUT
                ? "BROWNOUT (KUCHLANISH CHOKDI!)" : "normal");

  gateMutex   = xSemaphoreCreateMutex();
  notifyMutex = xSemaphoreCreateMutex();
  cardsMutex  = xSemaphoreCreateMutex();
  evMutex     = xSemaphoreCreateMutex();

  // Ishga tushish hodisasi. Soat hali sozlanmagan - ts=0 ketadi va serverning
  // o'zi qabul qilingan vaqtni qo'yadi. Qayta yuklanishlarni kuzatish uchun kerak:
  // tez-tez takrorlansa, bu odatda kuchlanish cho'kishidan darak beradi.
  pushEvent(nullptr, "", "boot");

  // Kartalarni RFID vazifasidan OLDIN yuklaymiz, aks holda birinchi soniyalarda
  // ro'yxat bo'sh bo'lib, haqiqiy karta rad etilardi.
  prefs.begin("qulf", false);
  loadCards();

  // Kartani BIRINCHI ishga tushiramiz - WiFi qanday bo'lishidan qat'i nazar ishlaydi
  xTaskCreatePinnedToCore(rfidTask, "rfid", 6144, NULL, 1, NULL, 0);

  // WiFi - bloklamaydigan rejimda. setup() shu yerda to'xtab qolmaydi.
  pinMode(BOOT_BTN, INPUT_PULLUP);
  wm.setConfigPortalBlocking(false);
  wm.setConfigPortalTimeout(0);          // bloklamagani uchun cheksiz turaversin
  wm.autoConnect(SETUP_AP_NAME, SETUP_AP_PASS);

  Serial.println("Kartalar hoziroq ishlaydi. Telegram - WiFi ulangach.");
  Serial.println("Sozlashni ochish: BOOT tugmasini 2 soniya bosib turing.\n");
}

// ================= YADRO 1: WiFi, Telegram, avtomatik yopilish =================
// BOOT tugmasi 2 soniya bosib turilsa - sozlash portalini ochamiz.
// Diqqat: GPIO0 ni RESET paytida bosib turib bo'lmaydi - u holda ESP32
// proshivka yuklash rejimiga kiradi va sketch umuman ishlamaydi.
// Shuning uchun tugmani ish paytida, loop ichida kuzatamiz.
void checkSetupButton() {
  static unsigned long btnDown = 0;

  if (digitalRead(BOOT_BTN) == LOW) {
    if (btnDown == 0) {
      btnDown = millis();
    } else if (millis() - btnDown > 2000 && !wm.getConfigPortalActive()) {
      Serial.println("\nBOOT 2 soniya bosildi -> sozlash portali ochilmoqda");
      Serial.printf("  Telefondan '%s' ga ulaning (parol: %s)\n\n",
                    SETUP_AP_NAME, SETUP_AP_PASS);
      wm.startConfigPortal(SETUP_AP_NAME, SETUP_AP_PASS);
      btnDown = 0;
    }
  } else {
    btnDown = 0;
  }
}

void loop() {
  checkSetupButton();
  wm.process();                       // sozlash portalini yuritadi

  // O'qitish rejimi vaqti tugadimi?
  if (learnMode && millis() > learnUntil) {
    learnMode = false;
    queueNotify("\xE2\x8F\xB1 Vaqt tugadi - karta tegizilmadi. /add bilan qaytadan urinib ko'ring.");
  }

  // WiFi endigina ulandimi?
  if (!botReady && WiFi.status() == WL_CONNECTED) {
    secured.setInsecure();
    bot.longPoll = 0;
    botReady = true;
    Serial.println("WiFi: " + WiFi.SSID() + "   IP: " + WiFi.localIP().toString());
    Serial.println("Bot tayyor. Telegramda /start yuboring.");

    // Soatni NTP dan olamiz - hodisalarga to'g'ri vaqt qo'yish uchun.
    // UTC da yuritamiz; mahalliy vaqtga o'girishni CRM bajaradi.
    configTime(0, 0, "pool.ntp.org", "time.google.com");

    // Ulanishi bilan ro'yxatni serverdan olamiz
    if (strlen(CARDS_URL)) {
      String report;
      syncFromServer(report);
      lastSync = millis();
    }
  }

  // NTP javob berdimi? Shundan keyin hodisalarga haqiqiy vaqt qo'yiladi.
  if (!timeReady && WiFi.status() == WL_CONNECTED && time(nullptr) > 1577836800) {
    timeReady = true;
    Serial.println("Soat NTP dan olindi.");
  }
  if (botReady && WiFi.status() != WL_CONNECTED) {
    botReady = false;
    Serial.println("WiFi uzildi - kartalar ishlashda davom etadi.");
  }

  // WiFi uzoq vaqt yo'q bo'lsa - sozlash portalini qaytadan ochamiz
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiLostAt == 0) {
      wifiLostAt = millis();
    } else if (millis() - wifiLostAt > WIFI_LOST_TIMEOUT && !wm.getConfigPortalActive()) {
      Serial.println("WiFi 60 soniyadan beri yo'q -> sozlash portali ochilmoqda");
      Serial.printf("  Telefondan '%s' ga ulaning (parol: %s)\n", SETUP_AP_NAME, SETUP_AP_PASS);
      wm.startConfigPortal(SETUP_AP_NAME, SETUP_AP_PASS);
      wifiLostAt = 0;
    }
  } else {
    wifiLostAt = 0;
  }

  // Avtomatik yopilish
  if (gateOpen && millis() - openedAt >= AUTO_CLOSE_MS) {
    if (doClose("AVTO")) {
      pushEvent(nullptr, "AVTO", "close");
      queueNotify("\xF0\x9F\x94\x92 Vaqt tugadi - avtomatik yopildi");
    }
  }

  // Hodisalarni serverga yuborish. Telegram dan mustaqil - bot sozlanmagan
  // bo'lsa ham CRM tarixi to'lib boradi.
  if (strlen(EVENTS_URL) && millis() - lastFlush > FLUSH_MS) {
    flushEvents();
    lastFlush = millis();
  }

  if (botReady) {
    // Yadro 0 dan kelgan xabarlarni yuboramiz
    if (notifyChat.length()) {
      String msg;
      while (popNotify(msg)) bot.sendMessage(notifyChat, msg, "");
    }

    // Davriy sinxronlash
    if (strlen(CARDS_URL) && SYNC_INTERVAL_MS && millis() - lastSync > SYNC_INTERVAL_MS) {
      String report;
      syncFromServer(report);
      lastSync = millis();
    }

    if (millis() - lastPoll > POLL_MS) {
      int n = bot.getUpdates(bot.last_message_received + 1);
      while (n) {
        handleMessages(n);
        n = bot.getUpdates(bot.last_message_received + 1);
      }
      lastPoll = millis();
    }
  } else {
    clearNotifyQueue();               // WiFi yo'q - xabarlarni to'plamaymiz
  }

  delay(10);
}
