/*
  ESP32 + 2x RC522 + L298N + Telegram bot
  ---------------------------------------
  Ikki yadroli ishlash:
    Yadro 0 -> RFID (2 ta o'quvchi) + motor   (WiFi dan mustaqil, hech qachon to'xtamaydi)
    Yadro 1 -> WiFi + Telegram + avtomatik yopilish

  Shu sababli karta HAR DOIM ishlaydi:
    - WiFi sozlanmagan bo'lsa ham
    - Sozlash portali ochiq turganda ham
    - Telegram so'rovi ketayotganda ham

  Kutubxonalar: UniversalTelegramBot 1.3.0, ArduinoJson 6.21.5 (7.x EMAS), WiFiManager 2.0.17

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
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <MFRC522.h>
#include <esp_system.h>

// ================= SOZLAMALAR =================
// WiFi paroli kodda saqlanmaydi - foydalanuvchi telefondan kiritadi.
const char* SETUP_AP_NAME = "Eshik-Sozlash";
const char* SETUP_AP_PASS = "eshik1234";      // kamida 8 belgi

const char* BOT_TOKEN = "BU_YERGA_BOTFATHER_TOKENINI_YOZING";

// Bo'sh bo'lsa - botni topgan hamma boshqara oladi. Bot /start da chat_id ni ko'rsatadi.
String ALLOWED_CHAT_ID = "";

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

// ================= Ruxsat berilgan kartalar =================
// Ro'yxat ikkala o'quvchi uchun umumiy.
// Ichki/tashqi uchun alohida ro'yxat kerak bo'lsa - shu yerda ikkita massiv
// qilib, isAllowed() ga o'quvchi indeksini uzating.
const byte ALLOWED[][4] = {
  { 0x80, 0x96, 0x03, 0x9E },
};
const int ALLOWED_COUNT = sizeof(ALLOWED) / sizeof(ALLOWED[0]);

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

// WiFi shuncha vaqt yo'q bo'lsa - sozlash portalini qaytadan ochamiz,
// shunda foydalanuvchi qayta yuklamasdan yangi tarmoq kirita oladi.
const unsigned long WIFI_LOST_TIMEOUT = 60000;
unsigned long wifiLostAt = 0;

RTC_DATA_ATTR int bootCount = 0;

const String KEYBOARD =
  "[[{\"text\":\"\xF0\x9F\x94\x93 Ochish\",\"callback_data\":\"/open\"},"
  "{\"text\":\"\xF0\x9F\x94\x92 Yopish\",\"callback_data\":\"/close\"}],"
  "[{\"text\":\"\xE2\x84\xB9 Holat\",\"callback_data\":\"/status\"}]]";

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

// ================= Karta =================
bool isAllowed(MFRC522::Uid &uid) {
  if (uid.size != 4) return false;
  for (int i = 0; i < ALLOWED_COUNT; i++) {
    bool same = true;
    for (byte j = 0; j < 4; j++)
      if (uid.uidByte[j] != ALLOWED[i][j]) { same = false; break; }
    if (same) return true;
  }
  return false;
}

String uidToString(MFRC522::Uid &uid) {
  String s;
  for (byte i = 0; i < uid.size; i++) {
    if (uid.uidByte[i] < 0x10) s += "0";
    s += String(uid.uidByte[i], HEX);
    if (i + 1 < uid.size) s += " ";
  }
  s.toUpperCase();
  return s;
}

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

  String uid = uidToString(r.dev->uid);
  bool ok = isAllowed(r.dev->uid);
  r.dev->PICC_HaltA();
  r.dev->PCD_StopCrypto1();

  Serial.printf("[%s] Karta: %s  -> %s\n", r.name, uid.c_str(),
                ok ? "RUXSAT" : "RAD ETILDI");

  if (ok) {
    if (doOpen(r.name))
      queueNotify("\xF0\x9F\x93\xB3 " + String(r.name) + " o'quvchi - karta bilan ochildi: " + uid);
  } else {
    queueNotify("\xE2\x9B\x94 " + String(r.name) + " o'quvchi - notanish karta: " + uid);
  }

  r.waiting = true;
  r.absent  = 0;
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

    if (text == "/open") {
      String m = doOpen("TELEGRAM") ? "\xF0\x9F\x94\x93 Ochildi\n\n" : "Eshik allaqachon ochiq\n\n";
      bot.sendMessageWithInlineKeyboard(chat_id, m + statusText(), "", KEYBOARD);
    }
    else if (text == "/close") {
      String m = doClose("TELEGRAM") ? "\xF0\x9F\x94\x92 Yopildi\n\n" : "Eshik allaqachon yopiq\n\n";
      bot.sendMessageWithInlineKeyboard(chat_id, m + statusText(), "", KEYBOARD);
    }
    else if (text == "/wifi") {
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
    else if (text == "/wifireset") {
      bot.sendMessage(chat_id, "WiFi sozlamalari o'chirildi. Plata qayta yuklanmoqda.", "");
      wm.resetSettings();
      delay(1500);
      ESP.restart();
    }
    else if (text == "/status" || text == "/start") {
      String head = "\xF0\x9F\x9A\xAA Eshik boshqaruvi\n\n";
      if (!ALLOWED_CHAT_ID.length())
        head += "\xE2\x9A\xA0 Himoya yoqilmagan!\nchat_id: " + chat_id + "\n\n";
      bot.sendMessageWithInlineKeyboard(chat_id, head + statusText(), "", KEYBOARD);
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

  // WiFi endigina ulandimi?
  if (!botReady && WiFi.status() == WL_CONNECTED) {
    secured.setInsecure();
    bot.longPoll = 0;
    botReady = true;
    Serial.println("WiFi: " + WiFi.SSID() + "   IP: " + WiFi.localIP().toString());
    Serial.println("Bot tayyor. Telegramda /start yuboring.");
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
    if (doClose("AVTO")) queueNotify("\xF0\x9F\x94\x92 Vaqt tugadi - avtomatik yopildi");
  }

  if (botReady) {
    // Yadro 0 dan kelgan xabarlarni yuboramiz
    if (notifyChat.length()) {
      String msg;
      while (popNotify(msg)) bot.sendMessage(notifyChat, msg, "");
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
