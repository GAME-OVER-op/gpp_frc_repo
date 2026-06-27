# cleanfg — clean-room Zygisk frame-generation hook

Чистая реализация Zygisk-модуля генерации кадров (frame generation) **без какой-либо
сетевой активности, аналитики, лицензий и удалённого конфига**.

Проект — clean-room: код написан с нуля по наблюдаемой архитектуре оригинального
`lybfghook`, без копирования его бинарного кода.

## Что установлено про оригинал (факты из статического анализа)

- Это Zygisk-модуль: экспортирует `zygisk_module_entry` и `zygisk_companion_entry`.
- `liblybfghook.so` **NEEDED-линкуется** с `libEGL.so`, `libGLESv2.so`, `libGLESv3.so`,
  `liblog.so`, `libm.so`, `libdl.so`, `libc.so` — то есть принудительно затягивает
  графические библиотеки в адресное пространство процесса игры.
- В бинарнике **нет** импортов `gl*`/`egl*`/`vk*` через PLT и нет их имён в открытом виде —
  значит цели находятся и патчатся в рантайме, а не вызываются напрямую.
- Присутствует C++ класс инлайн-хук-движка `FunctionInlineHookRouting` плюс типичные
  примитивы инлайн-хукинга: `dl_iterate_phdr`, `/proc/self/maps`, `mprotect`, `mmap`,
  `munmap`, `madvise`, `getauxval`, `sigaction`, `__register_atfork`.
- Конфиг — файл `lyb.prop` рядом с модулем: `fgappver=7`, `vulkan=2`
  (профиль приложения + режим Vulkan).

## Вывод о механизме хука

`fg hook` = **frame generation**: модуль перехватывает функцию предъявления кадра
(present) графического API и между двумя «настоящими» кадрами вставляет
сгенерированный промежуточный кадр, повышая воспринимаемый FPS.

Два пути предъявления кадра, которые надо хукать:

- **OpenGL ES / EGL:** `eglSwapBuffers` (и `eglSwapInterval`).
- **Vulkan:** `vkQueuePresentKHR` (включается при `vulkan=...` в конфиге).

Механизм инлайн-хука (как в оригинале):

1. Zygisk инжектит `liblybfghook.so` в процесс целевой игры через
   `postAppSpecialize`.
2. Модуль ждёт/находит загруженные `libEGL.so` / `libvulkan.so` (через
   `dl_iterate_phdr` или `dlopen`+`dlsym`).
3. Разрешает адрес present-функции и ставит **inline hook** (trampoline): меняет
   защиту страницы `mprotect`, переписывает пролог на переход в свой обработчик,
   сохраняет оригинальные инструкции в trampoline, чистит I-cache.
4. В обработчике делает frame pacing + генерацию промежуточного кадра, затем
   вызывает оригинал.

## Чем этот проект отличается

- **Ноль сети.** Нет OpenSSL/curl/cpp-httplib, нет `socket/connect/send/recv`,
  нет лицензий и телеметрии.
- Инлайн-хукер — внешняя проверенная библиотека (ShadowHook или Dobby), а не
  переписанный из бинарника движок.
- Конфиг — локальный `cleanfg.prop`, читается с диска, наружу ничего не уходит.

## Структура

```
cleanfg/
├── README.md
├── ARCHITECTURE.md        # подробный разбор механизма и плана реализации
├── module/
│   ├── module.prop
│   ├── customize.sh
│   └── cleanfg.prop       # локальный конфиг (аналог lyb.prop)
└── jni/
    ├── CMakeLists.txt
    ├── zygisk.hpp         # официальный Zygisk C++ API header (заглушка-ссылка)
    ├── main.cpp           # Zygisk entrypoint + companion
    ├── hook_egl.cpp       # хук eglSwapBuffers (GLES путь)
    ├── hook_vulkan.cpp    # хук vkQueuePresentKHR (Vulkan путь)
    ├── frame_gen.cpp      # логика frame pacing / генерации кадра (TODO-ядро)
    ├── frame_gen.h
    ├── config.cpp         # парсер cleanfg.prop
    ├── config.h
    └── log.h
```

## Что ещё нужно для 1:1 функционала

Точная **математика генерации кадра** (warp/interpolation/extrapolation, работа с
motion vectors, depth, reprojection) — единственное, что нельзя достать из stripped
бинарника через `strings`/`readelf`. Её надо получить из декомпиляции через твой
GitHub-workflow: после фикса `ghidra_out` смотри `*.decompiled.c`, функции рядом с
perpresent-хуком и `glReadPixels`/`glBlitFramebuffer`/текстурными операциями. Перенос —
только на уровне алгоритма (clean-room), не байт-в-байт.

## Сборка

```bash
cd jni
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26
cmake --build build
```

Далее упаковать `module/` + собранную `.so` в zip и прошить через Magisk/KernelSU
(Zygisk включён).


---

## ⚙️ Автоматическая сборка (GitHub Actions)

Этот архив — полный исходник. Сам модуль (`.so`) компилируется автоматически в GitHub Actions — локально NDK ставить не нужно.

### Как получить готовый `cleanfg-magisk.zip`

1. Создайте новый репозиторий на GitHub (Public или Private — любой).
2. Загрузите туда всё содержимое этого архива (вместе с папкой `.github/`).
   - Через сайт: Add file → Upload files, либо `git push`.
3. Откройте вкладку **Actions** — сборка «Build cleanfg Magisk module» запустится сама. Если нет — нажмите **Run workflow**.
4. По окончании (~5–10 мин) откройте запуск → раздел **Artifacts** → скачайте **cleanfg-magisk**.
   - Внутри будет `cleanfg-magisk.zip` — это и есть файл для прошивки в Magisk/KernelSU.
5. (Необязательно) Создайте git-тег (`git tag v0.1.0 && git push --tags`) — тогда zip автоматически прикрепится к GitHub Release.

### Что делает workflow

- Ставит Android NDK r26 + CMake + Ninja.
- Скачивает официальный `zygisk.hpp` (с проверкой) — заменяет встроенную заглушку.
- Компилирует `.so` под `arm64-v8a` и `armeabi-v7a`, влинковывая Dobby статически (один самодостаточный файл).
- Кладёт их в `module/zygisk/<abi>.so` и упаковывает весь `module/` в `cleanfg-magisk.zip`.

### На какие приложения действует

Модуль АКТИВЕН ТОЛЬКО для пакетов из `target_packages` в `cleanfg.prop`. Это
список через запятую — можно перечислить сколько угодно игр И видео-приложений:

```
target_packages=com.miHoYo.GenshinImpact,com.miHoYo.*,org.videolan.vlc,is.xyz.mpv
```

Поддерживаются шаблоны (glob): `*` — любая последовательность символов, `?` —
один символ. Примеры: `com.miHoYo.*` (всё семейство miHoYo), `*.mpv`, `*vlc*`.
Чтобы включить для всех приложений — `target_packages=*` (не рекомендуется).

`mode=auto` (по умолчанию) ставит И EGL/GLES, И Vulkan хуки и **сам определяет**,
какую технику отображения использует каждое приложение: первый сработавший
present-вызов «закрепляет» движок за процессом, второй путь становится
прозрачным. Так один и тот же модуль работает и на GLES-, и на Vulkan-играх, и
на видеоплеерах — без ручного выбора режима под каждое приложение.

После установки конфиг лежит в `/data/adb/modules/cleanfg/cleanfg.prop`.

> Честное предупреждение: янтерполяция кадров через перехват EGL/Vulkan — экспериментальная техника. Работает не во всех играх и не на всех GPU; начните с одного тестового приложения и смотрите `logcat -s cleanfg`.
