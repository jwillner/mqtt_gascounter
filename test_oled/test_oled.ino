#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define OLED_SDA     0
#define OLED_SCL     1
#define OLED_ADDRESS 0x3C
#define SCREEN_W     128
#define SCREEN_H     64

Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== OLED Test ===");

  Wire.begin(OLED_SDA, OLED_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("FEHLER: SSD1306 nicht gefunden!");
    Serial.println("Pruefe Verkabelung und I2C-Adresse (0x3C oder 0x3D)");
    while (true) delay(1000);
  }

  Serial.println("OLED OK");

  // --- Seite 1: Grundtext ---
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(2);
  display.setCursor(0, 0);
  display.println("OLED OK!");

  display.setTextSize(1);
  display.setCursor(0, 20);
  display.println("128 x 64 Pixel");
  display.setCursor(0, 30);
  display.println("SSD1306  I2C");
  display.setCursor(0, 40);
  display.printf("SDA: GPIO%d", OLED_SDA);
  display.setCursor(0, 50);
  display.printf("SCL: GPIO%d", OLED_SCL);

  display.display();
  delay(3000);

  // --- Seite 2: Zeichengroessen ---
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Groesse 1: Abc123");
  display.setTextSize(2);
  display.setCursor(0, 16);
  display.println("Gr. 2");
  display.setTextSize(3);
  display.setCursor(0, 38);
  display.println("Gr3");
  display.display();
  delay(3000);

  // --- Seite 3: Linien & Rahmen ---
  display.clearDisplay();
  display.drawRect(0, 0, SCREEN_W, SCREEN_H, SSD1306_WHITE);
  display.drawFastHLine(0, 20, SCREEN_W, SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(4, 4);
  display.println("Linien & Rahmen");
  display.setCursor(4, 26);
  display.println("Zeile 2");
  display.setCursor(4, 36);
  display.println("Zeile 3");
  display.setCursor(4, 46);
  display.println("Zeile 4");
  display.setCursor(4, 56);
  display.println("Zeile 5 (unten)");
  display.display();
  delay(3000);

  // --- Seite 4: Simuliertes GasCounter-Layout ---
  display.clearDisplay();

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("GasCounter");
  display.setCursor(72, 0);
  display.print("RSSI:-67dB");

  display.drawFastHLine(0, 10, SCREEN_W, SSD1306_WHITE);

  display.setTextSize(2);
  display.setCursor(0, 14);
  display.print("12.34 kWh");

  display.setTextSize(1);
  display.setCursor(0, 32);
  display.print("Gesamt");

  display.drawFastHLine(0, 42, SCREEN_W, SSD1306_WHITE);

  display.setCursor(0, 46);
  display.print("Std: 0.105 kWh");
  display.setCursor(0, 56);
  display.print("Pulse: 1234");

  display.display();
  delay(3000);
}

void loop() {
  // Laufender Zaehler zum Testen der Refresh-Rate
  static uint32_t count = 0;
  static uint32_t lastMs = 0;

  if ((uint32_t)(millis() - lastMs) >= 1000) {
    lastMs = millis();
    count++;

    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("Loop-Test");
    display.drawFastHLine(0, 10, SCREEN_W, SSD1306_WHITE);
    display.setTextSize(2);
    display.setCursor(0, 18);
    display.printf("%u s", count);
    display.setTextSize(1);
    display.setCursor(0, 50);
    display.println("OLED laeuft stabil.");
    display.display();

    Serial.printf("Loop: %u s\n", count);
  }
}
