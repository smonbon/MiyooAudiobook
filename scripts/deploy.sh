#!/bin/bash
# ============================================================
#  deploy.sh – Copy app to SD card after building
#  Edit SD_CARD_PATH to match where your SD card is mounted.
# ============================================================

set -e

SD_CARD_PATH="/Volumes/MIYOO"       # ← change this if different
APP_DIR="$SD_CARD_PATH/App/MiyooAudiobook"

if [ ! -d "$SD_CARD_PATH" ]; then
    echo "SD card not found at $SD_CARD_PATH"
    echo "Plug in the card and check the name in Finder."
    exit 1
fi

# Generate icon
python3 "$(dirname "$0")/make_icon.py"

echo "Deploying to $APP_DIR ..."
mkdir -p "$APP_DIR/assets"
mkdir -p "$APP_DIR/lib"

cp audiobook-player          "$APP_DIR/"
cp scripts/launch.sh         "$APP_DIR/"
cp config.json               "$APP_DIR/"
cp assets/* "$APP_DIR/assets/" 2>/dev/null || true
cp lib/*    "$APP_DIR/lib/"   2>/dev/null || true

# Make sure Audiobooks folder exists
mkdir -p "$SD_CARD_PATH/Audiobooks"

echo ""
echo "Deployed! Files on SD card:"
ls -lh "$APP_DIR"
echo ""
echo "Eject the SD card, put it in the Miyoo and start the app."
