#!/system/bin/sh
# Magisk/KernelSU install script for cleanfg.
SKIPUNZIP=0

ui_print "- cleanfg: clean Zygisk frame-generation hook"
ui_print "- no network, no analytics"

# Положить собранную .so в zygisk/<abi>.so перед упаковкой.
if [ -d "$MODPATH/zygisk" ]; then
  ui_print "- zygisk libraries present"
else
  ui_print "! zygisk/ directory missing — соберите .so и положите zygisk/arm64-v8a.so"
fi

set_perm_recursive "$MODPATH" 0 0 0755 0644
