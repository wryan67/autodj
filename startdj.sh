#!/bin/ksh

# The BLSTR jack is the output. PipeWire calls it "Built-in Audio Pro"
# until WirePlumber renames it "BLSTR DAC". On startup that sink is 80%.
# A Bluetooth receiver (b06t3, KT-08UP, MEGABLAST) supersedes it
# while connected, and those are set to 81%. Do not start autodj on Dummy Output.
#
# ksh execs the last command, so this process becomes autodj.
# The volume watcher is a separate process and holds a lock.
# A pid check is not enough: after reboot the old pid can be reused.

bt_re='b06t3|kt-08up|megablast'
jack_re='built-in audio pro|blstr dac'
statedir=$HOME/.local/state/autodj
pidfile=$statedir/startdj-bt-volume.pid
lockfile=$statedir/startdj-bt-volume.lock
logfile=$statedir/startdj-bt-volume.log

sink_ids() {
  wpctl status | awk -v pat="$1" '
    $0 ~ /Sinks:/ { sinks = 1; next }
    $0 ~ /Sink endpoints:/ { sinks = 0 }
    sinks {
      line = tolower($0)
      if (line ~ /dummy output/) next
      if (pat != "" && line !~ pat) next
      for (i = 1; i <= NF; i++)
        if ($i ~ /^[0-9]+\.$/) { gsub(/\./, "", $i); print $i }
    }
  '
}

set_sink_volume() {
  ids=$(sink_ids "$1")
  for id in $ids; do
    wpctl set-mute "$id" 0
    wpctl set-volume "$id" "$2"
  done
}

set_bt_volume() {
  set_sink_volume "$bt_re" 81%
}

set_jack_volume() {
  set_sink_volume "$jack_re" 80%
}

# The receiver comes up muted, and sometimes reports 0 just after connect.
watch_bt_volume() {
  trap '' HUP
  echo $$ > "$pidfile"
  echo "$(date '+%F %T') watching pid $$"
  seen=" $(sink_ids "$bt_re") "
  while true; do
    for id in $(sink_ids "$bt_re"); do
      case "$seen" in
        *" $id "*) ;;
        *)
          sleep 5
          wpctl set-mute "$id" 0
          wpctl set-volume "$id" 81%
          sleep 1
          wpctl set-mute "$id" 0
          wpctl set-volume "$id" 81%
          echo "$(date '+%F %T') set sink $id to 81%"
          seen="$seen $id "
          ;;
      esac
    done
    sleep 2
  done
}

mkdir -p "$statedir"

if [ "$1" = --watch ]; then
  watch_bt_volume
  exit 0
fi

# flock holds the lock until the watcher exits. A second start does nothing.
if flock -n "$lockfile" true; then
  flock -n "$lockfile" -c "/home/pi/projects/autodj/startdj.sh --watch" >>"$logfile" 2>&1 &
fi

# The jack can appear after PipeWire. Dummy Output is not enough.
while [ -z "$(sink_ids "$jack_re")" ]; do
  sleep 1
done

# WirePlumber restores the last sink volume as the node appears.
# Set 80% after that, then once more in case the restore lands late.
set_jack_volume
sleep 1
set_jack_volume

if [ -n "$(sink_ids "$bt_re")" ]; then
  sleep 5
  set_bt_volume
  sleep 1
  set_bt_volume
fi

exec /home/pi/bin/autodj > /home/pi/logs/autodj.log 2>&1
