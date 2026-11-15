# Linux host (arm64)

Runs a game's Android data pack on arm64 Linux with an Adreno GPU and Mesa's Turnip driver, for example ROCKNIX on a Retroid Pocket Flip 2 (Snapdragon 865).
The game's rewritten code and SPIR-V shaders are the ones porter makes for Android; only the host is built for Linux.

## Build

The host is built per game, because its profile is compiled in. Build it from the same tree that made the data pack.

```sh
docker build -t nx2linux-build nxnative/dm/linux          # once: Debian trixie arm64, clang, glslc, SDL3
docker run --rm -v "$PWD":/src -w /src nx2linux-build \
  python3 nxnative/dm/linux/build.py --game <title ID>    # → nxnative/dm/linux/out/<title ID>/dmhost
```

On Apple silicon, colima or Docker Desktop runs the container natively. The executable needs glibc 2.38 or later and `libSDL3.so.0`.

## Data pack

`python -m porter port <game folder>` makes `<App>-data` beside the APK. Copy that folder to the device, and put `dmhost` in it.

## Run

```sh
dmhost <data dir> [-- game arguments]
```

On ROCKNIX, copy `launch.sh` to `/storage/roms/ports/<Game>.sh` and set `DATA` in it. It runs in EmulationStation's sway session and writes `log.txt` in the data folder.

- Controllers are mapped by position (the bottom face button is B, the right one A); the pause menu switches to labels. Guide opens the pause menu, Guide + Start quits.
- The keyboard works for desktop runs: arrows, Z/X = B/A, A/S = Y/X, Q/W = L/R, 1/2 = ZL/ZR, Enter = +, Backspace = -, Esc opens the pause menu, F10 quits.
- `launch.sh` sets the big cores' governor to `performance` while the game runs (`GOVERNOR=` to leave it) and puts it back when the game ends.
- `DM_WINDOWED=1` opens a 1280x720 window instead of fullscreen. `DM_HEADLESS=1` runs with no window (frames counted only, `DM_VK_SHOT`).
- `env.txt` in the data folder holds `KEY=VALUE` switches, as on Android.

## Differences from Android

| | Android | Linux |
|---|---|---|
| Thread slots | bionic's `TLS_SLOT_APP` (`tpidr_el0[2]`) | `tls_slot.c` holds the same word: the first word of the executable's TLS block |
| Memory pools | zero-copy through AHardwareBuffer | zero-copy through Vulkan memory exported as a dma-buf (`ahb_vk.c`) |
| Window, input | NativeActivity | SDL3 |
| Audio | AAudio | SDL3 audio stream |
| H.264 movies, AAC | MediaCodec | the system's FFmpeg, loaded at run time (`ffmpeg_linux.c`) |
| Pause menu, text entry | touch and controller | controller and keyboard (rumble, face buttons, frame rate, docked mode) |
| Rumble | Vibrator | SDL gamepad rumble, both motors |
| On-screen gamepad, save manager | yes | not yet |
