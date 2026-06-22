#!/system/bin/sh
# Phase 19a probe runner. Zapuskat ot root (su). Bezopasno: bez ocheredi buferov.
set -u
TS=$(date +%Y%m%d_%H%M%S)
DIR=/data/adb/19
mkdir -p "$DIR"
BIN="$DIR/vpp_probe"
LOG="$DIR/vpp_probe_$TS.log"

if [ ! -x "$BIN" ]; then echo "net $BIN - skopiruj tuda vpp_probe i chmod 755"; exit 1; fi

# SELinux v permissive (kak v proshlyh fazah)
getenforce > "$LOG" 2>&1
setenforce 0 2>>"$LOG" || true

# chistim logcat, zapuskaem so vsemi vendor-putyami
logcat -c 2>/dev/null || true
export LD_LIBRARY_PATH=/vendor/lib64:/system/lib64:/system/system/lib64:/odm/lib64
echo "LD_LIBRARY_PATH=$LD_LIBRARY_PATH" >> "$LOG"
echo "--- stdout/stderr ---" >> "$LOG"
"$BIN" >> "$LOG" 2>&1
RC=$?
echo "exit=$RC" >> "$LOG"

# sobiraem logcat (nashi + vpp/frc tegi)
sleep 1
echo "--- logcat VPP_PROBE ---" >> "$LOG"
logcat -d -s VPP_PROBE >> "$LOG" 2>/dev/null
echo "--- logcat vpp/frc/hcp ---" >> "$LOG"
logcat -d 2>/dev/null | grep -iE 'vpp|frc|hcp|hqv|motionengine|venus|extradata' | tail -300 >> "$LOG"

echo "gotovo -> $LOG"
cat "$LOG"
