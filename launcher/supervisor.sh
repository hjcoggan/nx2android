#!/bin/bash
# Flip 2 launcher supervisor (ROCKNIX), in place of EmulationStation and its sway session.
# Loop: the PocketJS launcher owns the display (DRM) until it prints "NX2L LAUNCH <id>"; the supervisor ends it, runs the game in a sway session of
# its own (sway-game.conf, which exits sway when the game ends) and starts the launcher again.
#   DIR: this folder (pocket-flip2, launcher.pocket, sway-game.conf); log in $DIR/supervisor.log
DIR="${DIR:-$(cd "$(dirname "$0")" && pwd)}"
LOG="$DIR/supervisor.log"
ROTATE="${ROTATE:-ccw}"   # the panel is mounted so that sway uses transform 270
. /etc/profile   # ROCKNIX's environment, as EmulationStation gets it
export HOME=/storage XDG_RUNTIME_DIR=/var/run/0-runtime-dir NX2L_DRM_STATS="${NX2L_DRM_STATS-}"
mkdir -p "$XDG_RUNTIME_DIR"
[ -S "$XDG_RUNTIME_DIR/bus" ] || dbus-daemon --session --address="unix:path=$XDG_RUNTIME_DIR/bus" &

game_cmd() {
  case "$1" in
    silksong)    echo 'DATA=/storage/nx2linux/silksong /storage/nx2linux/launch.sh' ;;
    hollowknight) echo 'DATA=/storage/nx2linux/hollowknight /storage/nx2linux/launch.sh' ;;
    retroarch) echo 'retroarch' ;;
    *) return 1 ;;
  esac
}

pick() {   # runs the launcher; prints the chosen id, empty if it ended without one
  rm -f /tmp/nx2l.choice
  "$DIR/pocket-flip2" --package "$DIR/launcher.pocket" ${ROTATE:+--rotate $ROTATE} 2>&1 | while IFS= read -r line; do
    echo "launcher: $line" >> "$LOG"
    case "$line" in
      "NX2L LAUNCH "*) echo "${line#NX2L LAUNCH }" > /tmp/nx2l.choice; pkill -TERM -x pocket-flip2 ;;
    esac
  done
  cat /tmp/nx2l.choice 2>/dev/null
}

play() {   # $1: the game's command line; a sway session that ends with it
  GAME="$1" WLR_BACKENDS=drm,libinput WLR_LIBINPUT_NO_DEVICES=1 WLR_OUTPUT_ROTATE_PLANE=1 \
    SDL_AUDIODRIVER=pulseaudio sway -c "$DIR/sway-game.conf" >> "$LOG" 2>&1
}

echo "$(date) supervisor start" >> "$LOG"
while :; do
  id=$(pick)
  cmd=$(game_cmd "$id") || { echo "$(date) launcher ended without a game ($id)" >> "$LOG"; sleep 1; continue; }
  echo "$(date) start $id: $cmd" >> "$LOG"
  play "$cmd"
  echo "$(date) $id ended" >> "$LOG"
done
