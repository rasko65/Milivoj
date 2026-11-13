# Milivojev bunar

Merenje nivoa vode u bunaru (0–10 m) i prikaz na dashboardu.

**Dashboard:** https://rasko65.github.io/Milivoj/

## Kako radi
Sonda 4–20 mA → HW-685 (4 mA = 0 V, 20 mA = 3,0 V) → ADS1115 (AIN0, 0x48) → ESP32 (SDA = IO21, SCL = IO22) → WiFi → ThingSpeak kanal `3540749` → dashboard (`index.html`, GitHub Pages).

Nivo se računa linearno: 0 V = 0 m, 3,0 V = 10 m vode iznad sonde.

| ThingSpeak polje | Sadržaj |
|---|---|
| field1 | nivo (m) |
| field2 | napon sa HW-685 (V) |
| field3 | WiFi RSSI (dBm) |
| status | verzija firmware-a i WiFi mreža (npr. `fw 3, HUAWEI-B311-472C`) |

## Firmware
`firmware/Milivoj/Milivoj.ino`, ploča **ESP32 Dev Module** sa particijama **Minimal SPIFFS (1.9MB APP with OTA)**. Biblioteke: `Adafruit ADS1X15`, `ThingSpeak`, `ArduinoJson`, `WiFiManager`.

Lozinke i ThingSpeak ključ su u NVS (trajna memorija ploče). Upisuje ih samo verzija prebačena **kablom** iz `firmware/Milivoj/secrets.h` (kopija `secrets.example.h`, ne ide na GitHub). Verzija koja se preuzima preko interneta ih ne sadrži.

### Nova WiFi mreža na licu mesta (bez kabla)
Ako ESP32 ne nađe nijednu poznatu mrežu, otvara mrežu **Milivoj-Setup** na 5 minuta (lozinka je `AP_PASSWORD` iz `secrets.h`).
1. Telefonom se poveži na `Milivoj-Setup`. Stranica se otvara sama, a ako ne, otvori `192.168.4.1`.
2. **Configure WiFi** → izaberi mrežu → upiši lozinku → **Save**.
3. ESP32 pamti mrežu i počinje da šalje. Ako niko ništa ne izabere, restartuje se i pokušava ponovo.

### Podešavanja odavde (`config.json`)
ESP32 čita `config.json` na svakih 10 min i pri svakom startu:

| ključ | značenje |
|---|---|
| `interval_s` | interval slanja u sekundama (15–86400) |
| `v_full` | napon pri 20 mA (V) |
| `level_full_m` | nivo pri 20 mA (m) |
| `fw_version`, `fw_url` | najnovija verzija firmware-a (popunjava `tools/release.sh`) |

Izmeni, commit, push. ESP32 preuzme promenu za 10–20 min (GitHub Pages keš + interval provere).

### Novi firmware odavde (OTA)
1. Izmeni kod i povećaj `FW_VERSION` u `Milivoj.ino`.
2. `tools/release.sh` napravi `firmware.bin` bez lozinki (i to proveri), pa upiše verziju u `config.json`.
3. Commit + push. ESP32 preuzme novu verziju i potvrdi je posle prvog uspešnog slanja na ThingSpeak. Ako nova verzija ne uspe da pošalje podatke, posle restarta se vraća stara i ta verzija se više ne pokušava.

Verzija koja trenutno radi vidi se u ThingSpeak statusu.

## Napomene
- Jumper J1 na HW-685 mora biti na 3,3 V, jer ADS1115 na 3,3 V ne sme da dobije više od ~3,6 V.
- Pokidan kabl ili isključena sonda prikazuju se kao 0 m.
