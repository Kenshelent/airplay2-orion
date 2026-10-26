#!/bin/sh
# Volume bar on the 36-LED ring, then give the ring back to Alice.
pct=$1
[ -n "$pct" ] || exit 0
[ "$pct" -lt 0 ] && pct=0
[ "$pct" -gt 100 ] && pct=100

LED=/sys/class/leds
echo $$ > /tmp/vol_ring.pid

restore() {
  i=0
  while [ $i -lt 36 ]; do
    echo ledfb-ring > $LED/orion:red:ring$i/trigger 2>/dev/null
    echo ledfb-ring > $LED/orion:green:ring$i/trigger 2>/dev/null
    echo ledfb-ring > $LED/orion:blue:ring$i/trigger 2>/dev/null
    i=$((i+1))
  done
  echo ledfb-ring-sync > $LED/orion:ringA:sync/trigger 2>/dev/null
  echo ledfb-ring-sync > $LED/orion:ringB:sync/trigger 2>/dev/null
  echo ledfb-ring-sync > $LED/orion:ringC:sync/trigger 2>/dev/null
}

trap restore EXIT

i=0
while [ $i -lt 36 ]; do
  echo none > $LED/orion:red:ring$i/trigger 2>/dev/null
  echo none > $LED/orion:green:ring$i/trigger 2>/dev/null
  echo none > $LED/orion:blue:ring$i/trigger 2>/dev/null
  i=$((i+1))
done
echo none > $LED/orion:ringA:sync/trigger 2>/dev/null
echo none > $LED/orion:ringB:sync/trigger 2>/dev/null
echo none > $LED/orion:ringC:sync/trigger 2>/dev/null

awk -v pct="$pct" 'BEGIN{
  n=36
  filled=pct*n/100
  r=int(220*pct/100)
  g=int(220*(100-pct)/100)
  b=8
  for(i=0;i<n;i++){
    f=filled-i
    if(f<0)f=0
    if(f>1)f=1
    printf "%d %d %d %d\n", i, int(r*f), int(g*f), int(b*f)
  }
}' | while read i r g b; do
  echo "$r" > $LED/orion:red:ring$i/brightness
  echo "$g" > $LED/orion:green:ring$i/brightness
  echo "$b" > $LED/orion:blue:ring$i/brightness
done

sleep 1
# EXIT trap restores ledfb-ring
