// Milivojev bunar — merenje nivoa vode i slanje na ThingSpeak.
//
// Sonda 4-20 mA -> HW-685 (4 mA = 0 V, 20 mA = 3.0 V) -> ADS1115 AIN0 -> ESP32 (I2C: SDA=IO21, SCL=IO22).
// Nivo: 0 V = 0 m, 3.0 V = 10 m vode iznad sonde.
//
// ThingSpeak polja: field1 = nivo (m), field2 = napon (V), field3 = WiFi RSSI (dBm).

#include <WiFi.h>
#include <WiFiMulti.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include "ThingSpeak.h"
#include <esp_task_wdt.h>
#include "secrets.h"

// ThingSpeak
const unsigned long CHANNEL_ID = 3540749;

// Merenje
const float    V_FULL       = 3.0f;   // napon pri 20 mA
const float    LEVEL_FULL_M = 10.0f;  // nivo pri 20 mA
const int      ADS_CHANNEL  = 0;      // AIN0
const int      SAMPLES      = 16;     // broj očitavanja koja se usrednjavaju

// Interval slanja (30 s za testove; ThingSpeak besplatno dozvoljava najmanje 15 s)
const uint32_t SEND_INTERVAL_MS = 30UL * 1000UL;

// Watchdog i oporavak
#define WDT_TIMEOUT_S 60
const int MAX_FAIL_COUNT = 3;

Adafruit_ADS1115 ads;
WiFiMulti wifiMulti;
WiFiClient client;

int thingspeakFailCount = 0;
uint32_t lastSendMs = 0;
bool firstSend = true;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== Milivojev bunar ===");

  // Watchdog: Arduino core 3.x ga već inicijalizuje, pa ga samo prepodesimo.
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WDT_TIMEOUT_S * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  if (esp_task_wdt_reconfigure(&wdt_config) != ESP_OK) {
    esp_task_wdt_init(&wdt_config);
  }
  esp_task_wdt_add(NULL);

  Wire.begin(21, 22);
  Serial.println("Inicijalizacija ADS1115...");
  if (!ads.begin(0x48)) {
    Serial.println("GREŠKA: ADS1115 nije pronađen na 0x48! Restart za 10 s...");
    delay(10000);
    ESP.restart();
  }
  ads.setGain(GAIN_ONE);  // ±4.096 V, 0.125 mV po koraku
  Serial.println("ADS1115 OK");

  WiFi.mode(WIFI_STA);
  wifiMulti.addAP(WIFI_SSID, WIFI_PASSWORD);
  if (strlen(WIFI_SSID2) > 0) {
    wifiMulti.addAP(WIFI_SSID2, WIFI_PASSWORD2);
  }
  connectWiFi();

  ThingSpeak.begin(client);
  Serial.println("=== Sistem spreman ===\n");
}

void loop() {
  esp_task_wdt_reset();

  if (!firstSend && millis() - lastSendMs < SEND_INTERVAL_MS) {
    delay(200);
    return;
  }
  firstSend = false;
  lastSendMs = millis();

  if (!connectWiFi()) {
    Serial.println("WiFi nedostupan. Restartujem ESP32...");
    delay(1000);
    ESP.restart();
  }

  float voltage = readVoltage();
  float level = voltage / V_FULL * LEVEL_FULL_M;
  level = constrain(level, 0.0f, LEVEL_FULL_M);
  int rssi = WiFi.RSSI();

  Serial.printf("Nivo: %.2f m (%.4f V), RSSI: %d dBm\n", level, voltage, rssi);

  if (sendToThingSpeak(level, voltage, rssi)) {
    thingspeakFailCount = 0;
  } else if (++thingspeakFailCount >= MAX_FAIL_COUNT) {
    Serial.println("Previše neuspelih slanja. Restartujem ESP32...");
    delay(1000);
    ESP.restart();
  }
}

// Prosek SAMPLES očitavanja sa AIN0, u voltima.
float readVoltage() {
  int32_t sum = 0;
  for (int i = 0; i < SAMPLES; i++) {
    sum += ads.readADC_SingleEnded(ADS_CHANNEL);
  }
  return ads.computeVolts((int16_t)(sum / SAMPLES));
}

// Vraća true ako je WiFi povezan (pokušava do ~20 s).
bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.print("Povezivanje na WiFi");
  for (int i = 0; i < 4; i++) {
    esp_task_wdt_reset();
    if (wifiMulti.run(5000) == WL_CONNECTED) {
      Serial.printf("\nWiFi povezan: %s, IP %s, RSSI %d dBm\n",
                    WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
      return true;
    }
    Serial.print(".");
  }
  Serial.println("\nNeuspelo povezivanje na WiFi!");
  return false;
}

bool sendToThingSpeak(float level, float voltage, int rssi) {
  for (int attempt = 1; attempt <= 2; attempt++) {
    ThingSpeak.setField(1, level);
    ThingSpeak.setField(2, voltage);
    ThingSpeak.setField(3, rssi);
    int response = ThingSpeak.writeFields(CHANNEL_ID, TS_WRITE_KEY);
    if (response == 200) {
      Serial.println("✓ Poslato na ThingSpeak");
      return true;
    }
    Serial.printf("✗ Greška pri slanju, HTTP kod: %d\n", response);
    esp_task_wdt_reset();
    delay(5000);
  }
  return false;
}
