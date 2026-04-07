# MiyooAudiobook

An audiobook player for the **Miyoo Mini+** running OnionOS.

Browse your library by artist and album, resume where you left off, control playback speed, and listen with the screen off to save battery.

## Features

- **Three-level library browser** — Artists > Albums > Tracks
- **Playback resume** — saves position per album, offers to continue on launch
- **Variable speed playback** — 0.5x to 2.0x with WSOLA pitch-preserving time-stretch (no chipmunk effect)
- **Screen off mode** — turns off the display while playing to save battery
- **Screen lock** — prevents accidental button presses (hold L2+R2)
- **Sleep timer** — auto-pause after 5–60 minutes
- **Cover art** — loads `cover.jpg` from album folders, or downloads automatically from Open Library
- **Clock** — shows current time on the Now Playing screen (synced via NTP)
- **Bilingual UI** — English and Deutsch
- **Configurable audiobook path** — choose from multiple SD card locations
- **Low memory footprint** — flat data layout, ~1MB total RAM usage

## Installation

### Option A: Pre-built (recommended)

1. Download `MiyooAudiobook.zip` from the [latest release](../../releases/latest)
2. Extract the zip — you will get an `App/MiyooAudiobook/` folder
3. Copy the `App/MiyooAudiobook/` folder to your SD card at `SD:/App/MiyooAudiobook/`
4. **Add a font:** download a free TTF font (e.g. [DejaVu Sans](https://dejavu-fonts.github.io)) and place it as `SD:/App/MiyooAudiobook/assets/font.ttf`
5. Eject the SD card, put it in your Miyoo Mini+ and launch the app from the OnionOS menu

### Option B: Build from source

Requirements: Docker Desktop (Mac/Linux)

```bash
git clone https://github.com/smonbon/MiyooAudiobook.git
cd MiyooAudiobook
./scripts/build.sh      # cross-compiles via Docker (first run ~3 min)
make package            # creates dist/MiyooAudiobook.zip
```

Then copy `dist/MiyooAudiobook.zip` to your SD card as described above.

### Deploy via WiFi (for developers)

If your Miyoo is connected to WiFi:

```bash
./scripts/deploy_wifi.sh 192.168.x.x
```

## Audiobook Folder Structure

Place your audiobooks on the SD card like this:

```
SD:/Audiobooks/
  Author Name/
    Book Title/
      cover.jpg          ← optional, downloaded automatically if missing
      01 - Chapter 1.mp3
      02 - Chapter 2.mp3
      ...
    Another Book/
      ...
```

Supported audio formats: MP3, OGG, WAV, FLAC

The default path is `/mnt/SDCARD/Audiobooks`. You can change it in Settings (Select button).

## Controls

| Button | Now Playing | Browse |
|--------|-------------|--------|
| **A** | Pause / Resume | Open selected |
| **B** | Back to track list | Back / Quit |
| **Start** | Toggle browse ↔ Now Playing | Jump to Now Playing |
| **Select** | Settings | Settings |
| **Up / Down** | Speed +/− | Navigate list |
| **Left / Right** | Previous / Next track | Page up / down |
| **L1 / R1** | Seek −15s / +15s | Seek −15s / +15s |
| **L2+R2 (hold)** | Lock / Unlock screen | Lock / Unlock screen |

## Settings

Open with the **Select** button from any screen:

| Setting | Options |
|---------|---------|
| Screen timeout | 5s / 10s / 15s / 30s / 60s / Never |
| Sleep timer | Off / 5–60 min |
| Language | English / Deutsch |
| Audiobook path | 5 preset SD card locations |
| Clock format | 24h / 12h AM/PM |
| Download covers | Fetches cover art from Open Library |
| Reset progress | Clears all saved positions |

## Building

| Command | Description |
|---------|-------------|
| `./scripts/build.sh` | Cross-compile via Docker |
| `make package` | Create SD-card ready zip in `dist/` |
| `./scripts/deploy.sh` | Copy to SD card (edit path in script) |
| `./scripts/deploy_wifi.sh <ip>` | Deploy via SSH/SCP over WiFi |

## Tech Stack

- **Language:** C (single source file: `src/main.c`)
- **Graphics/Input:** SDL 1.2, SDL_ttf, SDL_image
- **Audio:** SDL_mixer + mpg123 (loaded via `dlopen` for variable-speed decoding)
- **Time-stretch:** WSOLA algorithm (pitch-preserving, ~46ms window)
- **Cross-compilation:** Docker with `arm-linux-gnueabihf-gcc`
- **Target:** Miyoo Mini+ / OnionOS (ARMv7, Linux)

## License

[MIT](LICENSE) © 2026 Simon Speier

## Third-Party Licenses

This project bundles pre-compiled ARM shared libraries required to run on the Miyoo Mini+:

| Library | License | Source |
|---------|---------|--------|
| SDL 1.2 | LGPL 2.1 | [libsdl.org](https://www.libsdl.org) |
| SDL_mixer | LGPL 2.1 | [github.com/libsdl-org/SDL_mixer](https://github.com/libsdl-org/SDL_mixer) |
| SDL_ttf | LGPL 2.1 | [github.com/libsdl-org/SDL_ttf](https://github.com/libsdl-org/SDL_ttf) |
| SDL_image | LGPL 2.1 | [github.com/libsdl-org/SDL_image](https://github.com/libsdl-org/SDL_image) |
| mpg123 | LGPL 2.1 | [mpg123.de](https://www.mpg123.de) |
| libjpeg | BSD-like | [ijg.org](https://www.ijg.org) |

Per the LGPL, you are free to replace any of the `.so` files in the `lib/` folder with your own builds. Source code for all libraries is available at the links above.

Cover art data provided by [Open Library](https://openlibrary.org) (Internet Archive) under their open data license.

## FAQ

**The app doesn't start / shows a black screen**
You're probably missing the font file. Download a TTF font (e.g. [DejaVu Sans](https://dejavu-fonts.github.io), [Inter](https://fonts.google.com/specimen/Inter), or [Noto Sans](https://fonts.google.com/noto/specimen/Noto+Sans)) and place it as `App/MiyooAudiobook/assets/font.ttf` on your SD card.

**The app shows "No files in /mnt/SDCARD/Audiobooks"**
Your audiobooks need to be in a two-level folder structure: `Audiobooks/Author/Book/files.mp3`. Loose MP3 files directly in the Audiobooks folder won't be found. Also check that your path setting (Select > Settings) matches where your files actually are.

**The screen went black but I can still hear audio**
That's the screen-off feature saving battery! Press any button to wake the screen. You can change the timeout in Settings (Select button) or set it to "Never".

**I can't press any buttons / the app seems frozen**
The screen is probably locked. Hold **L2+R2** for one second, then press **A** to unlock.

**Variable speed doesn't work / no speed option**
Speed control requires the mpg123 library. Make sure the `lib/` folder with all `.so` files is present in the app directory. If you built from source, check that the Docker build completed successfully. For OGG/WAV/FLAC files, speed control is not available (only MP3).

**My audiobooks have a flat structure (no Author subfolder)**
The app expects `Audiobooks/Author/Book/tracks`. If your books are directly under `Audiobooks/BookTitle/tracks`, they will each appear as a separate "artist" with one album. This works fine.

**Cover art download doesn't find anything**
The search uses Open Library, which mainly has published books. Self-produced or niche audiobooks may not have covers. You can always place a `cover.jpg` (any size) manually in each album folder.

**How do I update to a newer version?**
Download the latest release zip and overwrite the files in `App/MiyooAudiobook/` on your SD card. Your `progress.txt` and `settings.txt` will be preserved — your listening progress is safe.

**Does it work on the original Miyoo Mini (v1)?**
Not tested. The app targets the Miyoo Mini+ with OnionOS. It might work on similar ARM devices running OnionOS, but the button mapping and display resolution (640x480) are hardcoded for the Mini+.

## Known Limitations

- Variable speed playback only works with **MP3** files (via mpg123). OGG/WAV/FLAC play at normal speed through SDL_mixer.
- Maximum library size: 32 artists, 128 albums, 4096 tracks (hardcoded limits to keep RAM usage low).
- Cover art search requires WiFi and uses the Open Library API, which may not have covers for every book.
- No chapter support for M4B/M4A audiobook files.
- The clock requires WiFi on first launch to sync via NTP.

## Contributing

Found a bug? Have a feature idea? [Open an issue](../../issues) or send a pull request.

If you're testing the app, feedback on these areas is especially welcome:
- Different audiobook folder structures
- OGG/WAV/FLAC file playback
- Various OnionOS versions
- Long listening sessions (battery, stability)
