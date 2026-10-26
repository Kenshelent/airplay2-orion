#!/bin/sh
# Duck/pause AirPlay while Alice talks or plays. Never kill shairport.
# Resume only if AirPlay was actually playing.
A=/data/shairport/ap2
LOG=/data/quasar/daemons_logs/mediad.log
CHUNK=/tmp/alice_yield.chunk
export LD_LIBRARY_PATH=$A/libs
export HOME=$A

echo $$ > /tmp/alice_yield.pid

dacp() {
  cmd=$1
  acre=$(cat /tmp/sps-acre 2>/dev/null)
  clip=$(cat /tmp/sps-clip 2>/dev/null)
  port=$(cat /tmp/sps-dapo 2>/dev/null)
  [ -n "$port" ] || port=3689
  [ -n "$acre" ] || return 1
  [ -n "$clip" ] || return 1
  case "$clip" in
    *:*) url="http://[$clip]:$port/ctrl-int/1/$cmd" ;;
    *)   url="http://$clip:$port/ctrl-int/1/$cmd" ;;
  esac
  wget -q -O /dev/null -T 2 --header "Active-Remote: $acre" "$url" 2>/dev/null
}

vol_airplay() {
  amixer -q sset 'AirPlay Playback Volume' "$1" 2>/dev/null
}

pause_airplay() {
  [ -f /tmp/sps-alice-hold ] && return 0
  if [ -f /tmp/sps-playing ]; then
    echo 1 > /tmp/sps-resume
  fi
  [ -f /tmp/sps-resume ] || return 0
  touch /tmp/sps-alice-hold
  dacp pause
  vol_airplay "0%"
}

resume_airplay() {
  [ -f /tmp/sps-alice-hold ] || [ -f /tmp/sps-resume ] || return 0
  [ -f /tmp/sps-resume ] || {
    rm -f /tmp/sps-alice-hold
    return 0
  }
  vol_airplay "100%"
  dacp play
  rm -f /tmp/sps-resume /tmp/sps-alice-hold
}

off=0
[ -f "$LOG" ] && off=$(wc -c < "$LOG" | tr -d ' ')
music=0
hold_until=0

while true; do
  now=$(date +%s)
  if [ -f "$LOG" ]; then
    sz=$(wc -c < "$LOG" | tr -d ' ')
    [ "$sz" -lt "$off" ] && off=0
    if [ "$sz" -gt "$off" ]; then
      n=$((sz-off))
      dd if="$LOG" of="$CHUNK" bs=1 skip="$off" count="$n" 2>/dev/null
      off=$sz
      while IFS= read -r line || [ -n "$line" ]; do
        echo "$line" | grep -qE 'handlePlayAudio|type=|type VOICE|Volume for player|destructed|destroy instance' || continue
        if echo "$line" | grep -q 'handlePlayAudio'; then
          pause_airplay
          hold_until=$((now+8))
        fi
        if echo "$line" | grep -qE 'type=VOICE|type VOICE|\[voice '; then
          pause_airplay
          hold_until=$((now+8))
        fi
        if echo "$line" | grep -qE 'type=(MUSIC|RADIO|FILE|AUDIO_FILE|TRACK|PODCAST|FM|VIDEO|KINO)'; then
          music=1
          pause_airplay
        fi
        if echo "$line" | grep -q 'Volume for player "' && echo "$line" | grep -qv 'player "voice"'; then
          music=1
          pause_airplay
        fi
        if echo "$line" | grep -qE 'destructed|destroy instance'; then
          if echo "$line" | grep -qv '\[voice '; then
            music=0
            hold_until=$((now+2))
          fi
        fi
      done < "$CHUNK"
    fi
  fi
  now=$(date +%s)
  if [ "$music" -eq 1 ]; then
    :
  elif [ "$now" -lt "$hold_until" ]; then
    :
  else
    resume_airplay
  fi
  sleep 1
done
