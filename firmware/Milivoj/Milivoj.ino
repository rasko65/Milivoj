// Milivojev bunar — merenje nivoa vode i slanje na ThingSpeak.
//
// Sonda 4-20 mA -> HW-685 (4 mA = 0 V, 20 mA = 3.0 V) -> ADS1115 AIN0 -> ESP32 (I2C: SDA=IO21, SCL=IO22).
// Nivo: 0 V = 0 m, 3.0 V = 10 m vode iznad sonde.
//
// ThingSpeak polja: field1 = nivo (m), field2 = napon (V), field3 = WiFi RSSI (dBm), status = verzija i mreža.
//
// Rad bez kabla:
//  - WiFi portal: ako ne nađe poznatu mrežu, ESP32 otvara mrežu "Milivoj-Setup" na 5 minuta.
//    Telefonom se poveži, izaberi mrežu i upiši lozinku. Mreža se pamti.
//  - Udaljena podešavanja: na svakih 10 min čita config.json sa GitHub Pages (interval, kalibracija).
//  - OTA: ako config.json ima veći fw_version, preuzima firmware.bin i instalira ga.
//    Nova verzija se potvrđuje tek posle prvog uspešnog slanja na ThingSpeak, inače se vraća stara.
//
// Lozinke i ThingSpeak ključ se čuvaju u NVS (trajna memorija ploče). Upisuju se samo iz verzije
// prebačene kablom (sa secrets.h). OTA verzija se pravi bez secrets.h (tools/release.sh), jer je javna.

#include <WiFi.h>
#include <WiFiMulti.h>
#include <Wire.h>
#include <Preferences.h>
#include <Adafruit_ADS1X15.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <NetworkClientSecure.h>
#include <WiFiManager.h>
#include <esp_ota_ops.h>
#include "ThingSpeak.h"
#include <esp_task_wdt.h>

#if !defined(OTA_BUILD) && __has_include("secrets.h")
#include "secrets.h"
#define HAVE_SECRETS 1
#endif

// Verzija firmware-a. Povećaj pre svake OTA objave (tools/release.sh to proverava).
#define FW_VERSION 2

// ThingSpeak
const unsigned long CHANNEL_ID = 3540749;

// Udaljena podešavanja i OTA
const char*    CONFIG_URL      = "https://rasko65.github.io/Milivoj/config.json";
const uint32_t CONFIG_CHECK_MS = 10UL * 60UL * 1000UL;

// WiFi portal
const char*    PORTAL_SSID      = "Milivoj-Setup";
const uint32_t PORTAL_TIMEOUT_S = 300;

// Merenje
const int ADS_CHANNEL = 0;   // AIN0
const int SAMPLES     = 16;  // broj očitavanja koja se usrednjavaju

// Podrazumevane vrednosti (config.json ih može promeniti; pamte se u NVS)
float    vFull          = 3.0f;   // napon pri 20 mA
float    levelFullM     = 10.0f;  // nivo pri 20 mA
uint32_t sendIntervalMs = 30UL * 1000UL;

// Watchdog i oporavak
#define WDT_TIMEOUT_S 60
const int MAX_FAIL_COUNT = 3;
const int WIFI_SLOTS     = 4;  // 0-2 iz secrets.h, 3 = mreža iz portala

Adafruit_ADS1115 ads;
WiFiMulti wifiMulti;
WiFiClient client;
Preferences prefs;

String tsWriteKey;
String apPassword;
int thingspeakFailCount = 0;
uint32_t lastSendMs = 0;
uint32_t lastConfigMs = 0;
bool firstSend = true;
bool otaPendingVerify = false;

// Nova OTA verzija se ne potvrđuje automatski pri startu, nego posle prvog uspešnog slanja.
extern "C" bool verifyRollbackLater() {
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.printf("\n=== Milivojev bunar (fw %d) ===\n", FW_VERSION);

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

  prefs.begin("milivoj", false);
  checkOtaState();
  seedSecrets();
  loadSettings();

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
  for (int i = 0; i < WIFI_SLOTS; i++) {
    String ssid = prefs.getString(("ssid" + String(i)).c_str(), "");
    if (ssid.length()) {
      wifiMulti.addAP(ssid.c_str(), prefs.getString(("pass" + String(i)).c_str(), "").c_str());
      Serial.printf("Poznata mreža %d: %s\n", i, ssid.c_str());
    }
  }
  if (!connectWiFi() && !runPortal()) {
    Serial.println("Nema WiFi-ja ni posle portala. Restartujem...");
    delay(1000);
    ESP.restart();
  }

  ThingSpeak.begin(client);
  checkRemoteConfig();
  Serial.println("=== Sistem spreman ===\n");
}

void loop() {
  esp_task_wdt_reset();

  if (millis() - lastConfigMs >= CONFIG_CHECK_MS) {
    checkRemoteConfig();
  }

  if (!firstSend && millis() - lastSendMs < sendIntervalMs) {
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
  float level = voltage / vFull * levelFullM;
  level = constrain(level, 0.0f, levelFullM);
  int rssi = WiFi.RSSI();

  Serial.printf("Nivo: %.2f m (%.4f V), RSSI: %d dBm\n", level, voltage, rssi);

  if (sendToThingSpeak(level, voltage, rssi)) {
    thingspeakFailCount = 0;
    confirmOta();
  } else if (++thingspeakFailCount >= MAX_FAIL_COUNT) {
    Serial.println("Previše neuspelih slanja. Restartujem ESP32...");
    delay(1000);
    ESP.restart();
  }
}

// ---------- Podešavanja ----------

// Verzija prebačena kablom upisuje lozinke i ključ iz secrets.h u NVS.
void seedSecrets() {
#ifdef HAVE_SECRETS
  prefs.putString("ts_key", TS_WRITE_KEY);
  prefs.putString("ap_pass", AP_PASSWORD);
  prefs.putString("ssid0", WIFI_SSID);
  prefs.putString("pass0", WIFI_PASSWORD);
  prefs.putString("ssid1", WIFI_SSID2);
  prefs.putString("pass1", WIFI_PASSWORD2);
#ifdef WIFI_SSID3
  prefs.putString("ssid2", WIFI_SSID3);
  prefs.putString("pass2", WIFI_PASSWORD3);
#else
  prefs.putString("ssid2", "");
  prefs.putString("pass2", "");
#endif
  Serial.println("Podešavanja iz secrets.h upisana u NVS");
#endif
}

void loadSettings() {
  tsWriteKey = prefs.getString("ts_key", "");
  apPassword = prefs.getString("ap_pass", "");
  vFull = prefs.getFloat("v_full", vFull);
  levelFullM = prefs.getFloat("level_full", levelFullM);
  sendIntervalMs = prefs.getUInt("interval_s", sendIntervalMs / 1000) * 1000UL;
  if (!tsWriteKey.length()) {
    Serial.println("UPOZORENJE: nema ThingSpeak ključa u NVS — prebaci firmware kablom sa secrets.h");
  }
  Serial.printf("Podešavanja: interval %lu s, %.2f V = %.2f m\n", sendIntervalMs / 1000, vFull, levelFullM);
}

// Čita config.json; menja interval/kalibraciju i pokreće OTA ako postoji novija verzija.
void checkRemoteConfig() {
  lastConfigMs = millis();
  if (WiFi.status() != WL_CONNECTED) return;

  NetworkClientSecure secure;
  secure.useBuiltinCACertBundle();
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(secure, CONFIG_URL)) return;
  int code = http.GET();
  if (code != 200) {
    Serial.printf("config.json: HTTP %d\n", code);
    http.end();
    return;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getString());
  http.end();
  if (err) {
    Serial.printf("config.json: neispravan JSON (%s)\n", err.c_str());
    return;
  }

  uint32_t interval = doc["interval_s"] | (sendIntervalMs / 1000);
  float vf = doc["v_full"] | vFull;
  float lf = doc["level_full_m"] | levelFullM;
  if (interval >= 15 && interval <= 86400 && interval * 1000UL != sendIntervalMs) {
    sendIntervalMs = interval * 1000UL;
    prefs.putUInt("interval_s", interval);
    Serial.printf("Novi interval: %lu s\n", interval);
  }
  if (vf >= 0.5f && vf <= 4.0f && vf != vFull) {
    vFull = vf;
    prefs.putFloat("v_full", vf);
    Serial.printf("Novi napon pune skale: %.3f V\n", vf);
  }
  if (lf >= 0.1f && lf <= 100.0f && lf != levelFullM) {
    levelFullM = lf;
    prefs.putFloat("level_full", lf);
    Serial.printf("Novi nivo pune skale: %.2f m\n", lf);
  }

  int fwVersion = doc["fw_version"] | 0;
  const char* fwUrl = doc["fw_url"] | "";
  if (fwVersion > FW_VERSION && strlen(fwUrl) > 0) {
    if (fwVersion == prefs.getInt("ota_bad", 0)) {
      Serial.printf("fw %d je ranije vraćen (rollback) — preskačem\n", fwVersion);
    } else {
      runOta(fwVersion, fwUrl);
    }
  }
}

// ---------- OTA ----------

void runOta(int version, const char* url) {
  Serial.printf("OTA: preuzimam fw %d sa %s\n", version, url);
  prefs.putInt("ota_try", version);
  esp_task_wdt_delete(NULL);  // preuzimanje može trajati duže od watchdog-a

  NetworkClientSecure secure;
  secure.useBuiltinCACertBundle();
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.rebootOnUpdate(true);
  t_httpUpdate_return ret = httpUpdate.update(secure, url);

  // Do ovde se stiže samo ako OTA nije uspeo (uspeh = restart).
  esp_task_wdt_add(NULL);
  prefs.remove("ota_try");
  Serial.printf("OTA neuspešan: %s\n", httpUpdate.getLastErrorString().c_str());
}

// Ako smo pokušali OTA, a i dalje radi stara verzija, nova je vraćena — ne pokušavaj je ponovo.
void checkOtaState() {
  int tried = prefs.getInt("ota_try", 0);
  if (tried > FW_VERSION) {
    Serial.printf("fw %d nije potvrđen — vraćena je fw %d\n", tried, FW_VERSION);
    prefs.putInt("ota_bad", tried);
    prefs.remove("ota_try");
  }
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    otaPendingVerify = true;
    Serial.println("Nova verzija čeka potvrdu (prvo uspešno slanje)");
  }
}

void confirmOta() {
  if (!otaPendingVerify) return;
  esp_ota_mark_app_valid_cancel_rollback();
  prefs.remove("ota_try");
  otaPendingVerify = false;
  Serial.printf("fw %d potvrđen\n", FW_VERSION);
}

// ---------- WiFi ----------

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

// Otvara mrežu "Milivoj-Setup" za izbor nove WiFi mreže. Vraća true ako je povezan.
bool runPortal() {
  if (apPassword.length() < 8) {
    Serial.println("Nema lozinke za portal (min. 8 znakova) — portal se ne otvara");
    return false;
  }
  Serial.printf("Otvaram portal '%s' na %lu s...\n", PORTAL_SSID, PORTAL_TIMEOUT_S);
  esp_task_wdt_delete(NULL);  // portal blokira duže od watchdog-a

  WiFiManager wm;
  wm.setTitle("Milivojev bunar");
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT_S);
  wm.setConnectTimeout(20);
  bool ok = wm.startConfigPortal(PORTAL_SSID, apPassword.c_str());

  esp_task_wdt_add(NULL);
  if (!ok) return false;

  prefs.putString("ssid3", WiFi.SSID());
  prefs.putString("pass3", WiFi.psk());
  wifiMulti.addAP(WiFi.SSID().c_str(), WiFi.psk().c_str());
  Serial.printf("Nova mreža sačuvana: %s\n", WiFi.SSID().c_str());
  return true;
}

// ---------- Merenje i slanje ----------

// Prosek SAMPLES očitavanja sa AIN0, u voltima.
float readVoltage() {
  int32_t sum = 0;
  for (int i = 0; i < SAMPLES; i++) {
    sum += ads.readADC_SingleEnded(ADS_CHANNEL);
  }
  return ads.computeVolts((int16_t)(sum / SAMPLES));
}

bool sendToThingSpeak(float level, float voltage, int rssi) {
  String status = "fw " + String(FW_VERSION) + ", " + WiFi.SSID();
  for (int attempt = 1; attempt <= 2; attempt++) {
    ThingSpeak.setField(1, level);
    ThingSpeak.setField(2, voltage);
    ThingSpeak.setField(3, rssi);
    ThingSpeak.setStatus(status);
    int response = ThingSpeak.writeFields(CHANNEL_ID, tsWriteKey.c_str());
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
