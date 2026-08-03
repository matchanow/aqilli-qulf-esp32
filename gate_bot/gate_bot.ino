/*
  ESP32 + RC522 + L298N + Telegram bot
  ------------------------------------
  Ikki yadroli ishlash:
    Yadro 0 -> RFID + motor   (WiFi dan mustaqil, hech qachon to'xtamaydi)
    Yadro 1 -> WiFi + Telegram + avtomatik yopilish

  Shu sababli karta HAR DOIM ishlaydi:
    - WiFi sozlanmagan bo'lsa ham
    - Sozlash portali ochiq turganda ham
    - Telegram so'rovi ketayotganda ham

  Kutubxonalar: UniversalTelegramBot 1.3.0, ArduinoJson 6.21.5 (7.x EMAS), WiFiManager 2.0.17

  Ulash
  -----
  RC522:                 L298N:
    SDA/SS -> GPIO 5       IN1 -> GPIO 27
    SCK    -> GPIO 18      IN2 -> GPIO 26
    MOSI   -> GPIO 23      ENA -> GPIO 14  (jumper OLINGAN bo'lsin!)
    MISO   -> GPIO 19      OUT1/OUT2 -> motor
    RST    -> GPIO 22      GND -> ESP32 GND bilan UMUMIY
    VCC    -> 3.3V
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
#define SS_PIN   5
#define RST_PIN  22
#define SPI_SCK  18
#define SPI_MISO 19
#define SPI_MOSI 23

const int IN1 = 27;
const int IN2 = 26;
const int ENA = 14;

// ================= Vaqtlar =================
const int MOTOR_SPEED = 255;                // L298N ~2V yeydi, to'liq beramiz

const unsigned long OPEN_MS       = 700;    // ochish uchun aylanish
const unsigned long CLOSE_MS      = 700;    // yopish uchun aylanish
const unsigned long AUTO_CLOSE_MS = 10000;  // ochiq turish vaqti

const int REMOVAL_CHECKS = 5;

// ================= Ruxsat berilgan kartalar =================
const byte ALLOWED[][4] = {
  { 0x80, 0x96, 0x03, 0x9E },
};
const int ALLOWED_COUNT = sizeof(ALLOWED) / sizeof(ALLOWED[0]);

// ================= PWM =================
const int PWM_CH = 0, PWM_FREQ = 1000, PWM_RES = 8;

// ================= Global =================
MFRC522 rfid(SS_PIN, RST_PIN);
WiFiClientSecure secured;
UniversalTelegramBot bot(BOT_TOKEN, secured);
WiFiManager wm;

SemaphoreHandle_t gateMutex;      // motorni bir vaqtda faqat bitta yadro boshqarsin

volatile bool gateOpen = false;
volatile unsigned long openedAt = 0;

String  notifyChat  = "";         // karta hodisalari shu chatga yuboriladi
String  pendingMsg  = "";         // yadro 0 yozadi, yadro 1 yuboradi
volatile bool pendingNotify = false;

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
  pendingMsg    = msg;
  pendingNotify = true;
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
bool cardPresent() {
  byte atqa[2];
  byte len = sizeof(atqa);
  MFRC522::StatusCode s = rfid.PICC_WakeupA(atqa, &len);
  if (s == MFRC522::STATUS_OK || s == MFRC522::STATUS_COLLISION) {
    rfid.PICC_HaltA();
    return true;
  }
  return false;
}

void checkCard() {
  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial())   return;

  String uid = uidToString(rfid.uid);
  bool ok = isAllowed(rfid.uid);
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  Serial.print("Karta: ");
  Serial.print(uid);
  Serial.println(ok ? "  -> RUXSAT" : "  -> RAD ETILDI");

  if (ok) {
    if (doOpen("KARTA")) queueNotify("\xF0\x9F\x93\xB3 Karta bilan ochildi: " + uid);
  } else {
    queueNotify("\xE2\x9B\x94 Notanish karta: " + uid);
  }

  // Karta olinmaguncha kutamiz - aks holda sikl takrorlanadi
  int absent = 0;
  while (absent < REMOVAL_CHECKS) {
    absent = cardPresent() ? 0 : absent + 1;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ================= YADRO 0: faqat karta va motor =================
void rfidTask(void *arg) {
  Serial.println("RFID vazifasi yadro 0 da ishga tushdi");
  for (;;) {
    checkCard();
    vTaskDelay(pdMS_TO_TICKS(50));
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

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(500);
  bootCount++;

  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pwmSetup();
  motorStop();

  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, SS_PIN);
  rfid.PCD_Init();
  delay(50);

  Serial.println("\n=== Eshik boshqaruvi (RFID + Telegram) ===");
  Serial.printf("Ishga tushishlar: %d\n", bootCount);
  Serial.printf("Reset sababi    : %s\n",
                esp_reset_reason() == ESP_RST_BROWNOUT
                ? "BROWNOUT (KUCHLANISH CHOKDI!)" : "normal");

  gateMutex = xSemaphoreCreateMutex();

  // Kartani BIRINCHI ishga tushiramiz - WiFi qanday bo'lishidan qat'i nazar ishlaydi
  xTaskCreatePinnedToCore(rfidTask, "rfid", 6144, NULL, 1, NULL, 0);

  // WiFi - bloklamaydigan rejimda. setup() shu yerda to'xtab qolmaydi.
  pinMode(BOOT_BTN, INPUT_PULLUP);
  wm.setConfigPortalBlocking(false);
  wm.setConfigPortalTimeout(0);          // bloklamagani uchun cheksiz turaversin
  wm.autoConnect(SETUP_AP_NAME, SETUP_AP_PASS);

  Serial.println("Karta hoziroq ishlaydi. Telegram - WiFi ulangach.");
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
    Serial.println("WiFi uzildi - karta ishlashda davom etadi.");
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
    // Yadro 0 dan kelgan xabarni yuboramiz
    if (pendingNotify && notifyChat.length()) {
      bot.sendMessage(notifyChat, pendingMsg, "");
      pendingNotify = false;
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
    pendingNotify = false;            // WiFi yo'q - xabarni to'plamaymiz
  }

  delay(10);
}
