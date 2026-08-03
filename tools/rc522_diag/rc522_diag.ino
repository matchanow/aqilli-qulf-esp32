/*
  RC522 chuqur diagnostika — VersionReg ni turli SPI tezliklarida ko'p marta o'qiydi.

  Maqsad: 0x82 qiymati modulning haqiqiy javobimi, yoki signal buzilishimi?

  - Qiymat hamma tezlikda bir xil 0x82 bo'lsa  -> chip rostdan shunday deyapti (klon)
  - Tezlik pasayganda 0x92 ga aylansa          -> simlar/quvvat muammosi
  - Qiymatlar sakrab tursa                     -> aloqa beqaror, ulanish yomon
*/

#include <SPI.h>

#define SS_PIN   5
#define RST_PIN  22
#define SPI_SCK  18
#define SPI_MISO 19
#define SPI_MOSI 23

const uint8_t VersionReg = 0x37 << 1;   // 0x6E

byte readReg(uint8_t reg, uint32_t clk) {
  SPI.beginTransaction(SPISettings(clk, MSBFIRST, SPI_MODE0));
  digitalWrite(SS_PIN, LOW);
  SPI.transfer(0x80 | reg);          // bit7=1 -> o'qish
  byte v = SPI.transfer(0x00);
  digitalWrite(SS_PIN, HIGH);
  SPI.endTransaction();
  return v;
}

void testAt(uint32_t clk) {
  byte vals[8];
  int  cnt[8];
  int  n = 0;

  for (int i = 0; i < 40; i++) {
    byte v = readReg(VersionReg, clk);
    int j;
    for (j = 0; j < n; j++) {
      if (vals[j] == v) { cnt[j]++; break; }
    }
    if (j == n && n < 8) { vals[n] = v; cnt[n] = 1; n++; }
    delay(2);
  }

  Serial.print("  ");
  Serial.print(clk / 1000);
  Serial.print(" kHz -> ");
  for (int j = 0; j < n; j++) {
    Serial.print("0x");
    if (vals[j] < 0x10) Serial.print("0");
    Serial.print(vals[j], HEX);
    Serial.print(" x");
    Serial.print(cnt[j]);
    Serial.print("   ");
  }
  if (n == 1) Serial.print("[BARQAROR]"); else Serial.print("[SAKRAYAPTI]");
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== RC522 chuqur diagnostika ===");

  pinMode(SS_PIN, OUTPUT);
  digitalWrite(SS_PIN, HIGH);

  // Apparat reset
  pinMode(RST_PIN, OUTPUT);
  digitalWrite(RST_PIN, LOW);
  delay(20);
  digitalWrite(RST_PIN, HIGH);
  delay(100);

  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, SS_PIN);

  Serial.println("VersionReg ni har bir tezlikda 40 martadan o'qiymiz:\n");
  testAt(4000000);
  testAt(2000000);
  testAt(1000000);
  testAt(500000);
  testAt(200000);
  testAt(100000);

  Serial.println("\nKutilgan qiymatlar: 0x91=v1.0  0x92=v2.0  0x88=FM17522(klon)");
  Serial.println("0x00 yoki 0xFF = aloqa umuman yo'q");
}

void loop() {
  delay(1000);
}
