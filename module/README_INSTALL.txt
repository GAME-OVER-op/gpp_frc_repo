gpp_frc_repo Magisk module

Install the generated gpp_frc_repo-magisk.zip through Magisk/KernelSU with Zygisk enabled.

Configuration after install:
  /data/adb/modules/gpp_frc_repo/gpp_frc_repo.prop

Only one setting is required:
  target_packages=com.example.game,com.example.secondgame

Use one package first while testing. A wildcard is supported but not recommended:
  target_packages=*
