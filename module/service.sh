#!/system/bin/sh
# cleanfg service.sh -- runs as root at late_start.
#
# The FRC engine (/system/lib64/libgppvppgfrcplussession.so) depends on the
# vendor VPP HAL (vendor.qti.hardware.vpp-V1-ndk.so + libvpp*.so in /vendor/lib64).
# Inside the game process (untrusted_app*) SELinux forbids executing files labeled
# vendor_file, so dlopen() of those libs fails as "not found" although the files
# exist (/system libs load fine -- only /vendor is blocked). We relax SELinux
# globally with magiskpolicy --live (ignores neverallow) so the app can load the
# vendor .so files and reach the vpp HAL binder service -- the same privilege a
# root shell already has, which is why the standalone harness worked but the
# in-app module did not.

MP=""
for c in /data/adb/magisk/magiskpolicy /data/adb/ksu/bin/magiskpolicy \
         /data/adb/ap/bin/magiskpolicy magiskpolicy; do
  if command -v "$c" >/dev/null 2>&1 || [ -x "$c" ]; then MP="$c"; break; fi
done
[ -z "$MP" ] && MP=magiskpolicy
A() { "$MP" --live "$@" >/dev/null 2>&1; }

for DOM in untrusted_app untrusted_app_all untrusted_app_25 untrusted_app_27 \
           untrusted_app_29 untrusted_app_30 untrusted_app_32 untrusted_app_34; do
  # (a) load vendor .so from the app process
  A "allow $DOM vendor_file file { open read getattr execute execute_no_trans map ioctl lock }"
  A "allow $DOM vendor_file dir { search open read getattr }"
  A "allow $DOM vendor_file lnk_file { read getattr open }"
  A "allow $DOM same_process_hal_file file { open read getattr execute execute_no_trans map }"
  # (b) reach the vpp HAL binder/hwbinder service
  A "allow $DOM hal_vpp_hwservice hwservice_manager find"
  A "allow $DOM vendor_hwservice hwservice_manager find"
  A "allow $DOM hal_vpp_default binder { call transfer }"
  A "allow $DOM vendor_vppservice binder { call transfer }"
  A "allow $DOM vndbinder_device chr_file { open read write ioctl map getattr }"
  # (c) gralloc / dma-buf heaps for shared FRC graphic buffers
  A "allow $DOM gpu_device chr_file { open read write ioctl map getattr }"
  A "allow $DOM dmabuf_system_heap_device chr_file { open read write ioctl map getattr }"
  A "allow $DOM ion_device chr_file { open read write ioctl map getattr }"
done

log -t cleanfg "service.sh: SELinux relax for vendor VPP/FRC applied (mp=$MP)"
