#!/bin/sh
# Read shairport-sync metadata FIFO: playing flag + DACP coordinates.
PIPE=/tmp/shairport-sync-ap2-metadata
echo $$ > /tmp/meta_watch.pid
[ -p "$PIPE" ] || mkfifo "$PIPE"
chmod 666 "$PIPE" 2>/dev/null

decode() {
  echo "$1" | base64 -d 2>/dev/null | tr -d '\n\r'
}

while true; do
  cat "$PIPE" | awk 'BEGIN{RS="</item>"}
  {
    type=""; code=""; data="";
    s=$0
    if (match(s, /<type>[0-9a-fA-F]+<\/type>/)) {
      t=substr(s, RSTART, RLENGTH); gsub(/<\/?type>/, "", t); type=t
    }
    if (match(s, /<code>[0-9a-fA-F]+<\/code>/)) {
      t=substr(s, RSTART, RLENGTH); gsub(/<\/?code>/, "", t); code=t
    }
    if (match(s, /<data[^>]*>[^<]*<\/data>/)) {
      t=substr(s, RSTART, RLENGTH); sub(/<data[^>]*>/, "", t); sub(/<\/data>/, "", t); data=t
    }
    if (type != "" && code != "") { print type, code, data; fflush() }
  }' | while read -r type code data; do
    type=$(echo "$type" | tr 'A-F' 'a-f')
    code=$(echo "$code" | tr 'A-F' 'a-f')
    [ "$type" = "73736e63" ] || continue
    case "$code" in
      70626567|7072736d) echo 1 > /tmp/sps-playing ;;
      70656e64) rm -f /tmp/sps-playing ;;
      636c6970) decode "$data" > /tmp/sps-clip ;;
      61637265) decode "$data" > /tmp/sps-acre ;;
      6461706f) decode "$data" > /tmp/sps-dapo ;;
      70766f6c)
        vol=$(decode "$data" | awk -F, '{print $1+0}')
        pct=$(awk -v v="$vol" 'BEGIN{
          if (v<=-100) p=0
          else p=int(100*exp(v/40*log(10))+0.5)
          if (p<0) p=0
          if (p>100) p=100
          print p
        }')
        last=$(cat /tmp/sps-vol-last 2>/dev/null)
        [ "$pct" = "$last" ] && continue
        echo "$pct" > /tmp/sps-vol-last
        old=$(cat /tmp/vol_ring.pid 2>/dev/null)
        [ -n "$old" ] && kill "$old" 2>/dev/null
        sh /data/shairport/ap2/vol_ring.sh "$pct" >/dev/null 2>&1 &
        ;;
    esac
  done
  sleep 1
done
