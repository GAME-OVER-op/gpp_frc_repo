# ARCHITECTURE — как работает хук и как воспроизвести функционал

## 1. Модель работы frame generation (FG)

Игра рисует кадр и «предъявляет» его на экран через present-вызов:

- GLES/EGL: `eglSwapBuffers(dpy, surface)`
- Vulkan:   `vkQueuePresentKHR(queue, pPresentInfo)`

FG перехватывает этот вызов. На каждый настоящий кадр N модуль:

1. Сохраняет содержимое текущего бэкбаффера (цвет, при наличии — depth).
2. Из кадров N-1 и N строит промежуточный кадр (interpolation) либо из N и векторов
   движения — экстраполяцию (extrapolation/reprojection).
3. Предъявляет сгенерированный кадр (второй present) + реальный кадр, распределяя
   их по времени (frame pacing) — так воспринимаемый FPS растёт в ~2х.

Именно шаг 2 — «ядро» FG, и именно его надо достать из декомпиляции, чтобы получить
ту же картинку/поведение, что у оригинала.

## 2. Цепочка инъекции (Zygisk)

```
zygote fork -> app process
        |
   Zygisk loads liblybfghook.so
        |
   preAppSpecialize:  решаем, целевой ли это процесс (имя пакета)
        |               если нет — setOption(DLCLOSE_MODULE_LIBRARY), выход
        |
   postAppSpecialize: процесс уже «стал» игрой -> ставим хуки
```

Целевой пакет в оригинале, видимо, берётся из конфига (lyb.prop / companion).
Компаньон (`zygisk_companion_entry`) работает в root-контексте демона и обычно
отдаёт модулю файлы/конфиг, которые недоступны из sandbox приложения.

## 3. Механизм инлайн-хука

Оригинал использует собственный `FunctionInlineHookRouting`. Мы в clean-room берём
готовый инлайн-хукер (ShadowHook от bytedance или Dobby). Логика та:

1. Разрешить адрес present-функции:
   - GLES: `dlopen("libEGL.so")` -> `dlsym("eglSwapBuffers")`.
   - Vulkan: через `vkGetInstanceProcAddr`/`vkGetDeviceProcAddr` либо `dlsym` из `libvulkan.so`.
2. Поставить trampoline-хук: сохранить оригинал, вставить переход на наш callback.
   Низкоуровневые примитивы (как в оригинале): `mprotect` для RWX, запись
   инструкций, `__builtin___clear_cache`, `dl_iterate_phdr`/`/proc/self/maps` для
   поиска базы модуля.
3. В callback вызвать ядро FG, потом оригинал.

## 4. Конфиг (локально, без сети)

Оригинальный `lyb.prop`: `fgappver=7`, `vulkan=2`. Наш `cleanfg.prop`:

```
target_packages=com.example.game
mode=auto         # auto | gles | vulkan
multiplier=2      # 2 = удвоение кадров
method=interp     # interp | reproject
debug=0
```

Читается с диска один раз при старте. Никаких удалённых обновлений.

## 5. Порядок работ для 1:1

1. Починить GitHub-workflow (уже сделано) -> получить `ghidra_out/*.decompiled.c`.
2. Найти callback present-хука (функция, которая вызывает сохранённый оригинал
   present дважды / работает с GL-текстурами/FBO).
3. Вынести алгоритм генерации кадра на уровне логики в `frame_gen.cpp`.
4. Собрать, проверить FPS/латенси на целевой игре.
