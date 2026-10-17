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

## Firmware
`firmware/Milivoj/Milivoj.ino`, ploča **ESP32 Dev Module**, biblioteke `Adafruit ADS1X15` i `ThingSpeak`.

1. Kopiraj `firmware/Milivoj/secrets.example.h` u `secrets.h` i upiši WiFi i ThingSpeak Write API Key. `secrets.h` ne ide na GitHub.
2. Interval slanja je `SEND_INTERVAL_MS` u `Milivoj.ino` (trenutno 30 s, za testove).
3. Za komšijinu mrežu upiši `WIFI_SSID2` / `WIFI_PASSWORD2`. ESP32 se sam poveže na mrežu koju vidi.

## Napomene
- Jumper J1 na HW-685 mora biti na 3,3 V, jer ADS1115 na 3,3 V ne sme da dobije više od ~3,6 V.
- Pokidan kabl ili isključena sonda prikazuju se kao 0 m.
