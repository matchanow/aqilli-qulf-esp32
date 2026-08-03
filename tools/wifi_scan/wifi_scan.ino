/*
  ESP32 atrofdagi WiFi tarmoqlarni ko'rsatadi.
  ESP32 faqat 2.4 GHz ni ko'radi - 5 GHz tarmoqlar bu ro'yxatda chiqmaydi.
*/

#include <WiFi.h>

void setup() {
  Serial.begin(115200);
  delay(1000);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  Serial.println("\n=== WiFi tarmoqlarni qidirish ===\n");

  int n = WiFi.scanNetworks();

  if (n == 0) {
    Serial.println("Hech qanday tarmoq topilmadi.");
  } else {
    Serial.print("Topildi: ");
    Serial.print(n);
    Serial.println(" ta\n");
    Serial.println("  Signal  Himoya   Nomi");
    Serial.println("  ------  -------  ----------------------");

    for (int i = 0; i < n; i++) {
      Serial.print("  ");
      Serial.print(WiFi.RSSI(i));
      Serial.print(" dBm  ");
      Serial.print(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "OCHIQ  " : "parolli");
      Serial.print("  ");
      Serial.println(WiFi.SSID(i));
      delay(10);
    }
  }

  Serial.println("\nSignal: -30..-60 juda yaxshi, -70 o'rtacha, -80 dan past zaif.");
}

void loop() {
  delay(1000);
}
