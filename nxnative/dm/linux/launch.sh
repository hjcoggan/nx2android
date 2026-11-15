#!/bin/sh
# ROCKNIX launcher (roms/ports/<Game>.sh): runs dmhost on a data pack in the sway session.
#   DATA: the data pack (porter's <App>-data); DMHOST: the host built for its game (dm/linux/build.py --game <title ID>)
#   GOVERNOR: the big cores' cpufreq governor while the game runs (default performance; empty leaves it): the game's main thread is CPU-bound and ondemand
#   ramps it too slowly (a Snapdragon 865's prime core seen at 960 of 2841 MHz, frames of 23-30 ms). The governors are put back when the game ends.
DATA="${DATA:-/storage/nx2linux/silksong}"
DMHOST="${DMHOST:-$DATA/dmhost}"
GOVERNOR="${GOVERNOR-performance}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/var/run/0-runtime-dir}" WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-1}"
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-wayland}" SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-pulseaudio}"
export SDL_GAMECONTROLLERCONFIG_FILE="${SDL_GAMECONTROLLERCONFIG_FILE:-/storage/.config/SDL-GameControllerDB/gamecontrollerdb.txt}"
cd "$DATA" || exit 1
mv -f log.txt log.prev.txt 2>/dev/null

# every policy but the first (the little cores) is a big core cluster
SAVED=""
if [ -n "$GOVERNOR" ]; then
  for p in $(ls -d /sys/devices/system/cpu/cpufreq/policy* | sort -V | tail -n +2); do
    [ -w "$p/scaling_governor" ] || continue
    SAVED="$SAVED $p:$(cat "$p/scaling_governor")"
    echo "$GOVERNOR" > "$p/scaling_governor"
  done
fi
restore() { for s in $SAVED; do echo "${s#*:}" > "${s%%:*}/scaling_governor"; done; }
trap restore EXIT INT TERM

"$DMHOST" "$DATA" > log.txt 2>&1
