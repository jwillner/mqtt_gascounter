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
static const char* WIFI_SSID = "YOUR_WIFI_SSID";
static const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// -------- MQTT ----------
static const char* MQTT_HOST = "YOUR_MQTT_BROKER_IP";
static const uint16_t MQTT_PORT = 1883;

static const char* MQTT_CLIENT_ID   = "gas_counter_esp32c6";
static const char* MQTT_TOPIC_STATE = "gas_counter/state";
static const char* MQTT_TOPIC_AVAIL = "gas_counter/availability";

// -------- Hardware ----------
static const uint8_t PIN_SENSOR   = 3;        // Sensor input (analog)
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

// -------- Analog Sensor Threshold (0..4095) ----------
// Hysterese: unter LOW = ausgelöst, über HIGH = zurückgesetzt
static const uint16_t ADC_THRESHOLD_LOW  = 1000;  // Puls erkannt
static const uint16_t ADC_THRESHOLD_HIGH = 2000;  // Sensor zurückgesetzt

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

// Pulse counters
uint32_t pulses_total   = 0;
uint32_t pulses_hour    = 0;
uint32_t pending_pulses = 0;

bool red_pulse_pending = false;

// Analog sensor state
uint16_t adcValue       = 0;
bool sensorTriggered    = false;

// LED pulse timing
bool redActive    = false;
uint32_t redStartMs = 0;

// hour window
uint32_t hourStartMs = 0;

// persistence offset (kWh)
float total_kwh_offset = 0.0f;

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
// ===================== ANALOG SENSOR ========================
// ============================================================
//
// Hysterese-Schwellwert: verhindert Mehrfachzählung pro Puls.
// Sensor gilt als ausgelöst wenn ADC < ADC_THRESHOLD_LOW,
// zurückgesetzt wenn ADC > ADC_THRESHOLD_HIGH.
//

void handleSensorAnalog() {
  adcValue = analogRead(PIN_SENSOR);

  if (!sensorTriggered && adcValue < ADC_THRESHOLD_LOW) {
    sensorTriggered = true;
    pulses_total++;
    pulses_hour++;
    pending_pulses++;
    red_pulse_pending = true;
    Serial.printf("Puls! ADC=%u  total=%u\r\n", adcValue, pulses_total);
  } else if (sensorTriggered && adcValue > ADC_THRESHOLD_HIGH) {
    sensorTriggered = false;
  }
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

  float total_kwh = total_kwh_offset + pulsesToKwh(pulses_total);
  float hour_kwh  = pulsesToKwh(pulses_hour);

  char payload[256];
  snprintf(payload, sizeof(payload),
           "{\"total_kwh\":%.3f,\"hour_kwh\":%.3f,"
           "\"pulses_total\":%u,\"pulses_hour\":%u,"
           "\"adc\":%u,\"rssi\":%d}",
           total_kwh, hour_kwh, pulses_total, pulses_hour,
           adcValue, WiFi.RSSI());

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
// ===================== LED ==================================
// ============================================================
//

void updateStatusLed() {
  if (red_pulse_pending) {
    red_pulse_pending = false;
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

  float total_kwh = total_kwh_offset + pulsesToKwh(pulses_total);
  float hour_kwh  = pulsesToKwh(pulses_hour);

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

  // --- Row 5: ADC-Wert + Schwellwert-Status ---
  display.setCursor(0, 56);
  display.printf("ADC:%4u P:%u", adcValue, pulses_total);

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
    pulses_total     = 0;
    pulses_hour      = 0;
    pending_pulses   = 0;
    red_pulse_pending = false;
    sensorTriggered  = false;

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

  // Sensor als analoger Eingang (kein pullup, kein Interrupt)
  pinMode(PIN_SENSOR, INPUT);
  analogSetAttenuation(ADC_11db);  // 0–3.3V Messbereich

  pinMode(PIN_BUTTON, INPUT_PULLUP);

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

  Serial.printf("ADC Schwellwert LOW=%u HIGH=%u\r\n",
                ADC_THRESHOLD_LOW, ADC_THRESHOLD_HIGH);

  publishState(true);
}

//
// ============================================================
// ===================== LOOP =================================
// ============================================================
//

void loop() {
  handleSensorAnalog();
  updateStatusLed();
  updateDisplay();
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
    pending_pulses = 0;
    publishState(true);
  }

  publishState(false);

  uint32_t now = millis();
  if ((uint32_t)(now - hourStartMs) >= HOUR_INTERVAL_MS) {
    hourStartMs += HOUR_INTERVAL_MS;
    pulses_hour = 0;

    Serial.print("Hour rollover -> reset hour pulses\r\n");
    publishState(true);
  }

  delay(MAIN_LOOP_DELAY_MS);
}
