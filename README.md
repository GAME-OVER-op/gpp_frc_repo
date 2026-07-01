# gpp_frc_repo

`gpp_frc_repo` — Android Zygisk/Magisk модуль для генерации промежуточных кадров в выбранных приложениях. Проект работает на стороне процесса приложения: перехватывает вывод через Vulkan или GLES, автоматически выбирает активный графический backend и добавляет один сгенерированный кадр между реальными кадрами приложения.

Цель проекта — более плавная картинка при минимальной настройке и без тяжёлой нагрузки на GPU.

## Возможности

- Автоматический выбор backend: Vulkan или GLES.
- Единая системная логика генерации кадров без ручного выбора режима.
- Адаптивный GPU blend для сглаживания движения:
  - реакция на изменения яркости и цвета;
  - защита мелких статичных деталей;
  - мягкое сглаживание в зонах движения;
  - ограничение цвета по соседним пикселям, чтобы уменьшать пятна и разрывы.
- GLES путь с безопасным сохранением/восстановлением GL-состояния.
- GLES захват через `glBlitFramebuffer` с fallback на `glCopyTexSubImage2D`.
- Запрос высокой частоты экрана через Android frame-rate API, если устройство это поддерживает.
- Минимальный конфиг: пользователь указывает только список приложений.

## Конфигурация

Файл после установки:

```ini
/data/adb/modules/gpp_frc_repo/gpp_frc_repo.prop
```

Единственный пользовательский параметр:

```ini
target_packages=com.example.game
```

Можно указать несколько пакетов через запятую:

```ini
target_packages=com.example.game,com.example.secondgame
```

Wildcard поддерживается, но для тестов лучше включать модуль только для одного приложения:

```ini
target_packages=*
```

Все остальные параметры зашиты в коде и подбираются автоматически.

## Сборка через GitHub Actions

1. Загрузите проект в репозиторий GitHub.
2. Откройте вкладку **Actions**.
3. Запустите workflow **Build gpp_frc_repo Magisk module**.
4. После завершения скачайте artifact **gpp_frc_repo-magisk**.
5. Внутри artifact будет готовый файл `gpp_frc_repo-magisk.zip`.

## Локальная сборка

Требования:

- Android NDK r26+;
- CMake;
- Ninja;
- `glslangValidator`;
- доступ к интернету для загрузки Dobby и Zygisk header.

```bash
export ANDROID_NDK=/path/to/android-ndk
./build.sh
```

Результат:

```text
gpp_frc_repo-magisk.zip
```

## Установка

1. Включите Zygisk в Magisk/KernelSU/APatch-совместимой среде.
2. Установите `gpp_frc_repo-magisk.zip` как обычный модуль.
3. Перезагрузите устройство.
4. Отредактируйте `/data/adb/modules/gpp_frc_repo/gpp_frc_repo.prop`.
5. Запустите целевое приложение заново.

Логи:

```bash
logcat -s gpp_frc_repo
```

## Ограничения

- Это низкоуровневый графический hook, поэтому совместимость зависит от игры, GPU, драйвера и Android-сборки.
- Некоторые приложения могут использовать нестандартный вывод, защищённые поверхности или собственный frame pacing.
- Начинайте тесты с одного пакета в `target_packages`.
- Если приложение нестабильно, удалите пакет из конфига или отключите модуль.

## Структура проекта

```text
gpp_frc_repo/
├── .github/workflows/build.yml   # CI сборка Magisk zip
├── build.sh                      # локальная сборка
├── jni/                          # native Zygisk код
│   ├── main.cpp                  # вход Zygisk и чтение конфига
│   ├── hook_egl.cpp              # GLES/EGL present hook
│   ├── hook_vulkan.cpp           # Vulkan present hook
│   ├── frame_gen.cpp             # GLES генерация кадров
│   ├── blend.comp                # Vulkan compute shader
│   ├── backend_select.*          # автоматический выбор backend
│   ├── display_rate.*            # запрос частоты экрана
│   └── config.*                  # минимальный конфиг
└── module/                       # файлы Magisk модуля
    ├── module.prop
    ├── gpp_frc_repo.prop
    ├── customize.sh
    └── service.sh
```
