**English** · [Русский](#русский)

# 0.6-pre1 — pre-release for testing

bbport is the counterpart of Wine + DXVK for a single game: Bloodborne (v1.09) on Linux.
First 0.6 pre-release, built from the `testing` branch. Changes since
[0.51](https://github.com/deadinside28/bloodborne_pc/releases/tag/0.51); details and measurements:
[CHANGES_after_0.5.md](https://github.com/deadinside28/bloodborne_pc/blob/testing/docs/CHANGES_after_0.5.md)
(Russian).

Questions about this project: Discord https://discord.gg/KYZRKk9CB — not the shadPS4 server.

### The new memory model is the default on every GPU

- *Auto* (the launcher's default) is now the new memory and translation model on **every** GPU,
  NVIDIA and Intel included, not only on AMD. The game's memory goes through the layer's memory
  module everywhere: no sparse binding of the game's memory (NVIDIA's sparse binding froze the
  GPU for seconds), so nothing vendor-specific stands in the way. On AMD the module is faster
  than the former sparse arena too (RX 7800 XT: a route in Yharnam 4.30 vs 4.60 ms a frame,
  Oedon Chapel 234 vs 213 FPS), and only with it a release without the game's profile is right.
- The old model (as in 0.3) stays in the launcher: *Memory model → Old* (`BB_PC_MODEL=0`). A
  driver that fails the startup checks still falls back to VRAM copies by itself.
- **Please report** how the new model runs on NVIDIA and Intel (FPS, stutter, VRAM), with the
  log.

### New

- **Seamless co-op (BBSC)**: the session survives deaths, lamps, area changes and bosses, up to
  five hunters (BBSC, GPL-2.0+, carried in the online module). Launcher → *Online → Seamless
  co-op (BBSC)*, on by default; every player needs it and the same server.
- **Online servers: Seamless Hunt by default** (BBSC's public servers): shadNet
  `play.seamlesshunt.com:31313`, WebAPI `http://play.seamlesshunt.com:31315`, game server
  `http://play.seamlesshunt.com`. Create the account at https://seamlesshunt.com and enter it in
  the launcher. Settings saved with the old defaults switch over by themselves.
  shadPS4's public server (`srv.shadps4.net`) accepts only shadPS4's own builds since
  2026-10-12: it signed the port out 20–40 s after login (in game: "Network connection lost")
  and then refused logins for a while. The port now reports its version to the server and, if
  refused, stays offline from the start with the server's reason in the log, without the popup
  in the middle of play and without retrying.
- **Any aspect ratio, live, through hooks** (the game's code is not patched): 4:3, 16:10, 16:9,
  21:9, 32:9; aspect ratio and resolution are separate settings (game menu *Display*, overlay,
  launcher). The camera shows more at the sides (wider) or above and below (taller), the HUD is
  anchored to the screen's edges, full-screen fills and menu backdrops cover the whole output,
  markers over characters stay on them (#183, #151, #76, #35, #82). The removed variant patched
  the game's frame size.
- **Frame rate through hooks, not patches**: above 30 FPS the game's main loop and 48 places tied
  to the 30 FPS frame use the real frame time (found anew: timers of physics-driven body parts ran
  6–7× faster at 200 FPS; running fell to walking above ~90 FPS). The community patches remain
  for comparison (`BB_FRAME_HOOKS=0`).
- **Keyboard and mouse:**
  - mouse look turns the game's camera directly (no stick dead zone, acceleration or speed
    limit); where the camera does not take it (lock-on, menus) the mouse drives the right stick
    as before;
  - a **mouse pointer in the game's menus**: hover selects, left click confirms, right click goes
    back, the wheel scrolls;
  - **a layout close to Dark Souls III**: WASD move, Space dodge/run, F/Enter interact, LMB
    attack, Shift+LMB strong attack, RMB shoot, Q blood vial, E weapon transform, Tab/R quick
    item, 1/2 left/right weapon, 3/4 up/down d-pad (quick items), C lock-on, Esc menu, G gestures,
    Backspace personal items. Bindings may be chords ("Left Shift+Mouse Left"). Your own
    bindings are kept;
  - **button prompts** for the device in use, also in the HUD ("Examine" at lamps, gravestones,
    items): the bound key or mouse button, or the buttons of an Xbox or another gamepad;
    PlayStation icons only with a PlayStation pad.
- **The game's occlusion queries are real by default** in the new model, counted at the game's
  resolution whatever the render scale (the lamp glow no longer flashes grey with the upscaler).
- **The port's own code instead of shadPS4** (except the online module): command processor, GCN
  definitions, shader translator, renderer, caches and platform libraries
  (`gpu/VENDOR.txt`). The translator gives byte-identical SPIR-V on the game's shader corpus.

### Fixes

- NVIDIA: 0.51 did not start (`CreateDevice: ErrorInitializationFailed`, code 134/-1): the
  launcher now provides `libcuda`, and the device is created again without NGX extensions if
  needed (DLSS then off, FSR 3.1) (#135, #147, #152, #187, #189).
- Crashes at RIP=0 on NVIDIA and Intel: an extension was enabled but its commands were missing
  (#161, #177, #181).
- New model at 4K (scene 2560×1440): a frame-sized buffer was written over the bus; 95 → 132 FPS
  on an RX 7800 XT (#149).
- New model: FPS fell ~28 % after 15–20 minutes of play with lamp travel; now stays.
- New model: stale VRAM copies after a race in the write-trap handler (stretched "exploding"
  vertices, possibly some GPU hangs).
- Loading hang at lamp travel: thread handles above 40 bits never matched the owner stored by
  the game's lock (analysis by the author of PR #193; reimplemented).
- Crash at `0x2199310` (the game's per-frame GX heap full) (#162, #192).
- Old model: VRAM copies accumulated until "VRAM is full" after an hour or two on 4 GB cards
  (#142).
- Empty character-creator preview (#178).
- Decoder and format asserts during draw preparation (#144, #165, #166).
- In-game menu language with `--play` / stuck in bbport.ini (#134).
- Steam Deck Game Mode: pressure-vessel's Vulkan variables hid the bundled drivers (#172).
- FSR 4.1.1 assets on RX 9000: the FP8 passes are now recorded (#186).
- `AppImage --play` failed at once ("Software caused connection abort").
- HUD: white streaks at 21:9.
- Exit after the game stops itself: a crash in the online module (`libbbnet.so`, "terminate
  called without an active exception"); the log now names the game code that stopped.
- Pipelines from the disk cache: geometry-shader pipelines got an empty fetch shader; on NVIDIA
  the clip-distance discard shader was not saved; a draw without a pixel shader took the previous
  draw's fragment settings.
- Mouse: the camera no longer jumps when the mouse is captured.
- Building: Debian without a miniz package (#136); the build names the packages missing for the
  online module (#145).

### Faster

- The draw-recording stage spends 6 % less on buffer bindings through the memory module; it
  matters on slower CPUs (on the test machine the frame is GPU-bound).

**Known limits:** real co-op through Seamless Hunt was not tested by the author yet (one
account); the new model on NVIDIA and Intel needs your reports. Frame generation is in the code
but hidden until its artifacts with upscaling are fixed.

The AppImage contains no game files. AMD's FSR 4 model assets are not bundled. `SHA256SUMS`
accompanies the download.

## Русский

# 0.6-pre1 — пререлиз для тестирования

bbport — аналог Wine + DXVK для одной игры: Bloodborne (v1.09) на Linux. Первый пререлиз 0.6,
из ветки `testing`. Изменения после
[0.51](https://github.com/deadinside28/bloodborne_pc/releases/tag/0.51); подробности и замеры —
[CHANGES_after_0.5.md](https://github.com/deadinside28/bloodborne_pc/blob/testing/docs/CHANGES_after_0.5.md).

Вопросы по проекту: Discord https://discord.gg/KYZRKk9CB — не сервер shadPS4.

### Новая модель памяти — по умолчанию на любой видеокарте

- *Авто* (по умолчанию в лаунчере) — теперь новая модель памяти и трансляции на **любой**
  видеокарте, включая NVIDIA и Intel, а не только на AMD. Память игры везде идёт через модуль
  памяти прослойки: без sparse-привязки памяти игры (на NVIDIA sparse binding держал видеочип
  секундами), поэтому ничего зависящего от производителя на пути нет. На AMD модуль быстрее
  прежней sparse-арены (RX 7800 XT: маршрут по Ярнаму 4,30 против 4,60 мс на кадр, часовня Одеона
  234 против 213 FPS), и только с ним верно идёт издание без профиля игры.
- Старая модель (как в 0.3) остаётся в лаунчере: *Модель памяти → Старая* (`BB_PC_MODEL=0`).
  Если драйвер не проходит проверки при запуске, порт сам переходит на копии в VRAM.
- **Пожалуйста, сообщайте**, как новая модель работает на NVIDIA и Intel (FPS, рывки, VRAM), с
  журналом.

### Новое

- **Бесшовный кооператив (BBSC)**: сессия переживает смерти, лампы, смену локаций и боссов, до
  пяти охотников (BBSC, GPL-2.0+, в онлайн-модуле). Лаунчер → *Онлайн → Бесшовный кооператив
  (BBSC)*, по умолчанию включён; нужен у всех игроков и один сервер.
- **Серверы по умолчанию — Seamless Hunt** (публичные серверы BBSC): shadNet
  `play.seamlesshunt.com:31313`, WebAPI `http://play.seamlesshunt.com:31315`, игровой сервер
  `http://play.seamlesshunt.com`. Учётную запись создайте на https://seamlesshunt.com и введите
  в лаунчере. Сохранённые старые адреса по умолчанию меняются сами.
  Публичный сервер shadPS4 (`srv.shadps4.net`) с 12.10.2026 принимает только сборки shadPS4: он
  выкидывал порт через 20–40 с после входа (в игре — «Потеряно соединение с сетью»), а потом
  какое-то время отклонял вход. Теперь порт сообщает серверу свою версию и при отказе с самого
  начала остаётся офлайн — с причиной от сервера в журнале, без окна посреди игры и без повторных
  попыток.
- **Любое соотношение сторон, на лету, хуками** (код игры не патчится): 4:3, 16:10, 16:9, 21:9,
  32:9; соотношение и разрешение — отдельные настройки (меню игры *Изображение*, оверлей,
  лаунчер). Камера видит больше по бокам (шире) или сверху и снизу (выше), HUD прижат к краям
  экрана, полноэкранные заливки и подложки меню — на весь вывод, метки над персонажами остаются на
  них (#183, #151, #76, #35, #82). Прежний вариант с патчем размера кадра игры убран.
- **Частота кадров — хуками, а не патчами**: выше 30 FPS главный цикл игры и 48 мест, привязанных к
  кадру 30 FPS, идут по реальному времени кадра (найдено заново: таймеры частей тела, которые
  двигает физика, на 200 FPS шли в 6–7 раз быстрее; выше ~90 FPS бег падал до шага). Патчи
  сообщества остались для сравнения (`BB_FRAME_HOOKS=0`).
- **Клавиатура и мышь:**
  - камера мышью поворачивает камеру игры напрямую (без мёртвой зоны стика, разгона и предела
    скорости); где камера поворот не берёт (захват цели, меню) — мышь, как раньше, ведёт правый
    стик;
  - **указатель мыши в меню игры**: наведение выбирает, левая кнопка — подтвердить, правая —
    назад, колесо — прокрутка;
  - **раскладка как в Dark Souls III**: WASD — движение, Space — уклонение/бег, F/Enter —
    взаимодействие, ЛКМ — атака, Shift+ЛКМ — сильная атака, ПКМ — выстрел, Q — флакон крови, E —
    трансформация оружия, Tab/R — быстрый предмет, 1/2 — оружие левой/правой руки, 3/4 —
    крестовина вверх/вниз (быстрые предметы), C — захват цели, Esc — меню, G — жесты, Backspace —
    личные вещи. Назначения могут быть сочетаниями («Left Shift+Mouse Left»). Свои назначения
    сохраняются;
  - **подсказки кнопок** для используемого устройства, и в HUD тоже («Осмотреть» у ламп,
    надгробий, предметов): назначенная клавиша или кнопка мыши либо кнопки Xbox или другого
    геймпада; значки PlayStation — только с геймпадом PlayStation.
- **Occlusion-запросы игры по умолчанию настоящие** в новой модели, со счётом в разрешении игры
  при любом масштабе рендера (свечение лампы больше не вспыхивает серым с апскейлером).
- **Свой код вместо shadPS4** (кроме онлайн-модуля): командный процессор, описания GCN, транслятор
  шейдеров, рендер, кэши и платформенные библиотеки (`gpu/VENDOR.txt`). Транслятор даёт побайтно
  тот же SPIR-V на корпусе шейдеров игры.

### Исправления

- NVIDIA: 0.51 не запускалась (`CreateDevice: ErrorInitializationFailed`, код 134/-1): лаунчер
  теперь даёт `libcuda`, а устройство при нужде создаётся заново без расширений NGX (тогда DLSS
  выключен, FSR 3.1) (#135, #147, #152, #187, #189).
- Падения с RIP=0 на NVIDIA и Intel: расширение включено, а его команд нет (#161, #177, #181).
- Новая модель в 4K (сцена 2560×1440): буфер размером с кадр писался через шину; 95 → 132 FPS на
  RX 7800 XT (#149).
- Новая модель: FPS падал на ~28 % после 15–20 минут игры с перемещениями по лампам; теперь нет.
- Новая модель: устаревшие копии в VRAM после гонки в обработчике ловушек записи («взрывающиеся»
  растянутые вершины, возможно, часть зависаний видеочипа).
- Зависание загрузки при перемещении по лампам: дескрипторы потоков выше 40 бит не совпадали с
  владельцем, которого хранит блокировка игры (анализ автора PR #193; сделано заново).
- Падение на `0x2199310` (переполнена покадровая куча GX игры) (#162, #192).
- Старая модель: копии в VRAM копились до «VRAM is full» через час-два на картах 4 ГБ (#142).
- Пустой предпросмотр в редакторе персонажа (#178).
- Остановки декодера и форматов при подготовке отрисовок (#144, #165, #166).
- Язык меню игры с `--play` / застревал в bbport.ini (#134).
- Steam Deck, игровой режим: переменные Vulkan от pressure-vessel скрывали драйверы из пакета
  (#172).
- Ассеты FSR 4.1.1 на RX 9000: теперь записываются и проходы FP8 (#186).
- `AppImage --play` падал сразу («Software caused connection abort»).
- HUD: белые полосы при 21:9.
- Выход после того, как игра сама остановилась: падение в онлайн-модуле (`libbbnet.so`,
  «terminate called without an active exception»); журнал теперь называет код игры, который её
  остановил.
- Конвейеры из кэша на диске: с геометрическим шейдером получали пустой fetch-шейдер; на NVIDIA
  не сохранялся шейдер отбрасывания по clip distances; отрисовка без пиксельного шейдера брала
  настройки фрагментной стадии предыдущей.
- Мышь: камера больше не дёргается при захвате мыши.
- Сборка: Debian без пакета miniz (#136); сборка называет пакеты, которых не хватает онлайн-модулю
  (#145).

### Быстрее

- Стадия записи отрисовок тратит на 6 % меньше на привязки буферов через модуль памяти — важно
  на процессорах слабее (на тестовой машине кадр упирается в видеокарту).

**Известные ограничения:** настоящий кооператив через Seamless Hunt автор ещё не проверял (одна
учётная запись); новая модель на NVIDIA и Intel ждёт ваших отчётов. Генерация кадров в коде есть,
но скрыта, пока не исправлены её артефакты с апскейлером.

AppImage не содержит файлов игры. Ассеты модели FSR 4 от AMD не входят. К загрузке приложен
`SHA256SUMS`.
