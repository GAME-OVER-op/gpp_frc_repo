#!/system/bin/sh
# run.sh - zapusk gpp_frc_test (Phase 5) iz toj zhe papki, gde lezhit binar.
# Ispolzovanie:
#   sh run.sh                    # obychnyj zapusk (sam podnimet root cherez su), 150 kadrov
#   sh run.sh 300                # zadat chislo kadrov
#   sh run.sh 150 --no-su        # bez su (esli uzhe root)
#   sh run.sh --permissive       # TEST: na vremya zapuska setenforce 0 (proverka SELinux),
#                                #       posle zapuska SELinux vozvraschaetsya kak byl

BIN="gpp_frc_test"

# --- razobrat argumenty ---
FRAMES=""
NOSU=0
PERMISSIVE=0
for a in "$@"; do
  case "$a" in
    --no-su)      NOSU=1 ;;
    --permissive) PERMISSIVE=1 ;;
    *[0-9]*)      FRAMES="$a" ;;
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
  EXTRA=""
  [ "$PERMISSIVE" = "1" ] && EXTRA="--permissive"
  exec su -c "sh '$DIR/$(basename "$0")' $FRAMES $EXTRA --no-su"
fi

LOG="$DIR/gpp_frc_run_$(date +%Y%m%d_%H%M%S).log"
LIBS="/data/adb/modules/gpp_frc_framegen/system/lib64:/system/lib64:/system/system/lib64:/vendor/lib64"

chmod 755 "$BINPATH" 2>/dev/null

echo "=================================================="
echo " bin    : $BINPATH"
echo " log    : $LOG"
echo " frames : ${FRAMES:-150 (po umolchaniyu)}"
echo " permissive : $PERMISSIVE"
echo " LD_LIBRARY_PATH : $LIBS"
echo "=================================================="

# --- SELinux: zapomnit i (opcionalno) vremenno snyat enforcing ---
SE_WAS=$(getenforce 2>/dev/null)
if [ "$PERMISSIVE" = "1" ]; then
  echo "[*] SELinux bylo: $SE_WAS -> stavlyu Permissive na vremya testa"
  setenforce 0 2>/dev/null
  echo "[*] SELinux teper: $(getenforce 2>/dev/null)"
fi

# --- ochistit kernel audit, sbrosit i nachat zahvat POLNOGO logcat v fone ---
dmesg -c >/dev/null 2>&1
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

# --- vernut SELinux kak bylo ---
if [ "$PERMISSIVE" = "1" ]; then
  case "$SE_WAS" in
    Enforcing) setenforce 1 2>/dev/null; echo "[*] SELinux vozvraschyon v Enforcing" ;;
    *)         echo "[*] SELinux ostavlen kak byl ($SE_WAS)" ;;
  esac
fi

# --- kernel audit (avc denied, kotorye logcat ne pokazyvaet) ---
dmesg 2>/dev/null | grep -iE 'avc|selinux|vpp|gpp' > "$LOG.dmesg" 2>/dev/null

# --- vydernut interesnoe iz polnogo logcat ryadom ---
grep -iE 'GPP|FRC|MotionEngine|VTxr|BufferQueue|Vpp|SurfaceFlinger|interpolat|Non-Interpolated|avc|denied|configstore|hexlp' "$LOG.full" > "$LOG.gpp" 2>/dev/null

echo
echo "[+] gotovo."
echo "[+] stdout binarya : $LOG"
echo "[+] polnyj logcat  : $LOG.full"
echo "[+] otfiltrovano   : $LOG.gpp"
echo "[+] kernel audit   : $LOG.dmesg"
echo "[+] skin' mne $LOG, $LOG.gpp i $LOG.dmesg"
