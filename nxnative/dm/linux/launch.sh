#!/bin/sh
# ROCKNIX launcher (roms/ports/<Game>.sh): runs dmhost on a data pack in EmulationStation's sway session.
#   DATA: the data pack (porter's <App>-data); DMHOST: the host built for its game (dm/linux/build.py --game <title ID>)
DATA="${DATA:-/storage/nx2linux/silksong}"
DMHOST="${DMHOST:-$DATA/dmhost}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/var/run/0-runtime-dir}" WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-1}"
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-wayland}" SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-pulseaudio}"
export SDL_GAMECONTROLLERCONFIG_FILE="${SDL_GAMECONTROLLERCONFIG_FILE:-/storage/.config/SDL-GameControllerDB/gamecontrollerdb.txt}"
cd "$DATA" || exit 1
mv -f log.txt log.prev.txt 2>/dev/null
exec "$DMHOST" "$DATA" > log.txt 2>&1
