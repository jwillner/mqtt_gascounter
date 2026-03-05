#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

//
// ============================================================
// ===================== CONFIGURATION ========================
// ============================================================
//

// -------- WLAN ----------
static const char* WIFI_SSID = "RouterJoachimWillner";
static const char* WIFI_PASS = "JoWiBu-456";

// -------- MQTT ----------
static const char* MQTT_HOST = "192.168.1.16";
static const uint16_t MQTT_PORT = 1883;

static const char* MQTT_CLIENT_ID   = "gas_counter_esp32c6";
static const char* MQTT_TOPIC_STATE = "gas_counter/state";
static const char* MQTT_TOPIC_GPIO2 = "gas_counter/gpio2";
static const char* MQTT_TOPIC_AVAIL = "gas_counter/availability";

// -------- Hardware ----------
static const uint8_t PIN_SENSOR   = 3;        // Sensor input (pulse)
static const uint8_t PIN_NEOPIXEL = 8;        // WS2812 data pin
static const uint8_t PIN_BUTTON   = BOOT_PIN; // BOOT button
static const uint8_t NUM_PIXELS   = 1;

// -------- OLED ----------
static const uint8_t OLED_SDA     = 0;
static const uint8_t OLED_SCL     = 1;
static const uint8_t SCREEN_WIDTH  = 128;
static const uint8_t SCREEN_HEIGHT = 64;
static const uint8_t OLED_ADDRESS  = 0x3C;

static const uint32_t DISPLAY_INTERVAL_MS = 2000UL;

// -------- Gas Parameters ----------
static const float PULSE_VOLUME_M3 = 0.01f;  // 1 pulse = 0.01 m³
static const float GAS_KWH_PER_M3  = 10.5f;  // adjust for your gas conversion!
static const uint32_t DEBOUNCE_US  = 20000UL; // 20ms debounce in ISR

// -------- Timing ----------
static const uint32_t WIFI_TIMEOUT_MS     = 20000UL;
static const uint32_t WIFI_RETRY_DELAY_MS = 300UL;

static const uint32_t MQTT_RETRY_DELAY_MS = 2000UL;

static const uint32_t PUBLISH_INTERVAL_MS = 10000UL;
static const uint32_t PUBLISH_MIN_GAP_MS  = 1000UL;
static const uint32_t PERSIST_INTERVAL_MS = 60000UL;

static const uint32_t HOUR_INTERVAL_MS    = 3600000UL;
static const uint32_t MAIN_LOOP_DELAY_MS  = 10UL;

static const uint32_t RED_PULSE_DURATION_MS = 1000UL;

// -------- LED Brightness ----------
static const uint8_t LED_BRIGHTNESS = 50;

// -------- LED Colors (RGB) ----------
static const uint8_t LED_GREEN_R = 0;
static const uint8_t LED_GREEN_G = 150;
static const uint8_t LED_GREEN_B = 0;

static const uint8_t LED_RED_R   = 150;
static const uint8_t LED_RED_G   = 0;
static const uint8_t LED_RED_B   = 0;

static const uint8_t LED_OFF_R   = 0;
static const uint8_t LED_OFF_G   = 0;
static const uint8_t LED_OFF_B   = 0;

// -------- Storage ----------
static const char* NVS_NAMESPACE     = "gas";
static const char* NVS_KEY_TOTAL_MWH = "total_mWh";

//
// ============================================================
// ===================== GLOBALS ==============================
// ============================================================
//

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
Preferences prefs;

Adafruit_NeoPixel strip(NUM_PIXELS, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

bool oledOk = false;

// Pulse counters (ISR)
volatile uint32_t pulses_total   = 0;
volatile uint32_t pulses_hour    = 0;
volatile uint32_t pending_pulses = 0;
volatile uint32_t last_isr_us    = 0;

volatile bool red_pulse_pending = false;

// LED pulse timing
bool redActive    = false;
uint32_t redStartMs = 0;

// hour window
uint32_t hourStartMs = 0;

// persistence offset (kWh)
float total_kwh_offset = 0.0f;

// GPIO2 state tracking
int last_gpio_state = -1;

// connectivity state
bool mqttOnline = false;

//
// ============================================================
// ===================== UTIL =================================
// ============================================================
//

static inline float pulsesToKwh(uint32_t p) {
  return (p * PULSE_VOLUME_M3) * GAS_KWH_PER_M3;
}

static inline uint32_t kWhToMWh(float kwh) {
  if (kwh <= 0.0f) return 0U;
  double v = (double)kwh * 1000000.0;
  if (v > 4294967295.0) v = 4294967295.0;
  return (uint32_t)(v + 0.5);
}

static inline float mWhToKWh(uint32_t mwh) {
  return (float)mwh / 1000000.0f;
}

static inline void setLed(uint8_t r, uint8_t g, uint8_t b) {
  strip.setPixelColor(0, strip.Color(r, g, b));
  strip.show();
}

//
// ============================================================
// ===================== ISR ==================================
// ============================================================
//

void IRAM_ATTR isrPulse() {
  uint32_t now = (uint32_t)micros();

  if ((uint32_t)(now - last_isr_us) < DEBOUNCE_US) {
    return;
  }

  last_isr_us = now;

  pulses_total++;
  pulses_hour++;
  pending_pulses++;

  red_pulse_pending = true;
}

//
// ============================================================
// ===================== WIFI =================================
// ============================================================
//

void wifiConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.printf("WiFi connecting to %s\r\n", WIFI_SSID);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(WIFI_RETRY_DELAY_MS);

    if ((uint32_t)(millis() - start) > WIFI_TIMEOUT_MS) {
      Serial.print("WiFi timeout -> reboot\r\n");
      delay(500);
      ESP.restart();
    }
  }

  Serial.printf("WiFi connected, IP=%s\r\n", WiFi.localIP().toString().c_str());
}

//
// ============================================================
// ===================== MQTT =================================
// ============================================================
//

void mqttConnect() {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);

  while (!mqtt.connected()) {
    mqttOnline = false;

    Serial.printf("MQTT connecting to %s:%u\r\n", MQTT_HOST, MQTT_PORT);

    if (mqtt.connect(MQTT_CLIENT_ID, MQTT_TOPIC_AVAIL, 1, true, "offline")) {
      Serial.print("MQTT connected\r\n");
      mqtt.publish(MQTT_TOPIC_AVAIL, "online", true);
      mqttOnline = true;

      int s = digitalRead(PIN_SENSOR);
      mqtt.publish(MQTT_TOPIC_GPIO2, (s == LOW) ? "LOW" : "HIGH", true);
    } else {
      Serial.printf("MQTT connect failed rc=%d -> retry\r\n", mqtt.state());
      delay(MQTT_RETRY_DELAY_MS);
    }
  }
}

//
// ============================================================
// ===================== PUBLISH ==============================
// ============================================================
//

void publishState(bool force) {
  static uint32_t lastPublishMs = 0;
  static uint32_t lastForceMs   = 0;

  uint32_t now = millis();

  if (!force) {
    if ((uint32_t)(now - lastPublishMs) < PUBLISH_INTERVAL_MS) return;
  } else {
    if ((uint32_t)(now - lastForceMs) < PUBLISH_MIN_GAP_MS) return;
    lastForceMs = now;
  }

  lastPublishMs = now;

  uint32_t t, h;
  noInterrupts();
  t = pulses_total;
  h = pulses_hour;
  interrupts();

  float total_kwh = total_kwh_offset + pulsesToKwh(t);
  float hour_kwh  = pulsesToKwh(h);

  char payload[256];
  snprintf(payload, sizeof(payload),
           "{\"total_kwh\":%.3f,\"hour_kwh\":%.3f,"
           "\"pulses_total\":%u,\"pulses_hour\":%u,"
           "\"rssi\":%d}",
           total_kwh, hour_kwh, t, h, WiFi.RSSI());

  if (mqtt.connected()) {
    bool ok = mqtt.publish(MQTT_TOPIC_STATE, payload, true);
    Serial.printf("MQTT publish -> %s\r\n", MQTT_TOPIC_STATE);
    Serial.printf("Payload: %s\r\n", payload);
    Serial.printf("Result: %s\r\n\r\n", ok ? "OK" : "FAILED");
  }

  static uint32_t lastPersistMs = 0;
  if ((uint32_t)(now - lastPersistMs) >= PERSIST_INTERVAL_MS) {
    lastPersistMs = now;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putUInt(NVS_KEY_TOTAL_MWH, kWhToMWh(total_kwh));
    prefs.end();
  }
}

//
// ============================================================
// ===================== GPIO2 State ==========================
// ============================================================
//

void handleGpio2Change() {
  int s = digitalRead(PIN_SENSOR);
  if (s != last_gpio_state) {
    last_gpio_state = s;

    Serial.printf("GPIO2 state=%s\r\n", (s == LOW) ? "LOW" : "HIGH");

    if (mqtt.connected()) {
      bool ok = mqtt.publish(MQTT_TOPIC_GPIO2, (s == LOW) ? "LOW" : "HIGH", true);
      Serial.printf("MQTT publish -> %s\r\n", MQTT_TOPIC_GPIO2);
      Serial.printf("Payload: %s\r\n", (s == LOW) ? "LOW" : "HIGH");
      Serial.printf("Result: %s\r\n\r\n", ok ? "OK" : "FAILED");
    }
  }
}

//
// ============================================================
// ===================== LED ==================================
// ============================================================
//

void updateStatusLed() {
  if (red_pulse_pending) {
    noInterrupts();
    red_pulse_pending = false;
    interrupts();

    redActive   = true;
    redStartMs  = millis();
  }

  if (redActive) {
    if ((uint32_t)(millis() - redStartMs) < RED_PULSE_DURATION_MS) {
      setLed(LED_RED_R, LED_RED_G, LED_RED_B);
      return;
    } else {
      redActive = false;
    }
  }

  if (mqttOnline) {
    setLed(LED_GREEN_R, LED_GREEN_G, LED_GREEN_B);
    return;
  }

  setLed(LED_OFF_R, LED_OFF_G, LED_OFF_B);
}

//
// ============================================================
// ===================== OLED =================================
// ============================================================
//

void updateDisplay() {
  if (!oledOk) return;

  static uint32_t lastDisplayMs = 0;
  uint32_t now = millis();
  if ((uint32_t)(now - lastDisplayMs) < DISPLAY_INTERVAL_MS) return;
  lastDisplayMs = now;

  uint32_t t, h;
  noInterrupts();
  t = pulses_total;
  h = pulses_hour;
  interrupts();

  float total_kwh = total_kwh_offset + pulsesToKwh(t);
  float hour_kwh  = pulsesToKwh(h);

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // --- Row 1: Title + status ---
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("GasCounter");
  display.setCursor(72, 0);
  if (mqttOnline) {
    display.printf("RSSI:%ddBm", WiFi.RSSI());
  } else {
    display.print("OFFLINE");
  }

  // --- Divider ---
  display.drawFastHLine(0, 10, SCREEN_WIDTH, SSD1306_WHITE);

  // --- Row 2: Total kWh (large) ---
  display.setTextSize(2);
  display.setCursor(0, 14);
  display.printf("%.2f kWh", total_kwh);

  // --- Row 3: Label ---
  display.setTextSize(1);
  display.setCursor(0, 32);
  display.print("Gesamt");

  // --- Divider ---
  display.drawFastHLine(0, 42, SCREEN_WIDTH, SSD1306_WHITE);

  // --- Row 4: Hour stats ---
  display.setCursor(0, 46);
  display.printf("Std: %.3f kWh", hour_kwh);

  // --- Row 5: Pulse count ---
  display.setCursor(0, 56);
  display.printf("Pulse: %u", t);

  display.display();
}

//
// ============================================================
// ===================== BOOT Button ==========================
// ============================================================
//

void handleBootButton() {
  static bool lastBtn = true;
  bool btn = digitalRead(PIN_BUTTON);

  if (lastBtn && !btn) {
    noInterrupts();
    pulses_total     = 0;
    pulses_hour      = 0;
    pending_pulses   = 0;
    red_pulse_pending = false;
    interrupts();

    total_kwh_offset = 0.0f;
    hourStartMs      = millis();

    prefs.begin(NVS_NAMESPACE, false);
    prefs.putUInt(NVS_KEY_TOTAL_MWH, 0);
    prefs.end();

    Serial.print("BOOT: reset total+hour\r\n");
    publishState(true);
  }

  lastBtn = btn;
}

//
// ============================================================
// ===================== SETUP ================================
// ============================================================
//

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.print("\r\n=== GasCounter WiFi MQTT ===\r\n");

  pinMode(PIN_SENSOR, INPUT_PULLUP);
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(PIN_SENSOR), isrPulse, FALLING);

  strip.begin();
  strip.setBrightness(LED_BRIGHTNESS);
  setLed(LED_OFF_R, LED_OFF_G, LED_OFF_B);

  // OLED init
  Wire.begin(OLED_SDA, OLED_SCL);
  if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    oledOk = true;
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.print("GasCounter");
    display.setCursor(0, 16);
    display.print("Connecting...");
    display.display();
    Serial.print("OLED init OK\r\n");
  } else {
    Serial.print("OLED init FAILED\r\n");
  }

  // Load persisted total offset (kWh)
  prefs.begin(NVS_NAMESPACE, false);
  total_kwh_offset = mWhToKWh(prefs.getUInt(NVS_KEY_TOTAL_MWH, 0));
  prefs.end();

  hourStartMs = millis();

  wifiConnect();
  mqttConnect();

  last_gpio_state = digitalRead(PIN_SENSOR);
  Serial.printf("GPIO2 state=%s\r\n", (last_gpio_state == LOW) ? "LOW" : "HIGH");

  publishState(true);
}

//
// ============================================================
// ===================== LOOP =================================
// ============================================================
//

void loop() {
  updateStatusLed();
  updateDisplay();
  handleGpio2Change();
  handleBootButton();

  if (WiFi.status() != WL_CONNECTED) {
    mqttOnline = false;
    wifiConnect();
  }

  if (!mqtt.connected()) {
    mqttOnline = false;
    mqttConnect();
  }
  mqtt.loop();

  if (pending_pulses) {
    noInterrupts();
    pending_pulses = 0;
    interrupts();
    publishState(true);
  }

  publishState(false);

  uint32_t now = millis();
  if ((uint32_t)(now - hourStartMs) >= HOUR_INTERVAL_MS) {
    hourStartMs += HOUR_INTERVAL_MS;
    noInterrupts();
    pulses_hour = 0;
    interrupts();

    Serial.print("Hour rollover -> reset hour pulses\r\n");
    publishState(true);
  }

  delay(MAIN_LOOP_DELAY_MS);
}
