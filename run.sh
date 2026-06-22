#!/system/bin/sh
# run.sh - zapusk gpp_frc_test (Phase 2) iz toj zhe papki, gde lezhit binar.
# Ispolzovanie:
#   sh run.sh             # obychnyj zapusk (sam podnimet root cherez su), 150 kadrov
#   sh run.sh 300         # zadat chislo kadrov
#   sh run.sh 150 --no-su  # bez su (esli uzhe root)

BIN="gpp_frc_test"

# --- razobrat argumenty: chislo kadrov i/ili --no-su ---
FRAMES=""
NOSU=0
for a in "$@"; do
  case "$a" in
    --no-su) NOSU=1 ;;
    *[0-9]*) FRAMES="$a" ;;
  esac
done

# --- papka skripta (ryadom lezhit binar) ---
DIR=$(cd "$(dirname "$0")" 2>/dev/null && pwd)
[ -z "$DIR" ] && DIR=$(pwd)
BINPATH="$DIR/$BIN"

if [ ! -f "$BINPATH" ]; then
  echo "[!] ne nashyol $BIN ryadom so skriptom ($DIR)"
  exit 1
fi

# --- esli ne root i ne prosili --no-su - perezapustitsya cherez su ---
if [ "$NOSU" != "1" ] && [ "$(id -u 2>/dev/null)" != "0" ]; then
  echo "[*] podnimayu root cherez su ..."
  exec su -c "sh '$DIR/$(basename "$0")' $FRAMES --no-su"
fi

LOG="$DIR/gpp_frc_run_$(date +%Y%m%d_%H%M%S).log"
LIBS="/data/adb/modules/gpp_frc_framegen/system/lib64:/system/lib64:/system/system/lib64:/vendor/lib64"

chmod 755 "$BINPATH" 2>/dev/null

echo "=================================================="
echo " bin    : $BINPATH"
echo " log    : $LOG"
echo " frames : ${FRAMES:-150 (po umolchaniyu)}"
echo " LD_LIBRARY_PATH : $LIBS"
echo "=================================================="

# --- sbrosit i nachat zahvat POLNOGO logcat v fone (lovim FRC/BufferQueue/SurfaceFlinger) ---
logcat -c 2>/dev/null
logcat -v time > "$LOG.full" 2>/dev/null &
LOGPID=$!
sleep 1

# --- zapusk binarya (stdout tozhe v log) ---
echo "----- zapusk $BIN $FRAMES -----"
LD_LIBRARY_PATH="$LIBS" "$BINPATH" $FRAMES 2>&1 | tee "$LOG"
RC=$?
echo "----- binar zavershyon, kod $RC -----"

# --- dat logam dobratsya i ostanovit zahvat ---
sleep 2
kill "$LOGPID" 2>/dev/null

# --- vydernut interesnoe iz polnogo logcat ryadom ---
grep -iE 'GPP|FRC|MotionEngine|VTxr|BufferQueue|Vpp|SurfaceFlinger|interpolat|Non-Interpolated' "$LOG.full" > "$LOG.gpp" 2>/dev/null

echo
echo "[+] gotovo."
echo "[+] stdout binarya : $LOG"
echo "[+] polnyj logcat  : $LOG.full"
echo "[+] otfiltrovano   : $LOG.gpp"
echo "[+] skinь mne $LOG i $LOG.gpp (a esli malo - i .full)."
