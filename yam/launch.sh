#!/bin/sh
# YAM service wrapper: keep AirPlay in the foreground so yamd can stop the group.
ROOT=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
DEST=/data/shairport

mkdir -p "$DEST/ap2"
# Payload files live next to this script inside the .yap
for f in airplay.sh airplayd VERSION; do
  if [ -f "$ROOT/$f" ]; then
    cp -f "$ROOT/$f" "$DEST/$f"
  fi
done
if [ -d "$ROOT/ap2" ]; then
  cp -a "$ROOT/ap2/." "$DEST/ap2/"
fi
chmod 755 "$DEST/airplay.sh" "$DEST/airplayd" 2>/dev/null
chmod 755 "$DEST/ap2/shairport-sync-ap2" "$DEST/ap2/nqptp" "$DEST/ap2/avahi-daemon" 2>/dev/null

stop() {
  "$DEST/airplay.sh" stop >/dev/null 2>&1
  exit 0
}
trap stop TERM INT HUP

"$DEST/airplay.sh" setup >/dev/null 2>&1 || true
"$DEST/airplay.sh" start

while ps | grep -q "[s]hairport-sync-ap2"; do
  sleep 2
done
exit 1
