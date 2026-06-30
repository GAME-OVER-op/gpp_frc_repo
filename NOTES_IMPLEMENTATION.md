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
