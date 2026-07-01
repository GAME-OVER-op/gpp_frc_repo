# Architecture

`gpp_frc_repo` состоит из Magisk/Zygisk части и native графического runtime внутри целевого процесса приложения.

## 1. Zygisk entrypoint

`jni/main.cpp` загружается Zygisk в процессы приложений. На этапе `preAppSpecialize` модуль читает конфиг из:

```text
/data/adb/modules/gpp_frc_repo/gpp_frc_repo.prop
```

Так как обычный процесс приложения не имеет прямого доступа к `/data/adb`, чтение выполняется через fd каталога модуля или companion-процесс. После чтения модуль сравнивает имя процесса с `target_packages`. Если пакет не подходит, библиотека выгружается и ничего не перехватывает.

## 2. Минимальная конфигурация

Публично доступен только список пакетов:

```ini
target_packages=com.example.game
```

Внутренние параметры зафиксированы в коде:

- backend: auto;
- generation multiplier: 2x;
- frame-rate request: enabled;
- GLES swap interval unlock: enabled;
- Vulkan generated-present bridge: enabled;
- adaptive blend constants: internal defaults.

Это уменьшает риск плохих пользовательских настроек и делает поведение одинаковым на всех тестах.

## 3. Автоматический выбор backend

`backend_select.cpp` управляет выбором runtime backend:

- если приложение реально доходит до `vkQueuePresentKHR`, активируется Vulkan;
- если используется `eglSwapBuffers`, активируется GLES;
- backend фиксируется после первого подтверждённого present-пути;
- в auto mode оба hook устанавливаются заранее, но работает только выбранный путь.

## 4. Vulkan path

`hook_vulkan.cpp` перехватывает ключевые Vulkan функции через `vkGetInstanceProcAddr` и `vkGetDeviceProcAddr`:

- `vkCreateDevice`;
- `vkCreateAndroidSurfaceKHR`;
- `vkCreateSwapchainKHR`;
- `vkQueuePresentKHR`.

При создании swapchain модуль добавляет transfer usage и увеличивает количество images, чтобы был запас для промежуточного кадра. На present:

1. текущий swapchain image копируется во внутренний `gCur`;
2. compute shader `blend.comp` генерирует midpoint между `gPrev` и `gCur`;
3. midpoint копируется в свободный swapchain image и презентуется;
4. затем презентуется настоящий кадр приложения;
5. `gCur` становится новой историей `gPrev`.

Вся генерация выполняется на GPU без CPU readback.

## 5. GLES path

`hook_egl.cpp` перехватывает `eglSwapBuffers`. GLES runtime находится в `frame_gen.cpp`:

1. перед present сохраняется текущий framebuffer в texture history;
2. генерируется промежуточный fullscreen кадр shader-проходом;
3. сгенерированный кадр презентуется;
4. текущий реальный кадр перерисовывается и презентуется обычным путём;
5. GL-состояние приложения восстанавливается.

Для совместимости захват кадра сначала использует `glBlitFramebuffer`; если драйвер не позволяет blit из default framebuffer, используется `glCopyTexSubImage2D` fallback.

## 6. Adaptive blend

Генерация не является простым `mix(prev, cur)`. Shader использует несколько дешёвых сигналов:

- luma/chroma difference для определения движения;
- reactive mask для резких изменений;
- composition/instability mask для сложных зон;
- directional blur в динамике;
- static detail protection для мелких стабильных элементов;
- neighborhood color bounds, чтобы итоговый цвет не уходил в артефакты.

Такой подход сохраняет плавность, но старается не замыливать интерфейс и мелкие детали сильнее необходимого.

## 7. Display rate

`display_rate.cpp` запрашивает повышенную частоту у `ANativeWindow`, когда известна поверхность вывода. Android и драйвер всё равно ограничивают результат доступными режимами панели.

## 8. Сборка

CI выполняет следующие шаги:

1. загружает Android NDK;
2. получает актуальный `zygisk.hpp`;
3. компилирует `jni/blend.comp` в `jni/blend_comp.h`;
4. собирает `libgpp_frc_repo.so` для `arm64-v8a` и `armeabi-v7a`;
5. кладёт библиотеки в `module/zygisk/<abi>.so`;
6. упаковывает `gpp_frc_repo-magisk.zip`.
