# ============================================================
#  MiyooAudiobook – Makefile
#  Builds inside the miyoomini Docker toolchain container.
#  Usage:
#    make          → compile
#    make clean    → remove build artifacts
#    make package  → create SD-card ready zip
# ============================================================

TARGET     = audiobook-player
SRC        = src/main.c

# Cross-compiler (inside Docker / Ubuntu armhf)
CC         = arm-linux-gnueabihf-gcc

CFLAGS     = -O2 -Wall -no-pie \
             $(shell pkg-config --cflags sdl SDL_mixer SDL_ttf SDL_image 2>/dev/null || \
               echo -I/usr/include/SDL) \
             -I/usr/arm-linux-gnueabihf/include

LDFLAGS    = -no-pie \
             $(shell pkg-config --libs sdl SDL_mixer SDL_ttf SDL_image 2>/dev/null || \
               echo -lSDL -lSDL_mixer -lSDL_ttf -lSDL_image) -lm -ldl

.PHONY: all clean package

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Build OK → $(TARGET)"

clean:
	rm -f $(TARGET)

# Creates ./dist/MiyooAudiobook.zip ready to copy to SD card
package: $(TARGET)
	mkdir -p dist/App/MiyooAudiobook/assets
	mkdir -p dist/App/MiyooAudiobook/lib
	cp $(TARGET)          dist/App/MiyooAudiobook/
	cp scripts/launch.sh  dist/App/MiyooAudiobook/
	cp config.json        dist/App/MiyooAudiobook/
	cp assets/*           dist/App/MiyooAudiobook/assets/ 2>/dev/null || true
	cp lib/*.so*          dist/App/MiyooAudiobook/lib/    2>/dev/null || true
	cd dist && zip -r MiyooAudiobook.zip App/
	@echo "Package ready → dist/MiyooAudiobook.zip"
