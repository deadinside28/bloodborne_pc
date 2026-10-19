# 0.5-pre5 — pre-release for testing

Fifth 0.5 prerelease, built from the `testing` branch, after the tests of 0.5-pre4 on NVIDIA
(RTX 3050 4 GB, GTX 1060 6 GB). Changes are listed relative to
[0.5-pre4](https://github.com/deadinside28/bloodborne_pc/releases/tag/0.5-pre4). Plan and progress:
[EMULATION_REMOVAL_PLAN.ru.md](https://github.com/deadinside28/bloodborne_pc/blob/testing/docs/EMULATION_REMOVAL_PLAN.ru.md),
[TRANSLATOR_ARCHITECTURE.ru.md](https://github.com/deadinside28/bloodborne_pc/blob/testing/docs/TRANSLATOR_ARCHITECTURE.ru.md).

## English

- **FPS no longer falls the longer you play on cards with little VRAM.** On budgets under 8 GB
  the texture collector's pressure mark is 40 % of the budget, so usage stayed over it all the
  time and every texture unused for about 80 submissions (0.1–0.3 s) was evicted: turning the
  camera re-created and re-read over the bus about 12 000 textures per 45 s (VRAM budget limited
  to 4.5 GB and 3.7 GB on an RX 7800 XT). Under pressure a texture now goes once unused for 5 s
  (`BB_GC_PRESSURE_IDLE_SECONDS`), over the critical mark after 1 s, and only then by
  submissions as before: 144–167 re-created in the same test.
- **Some launches no longer run the GPU 1.3–1.7 times slower.** In about half of the launches
  (more often with FSR) the new memory model put a few blocks the CPU writes into a VRAM copy as
  "volatile" blocks, refreshed from the game's memory before every binding that read them. A
  refresh is a copy, and a copy cannot run inside a render pass: the game's light volumes (one
  draw each) broke their pass up to ~100 times a frame, each break storing and loading the
  attachments again — 92 000–109 000 breaks per 5 s from these copies instead of ~700, GPU
  5.4–7.3 ms a frame instead of 4.3 on a recorded route. The same happened on NVIDIA. Volatile
  blocks are now off (`BB_LAYER_VOLATILE=1` turns them on; reads in an open render pass then go
  in place): 10 of 10 runs fast (4.29–4.35 ms with FSR, 4.17 without an upscaler), also with the
  VRAM budget limited to 4.5 GB.
- **Occlusion results before later labels.** The results of the game's occlusion queries are now
  written before any later GPU label in the stream, as on the PS4 (`BB_OCCLUSION_LABEL_ORDER=0` —
  the old order). No speed change: the game reads them 2–10 ms after the query and finds them
  written either way.
- **The new memory model uses the same path on AMD as on NVIDIA.** VRAM copies of the game's
  memory are kept by the port itself on every GPU; AMD's sparse arena is left as
  `BB_LAYER_MEMORY=0`. No `vkQueueBindSparse` calls by default.
- **One owner for write traps.** The runtime now keeps the page protections the GPU side uses
  to notice CPU writes, with a reason per page: VRAM copies and textures trap the same pages
  without one clearing the other's trap; the game's own `mprotect` keeps them, a new mapping
  starts without them. Pages under textures are trapped by default (`BB_IMAGE_TRAPS=0` — as
  before); writes by the runtime's libc functions and the game's loaders announce themselves
  first and do not fault. On the recorded route: same GPU time as without the traps, 4–29 trap
  hits per 5 s.
- **Command processor leftovers on the GPU timeline** (Bloodborne uses neither; for other games):
  `MEM_SEMAPHORE` signals once the GPU has finished the work before them, in stream order
  (`BB_MEM_SEMAPHORE_ORDER=0` — when decoded); the GPU clock of end-of-pipe timestamps and
  `COPY_DATA` is the PS4's 100 MHz, written by the GPU itself from Vulkan timestamps, in one time
  line with the CPU's (`BB_GPU_TIMESTAMPS=0` — written by the CPU). The in-game PM4 self-test
  checks both (11 of 11).
- **Game-specific code in one place.** Bloodborne's code hooks, the addresses of its per-frame
  writers and the hash of its buffer copy shader moved into a game profile (`gpu/games/`),
  chosen by the game's TITLE_ID. `BB_GAME_PROFILE=none` runs the game as an unknown one,
  `BB_GUEST_HOOKS=0` without code hooks: a step towards other games on the same translator.
- **Texture mips detiled with their own tiling** (upstream shadPS4 #5196): small mips of tiled
  textures, stored micro-tiled, and thin 3D PRT textures are now detiled the way the GPU stores
  them.
- **In-game settings rows as sliders.** In the game's System menu the quality preset, output
  resolution and model detail rows are sliders with ranges (the help line lists them); the pop-up
  list let the rows below show through it. The upscaler stays a list, the last row of its page.
- **Diagnostics:** frames over 40 ms show where the new model's VRAM copies spent the time
  (traps, page table entries: staging, guest view, copies, copy command); `BB_IMAGE_SYNC_LOG=1`
  prints texture refreshes per 5 s; `BB_PASS_BREAK_TRACE=1` — who ended a render pass that began
  again unchanged; `BB_OCCLUSION_READ_TRACE=1` — when the game first reads its occlusion
  counters and whether the results were there. The runtime's traps can now also stop reads (no
  access), for diagnostics so far.

**How to test:** enable *New memory and translation model* in the launcher, or set
`BB_PC_MODEL=1`; the mode remains **off by default**. NVIDIA owners with 4–6 GB: please compare a
longer session (10+ minutes, the same area) with pre4 and send the log with `BB_FRAME_STATS=1`
and the GPU profile (`BB_GPU_PROFILE=1`).

**Validation and limits:** RX 7800 XT/RADV: recorded 25 s route and camera turns in place,
VRAM budget limited to 4.5 and 3.7 GB (`BB_VRAM_LIMIT_MB`); runtime tests (write traps by reason,
kept across `mprotect`, forgotten on unmap), unit tests (106) and the in-game PM4 self-test
(11 of 11: occlusion, semaphores and the GPU clock included) passed. NVIDIA's own path cannot
run on AMD: the GC fix is not yet confirmed on NVIDIA. Banding in dark gradients is not confirmed
fixed. Without the game profile (`BB_GAME_PROFILE=none`) the recorded route renders the same and
runs as fast (3 of 3 runs, 4.29–4.30 ms), with more write-trap hits.

The AppImage contains no game files. Proprietary FSR 4 model assets and NVIDIA's DLSS SDK/libraries
are not bundled. `SHA256SUMS` accompanies the download.

## Русский

- **FPS больше не падает со временем на картах с малым объёмом VRAM.** При бюджете меньше 8 ГБ
  отметка давления сборщика текстур — 40 % бюджета, поэтому использование всё время было выше
  неё и выгружалась каждая текстура, не использованная около 80 отправок (0,1–0,3 с): поворот
  камеры пересоздавал и заново читал через шину около 12 000 текстур за 45 с (RX 7800 XT с
  бюджетом VRAM, ограниченным до 4,5 и 3,7 ГБ). Теперь под давлением текстура уходит, если не
  использовалась 5 с (`BB_GC_PRESSURE_IDLE_SECONDS`), выше критической отметки — через 1 с, и
  только потом по числу отправок, как раньше: в том же тесте пересоздано 144–167.
- **Некоторые запуски больше не гоняют видеокарту в 1,3–1,7 раза медленнее.** Примерно в половине
  запусков (чаще с FSR) новая модель памяти клала несколько блоков, в которые пишет процессор, в
  копию в VRAM как «летучие»: они обновлялись из памяти игры перед каждой привязкой на чтение.
  Обновление — это копирование, а копирование не может идти внутри прохода рендера: объёмы света
  игры (по одной отрисовке на каждый) разрывали свой проход до ~100 раз за кадр, и при каждом
  разрыве вложения сохранялись и загружались заново — 92 000–109 000 разрывов за 5 с от этих
  копий вместо ~700, 5,4–7,3 мс видеочипа за кадр вместо 4,3 на записанном маршруте. На NVIDIA
  было то же самое. Теперь летучие блоки выключены (`BB_LAYER_VOLATILE=1` — включить; чтения
  внутри открытого прохода тогда идут на месте): быстрые 10 прогонов из 10 (4,29–4,35 мс с FSR,
  4,17 без апскейлера), в том числе с бюджетом VRAM 4,5 ГБ.
- **Результаты occlusion — до последующих меток.** Результаты occlusion-запросов игры теперь
  пишутся раньше любой следующей метки видеокарты в потоке, как на PS4
  (`BB_OCCLUSION_LABEL_ORDER=0` — старый порядок). На скорость не влияет: игра читает их через
  2–10 мс после запроса и в обоих случаях находит уже записанными.
- **Новая модель памяти на AMD идёт тем же путём, что и на NVIDIA.** Копии памяти игры в VRAM
  порт держит сам на любой видеокарте; sparse-арена AMD осталась как `BB_LAYER_MEMORY=0`. По
  умолчанию — ни одного вызова `vkQueueBindSparse`.
- **Один владелец ловушек записи.** Защиту страниц, по которой видеочасть замечает записи
  процессора, теперь ведёт рантайм, с причиной на каждую страницу: копии в VRAM и текстуры
  ставят ловушки на одни и те же страницы, не снимая чужих; `mprotect` игры их сохраняет, новое
  отображение начинается без них. Страницы под текстурами под ловушками по умолчанию
  (`BB_IMAGE_TRAPS=0` — как раньше); записи функций libc рантайма и загрузчиков игры объявляются
  заранее и ловушек не вызывают. На записанном маршруте — то же время видеочипа, что без ловушек,
  4–29 срабатываний за 5 с.
- **Остатки командного процессора — во времени видеочипа** (Bloodborne их не использует; для
  других игр): сигнал `MEM_SEMAPHORE` — после завершения работы видеочипа перед ним, в порядке
  потока (`BB_MEM_SEMAPHORE_ORDER=0` — при разборе); часы видеочипа в метках конца конвейера и
  `COPY_DATA` — 100 МГц, как у PS4, пишет сам видеочип по меткам времени Vulkan, в одной шкале с
  часами процессора (`BB_GPU_TIMESTAMPS=0` — пишет процессор). PM4-самопроверка в игре проверяет
  и то, и другое (11 из 11).
- **Код под конкретную игру — в одном месте.** Перехваты кода Bloodborne, адреса её кода,
  пишущего данные каждый кадр, и хеш её шейдера копирования вынесены в профиль игры
  (`gpu/games/`), который выбирается по TITLE_ID. `BB_GAME_PROFILE=none` — игра как незнакомая,
  `BB_GUEST_HOOKS=0` — без перехватов кода: шаг к другим играм на том же трансляторе.
- **Мип-уровни текстур раскладываются со своей раскладкой** (shadPS4 #5196): мелкие мипы
  тайловых текстур, хранящиеся в микротайлах, и тонкие 3D PRT-текстуры теперь раскладываются
  так, как их хранит видеокарта.
- **Строки настроек в игре — ползунки.** В системном меню игры пресет качества, разрешение
  вывода и детализация моделей — ползунки с диапазонами (они перечислены в подсказке);
  всплывающий список просвечивал строками под ним. Апскейлер остался списком, последней строкой
  своей страницы.
- **Диагностика:** для кадров дольше 40 мс видно, на что ушло время копий новой модели в VRAM
  (ловушки, записи таблицы страниц: запрос в staging, вид памяти игры, копии, команда
  копирования); `BB_IMAGE_SYNC_LOG=1` — обновления текстур за 5 с; `BB_PASS_BREAK_TRACE=1` — кто
  завершил проход рендера, начатый затем заново без изменений; `BB_OCCLUSION_READ_TRACE=1` — когда
  игра впервые читает счётчики occlusion и были ли там уже результаты. Ловушки рантайма теперь
  умеют останавливать и чтение (страница без доступа) — пока для диагностики.

**Как проверить:** включить *Новую модель памяти и трансляции* в лаунчере или задать
`BB_PC_MODEL=1`; режим по-прежнему **выключен по умолчанию**. Владельцы NVIDIA на 4–6 ГБ:
пожалуйста, сравните с pre4 длинную сессию (10+ минут в одном месте) и пришлите лог с
`BB_FRAME_STATS=1` и профилем видеокарты (`BB_GPU_PROFILE=1`).

**Проверки и ограничения:** RX 7800 XT/RADV: записанный маршрут на 25 с и повороты камеры на
месте, бюджет VRAM ограничен до 4,5 и 3,7 ГБ (`BB_VRAM_LIMIT_MB`); тесты рантайма (ловушки записи
по причинам, сохраняются через `mprotect`, забываются при снятии отображения), юнит-тесты (106) и
PM4-самопроверка в игре (11 из 11, вместе с occlusion, семафорами и часами видеочипа)
пройдены. Собственный путь NVIDIA на AMD не запустить: исправление сборщика на NVIDIA ещё не
подтверждено. Полосы в тёмных градиентах — исправление не подтверждено. Без профиля игры
(`BB_GAME_PROFILE=none`) записанный маршрут выглядит так же и идёт так же быстро (3 прогона из 3,
4,29–4,30 мс), срабатываний ловушек записи больше.

Игровых файлов в AppImage нет. Проприетарные модели FSR 4 и SDK/библиотеки DLSS не включены.
К загрузке приложен `SHA256SUMS`.
