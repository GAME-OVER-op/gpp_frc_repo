# gpp-frc-test

Stend dlya zapuska frejmgen-dvizhka RedMagic OS (G-FRC+ / MotionEngine Vulkan) na kastome.
Sborka idyot v **GitHub Actions** s Android NDK - lokalnyj kompilyator ne nuzhen.

## Phase 8 - podacha kadrov

`gpp_frc_test` v realnom processe na telefone:
1. `dlopen` dvizhka `libgppvppgfrcplussession.so` + razreshenie tochnyh simvolov
   (`CreateFactory`, `GPPSession::GPPSession/connect`, `Surface::Surface/getIGraphicBufferProducer`);
2. **S2** dyorgaet `CreateFactory()` i opredelyaet klass vozvrashyonnogo obyekta po vtable;
3. **S3** sozdayot `AImageReader` (vyhodnoj priyomnik) i beryot ego `IGraphicBufferProducer`;
4. **S4** sozdayot `GPPSession`;
5. **S5** zovyot `GPPSession::connect(pkg, layer, output, &input)` - poluchaet vhodnoj producer;
6. **S6** oborachivaet vhodnoj producer v `Surface`;
7. **S7** gonit ~150 animirovannyh RGBA-kadrov v vhod (~60 fps);
8. **S8** best-effort zabiraet vyhodnye kadry iz `AImageReader` v `/data/local/tmp/gpp_out_*.bin`.

Kazhdaya stadiya pod zashitoj ot krasha (SIGSEGV/SIGABRT/SIGBUS -> log + perehod dalshe),
poetomu lyuboj sboj ostavlyaet log do tochki padeniya (vidno nomer stadii).

**Priznak uspeha** - v logcat poyavlyayutsya stroki dvizhka:
`create BufferQueue done`, `FRC will do Nx interpolation`, `Send the Non-Interpolated frame`,
logi MotionEngine/VTxr.

## Kak sobrat

1. Sozdaj novyj repozitorij na GitHub i zalej tuda soderzhimoe etogo arhiva:
   ```bash
   git init && git add . && git commit -m "phase 3"
   git branch -M main
   git remote add origin git@github.com:<ty>/gpp-frc-test.git
   git push -u origin main
   ```
2. Otkroj vkladku **Actions** - sborka zapustitsya avtomaticheski.
3. Skachaj artefakt **gpp_frc_test-arm64-v8a**.

## Kak zapustit na ustrojstve

Polozhi `gpp_frc_test` i `run.sh` v odnu papku (naprimer `/data/local/tmp/`) i:
```sh
sh run.sh            # 150 kadrov, sam podnimet root
sh run.sh 300        # 300 kadrov
```
Skript sam sbrosit logcat, zapustit binar i soberyot:
`*.log` (stdout), `*.log.full` (ves logcat), `*.log.gpp` (otfiltrovannye GPP/FRC stroki).
Skinь mne `.log` i `.log.gpp`.

## Struktura
```
.github/workflows/build.yml   - CI: NDK -> arm64-v8a -> artefakt
app/CMakeLists.txt            - sborka (NDK: log dl mediandk android nativewindow)
app/src/main.cpp              - Phase 8 harness
run.sh                        - zapusk na ustrojstve + zahvat logov
```
