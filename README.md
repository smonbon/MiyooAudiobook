# MiyooAudiobook

An audiobook player for the **Miyoo Mini+** running OnionOS.

Browse your library by artist and album, resume where you left off, control playback speed, and listen with the screen off to save battery.

## Features

- **Three-level library browser**: Artists > Albums > Tracks
- **Playback resume**: Saves position per album, offers to continue on launch
- **Variable speed playback** (0.5x - 2.0x) with WSOLA pitch-preserving time-stretch
- **Screen off mode**: Turns off the display while playing to save power
- **Screen lock**: Prevents accidental button presses (L2+R2 to lock/unlock)
- **Sleep timer**: Auto-pause after 5-60 minutes
- **Cover art**: Loads `cover.jpg` from album folders, or downloads from OpenLibrary
- **NTP time sync**: Shows a clock on the Now Playing screen
- **Bilingual UI**: English and German
- **Configurable audiobook path**: Choose from multiple SD card locations
- **Low memory footprint**: Flat arrays, ~1MB total RAM usage

## Screenshots

*Coming soon*

## Requirements

- Miyoo Mini+ with [OnionOS](https://github.com/OnionUI/Onion)
- Audiobooks as MP3 files in `/mnt/SDCARD/Audiobooks/Artist/Album/` structure
- A TTF font file (see [assets/FONT_GOES_HERE.txt](assets/FONT_GOES_HERE.txt))

## Building

You need Docker to cross-compile for ARM.

```bash
# Build (first run creates the Docker toolchain image)
./scripts/build.sh

# Package into a zip for SD card
make package
```

## Installation

### Via SD card

1. Run `./scripts/build.sh` then `./scripts/deploy.sh`
2. Or: run `make package` and extract `dist/MiyooAudiobook.zip` to your SD card root

### Via WiFi

```bash
./scripts/deploy_wifi.sh 192.168.x.x
```

## Directory Structure

```
/mnt/SDCARD/Audiobooks/
  Author Name/
    Book Title/
      cover.jpg          (optional)
      01 - Chapter 1.mp3
      02 - Chapter 2.mp3
      ...
```

## Controls

| Button | Now Playing | Browse |
|--------|-------------|--------|
| A | Pause/Resume | Open |
| B | Back to tracks | Back / Quit |
| Start | Toggle browse/playing | Go to Now Playing |
| Select | Settings | Settings |
| Left/Right | Prev/Next track | Page up/down |
| Up/Down | Speed +/- | Navigate list |
| L1/R1 | Seek -/+15s | Seek -/+15s |
| L2+R2 (hold) | Lock/Unlock | Lock/Unlock |

## Configuration

Settings are accessible via the **Select** button from any screen:

- Screen timeout (5s - 60s, or never)
- Sleep timer (5 - 60 minutes)
- Language (English / Deutsch)
- Audiobook source path
- Clock format (12h / 24h)
- Download covers from OpenLibrary
- Reset all progress

## Tech Stack

- C (single-file: `src/main.c`)
- SDL 1.2 + SDL_mixer + SDL_ttf + SDL_image
- mpg123 (loaded via dlopen for variable speed decoding)
- Cross-compiled for ARM (armhf) via Docker

## License

[MIT](LICENSE)
