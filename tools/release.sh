#!/bin/bash
# Pravi OTA firmware (bez secrets.h) i priprema ga za objavu na GitHub Pages.
#   1. Povećaj FW_VERSION u firmware/Milivoj/Milivoj.ino
#   2. tools/release.sh
#   3. git add -A && git commit && git push  -> ESP32 preuzima novu verziju u roku od ~10-20 min
set -euo pipefail
cd "$(dirname "$0")/.."

CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
FQBN="esp32:esp32:esp32:PartitionScheme=min_spiffs"
SKETCH=firmware/Milivoj
BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT

VER=$(awk '/^#define FW_VERSION/ {print $3}' "$SKETCH/Milivoj.ino")
CUR=$(python3 -I -c 'import json; print(json.load(open("config.json"))["fw_version"])')
if [ "$VER" -le "$CUR" ]; then
  echo "GREŠKA: FW_VERSION ($VER) mora biti veći od fw_version u config.json ($CUR)." >&2
  exit 1
fi

echo "Kompajliram fw $VER (OTA, bez secrets.h)..."
"$CLI" compile --fqbn "$FQBN" --build-path "$BUILD" \
  --build-property "compiler.cpp.extra_flags=-DOTA_BUILD" "$SKETCH" | grep 'Sketch uses'

# firmware.bin je javan — ne sme da sadrži nijednu vrednost iz secrets.h.
if [ -f "$SKETCH/secrets.h" ]; then
  sed -nE 's/^#define [A-Z0-9_]+ +"([^"]{4,})".*/\1/p' "$SKETCH/secrets.h" | while read -r secret; do
    if grep -a -q -F -- "$secret" "$BUILD/Milivoj.ino.bin"; then
      echo "GREŠKA: firmware.bin sadrži vrednost iz secrets.h — prekidam." >&2
      exit 1
    fi
  done
fi

cp "$BUILD/Milivoj.ino.bin" firmware.bin
python3 -I - "$VER" <<'PY'
import json, sys
p = "config.json"; c = json.load(open(p))
c["fw_version"] = int(sys.argv[1])
open(p, "w").write(json.dumps(c, indent=2) + "\n")
PY
echo "Spremno: firmware.bin (fw $VER) i config.json. Sledeće: git add -A && git commit && git push"
