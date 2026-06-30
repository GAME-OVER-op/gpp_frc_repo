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

## Stage 3: screen-space optical-flow interpolation

Добавлен первый вариант вычислительной логики генерации кадров для Vulkan
present-bridge: `method=flow`.

В отличие от старого `method=blend`, новый путь не просто смешивает два кадра.
Compute shader в `jni/blend.comp` теперь умеет локально оценивать движение по
яркости между `prev` и `cur` кадрами:

1. Для каждого пикселя ищется лучший локальный offset `current -> previous` по
   SAD/MAD яркости на маленьком patch'е.
2. По найденному вектору shader берёт выборки из предыдущего и текущего кадра на
   половине смещения.
3. Рассчитывается confidence по качеству матча, неоднозначности второго лучшего
   вектора и ошибке после warp.
4. Если confidence низкий, результат мягко уходит к текущему реальному кадру —
   это важнее, чем агрессивно рисовать неверный вектор и получать рваные края.

Новые параметры `cleanfg.prop`:

```ini
method=flow
flow_search_radius=4
flow_patch_radius=1
flow_confidence_scale=6.0
```

Режим `method=blend` оставлен как fallback: тот же shader переключается через
push constants, поэтому можно откатываться конфигом без изменения кода.

Это ещё не полноценный FSR/optical-flow pipeline с пирамидами, depth и
forward/backward consistency, но это первый шаг от blend к motion-compensated
interpolation. Следующий качественный этап — вынести оценку motion vectors в
отдельные pass'ы с low-res pyramid, confidence/disocclusion mask и temporal
stability.

`max_fps=0` значит: просить `measured_fps * multiplier`, а Android сам зажмёт до доступного режима панели. Если хочешь жёстко ограничить — поставь `max_fps=120` или `144`.

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
