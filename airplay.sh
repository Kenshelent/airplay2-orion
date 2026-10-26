#!/bin/sh
# AirPlay 2 for Yandex Station 3 (orion, aarch64) — also runs on other aarch64 Linux stations.
# Layout under /data/shairport:
#   airplay.sh / airplayd / VERSION
#   ap2/   shairport-sync-ap2, nqptp, avahi-daemon, libs, config
#
# Alice stays on boot. During an AirPlay session we pause quasar so ALSA/buttons
# are free, then start it again when the stream ends.

D=/data/shairport
A=$D/ap2
BIN=$A/shairport-sync-ap2
CONF=$A/shairport.conf
DAEMON=$D/airplayd
export LD_LIBRARY_PATH=$A/libs
export HOME=$A
export TZ="$(cat $D/tz 2>/dev/null || echo MSK-6)"

sps_pids(){ ps | grep "[s]hairport-sync-ap2" | awk '{print $1}'; }
dmn_pids(){ ps | grep "[a]irplayd" | awk '{print $1}'; }
nq_pids(){  ps | grep "[n]qptp" | awk '{print $1}'; }
av_pids(){  ps | grep "[a]vahi-daemon" | awk '{print $1}'; }
yld_pids(){ ps | grep "[a]lice_yield" | awk '{print $1}'; }
meta_pids(){ ps | grep "[m]eta_watch" | awk '{print $1}'; }
q_pids(){   ps | grep "[s]ystem/vendor/quasar" | awk '{print $1}'; }

platform(){
  p=$(grep -o '"deviceType" *: *"[^"]*"' /system/vendor/quasar/quasar.cfg 2>/dev/null | head -1 | sed 's/.*: *"\(.*\)"/\1/')
  [ -z "$p" ] && p=$(tr '\0' '\n' < /proc/device-tree/compatible 2>/dev/null | sed -n 's/^yandex,\([a-z0-9_]*\).*/\1/p' | head -1)
  echo "$p"
}
model_name(){
  case "$1" in
    yandexstation)   echo "Я.Станция" ;;
    yandexstation_2) echo "Я.Станция Макс" ;;
    yandexmini)      echo "Я.Станция Мини" ;;
    yandexmini_2)    echo "Я.Станция Мини 2" ;;
    bergamot)        echo "Я.Станция Мини 3" ;;
    yandexmicro)     echo "Я.Станция Лайт" ;;
    plum)            echo "Я.Станция Лайт 2" ;;
    yandexmidi)      echo "Я.Станция 2" ;;
    cucumber)        echo "Я.Станция Миди" ;;
    chiron)          echo "Я.Станция Дуо Макс" ;;
    orion)           echo "Я.Станция 3" ;;
    mango)           echo "Я.Станция Стрит" ;;
    pickle)          echo "Я.Станция Мини 3 Про" ;;
    *)               echo "Я.Станция" ;;
  esac
}

ensure_avahi(){
  if ! grep -q "^avahi:" /etc/passwd || [ ! -f /etc/dbus-1/system.d/avahi-dbus.conf ]; then
    mount -o remount,rw /
    grep -q "^avahi:" /etc/passwd || echo "avahi:x:105:105:avahi:/var/run/avahi-daemon:/bin/false" >> /etc/passwd
    grep -q "^avahi:" /etc/group  || echo "avahi:x:105:" >> /etc/group
    mkdir -p /etc/dbus-1/system.d /var/run/avahi-daemon
    cp $A/avahi-dbus.conf /etc/dbus-1/system.d/avahi-dbus.conf
    mount -o remount,ro /
    kill -HUP $(pidof dbus-daemon | awk '{print $1}') 2>/dev/null; sleep 1
  fi
  mkdir -p /var/run/avahi-daemon
}

write_helpers(){
  cat > $A/quasar_stop.sh <<'EOS'
#!/bin/sh
for p in $(ps | grep "[q]uasar_launcher2" | awk '{print $1}'); do
  kill "$p" 2>/dev/null
done
for p in $(ps | grep "[s]ystem/vendor/quasar" | awk '{print $1}'); do
  kill "$p" 2>/dev/null
done
sleep 0.3
for p in $(ps | grep "[s]ystem/vendor/quasar" | awk '{print $1}'); do
  kill -9 "$p" 2>/dev/null
done
EOS
  cat > $A/quasar_start.sh <<'EOS'
#!/bin/sh
if ps w | grep -q "[q]uasar_launcher2"; then
  exit 0
fi
echo 1 > /proc/sys/vm/overcommit_memory
ulimit -n 10240
ulimit -s 1024
export LD_LIBRARY_PATH=/system/vendor/quasar:/system/vendor/quasar/libs:$LD_LIBRARY_PATH
export GST_PLUGIN_PATH=/system/vendor/quasar/libs
export PATH=/system/vendor/quasar:$PATH
export CA_CERTIFICATES=/system/vendor/quasar/ca-certificates.crt
cd /data/quasar || exit 1
nohup /system/vendor/quasar/quasar_launcher2 /system/vendor/quasar/launch.json \
  > launcher2.out 2>&1 &
EOS
  cat > $A/on_play.sh <<'EOS'
#!/bin/sh
echo 1 > /tmp/sps-playing
exit 0
EOS
  cat > $A/on_stop.sh <<'EOS'
#!/bin/sh
rm -f /tmp/sps-playing
exit 0
EOS
  chmod 755 $A/quasar_stop.sh $A/quasar_start.sh $A/on_play.sh $A/on_stop.sh
}

alsa_out(){
  plat=$(platform)
  if grep -q 'pcm.speakers' /etc/asound.conf 2>/dev/null; then
    echo speakers
    return
  fi
  case "$plat" in
    orion) echo sps_orion ;;
    *)     echo sps_out ;;
  esac
}

setup(){
  plat=$(platform)
  if [ -n "$1" ]; then NAME="$1"; echo "$1" > $D/name
  elif [ -s $D/name ]; then NAME=$(cat $D/name)
  else NAME=$(model_name "$plat"); fi

  mac=$(cat /sys/class/net/wlan0/address 2>/dev/null || cat /sys/class/net/eth0/address 2>/dev/null)
  hex=$(echo "$mac" | tr -d ':' | tr 'a-f' 'A-F')
  [ ${#hex} -ne 12 ] && hex=ACBA00000001

  OUT=$(alsa_out)
  write_helpers

  cat > $A/.asoundrc <<EOF
pcm.sps_out {
  type plug
  slave { pcm "hw:0,0"; rate 48000 }
}
pcm.sps_orion {
  type plug
  slave { pcm "hw:0,1"; rate 48000 }
}
pcm.airplay {
  type softvol
  slave.pcm "speakers"
  control {
    name "AirPlay"
    card 0
  }
  min_dB -70.0
  max_dB 0.0
}
EOF

  cat > $CONF <<EOF
general = {
  name = "$NAME";
  output_backend = "alsa";
  mdns_backend = "avahi";
  drift_tolerance_in_seconds = 0.005;
  ignore_volume_control = "yes";
  volume_range_db = 60;
  airplay_device_id = 0x${hex}L;
};
alsa = {
  output_device = "speakers";
  output_format = "S16";
  output_rate = 48000;
  use_mmap_if_available = "no";
  disable_standby_mode = "never";
};
sessioncontrol = {
  allow_session_interruption = "yes";
  wait_for_completion = "no";
  run_this_before_play_begins = "/data/shairport/ap2/on_play.sh";
  run_this_after_play_ends = "/data/shairport/ap2/on_stop.sh";
};
diagnostics = { log_verbosity = 0; };
metadata = { enabled = "yes"; include_cover_art = "no"; pipe_name = "/tmp/shairport-sync-ap2-metadata"; };
EOF
  echo "platform: ${plat:-unknown}   name: $NAME   device id: $hex   output: $OUT"
}

case "$1" in
  start)
    [ -s $CONF ] || setup
    trap '' PIPE
    ensure_avahi
    mkdir -p /tmp /var/run/avahi-daemon
    [ -z "$(nq_pids)" ] && start-stop-daemon -S -b -x "$A/nqptp"
    [ -z "$(av_pids)" ] && start-stop-daemon -S -b -x "$A/avahi-daemon" -- --no-drop-root --no-chroot -f "$A/avahi-daemon.conf"
    sleep 2
    [ -z "$(sps_pids)" ] && start-stop-daemon -S -b -x "$BIN" -- -c "$CONF"
    # airplayd: phone volume -> Master (same as Alice) + ring bar. No meta_watch (it steals the FIFO).
    [ -z "$(dmn_pids)" ] && start-stop-daemon -S -b -x "$DAEMON"
    sleep 1
    echo "shairport: $(sps_pids)   airplayd: $(dmn_pids)   nqptp: $(nq_pids)   avahi: $(av_pids)"
    ;;
  stop)
    for p in $(yld_pids) $(meta_pids) $(sps_pids) $(dmn_pids) $(nq_pids) $(av_pids); do kill $p 2>/dev/null; done; sleep 1
    for p in $(yld_pids) $(meta_pids) $(sps_pids) $(dmn_pids) $(nq_pids) $(av_pids); do kill -9 $p 2>/dev/null; done
    rm -f /tmp/sps-airplay-playing /tmp/sps-playing /tmp/sps-resume /tmp/sps-alice-hold
    echo "stopped"
    ;;
  restart) "$0" stop; "$0" start ;;
  setup)   setup "$2" ;;
  rename)  [ -n "$2" ] || { echo "usage: airplay.sh rename \"New name\""; exit 1; }
           setup "$2"; "$0" restart ;;
  killquasar)
    for p in $(q_pids); do kill $p 2>/dev/null; done; sleep 1
    for p in $(q_pids); do kill -9 $p 2>/dev/null; done
    echo "quasar killed (Alice off)"
    ;;
  status)
    plat=$(platform)
    echo "platform:       ${plat:-unknown} ($(model_name "$plat"))"
    echo "name:           $(grep -o 'name = "[^"]*"' $CONF 2>/dev/null | head -1)"
    echo "shairport-sync: $([ -n "$(sps_pids)" ] && echo "RUNNING $(sps_pids)" || echo stopped)"
    echo "nqptp:          $([ -n "$(nq_pids)" ] && echo "RUNNING $(nq_pids)" || echo stopped)"
    echo "avahi:          $([ -n "$(av_pids)" ] && echo "RUNNING $(av_pids)" || echo stopped)"
    echo "airplayd:       $([ -n "$(dmn_pids)" ] && echo "RUNNING $(dmn_pids)" || echo stopped)"
    echo "quasar/Alice:   $([ -n "$(q_pids)" ] && echo "running" || echo "killed")"
    netstat -tln 2>/dev/null | grep -q ":7000 " && echo "RTSP :7000 listening (AirPlay 2)" || echo ":7000 not listening"
    ;;
  vol) amixer -c0 sset Master "${2:-20}%" 2>/dev/null | grep -oE "\[[0-9]+%\]" | head -1 ;;
  install)
    mount -o remount,rw /
    cat > /etc/init.d/S99zairplay <<EOF
#!/bin/sh
case "\$1" in
  start) /data/shairport/airplay.sh start ;;
  stop)  /data/shairport/airplay.sh stop ;;
esac
EOF
    chmod +x /etc/init.d/S99zairplay
    mount -o remount,ro /
    echo "autostart installed: /etc/init.d/S99zairplay (Alice stays; paused only while AirPlay plays)"
    ;;
  uninstall)
    "$0" stop
    mount -o remount,rw /
    rm -f /etc/init.d/S99zairplay /etc/dbus-1/system.d/avahi-dbus.conf
    sed -i '/^avahi:/d' /etc/passwd /etc/group
    mount -o remount,ro /
    echo "autostart removed. Files stay in /data/shairport."
    ;;
  *)
    echo "usage: airplay.sh {start|stop|restart|status|setup [name]|rename \"name\"|killquasar|vol N|install|uninstall}"
    ;;
esac
