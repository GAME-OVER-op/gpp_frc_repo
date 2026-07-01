# Что дописано в этой версии

Эта версия уже не просто пустой каркас:

- хукает `eglCreateWindowSurface`, чтобы запомнить `ANativeWindow` для surface;
- хукает `eglSwapInterval` и может принудительно ставить interval=0;
- хукает `eglSwapBuffers`;
- измеряет реальный FPS игры по интервалам present;
- просит у Android более высокую частоту панели через `ANativeWindow_setFrameRate`:
  - 60 -> 120;
  - 72 -> 144;
  - 90 -> 180, но будет ограничено `max_fps` или самой ОС;
- сохраняет текущий кадр в GL texture через `glCopyTexSubImage2D`;
- хранит две history-текстуры;
- строит промежуточный кадр простым shader blend `mix(prev, cur, 0.5)`;
- делает дополнительный `eglSwapBuffers` для сгенерированного кадра, затем обычный present.

Это **рабочая базовая интерполяция**, не 1:1 алгоритм `lybfghook`. Для 1:1 ещё нужен вывод Ghidra, чтобы заменить blend на тот же warp/reprojection/pacing, который использует оригинал.

## Важное ограничение

Без реального повышения частоты панели генерацию визуально не будет видно. Поэтому `elevate_rate=1` включён по умолчанию.

## Конфиг

`module/cleanfg.prop`:

```ini
target_packages=com.example.game
mode=auto
multiplier=2
max_fps=0
elevate_rate=1
force_swap_interval_0=1
method=blend
debug=0
```

## Stage 3: развитие единого `method=blend`

Полноэкранный `method=flow` был признан слишком тяжёлым для мобильного GPU и
убран из production-пути. Дальше развиваем один метод генерации — `method=blend`,
но внутри shader теперь не простой `mix(prev, cur, 0.5)`, а единая адаптивная
логика:

1. **Static midpoint** — на спокойных областях кадр остаётся близким к
   середине между `prev` и `cur`, чтобы сохранять ощущение 120 FPS.
2. **Reactivity по яркости** — как в идее FSR reactive mask: где кадры сильно
   отличаются, уменьшаем влияние истории и больше доверяем текущему кадру.
3. **Mini reset для резких изменений** — при быстром повороте/скачке кадра
   shader не пытается сохранять старый кадр, а мягко уходит к current-frame
   transition. Это лучше, чем раздвоение/рваные силуэты.
4. **Дешёвое motion-zone softening** — в движущихся областях добавляется очень
   лёгкое размытие текущего кадра, чтобы межкадр выглядел как мягкий переход,
   но без тяжёлого optical-flow поиска.

Актуальный тестовый конфиг:

```ini
method=blend
blend_alpha=0.52
diff_threshold=0.055
diff_softness=0.18
motion_strength=0.85
blur_radius=1
```

Это всё ещё один метод (`blend`), но он содержит несколько внутренних веток
качества: спокойная сцена, движение, резкий поворот/потеря temporal confidence.
Позже эти параметры можно убрать из `cleanfg.prop` и зашить как системные
константы после подбора приятной картинки.

### Stage 3.1: стабилизация мелких элементов и эффектов

По результатам теста `method=blend` дал приятную и плавную картинку, но в очень
быстрых сценах с большим количеством мелких элементов появлялись локальные
"пятнышки". Вместо отключения blur на деталях применён более близкий к FSR
подход: blur остаётся, но маски и цветовой диапазон стали стабильнее.

В `jni/blend.comp` добавлено:

1. **Luma + chroma reactivity** — движение считается не только по яркости, но и
   по максимальному изменению RGB-компонент. Это лучше ловит частицы, свечения и
   elemental effects.
2. **Разделение внутренних масок** при том же внешнем `method=blend`:
   - `reactive` — насколько уменьшить влияние history и доверять current frame;
   - `composition` — насколько применять мягкий transition/blur, причём маска
     пространственно связнее, чтобы не было pixel-speckle;
   - `instability` — mini-reset только для действительно резких изменений.
3. **Neighborhood color clipping** — результат ограничивается min/max диапазоном
   маленькой окрестности `prev` и `cur`. Это снижает цветные пятна на эффектах,
   не убивая яркие частицы.
4. **Мягкая detail moderation** — мелкие элементы больше не выключают blur
   агрессивно; они только слегка уменьшают его, чтобы сохранить плавность в
   играх с большим количеством мелких деталей.

Идея: сохранять текущую приятную плавность, но сделать motion/reactive mask менее
пятнистой в extreme-сценах.

### Stage 3.2: лёгкие улучшения без роста нагрузки GPU

SurfaceFlinger/system-wide hook отложен; продолжаем app-side путь. По просьбе
не добавлялись тяжёлые inpainting-пирамиды и debug overlay. Вместо этого в
единственный production-метод `method=blend` добавлены дешёвые эвристики, которые
не требуют новых Vulkan images, дополнительных compute pass'ов или больших search
loops:

1. **Lightweight pan direction** — shader сравнивает `cur` с `prev` в четырёх
   направлениях на маленьком offset'е и получает грубую подсказку направления
   быстрого движения/поворота камеры. Это не optical flow, а 4 дополнительных
   дешёвых проверки.
2. **Directional transition blur** — при уверенном pan используется не случайное
   локальное размытие, а мягкий blur вдоль предполагаемого направления движения.
   Это должно уменьшить пятнистость при быстрых поворотах и сохранить ощущение
   плавности.
3. **UI/static-detail heuristic** — резкие экранные детали, которые почти не
   двигаются, становятся более current-biased и получают меньше blur. Это должно
   беречь HUD/текст/тонкие статичные элементы без отключения плавности для частиц
   и движущихся мелких объектов.
4. **Adaptive generation strength** — при хаотичных сценах history уменьшается
   плавно, а не через резкий hard cut; при directional pan часть transition берёт
   направленное размытие.
5. **Chroma-aware clamp margin** — color clipping теперь чуть шире для ярких VFX,
   чтобы не зажимать частицы, но всё ещё ограничивает невозможные цветные пятна.

Сохранено: один shader pass, один `method=blend`, те же push constants и ресурсы.
Ожидаемый overhead относительно Stage 3.1 небольшой: добавлены только несколько
локальных выборок из `prev/cur`, без full-res optical flow и без дополнительных
dispatch'ей.

## Загрузка движка GPP через собственный linker-namespace (фикс)

Лог показал: Vulkan-захват работает (`vk: capture ready ...`), но движок
`libgppvppgfrcplussession.so` не грузился:

```
dlopen failed: library "vendor.qti.hardware.vpp-V1-ndk.so" not found:
  needed by /system/lib64/libgppvppgfrcplussession.so
```

Причина — изоляция linker-namespace (Treble):

- движок лежит в `/system/lib64` → его видят только namespace `default`/`system`,
  но им ЗАПРЕЩЕНО резолвить вендорную зависимость `vendor.qti.hardware.vpp-V1-ndk.so`;
- namespace `sphal` умеет грузить вендорные либы, но ОТКАЗЫВАЕТСЯ открывать путь
  `/system/lib64` (`...is not accessible for the namespace`).

Ни один штатный namespace не видит обе партиции сразу. Решение в `gpp_engine.cpp`:
создаём СВОЙ namespace `cleanfg_gpp` через `android_create_namespace`
(тип SHARED, не isolated) с путём поиска по `/system/lib64` + `/vendor/lib64` +
`/odm/lib64`, и линкуем его к `sphal`/`vndk`/`default`
(`android_link_namespaces`) с нужными allowlist'ами sonames. Затем грузим движок
через `android_dlopen_ext` уже в этом namespace — DT_NEEDED на вендорную либу
резолвится. Старые способы (plain dlopen, exported namespaces) оставлены как
fallback.

### Stage 3.4: GLES state isolation

Asphalt 8 still showed a black screen after the repaint fix, which points to GL state corruption or drawing with inherited app state. GLES games often leave depth/scissor/cull tests, color masks, texture bindings, VAOs, read/draw FBOs and active texture units in arbitrary states at `eglSwapBuffers`. The hook was not restoring enough state after capture/render, so a frame-generation draw could break the game's next frame or draw black under inherited state.

Fix: isolate GLES capture/render with explicit state save/restore for framebuffers, active texture and texture bindings, VAO/VBO, viewport, program, color/depth masks and common enable bits. The generated/current fullscreen pass now disables depth/stencil/scissor/cull/blend and forces color writes before drawing, then restores the app state.

### Stage 3.5: GLES compatibility diagnostic modes

The Asphalt diagnostic archive showed that the installed module was not the newest state-isolation build: the loaded `arm64-v8a.so` strings did not contain the new `gles frame-gen draw active`, `gles capture err`, or `gles render err` markers. The capture also confirmed the game uses a compressed 16-bit SurfaceView buffer (`req fmt:277`, fourcc `BG16`, UBWC), and the screenshot was ~96% black while EGL hooks and GLES init were active.

To isolate the exact GLES failure point, add `gles_debug_mode`:
- `framegen` / `0`: full GLES generated-frame path.
- `passthrough` / `1`: hook only, no GL capture/draw and no extra present.
- `capture_only` / `2`: run `glCopyTexSubImage2D`, then normal present only.
- `draw_only` / `3`: capture and draw the current frame once, then normal present.
- `double_present` / `4`: no draw between two presents; tests whether extra `eglSwapBuffers` itself breaks the Surface.
- `current_only` / `5`: replace the real frame with captured current texture once.

This build also clamps unstable first FPS samples above 180 FPS to a 120 Hz request to avoid absurd mode requests during startup.

### Stage 3.6: build fix for GLES diagnostic modes

GitHub Actions failed because `hook_egl.cpp` called `fgRenderCurrentGles(...)`, but the function prototype was missing from `frame_gen.h`. Add the declaration so both `hook_egl.cpp` and `frame_gen.cpp` agree during compilation for arm64 and armeabi-v7a.

### Stage 3.7: GLES 60 FPS follow-up instrumentation

After the diagnostic build produced a visible image but stayed at 60 FPS, add GLES-side instrumentation and force `eglSwapInterval(0)` from inside the first hooked `eglSwapBuffers` call as well as through the hooked `eglSwapInterval` entry point. Some GLES games set/restore swap interval before or after our install path, so forcing once from the active swap path makes the behavior explicit. The hook now logs app swap count, generated-present count, selected `gles_debug_mode`, and measured app FPS every ~120 swaps.

### Stage 3.8: GLES capture fix for Asphalt 8

`log5.txt` showed the image is visible, the mode is `framegen`, but `generatedPresents=0` because every capture fails with `GL_INVALID_OPERATION` (`gles capture err=0x502`). That means the interpolation path never receives valid history/current textures and cannot present extra generated frames.

Update the GLES capture path to prefer GLES3 `glBlitFramebuffer` from the default framebuffer into our texture-backed FBO, with the previous `glCopyTexSubImage2D` path kept as fallback. Also reduce log spam by rate-limiting repeated capture errors and log framebuffer/capture diagnostics once.

### Stage 3.9: GLES adaptive blend parity with Vulkan

After the GLES blit capture fix made Asphalt 8 render normally and allowed frame history to work, port the current Vulkan `blend.comp` generation logic into the GLES fullscreen fragment shader. GLES now uses the same internal generation model as Vulkan: reactive mask, composition mask, instability/current reset, directional pan blur, static sharp-detail/HUD protection, and neighborhood color clipping. This keeps the public method as `method=blend` while making the GLES visual output match the smooth Vulkan path as closely as possible.

### Stage 4.0: production auto mode and sealed tuning

The production configuration is now intentionally minimal: `cleanfg.prop` exposes only `target_packages`. All runtime tuning is internal and defaults to the validated single `method=blend` generation path: auto backend, 2x generation, display-rate elevation, swap-interval unlock for GLES, Vulkan present bridge, debug off, and the validated blend/reactivity constants.

Auto backend selection installs both Vulkan and EGL hooks, but only the backend that actually presents frames becomes active. Vulkan surface/device/swapchain creation marks a Vulkan candidate; GLES waits briefly in auto mode so UI/EGL setup cannot steal the process from a real Vulkan renderer. The first real `vkQueuePresentKHR` selects Vulkan; otherwise `eglSwapBuffers` selects GLES. After selection, the other backend passes through without generating frames.

GLES now also resets temporal history after large frame-time discontinuities and uses `eglPresentationTimeANDROID` when available to hint generated/real frame pacing at half-frame intervals.
