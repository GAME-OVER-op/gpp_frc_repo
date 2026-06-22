# gpp-frc-test

Стенд для запуска фреймген-движка RedMagic OS (G-FRC+ / MotionEngine Vulkan) на кастоме.
Сборка идёт в **GitHub Actions** с Android NDK — локальный компилятор не нужен.

## Что делает Phase 1

`gpp_frc_test` в реальном процессе на телефоне:
1. загружает `libgppvppgfrcplussession.so` (и её зависимости) через `dlopen`;
2. разрешает ключевые символы: `CreateFactory`, `GPPSession::GPPSession()`,
   `GPPSession::connect(...)`, `GPPSession::createQueue(...)`, `GPPComponent::init/config`;
3. безопасно вызывает `CreateFactory()` и печатает результат.

Цель этого этапа — подтвердить, что движок достижим и сессию можно создать
без демона и без игры. Phase 2 добавит подачу кадров (BufferQueue) и захват выхода.

## Как собрать

1. Создай новый репозиторий на GitHub и залей туда содержимое этого архива:
   ```bash
   git init && git add . && git commit -m "phase 1"
   git branch -M main
   git remote add origin git@github.com:<ты>/gpp-frc-test.git
   git push -u origin main
   ```
2. Открой вкладку **Actions** — сборка запустится автоматически.
3. Скачай артефакт **gpp_frc_test-arm64-v8a**.

## Как запустить на устройстве

```bash
adb push gpp_frc_test /data/local/tmp/
adb shell
su
chmod 755 /data/local/tmp/gpp_frc_test
LD_LIBRARY_PATH=/data/adb/modules/gpp_frc_framegen/system/lib64:/system/lib64:/vendor/lib64 \
  /data/local/tmp/gpp_frc_test --call-factory
```

Параллельно смотри логи:
```bash
adb logcat -s GPP_FRC_TEST GPP-Session GPPSession VTxr MotionEngine
```

Скинь весь вывод — по нему я соберу Phase 2 (подача тестовых кадров + захват результата).

## Структура
```
.github/workflows/build.yml   — CI: NDK → arm64-v8a → артефакт
app/CMakeLists.txt            — сборка
app/src/main.cpp              — Phase 1 проба
```
