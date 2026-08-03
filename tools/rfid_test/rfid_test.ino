/*
  ESP32 + MFRC522 (RC522) — ulanishni tekshirish / diagnostika

  Ulash (ESP32 DevKit, VSPI):
    RC522 SDA(SS) -> GPIO 5
    RC522 SCK     -> GPIO 18
    RC522 MOSI    -> GPIO 23
    RC522 MISO    -> GPIO 19
    RC522 RST     -> GPIO 22
    RC522 IRQ     -> ulanmaydi
    RC522 GND     -> GND
    RC522 VCC     -> 3.3V   <-- 5V EMAS! 5V modulni kuydiradi.
*/

#include <SPI.h>
#include <MFRC522.h>

#define SS_PIN   5
#define RST_PIN  22

#define SPI_SCK  18
#define SPI_MISO 19
#define SPI_MOSI 23

MFRC522 rfid(SS_PIN, RST_PIN);

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== RC522 diagnostika ===");

  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, SS_PIN);
  rfid.PCD_Init();
  delay(50);

  // VersionReg — modul javob berayotganini bilishning eng ishonchli usuli
  byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.print("VersionReg = 0x");
  Serial.println(v, HEX);

  if (v == 0x00 || v == 0xFF) {
    Serial.println("\n>>> ALOQA YO'Q. Modul umuman javob bermayapti.");
    Serial.println("    1. Modul pinlari LEHIMLANGANmi? (ko'p modul lehimsiz keladi)");
    Serial.println("    2. VCC 3.3V ga ulanganmi? (5V modulni kuydiradi)");
    Serial.println("    3. GND umumiymi?");
    Serial.println("    4. Simlarni almashtirib ko'ring - dupont simlar tez-tez uziladi");
    Serial.println("    5. MISO/MOSI o'rni almashib qolmaganmi?");
  } else if (v == 0x91 || v == 0x92) {
    Serial.println(">>> OK — haqiqiy MFRC522 topildi.");
  } else {
    Serial.println(">>> Javob bor, lekin versiya notanish (klon modul bo'lishi mumkin).");
  }

  rfid.PCD_DumpVersionToSerial();
  Serial.println("\nKartani modulga yaqinlashtiring...");
}

void loop() {
  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial())   return;

  Serial.print("Karta UID:");
  for (byte i = 0; i < rfid.uid.size; i++) {
    Serial.print(rfid.uid.uidByte[i] < 0x10 ? " 0" : " ");
    Serial.print(rfid.uid.uidByte[i], HEX);
  }
  Serial.print("   turi: ");
  Serial.println(rfid.PICC_GetTypeName(rfid.PICC_GetType(rfid.uid.sak)));

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}
