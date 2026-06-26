# lybfghook static analysis starter

Этот репозиторий — минимальная заготовка для загрузки `lybfghook` Zygisk `.so` в GitHub и последующего анализа в Ghidra/Rizin/IDA.

## Что внутри

- `binaries/arm64-v8a.so` — AArch64 Zygisk module, stripped ELF, SONAME `liblybfghook.so`.
- `binaries/armeabi-v7a.so` — ARMv7 Zygisk module, stripped ELF.
- `notes/module.prop`, `notes/lyb.prop` — свойства Magisk/Zygisk-модуля.
- `scripts/basic_static.sh` — быстрый сбор строк, импортов, секций, зависимостей.
- `scripts/ghidra_headless.sh` — пример запуска headless Ghidra и экспорта decompile/C.

## Предварительные выводы

Модуль содержит стандартные Zygisk entrypoints:

- `zygisk_module_entry`
- `zygisk_companion_entry`

Зависимости ARM64:

- `liblog.so`
- `libGLESv3.so`
- `libGLESv2.so`
- `libEGL.so`
- `libm.so`
- `libdl.so`
- `libc.so`

В бинарнике статически видны OpenSSL/cpp-httplib/сетевые символы: `socket`, `connect`, `sendto`, `recvfrom`, `getaddrinfo`, HTTP/HTTPS строки, proxy env strings. Явных доменов аналитики при простом `strings` почти нет; скорее всего сетевой код либо общий bundled TLS/HTTP стек, либо адреса/пути собраны/зашифрованы/получаются динамически.

Лицензия упоминает `Zygisk-UnityHook` от Rikka, поэтому начинать чистую реализацию логично с минимального Zygisk + Unity/GL hook, без embedded HTTP/TLS.

## Рекомендуемый clean-room план

1. Не переносить код из `.so` напрямую, только поведение/архитектурные идеи.
2. Стартовать от официального Zygisk API / минимального шаблона.
3. Оставить только:
   - фильтр целевого process/package;
   - `preAppSpecialize` / `postAppSpecialize`;
   - `dlopen`/`dlsym` detection нужной Unity/GL библиотеки;
   - локальный hook нужных функций;
   - логирование через `__android_log_print` под debug flag.
4. Убрать полностью:
   - OpenSSL/curl/cpp-httplib;
   - socket/connect/send/recv;
   - любые remote config, license check, telemetry, analytics;
   - фоновые сетевые threads.

## Ghidra

Установите Ghidra локально и запустите:

```bash
GHIDRA_INSTALL_DIR=/path/to/ghidra ./scripts/ghidra_headless.sh
```

После импорта ищите:

- exports: `zygisk_module_entry`, `zygisk_companion_entry`;
- references to `dlopen`, `dlsym`, `pthread_create`;
- calls to `connect`, `sendto`, `recvfrom`, `getaddrinfo`;
- references to `libGLESv2.so`, `libGLESv3.so`, `libEGL.so`;
- init arrays / constructors.


## GitHub Actions workflow

В репозитории добавлен workflow `.github/workflows/analyze.yml`.

Он запускается при каждом `push`, `pull_request` и вручную через `workflow_dispatch`.

Что делает workflow:

1. Устанавливает базовые ELF-инструменты.
2. Запускает `scripts/basic_static.sh`.
3. Скачивает Ghidra.
4. Запускает headless Ghidra через `scripts/ghidra_full_analysis.sh`.
5. Экспортирует результаты в:
   - `analysis_out/`
   - `ghidra_out/`
6. Загружает ZIP с результатами как GitHub Actions artifact.

Важно: Ghidra ZIP URL в workflow привязан к конкретной версии. Если GitHub/NSA изменит имя релиза, поменяйте `GHIDRA_VERSION` и URL в `.github/workflows/analyze.yml`.
