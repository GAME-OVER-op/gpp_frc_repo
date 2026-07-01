#!/system/bin/sh
SKIPUNZIP=0

ui_print "- gpp_frc_repo: automatic Vulkan/GLES frame generation"
ui_print "- configure apps in gpp_frc_repo.prop"
ui_print "- no network, no analytics"

if [ -d "$MODPATH/zygisk" ]; then
  ui_print "- Zygisk libraries present"
else
  ui_print "! zygisk/ directory missing"
fi

set_perm_recursive "$MODPATH" 0 0 0755 0644
