**English** · [Русский](#русский)

# 0.6

bbport is the counterpart of Wine + DXVK for a single game: Bloodborne (v1.09) on Linux.
Changes since [0.51](https://github.com/deadinside28/bloodborne_pc/releases/tag/0.51), the
0.6-pre1 pre-release included. Details and measurements (Russian):
[CHANGES_0.6-pre1.md](https://github.com/deadinside28/bloodborne_pc/blob/master/docs/CHANGES_0.6-pre1.md),
[CHANGES_after_0.6-pre1.md](https://github.com/deadinside28/bloodborne_pc/blob/master/docs/CHANGES_after_0.6-pre1.md).

Questions about this project: Discord https://discord.gg/KYZRKk9CB — not the shadPS4 server.

### The new memory model is the default on every GPU

- *Auto* (the launcher's default) is now the new memory and translation model on **every** GPU,
  NVIDIA and Intel included, not only on AMD. The game's memory goes through the layer's memory
  module everywhere: no sparse binding of the game's memory (NVIDIA's sparse binding froze the
  GPU for seconds). On AMD the module is faster than the former sparse arena too (RX 7800 XT: a
  route in Yharnam 4.30 vs 4.60 ms a frame, Oedon Chapel 234 vs 213 FPS).
- The old model (as in 0.3) stays in the launcher: *Memory model → Old* (`BB_PC_MODEL=0`). A
  driver that fails the startup checks still falls back to VRAM copies by itself.
- **Please report** how the new model runs on NVIDIA and Intel (FPS, stutter, VRAM), with the
  log.

### New

- **Online: two pools, on shadPS4 0.20's network stack.** The launcher's *Online* page has a
  *Pool* list:
  - **Seamless Hunt** (default): bells, summons, messages and bloodstains through BBSC's public
    servers, with **seamless co-op (BBSC)** — the session survives deaths, lamps, area changes,
    bosses and the Hunter's Dream, up to five hunters (*Online → Seamless co-op (BBSC)*, on by
    default; every player needs it and the same server). Create the account at
    https://seamlesshunt.com and enter it in the launcher. Settings saved with the old defaults
    switch over by themselves.
  - **The Hunter's Dream** (thehuntersdream.com): sign in with Discord from the launcher (the code
    is confirmed on the site); no BBSC there.

  The online module now uses shadPS4 v0.20's network, NP and shadNet code (`libbbnet20.so`); the
  former shadp2p module stays as a fallback (`BB_NET_GEN=19`). shadPS4's public server
  (`srv.shadps4.net`) accepts only shadPS4's own builds since 2026-10-12; the port reports its
  version and, if refused, stays offline from the start with the server's reason in the log.
- **Any aspect ratio, live, through hooks** (the game's code is not patched): 4:3, 16:10, 16:9,
  21:9, 32:9; aspect ratio and resolution are separate settings (game menu *Display*, overlay,
  launcher). The camera shows more at the sides (wider) or above and below (taller), the HUD is
  anchored to the screen's edges, full-screen fills and menu backdrops cover the whole output,
  markers over characters stay on them; cinematics without a HUD are wide too (#183, #151, #76,
  #35, #82).
- **Frame rate through hooks, not patches**: above 30 FPS the game's main loop and 48 places tied
  to the 30 FPS frame use the real frame time (timers of physics-driven body parts ran 6–7× faster
  at 200 FPS; running fell to walking above ~90 FPS). The community patches remain for
  comparison (`BB_FRAME_HOOKS=0`). **F3** (or the overlay's *Frame rate limit (now)*) switches a
  30 FPS limit on and off while playing.
- **Effects switch without a restart**: chromatic aberration, depth of field, motion blur, SSAO,
  the game's anti-aliasing and dynamic-light shadows (game menu and overlay). Screen-space
  reflections, the intro, free camera and the debug menu still apply at start (the menus say
  so).
- **Keyboard and mouse:**
  - mouse look turns the game's camera directly (no stick dead zone, acceleration or speed
    limit); where the camera does not take it (lock-on, menus) the mouse drives the right stick;
  - a **mouse pointer in the game's menus**: hover selects, left click confirms, right click goes
    back, the wheel scrolls; the window keeps the mouse while focused, **F1** releases it;
  - **a layout close to Dark Souls III**: WASD move, Space dodge, Left Shift run, Shift+Space
    jump, F/Enter interact, LMB attack, Shift+LMB strong attack, RMB shoot, Q blood vial, E
    weapon transform, Tab/R quick item, 1/2 left/right weapon, 3/4 up/down d-pad, C lock-on, Esc
    menu (Esc goes back inside menus), G gestures, Backspace personal items. Bindings may be
    chords ("Left Shift+Mouse Left"); your own bindings are kept;
  - movement keys act as a stick that ramps up (70 ms) and turns smoothly instead of jumping
    straight to full deflection; short taps are no longer lost;
  - **button prompts** for the device in use, also in the HUD ("Examine" at lamps, gravestones,
    items): the bound key or mouse button, or the buttons of an Xbox or another gamepad;
    PlayStation icons only with a PlayStation pad;
  - the port's own cursor (`BB_CURSOR=0`: the system's); with the *Restore Debug Camera* patch the
    mouse turns the free camera.
- **Launcher**: a *GPU* choice (laptops with two GPUs; stored by name); the game's text
  language offers all 21 languages the game has (#249).
- **The game's occlusion queries are real by default** in the new model, counted at the game's
  resolution whatever the render scale.
- **The port's own code instead of shadPS4** (except the online module): command processor, GCN
  definitions, shader translator, renderer, caches and platform libraries
  (`gpu/VENDOR.txt`).

### Fixes

- **Laurence's cinematic above 30 FPS**: it hung (the game's render-job pool lost a slot on
  every failed job), and then models exploded or went missing (the game's render resource cache
  of 160 MiB ran out at high frame rates). The slots are given back, and the cache gets 512 MiB
  of the port's own memory, outside what the game's areas load into.
- **Single frames with the picture shifted by half the screen or squeezed into a corner**
  (bright or dark flashes, often in Laurence's cinematic): a compute pass of the occlusion
  results overwrote the push constants of the next full-screen draw.
- **Crash `guest raised debug exception 0xa0020007`** (code 23, #207): the game's own assertion
  on a blur kernel's sample count; the count is now kept valid.
- **Crash at save load** (`Bad PM4 packet`, code 23): the game's main thread and its render
  thread wrote one command buffer at once during an effect's initialisation; that clear is now
  done by the port. Command buffers are also copied at submit, so a buffer reused early by the
  game is no longer decoded half-rewritten.
- Crash 139 with missing shader resource tables during heavy streaming.
- NVIDIA: 0.51 did not start (`CreateDevice: ErrorInitializationFailed`, code 134/-1): the
  launcher now provides `libcuda`, and the device is created again without NGX extensions if
  needed (DLSS then off, FSR 3.1) (#135, #147, #152, #187, #189, #247, #252).
- Crashes at RIP=0 on NVIDIA and Intel: an extension was enabled but its commands were missing
  (#161, #177, #181).
- Debian 13 and similar with NVIDIA: the launcher found NVIDIA's libraries only next to the ICD
  and fell back to Intel (#205).
- **FPS drop when looking at light shafts** (new model, most of all on NVIDIA, #204, PR #210):
  tiling wrote scattered texels over the bus, and two light-shaft targets were copied through
  memory 5 times a frame; GPU frame 17.0 → 10.4 ms in the NVIDIA simulation on AMD.
- New model at 4K (scene 2560×1440): a frame-sized buffer was written over the bus; 95 → 132 FPS
  on an RX 7800 XT (#149).
- New model: FPS fell ~28 % after 15–20 minutes of play with lamp travel; now stays.
- New model: stale VRAM copies after a race in the write-trap handler (stretched "exploding"
  vertices, possibly some GPU hangs).
- VRAM: on 8 GB cards the texture collector started too early (#204); when VRAM is full, the
  textures being freed are waited for before an allocation fails (PR #206).
- Loading hang at lamp travel: thread handles above 40 bits never matched the owner stored by
  the game's lock (analysis by the author of PR #193; reimplemented).
- Crash at `0x2199310`, the game's per-frame GX command heap full: the two heaps get 1 MiB
  instead of 256 KiB (#162, #192, PR #224); the protective hooks, aspect ratio and mouse work in
  the old model too (PR #214); resize and HDR changes recreate the swapchain safely (PR #211).
- **White or broken picture after loading another save with FSR 4.1.1**: FSR 4.1.1's temporal
  buffers are cleared on a scene reset, and textures cleared by the GPU are refreshed.
- Crash after selecting an item (SSAO forced back where the game had turned it off).
- Corrupted scene at the first load with screen-space reflections switched while playing:
  SSR applies at start again.
- Single black or grey frames: the present waited for the wrong stage of the frame's work.
- RX 580 and other GCN 3/4 GPUs: stall after the first cinematic or guest fault at character
  creation (#203): end-of-pipe labels are written by the CPU there (`BB_GPU_LABELS=1`: GPU).
  Not verified on such a GPU yet.
- Old model: VRAM copies accumulated until "VRAM is full" after an hour or two on 4 GB cards
  (#142).
- Empty character-creator preview (#178).
- Decoder and format asserts during draw preparation (#144, #165, #166).
- In-game menu language with `--play` / stuck in bbport.ini (#134).
- Steam Deck Game Mode: pressure-vessel's Vulkan variables hid the bundled drivers (#172).
- FSR 4.1.1 assets: the FP8 passes are recorded on RX 9000; GE-Proton from the AUR
  (`/usr/share/steam/compatibilitytools.d`) is found (#186).
- `AppImage --play` failed at once ("Software caused connection abort").
- HUD: white streaks at 21:9.
- Exit after the game stops itself: a crash in the online module ("terminate called without an
  active exception"); the log now names the game code that stopped.
- Pipelines from the disk cache: geometry-shader pipelines got an empty fetch shader; on NVIDIA
  the clip-distance discard shader was not saved; a draw without a pixel shader took the previous
  draw's fragment settings.
- Mouse: the camera no longer jumps when the mouse is captured; Escape no longer opens and at
  once closes the game menu.
- Building: Debian without a miniz package (#136); the build names the packages missing for the
  online module (#145).

### Faster

- Draw preparation uses more threads on CPUs with 12 or more hardware threads (a quarter of
  them, up to 8).
- The draw-recording stage spends 6 % less on buffer bindings through the memory module; it
  matters on slower CPUs.

**Known limits:** two-player co-op is tested by users, not by the author (one account); the
fixes inside BBSC's Windows `bbsc.dll` releases have no published source and are not included;
the new model on NVIDIA and Intel needs your reports; Polaris (RX 400/500) fixes are unverified.
Frame generation is in the code but hidden until its artifacts with upscaling are fixed.

The AppImage contains no game files. AMD's FSR 4 model assets are not bundled. `SHA256SUMS`
accompanies the download.

## Русский

# 0.6

bbport — аналог Wine + DXVK для одной игры: Bloodborne (v1.09) на Linux. Изменения после
[0.51](https://github.com/deadinside28/bloodborne_pc/releases/tag/0.51), включая пререлиз
0.6-pre1. Подробности и замеры:
[CHANGES_0.6-pre1.md](https://github.com/deadinside28/bloodborne_pc/blob/master/docs/CHANGES_0.6-pre1.md),
[CHANGES_after_0.6-pre1.md](https://github.com/deadinside28/bloodborne_pc/blob/master/docs/CHANGES_after_0.6-pre1.md).

Вопросы по проекту: Discord https://discord.gg/KYZRKk9CB — не сервер shadPS4.

### Новая модель памяти — по умолчанию на любой видеокарте

- *Авто* (по умолчанию в лаунчере) — теперь новая модель памяти и трансляции на **любой**
  видеокарте, включая NVIDIA и Intel, а не только на AMD. Память игры везде идёт через модуль
  памяти прослойки: без sparse-привязки памяти игры (на NVIDIA sparse binding держал видеочип
  секундами). На AMD модуль быстрее прежней sparse-арены (RX 7800 XT: маршрут по Ярнаму 4,30
  против 4,60 мс на кадр, часовня Одеона 234 против 213 FPS).
- Старая модель (как в 0.3) остаётся в лаунчере: *Модель памяти → Старая* (`BB_PC_MODEL=0`).
  Если драйвер не проходит проверки при запуске, порт сам переходит на копии в VRAM.
- **Пожалуйста, сообщайте**, как новая модель работает на NVIDIA и Intel (FPS, рывки, VRAM), с
  журналом.

### Новое

- **Онлайн: два пула, на сетевом стеке shadPS4 0.20.** На странице лаунчера *Онлайн* есть список
  *Пул*:
  - **Seamless Hunt** (по умолчанию): колокола, призывы, послания и пятна крови через публичные
    серверы BBSC, с **бесшовным кооперативом (BBSC)** — сессия переживает смерти, лампы, смену
    локаций, боссов и Сон охотника, до пяти охотников (*Онлайн → Бесшовный кооператив (BBSC)*, по
    умолчанию включён; нужен у всех игроков и один сервер). Учётную запись создайте на
    https://seamlesshunt.com и введите в лаунчере. Сохранённые старые адреса по умолчанию меняются
    сами.
  - **The Hunter's Dream** (thehuntersdream.com): вход через Discord прямо из лаунчера (код
    подтверждается на сайте); BBSC там нет.

  Онлайн-модуль теперь на сетевом, NP- и shadNet-коде shadPS4 v0.20 (`libbbnet20.so`); прежний
  модуль на shadp2p остался запасным (`BB_NET_GEN=19`). Публичный сервер shadPS4
  (`srv.shadps4.net`) с 12.10.2026 принимает только сборки shadPS4; порт сообщает свою версию и при
  отказе с самого начала остаётся офлайн, с причиной от сервера в журнале.
- **Любое соотношение сторон, на лету, хуками** (код игры не патчится): 4:3, 16:10, 16:9, 21:9,
  32:9; соотношение и разрешение — отдельные настройки (меню игры *Изображение*, оверлей,
  лаунчер). Камера видит больше по бокам (шире) или сверху и снизу (выше), HUD прижат к краям
  экрана, полноэкранные заливки и подложки меню — на весь вывод, метки над персонажами остаются на
  них; катсцены без HUD тоже широкие (#183, #151, #76, #35, #82).
- **Частота кадров — хуками, а не патчами**: выше 30 FPS главный цикл игры и 48 мест, привязанных к
  кадру 30 FPS, идут по реальному времени кадра (таймеры частей тела, которые двигает физика, на
  200 FPS шли в 6–7 раз быстрее; выше ~90 FPS бег падал до шага). Патчи сообщества остались для
  сравнения (`BB_FRAME_HOOKS=0`). **F3** (или пункт оверлея *Лимит кадров (сейчас)*) включает и
  выключает лимит 30 FPS прямо в игре.
- **Эффекты переключаются без перезапуска**: хроматическая аберрация, глубина резкости, размытие в
  движении, SSAO, сглаживание игры и тени динамических источников (меню игры и оверлей). Отражения
  SSR, заставки, свободная камера и debug menu по-прежнему применяются при запуске (меню об этом
  предупреждают).
- **Клавиатура и мышь:**
  - камера мышью поворачивает камеру игры напрямую (без мёртвой зоны стика, разгона и предела
    скорости); где камера поворот не берёт (захват цели, меню) — мышь ведёт правый стик;
  - **указатель мыши в меню игры**: наведение выбирает, левая кнопка — подтвердить, правая —
    назад, колесо — прокрутка; окно держит мышь, пока оно в фокусе, **F1** её отпускает;
  - **раскладка как в Dark Souls III**: WASD — движение, Space — уклонение, Left Shift — бег,
    Shift+Space — прыжок, F/Enter — взаимодействие, ЛКМ — атака, Shift+ЛКМ — сильная атака, ПКМ —
    выстрел, Q — флакон крови, E — трансформация оружия, Tab/R — быстрый предмет, 1/2 — оружие
    левой/правой руки, 3/4 — крестовина вверх/вниз, C — захват цели, Esc — меню (внутри меню Esc —
    «назад»), G — жесты, Backspace — личные вещи. Назначения могут быть сочетаниями («Left
    Shift+Mouse Left»); свои назначения сохраняются;
  - клавиши движения работают как стик: отклонение нарастает за 70 мс и плавно поворачивается, а не
    прыгает сразу в упор; короткие нажатия больше не теряются;
  - **подсказки кнопок** для используемого устройства, и в HUD тоже («Осмотреть» у ламп,
    надгробий, предметов): назначенная клавиша или кнопка мыши либо кнопки Xbox или другого
    геймпада; значки PlayStation — только с геймпадом PlayStation;
  - свой курсор порта (`BB_CURSOR=0` — системный); с патчем *Restore Debug Camera* мышь крутит
    свободную камеру.
- **Лаунчер**: выбор *Видеокарты* (ноутбуки с двумя видеочипами; хранится по имени); язык текста
  игры — все 21 язык, которые есть в игре (#249).
- **Occlusion-запросы игры по умолчанию настоящие** в новой модели, со счётом в разрешении игры
  при любом масштабе рендера.
- **Свой код вместо shadPS4** (кроме онлайн-модуля): командный процессор, описания GCN, транслятор
  шейдеров, рендер, кэши и платформенные библиотеки (`gpu/VENDOR.txt`).

### Исправления

- **Катсцена Лоуренса выше 30 FPS**: зависала (пул заданий отрисовки игры терял слот на каждом
  неудачном задании), а затем модели «взрывались» или пропадали (кэш ресурсов рендера игры на
  160 МиБ кончался на высоком FPS). Слоты возвращаются, а кэш получает 512 МиБ собственной памяти
  порта — не из той, куда игра грузит локации.
- **Одиночные кадры с картинкой, сдвинутой на пол-экрана или сжатой в угол** (светлые и тёмные
  вспышки, часто в катсцене Лоуренса): проход результатов occlusion-запросов затирал
  push-константы следующей полноэкранной отрисовки.
- **Вылет `guest raised debug exception 0xa0020007`** (код 23, #207): собственная проверка игры
  на число отсчётов ядра размытия; число теперь держится допустимым.
- **Вылет при загрузке сохранения** (`Bad PM4 packet`, код 23): главный поток игры и её поток
  рендера одновременно писали один буфер команд при инициализации эффекта; эту очистку теперь
  делает порт. Буферы команд к тому же копируются при отправке, поэтому буфер, который игра
  переиспользовала раньше времени, больше не разбирается наполовину переписанным.
- Вылет 139 с пропавшими таблицами ресурсов шейдеров при активной подгрузке.
- NVIDIA: 0.51 не запускалась (`CreateDevice: ErrorInitializationFailed`, код 134/-1): лаунчер
  теперь даёт `libcuda`, а устройство при нужде создаётся заново без расширений NGX (тогда DLSS
  выключен, FSR 3.1) (#135, #147, #152, #187, #189, #247, #252).
- Падения с RIP=0 на NVIDIA и Intel: расширение включено, а его команд нет (#161, #177, #181).
- Debian 13 и подобные с NVIDIA: лаунчер искал библиотеки NVIDIA только рядом с ICD и переходил на
  Intel (#205).
- **Падение FPS при взгляде на лучи света** (новая модель, сильнее всего на NVIDIA, #204, PR #210):
  тайлинг писал разбросанные тексели через шину, а две цели лучей 5 раз за кадр копировались через
  память; кадр видеочипа 17,0 → 10,4 мс в имитации NVIDIA на AMD.
- Новая модель в 4K (сцена 2560×1440): буфер размером с кадр писался через шину; 95 → 132 FPS на
  RX 7800 XT (#149).
- Новая модель: FPS падал на ~28 % после 15–20 минут игры с перемещениями по лампам; теперь нет.
- Новая модель: устаревшие копии в VRAM после гонки в обработчике ловушек записи («взрывающиеся»
  растянутые вершины, возможно, часть зависаний видеочипа).
- VRAM: на картах 8 ГБ сборщик текстур включался слишком рано (#204); при заполненной VRAM перед
  отказом выделения порт дожидается освобождаемых текстур (PR #206).
- Зависание загрузки при перемещении по лампам: дескрипторы потоков выше 40 бит не совпадали с
  владельцем, которого хранит блокировка игры (анализ автора PR #193; сделано заново).
- Падение на `0x2199310`, переполнена покадровая куча команд GX игры: обе кучи — 1 МиБ вместо
  256 КиБ (#162, #192, PR #224); защитные хуки, соотношение сторон и мышь работают и в старой
  модели (PR #214); смена размера окна и HDR безопасно пересоздают swapchain (PR #211).
- **Белая или испорченная картинка после загрузки другого сохранения с FSR 4.1.1**: временные
  буферы FSR 4.1.1 очищаются при сбросе сцены, а текстуры, очищенные видеочипом, обновляются.
- Вылет после выбора предмета (SSAO принудительно возвращался туда, где игра его выключила).
- Битая сцена при первой загрузке с переключённым на лету SSR: SSR снова применяется при запуске.
- Одиночные чёрные или серые кадры: вывод ждал не ту стадию работы кадра.
- RX 580 и другие GCN 3/4: остановка после первой катсцены или guest fault в редакторе персонажа
  (#203): метки конца конвейера там пишет процессор (`BB_GPU_LABELS=1` — видеочип). На такой
  видеокарте ещё не проверено.
- Старая модель: копии в VRAM копились до «VRAM is full» через час-два на картах 4 ГБ (#142).
- Пустой предпросмотр в редакторе персонажа (#178).
- Остановки декодера и форматов при подготовке отрисовок (#144, #165, #166).
- Язык меню игры с `--play` / застревал в bbport.ini (#134).
- Steam Deck, игровой режим: переменные Vulkan от pressure-vessel скрывали драйверы из пакета
  (#172).
- Ассеты FSR 4.1.1: на RX 9000 записываются и проходы FP8; находится GE-Proton из AUR
  (`/usr/share/steam/compatibilitytools.d`) (#186).
- `AppImage --play` падал сразу («Software caused connection abort»).
- HUD: белые полосы при 21:9.
- Выход после того, как игра сама остановилась: падение в онлайн-модуле («terminate called without
  an active exception»); журнал теперь называет код игры, который её остановил.
- Конвейеры из кэша на диске: с геометрическим шейдером получали пустой fetch-шейдер; на NVIDIA
  не сохранялся шейдер отбрасывания по clip distances; отрисовка без пиксельного шейдера брала
  настройки фрагментной стадии предыдущей.
- Мышь: камера больше не дёргается при захвате мыши; Escape больше не открывает и тут же не
  закрывает меню игры.
- Сборка: Debian без пакета miniz (#136); сборка называет пакеты, которых не хватает онлайн-модулю
  (#145).

### Быстрее

- Подготовка отрисовок использует больше потоков на процессорах с 12 и более аппаратными потоками
  (четверть из них, до 8).
- Стадия записи отрисовок тратит на 6 % меньше на привязки буферов через модуль памяти — важно на
  процессорах слабее.

**Известные ограничения:** кооператив вдвоём проверяют игроки, а не автор (одна учётная запись);
исправления внутри Windows-релизов `bbsc.dll` от BBSC не опубликованы в исходниках и сюда не
вошли; новая модель на NVIDIA и Intel ждёт ваших отчётов; исправления для Polaris (RX 400/500) не
проверены. Генерация кадров в коде есть, но скрыта, пока не исправлены её артефакты с апскейлером.

AppImage не содержит файлов игры. Ассеты модели FSR 4 от AMD не входят. К загрузке приложен
`SHA256SUMS`.
