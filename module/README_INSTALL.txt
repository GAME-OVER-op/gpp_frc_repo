cleanfg Magisk module package

ВАЖНО:
- Этот zip имеет структуру Magisk-модуля.
- Для полностью рабочего установочного zip внутри должен быть zygisk/arm64-v8a.so.
- В текущей песочнице нет Android NDK/cmake, поэтому бинарник здесь не собран.
- Собери проект через build.sh на машине с Android NDK — он положит .so в module/zygisk/arm64-v8a.so и создаст cleanfg-magisk.zip.

Настройка:
1) Открой /data/adb/modules/cleanfg/cleanfg.prop после установки.
2) Замени target_packages=com.example.game на пакет игры.
3) Для всех приложений можно target_packages=*, но это рискованно.
4) Перезагрузи устройство.

Рекомендуется НЕ включать для всех приложений сразу.
