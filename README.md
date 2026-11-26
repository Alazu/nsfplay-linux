# NSFPlay for Linux

Download the latest AppImage from the [Releases](https://github.com/Alazu/nsfplay-linux/releases) page.

A Linux port of [NSFPlay](https://bbbradsmith.github.io/nsfplay/) (Brad Smith's fork of Brezza's NSFPlay).
The emulation core (`xgm/`, `vcm/`) is the original portable code, unchanged. The Windows GUI
(MFC + Winamp plugin framework) has been replaced by a new Qt6 front-end with ALSA audio
output (works with PipeWire / PulseAudio through their ALSA plugins).

## Run

    ./dist/NSFPlay-x86_64.AppImage [file.nsf]

The AppImage also contains the command-line tools:

    ./NSFPlay-x86_64.AppImage nsf2wav -t 3 -l 120000 song.nsf out.wav
    ./NSFPlay-x86_64.AppImage nsfmeta song.nsfe

Built on Ubuntu 22.04, so it needs glibc 2.35+ (Ubuntu 22.04+, Debian 12+, Fedora 36+, ...).

## Features

- NSF, NSFe, NSF2 and NEZ-style M3U playlist entries; all expansion chips (VRC6, VRC7/YM2413, FDS, MMC5, N163, 5B)
- Track list with NSFe titles and lengths, play/pause/stop/prev/next, seek bar, master volume
- Auto-advance / play forever / stop after track / repeat track
- Live per-channel activity meters with note names (left-click a channel to mute, right-click to solo)
- Mixer: per-device volume/mute, per-channel on/off, volume and pan
- Options: sample rate, quality, default play/fade time, loop count, silence auto-stop, loop auto-detect,
  region (NTSC/PAL/Dendy), VSync, NSF2 IRQ, CPU speed, VRC7 patch set, filters, all per-device emulation
  options, plus an "Advanced" editor for every raw config key (same keys as the Windows `nsfplay.ini`)
- WAV export of the current track or all tracks
- Drag & drop, settings saved to `~/.config/nsfplay/nsfplay.ini`

Keyboard: Z prev, X play, Space pause, V stop, B next, Left/Right seek 10 s, Ctrl+O open,
Ctrl+I info, Ctrl+M mixer, Ctrl+P options.

Environment: `NSFPLAY_ALSA_DEVICE=<pcm>` selects an ALSA device other than `default`.

## Build

Native (for local use):

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
    ./build/nsfplay

Requires Qt6 Widgets, ALSA headers, CMake, a C++17 compiler.

AppImage (portable, built inside an Ubuntu 22.04 container via podman or docker):

    ./build-appimage.sh            # -> dist/NSFPlay-x86_64.AppImage
    ./build-appimage.sh --native   # host toolchain instead (only runs on distros with glibc >= the host's)

## Changes vs. upstream source

- `linux/` – new Qt6/ALSA player (`main.cpp`, `engine.cpp/.h`), icon and .desktop file
- `CMakeLists.txt`, `build-appimage.sh` – new build system
- `contrib/nsf2wav.cpp` – fixed short options: `-t` (track) and `-q` (quiet) were documented but missing from getopt
- `test/make_test_nsf.py` – generates a tiny 2-track test NSF

## Credits / license

NSFPlay core © Brezza (original NSFPlay/NSFPlug) and Brad Smith (maintained fork, https://github.com/bbbradsmith/nsfplay).
The upstream readme (`readme.txt`) states the code is distributed freely and may be modified and redistributed;
the same permissive terms apply to the Linux front-end in this repository. No warranty.
