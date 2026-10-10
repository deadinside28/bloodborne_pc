#include "bbport_strings.h"
#include "bbport_settings.h"

#include <algorithm>

namespace BbStrings {

struct TextGroup {
    const char* en;
    const char* pt_br;
    const char* ru;
};

static constexpr TextGroup kTable[static_cast<size_t>(StringId::Count)] = {
    // WindowTitle
    {
        "Bloodborne — Settings",
        "Bloodborne — Configurações",
        "Bloodborne — настройки"
    },
    // FpsMs
    {
        "ms",
        "ms",
        "мс"
    },
    // SectionLanguage
    {
        "Language",
        "Idioma",
        "Язык"
    },
    // LanguageInterface
    {
        "Interface Language",
        "Idioma da Interface",
        "Язык интерфейса"
    },
    // SectionUpscaler
    {
        "Temporal Upscaler",
        "Upscaler Temporal",
        "Временной апскейлер"
    },
    // UpscalerCombo
    {
        "Upscaler",
        "Upscaler",
        "Апскейлер"
    },
    // UpscalerOff
    {
        "Off",
        "Desligado",
        "Выкл"
    },
    // UpscalerTaa
    {
        "TAA (Native AA)",
        "TAA (Anti-aliasing Nativo)",
        "TAA (нативное сглаживание)"
    },
    // UpscalerNotSupported
    {
        "— not supported by GPU",
        "— não suportado pela GPU",
        "— не поддерживается видеокартой"
    },
    // UpscalerWorkInProgress
    {
        "— work in progress",
        "— em desenvolvimento",
        "— в работе"
    },
    // Fsr4Unavailable
    {
        "FSR 4 unavailable",
        "FSR 4 indisponível",
        "FSR 4 недоступен"
    },
    // Fsr4ActiveModeSelected
    {
        "Active mode selected above. FSR 4 can be chosen again.",
        "Modo ativo selecionado acima. O FSR 4 pode ser selecionado novamente.",
        "Активен режим, выбранный выше. FSR 4 можно выбрать снова."
    },
    // HintFsr411
    {
        "FSR 4.1.1 in INT8 mode: model from AMD 4.1.1 DLL, replayed in Vulkan (bit-exact result). One model for Native..Performance and separate for Ultra Performance. Assets: tools/fsr4cap/build_assets.sh (needs DLL and Proton).",
        "FSR 4.1.1 em modo INT8: modelo da DLL AMD 4.1.1, reproduzido em Vulkan (resultado idêntico à DLL). Um modelo para Native..Performance e outro para Ultra Performance. Recursos: tools/fsr4cap/build_assets.sh.",
        "FSR 4.1.1 в режиме INT8: модель из DLL AMD 4.1.1, воспроизведённая в Vulkan (результат совпадает с DLL). Одна модель для Native..Performance и отдельная для Ultra Performance. Ассеты: tools/fsr4cap/build_assets.sh (нужны DLL и Proton)."
    },
    // HintFsr411Fp8
    {
        "FSR 4.1.1 in FP8 mode (Float8 cooperative matrices on RDNA4): model from AMD 4.1.1 DLL, replayed in Vulkan with native float precision.",
        "FSR 4.1.1 em modo FP8 (Float8 / matrizes cooperativas no RDNA4): modelo da DLL AMD 4.1.1, reproduzido em Vulkan com precisão float nativa.",
        "FSR 4.1.1 в режиме FP8 (Float8 / кооперативные матрицы RDNA4): модель из DLL AMD 4.1.1, воспроизведённая в Vulkan с точностью float."
    },
    // HintFsr4
    {
        "FSR 4 in INT8 mode (v07 model from AMD FidelityFX SDK source). Higher quality than FSR 3.1, heavier pass. Preset change rebuilds model (brief pause). Assets: tools/fetch_fsr4_assets.sh.",
        "FSR 4 em modo INT8 (modelo v07 do código-fonte AMD FidelityFX SDK). Qualidade superior ao FSR 3.1, porém mais pesado. Mudar o preset recompila o modelo (breve pausa). Recursos: tools/fetch_fsr4_assets.sh.",
        "FSR 4 в режиме INT8 (модель v07 из исходников AMD FidelityFX SDK). Качество выше, чем у FSR 3.1, но проход тяжелее. Смена пресета пересобирает модель (короткая пауза). Ассеты: tools/fetch_fsr4_assets.sh."
    },
    // Fsr4AutoExposure
    {
        "FSR 4: auto-exposure",
        "FSR 4: auto-exposição",
        "FSR 4: авто-экспозиция"
    },
    // Fsr4InvertJitter
    {
        "FSR 4: invert jitter sign",
        "FSR 4: inverter sinal de jitter",
        "FSR 4: обратный знак jitter"
    },
    // HintFsr4Ghosting
    {
        "Ghosting test: FSR 4 normalizes color by exposure and decides when to discard previous frames. Applies immediately.",
        "Teste para ghosting: o FSR 4 normaliza a cor pela exposição para descartar quadros anteriores. Aplica-se imediatamente.",
        "Проверка при гостинге: сеть FSR 4 нормирует цвет по экспозиции и по ней решает, когда отбросить прошлые кадры. Меняются сразу, без перезапуска."
    },
    // PresetCombo
    {
        "Preset",
        "Predefinição",
        "Пресет"
    },
    // PresetRenderWord
    {
        "render",
        "render",
        "рендер"
    },
    // HintTaa
    {
        "TAA antialiases scene at output resolution without FSR model or upscaling. Saved FSR preset restores upon selecting FSR.",
        "O TAA suaviza a cena na resolução de saída, sem modelo FSR ou upscaling. O preset do FSR é restaurado ao selecioná-lo.",
        "TAA сглаживает сцену в разрешении вывода, без модели FSR и апскейлинга. Сохранённый пресет FSR восстановится при выборе FSR."
    },
    // ActiveSceneRender
    {
        "Active scene render",
        "Renderização ativa da cena",
        "Активный рендер сцены"
    },
    // StartupPreset
    {
        "Startup preset",
        "Predefinição inicial",
        "Пресет при запуске"
    },
    // HintRenderResAuto
    {
        "Non-1080p output renders entire game at preset resolution (startup patch): fastest on Steam Deck/weak GPUs. Preset/output change requires restart. 'Dynamic resolution' below enables change without restart (postprocessing stays 1080p, slower).",
        "Em saídas diferentes de 1080p o jogo renderiza na resolução do preset (patch inicial): mais rápido no Steam Deck e GPUs modestas. Alterações exigem reinício. A opção 'Mudança em tempo real' abaixo permite troca sem reiniciar.",
        "При выводе не 1080p вся игра рисуется в разрешении пресета (патч при запуске): это быстрее всего на Steam Deck и слабых GPU. Смена пресета или разрешения вывода — после перезапуска. Пункт «Смена разрешения на лету» ниже включает смену без перезапуска (постобработка тогда остаётся в 1080p, медленнее)."
    },
    // HintRenderResManual
    {
        "BB_RENDER_RES fixes scene size at start. Remove this variable to change resolution and presets without restarting.",
        "BB_RENDER_RES fixa o tamanho da cena na inicialização. Remova esta variável para alterar resoluções sem reiniciar.",
        "BB_RENDER_RES фиксирует размер сцены при запуске. Уберите эту явную переменную для смены разрешения и пресетов без перезапуска игры."
    },
    // HintRenderResLive
    {
        "Native AA: FSR acts as antialiasing. Other presets reduce render resolution relative to output. UI renders at output resolution. Preset applies next frame without restart.",
        "Native AA: o FSR age como anti-aliasing. Outros presets reduzem a resolução da cena em relação à saída. A interface renderiza na resolução de saída. Preset aplica-se no próximo quadro.",
        "Native AA: FSR работает как сглаживание. Остальные пресеты уменьшают разрешение отрисовки сцены относительно вывода. Интерфейс рисуется в разрешении вывода. Пресет применяется со следующего кадра без перезапуска игры."
    },
    // SharpeningRcas
    {
        "Sharpening (RCAS)",
        "Nitidez (RCAS)",
        "Резкость (RCAS)"
    },
    // SharpnessIntensity
    {
        "Sharpness intensity",
        "Intensidade de nitidez",
        "Сила резкости"
    },
    // HintSharpness
    {
        "Up to 1.0 is upscaler's native RCAS. Above 1.0 adds an extra RCAS pass. Ctrl+Click to type exact value.",
        "Até 1.0 é o RCAS nativo do upscaler. Acima de 1.0 adiciona um passe extra de RCAS. Ctrl+Clique para digitar o valor exato.",
        "До 1 — резкость самого апскейлера (RCAS). Выше 1 добавляется ещё один проход RCAS. Ctrl+клик по ползунку — ввести точное значение."
    },
    // SubpixelJitter
    {
        "Subpixel jitter",
        "Deslocamento subpixel (jitter)",
        "Субпиксельный сдвиг (jitter)"
    },
    // HintJitter
    {
        "Each frame the scene shifts by a fraction of a pixel, allowing the upscaler to reconstruct more details. Without it, only history smoothing occurs.",
        "A cada quadro a cena é deslocada por uma fração de pixel, permitindo ao upscaler reconstruir mais detalhes. Sem isso, ocorre apenas suavização temporal.",
        "Каждый кадр сцена сдвигается на долю пикселя, и апскейлер собирает из нескольких кадров больше деталей. Без него получается только сглаживание по истории."
    },
    // SectionReactivity
    {
        "Reactivity Mask",
        "Máscara de Reatividade",
        "Маска реактивности"
    },
    // ReactivityEnable
    {
        "Enable mask",
        "Ativar máscara",
        "Включить маску"
    },
    // HintReactivity
    {
        "Marks transparent effects (particles, fog) so the upscaler relies less on previous frames. Reduces ghosting behind particles, but restores jitter under them.",
        "Marca efeitos transparentes (partículas, névoa) para que o upscaler dependa menos de quadros anteriores. Reduz ghosting atrás de efeitos.",
        "Помечает прозрачные эффекты (частицы, дымку), чтобы апскейлер меньше опирался на прошлые кадры. Меньше шлейфов за эффектами, но под ними возвращается дрожание."
    },
    // ReactivityScale
    {
        "Scale",
        "Escala",
        "Масштаб"
    },
    // ReactivityThreshold
    {
        "Threshold",
        "Limiar",
        "Порог"
    },
    // ReactivityMax
    {
        "Maximum",
        "Máximo",
        "Максимум"
    },
    // ReactivityShowMask
    {
        "Show mask (debug)",
        "Exibir máscara (depuração)",
        "Показать маску (отладка)"
    },
    // ObjectMotionVectors
    {
        "Character motion vectors",
        "Vetores de movimento de personagens",
        "Векторы движения персонажей"
    },
    // HintObjectMotion
    {
        "Precise vectors for animated objects: clothes and weapons break apart less during motion. Static scene receives no extra pass. Applies after restart.",
        "Vetores precisos para objetos animados: roupas e armas quebram menos durante o movimento. A cena estática não recebe passe extra. Aplica-se após reiniciar.",
        "Точные векторы для анимированных объектов: одежда и оружие меньше рассыпаются при движении. Статичная сцена не получает дополнительный проход. Изменение применяется после перезапуска игры."
    },
    // ObjectMotionShowDebug
    {
        "Show motion vectors (debug)",
        "Exibir vetores de movimento (depuração)",
        "Показать векторы движения (отладка)"
    },
    // HintObjectMotionColors
    {
        "Red/Green: horizontal/vertical motion (8 px = full brightness). Blue: pixel received exact object vector, not just camera motion. Moving objects without blue or red/green are treated as stationary by upscaler, causing trails.",
        "Vermelho/Verde: movimento horizontal/vertical (8 px = brilho total). Azul: o pixel recebeu vetor exato do objeto, não apenas da câmera. Objetos em movimento sem azul/vermelho/verde causam rastros.",
        "Красный/зелёный: движение по горизонтали/вертикали (8 пикселей = полная яркость). Синий: пиксель получил точный вектор объекта, а не только движение камеры. Движущийся предмет без синего и без красного/зелёного апскейлер считает неподвижным, отсюда шлейф."
    },
    // SectionOutputResolution
    {
        "Output Resolution",
        "Resolução de Saída",
        "Разрешение вывода"
    },
    // OutputResolutionCombo
    {
        "Output Resolution",
        "Resolução de Saída",
        "Разрешение вывода"
    },
    // HintOutputResolutionFixed
    {
        "Final frame and UI size. Preset sets scene size relative to output: 4K Performance = 1920x1080. Applies after restart.",
        "Tamanho final do quadro e interface. O preset define o tamanho da cena relativo à saída: 4K Performance = 1920x1080. Aplica-se após reiniciar.",
        "Размер готового кадра и интерфейса. Пресет задаёт размер сцены относительно вывода: 4K Performance = 1920x1080. Применяется после перезапуска игры."
    },
    // HintOutputResolutionLive
    {
        "Final frame and UI size changes on next frame boundary. Preset sets scene size relative to output: 4K Performance = 1920x1080. Size change resets FSR history and may cause a brief pause.",
        "Tamanho do quadro final e interface muda no próximo quadro. O preset define a resolução da cena em relação à saída. Mudar o tamanho reseta o histórico do FSR.",
        "Размер готового кадра и интерфейса меняется на границе следующего кадра. Пресет задаёт размер сцены относительно вывода: 4K Performance = 1920x1080. Смена размера сбрасывает историю FSR и может вызвать короткую паузу."
    },
    // DynamicResolutionChange
    {
        "Dynamic Resolution Change",
        "Mudança de Resolução em Tempo Real",
        "Смена разрешения на лету"
    },
    // LiveModeAuto
    {
        "Auto (by GPU)",
        "Automático (pela GPU)",
        "Авто (по видеокарте)"
    },
    // LiveModeDisabled
    {
        "Disabled (faster)",
        "Desativado (mais rápido)",
        "Выключена (быстрее)"
    },
    // LiveModeEnabled
    {
        "Enabled",
        "Ativado",
        "Включена"
    },
    // HintLiveResolution
    {
        "Enabled: output resolution and preset change without restarting, but postprocessing remains 1080p (noticeably slower on Steam Deck and older GPUs). Disabled: everything renders at preset resolution, changes via restart. Auto enables it on powerful discrete GPUs. Applies after restart.",
        "Ativado: resolução de saída e presets mudam sem reiniciar, mas o pós-processamento permanece em 1080p (mais lento em hardware modesto). Desativado: renderização na resolução do preset, mudanças exigem reinício. Automático ativa em GPUs potentes.",
        "Включена: разрешение вывода и пресет меняются без перезапуска, но постобработка игры остаётся в 1080p — на Steam Deck и старых видеокартах это заметно медленнее. Выключена: всё рисуется в разрешении пресета, смена — через перезапуск. Авто включает её на мощных дискретных видеокартах. Применяется после перезапуска игры."
    },
    // SectionGameEffects
    {
        "Game Effects (Restart Required)",
        "Efeitos do Jogo (Reinício Necessário)",
        "Эффекты игры (после перезапуска)"
    },
    // LodCombo
    {
        "Model Detail (LOD)",
        "Detalhes dos Modelos (LOD)",
        "Детализация моделей"
    },
    // LodMax
    {
        "Maximum (-2)",
        "Máximo (-2)",
        "Максимальная (-2)"
    },
    // LodDefault
    {
        "Game Default",
        "Padrão do Jogo",
        "Как в игре"
    },
    // LodLower
    {
        "Lower (1)",
        "Mais Baixo (1)",
        "Ниже (1)"
    },
    // LodMin
    {
        "Minimum (2)",
        "Mínimo (2)",
        "Минимальная (2)"
    },
    // HintGameEffects
    {
        "Effects are enabled or disabled via game patches at startup (patches/Bloodborne.xml). Motion blur and dynamic shadows noticeably increase GPU load.",
        "Efeitos são ativados/desativados via patches na inicialização (patches/Bloodborne.xml). Desfoque de movimento e sombras dinâmicas pesam na GPU.",
        "Эффекты включаются и выключаются патчами игры при запуске (patches/Bloodborne.xml). Размытие в движении и тени от динамических источников заметно нагружают GPU."
    },
    // HintSpecialControls
    {
        "Free camera: hold Cross and press L3 (keyboard: Space + Z). Debug menu: left touchpad / Tab (needs DbgFont14h.ccm and DbgFont14h.tpf in dvdroot_ps4/font from Nexus #253). Right touchpad: Backspace.",
        "Câmera livre: segure Cross e pressione L3 (teclado: Espaço + Z). Menu de debug: touchpad esquerdo / Tab (requer fontes DbgFont14h no dvdroot_ps4/font). Touchpad direito: Backspace.",
        "Свободная камера: удерживайте Cross и нажимайте L3 (клавиатура: Space + Z). Debug menu: левый touchpad / Tab. Нужны DbgFont14h.ccm и DbgFont14h.tpf в dvdroot_ps4/font из мода Nexus #253. Правый touchpad: Backspace."
    },
    // RestartWarning
    {
        "Changes will apply after restarting the game",
        "As alterações serão aplicadas após reiniciar o jogo",
        "Изменения применятся после перезапуска игры"
    },
    // ApplyAndRestart
    {
        "Apply and Restart Game",
        "Aplicar e Reiniciar o Jogo",
        "Применить и перезапустить игру"
    },
    // SectionMisc
    {
        "Miscellaneous",
        "Diversos",
        "Прочее"
    },
    // ShowFpsCorner
    {
        "Show FPS in Corner",
        "Contador de FPS no Canto",
        "Счётчик FPS в углу"
    },
    // PuddleReflections
    {
        "Water Puddle Reflections",
        "Reflexos em Poças d'Água",
        "Отражения в лужах"
    },
    // HintPuddleReflections
    {
        "Disabled: fixes inverted buildings, flickering and streaks in the sky. Enabled: renders the game's planar reflections.",
        "Desativado: remove prédios invertidos, falhas e faixas no céu. Ativado: renderiza os reflexos planares originais.",
        "Выключено: убирает перевёрнутые здания и полосы на небе. Включено: исходные плоские отражения."
    },
    // DrawPipe
    {
        "Draw Pipe (Two-Stage Pipeline)",
        "Draw Pipe (Pipeline em 2 Estágios)",
        "Двухстадийный конвейер (Draw Pipe)"
    },
    // HintDrawPipe
    {
        "Hybrid: keeps +20-30% FPS in gameplay, runs synchronously in cutscenes without glitches.",
        "Híbrido: mantém +20-30% FPS no gameplay e roda de forma síncrona em cutscenes sem falhas visuais.",
        "Гибридный: сохраняет +20-30% FPS в игре и работает синхронно в кат-сценах без визуальных сбоев."
    },
    // DrawPipeModeOff
    {
        "Off (Standard)",
        "Desligado (Modo Padrão)",
        "Выключен (Стандартный)"
    },
    // DrawPipeModeOn
    {
        "On (Always 2 stages)",
        "Ligado (Sempre 2 estágios)",
        "Включён (всегда 2 стадии)"
    },
    // DrawPipeModeHybrid
    {
        "Hybrid (Recommended)",
        "Híbrido (Recomendado)",
        "Гибридный (Рекомендуется)"
    },
    // CloseButton
    {
        "Close",
        "Fechar",
        "Закрыть"
    },
    // SavedToIni
    {
        "Settings saved to bbport.ini",
        "Configurações salvas em bbport.ini",
        "Настройки сохраняются в bbport.ini"
    },
    // PromptKeyboardHelp
    {
        "Keyboard: type, Backspace = delete, Enter = OK, Esc = cancel",
        "Teclado: digite, Backspace = apagar, Enter = OK, Esc = cancelar",
        "Клавиатура: ввод, Backspace = удалить, Enter = OK, Esc = отмена"
    },
    // KbSpace
    {
        "Space",
        "Espaço",
        "Пробел"
    },
    // KbBackspace
    {
        "Delete",
        "Apagar",
        "Стереть"
    },
    // KbClear
    {
        "Clear",
        "Limpar",
        "Очистить"
    },
    // KbConfirm
    {
        "Confirm (OK)",
        "Confirmar (OK)",
        "Готово (OK)"
    },
    // KbCancel
    {
        "Cancel",
        "Cancelar",
        "Отмена"
    },
    // KbCaps
    {
        "Caps",
        "Caps",
        "Регистр"
    },
    // PromptGamepadHelp
    {
        "Gamepad: (X/A) Type  (Square/X) Delete  (Triangle/Y) Space  (L1/R1) Caps  (Start) OK  (Circle/B) Cancel",
        "Controle: (X/A) Inserir  (Quadrado/X) Apagar  (Triângulo/Y) Espaço  (L1/R1) Caps  (Start) OK  (Círculo/B) Cancelar",
        "Геймпад: (Крест/A) Ввод  (Квадрат/X) Удалить  (Треугольник/Y) Пробел  (L1/R1) Регистр  (Start) OK  (Круг/B) Отмена"
    },
    // SectionFrameGeneration
    {
        "Frame Generation",
        "Geração de Quadros (Frame Generation)",
        "Генерация кадров (Frame Generation)"
    },
    // FrameGenerationEnable
    {
        "Enable FSR 3.1 Frame Generation",
        "Ativar FSR 3.1 Frame Generation",
        "Включить FSR 3.1 Frame Generation"
    },
    // HintFrameGeneration
    {
        "Generates synthetic interpolated frames between real frames. Requires minImageCount + 2 swapchain support. Independent from the selected upscaler.",
        "Gera quadros interpolados entre quadros reais. Requer suporte a minImageCount + 2 na swapchain. Opera de forma independente do upscaler selecionado.",
        "Генерирует интерполированные кадры между реальными. Требует поддержки minImageCount + 2 в swapchain. Независимо от выбранного апскейлера."
    },
    // FrameGenerationNotSupported
    {
        "Frame Generation is not supported on this GPU or swapchain configuration.",
        "Frame Generation não é suportado nesta GPU ou configuração de swapchain.",
        "Генерация кадров не поддерживается этой видеокартой или конфигурацией swapchain."
    }
};

const char* Get(StringId id, int lang) {
    const size_t index = static_cast<size_t>(id);
    if (index >= static_cast<size_t>(StringId::Count)) {
        return "";
    }
    const auto& group = kTable[index];
    switch (lang) {
    case BbSettings::LangPortuguese:
        return group.pt_br ? group.pt_br : group.en;
    case BbSettings::LangRussian:
        return group.ru ? group.ru : group.en;
    case BbSettings::LangEnglish:
    default:
        return group.en ? group.en : "";
    }
}

const char* EffectLabel(int effect_index, int lang) {
    static constexpr TextGroup kEffects[BbSettings::EffectCount] = {
        {"Chromatic Aberration", "Aberração Cromática", "Хроматическая аберрация"},
        {"Depth of Field (DoF)", "Profundidade de Campo (DoF)", "Глубина резкости (DoF)"},
        {"Motion Blur", "Desfoque de Movimento", "Размытие в движении"},
        {"SSAO Ambient Occlusion", "Oclusão Ambiental SSAO", "Затенение SSAO"},
        {"Game's Native AA", "Anti-Aliasing Nativo do Jogo", "Собственное сглаживание игры"},
        {"Dynamic Light Shadows", "Sombras de Luzes Dinâmicas", "Тени от динамических источников"},
        {"SSR Reflections (not in base game)", "Reflexos SSR (não existia no jogo base)", "Отражения SSR (не было в игре)"},
        {"Skip Intro Cutscenes", "Pular Telas Iniciais", "Пропуск заставок при запуске"},
        {"Free Camera (Cross + L3)", "Câmera Livre (Cross + L3)", "Свободная камера (Cross + L3)"},
        {"Debug Menu (requires fonts)", "Menu de Depuração (requer fontes)", "Debug menu (нужны файлы шрифтов)"},
    };
    if (effect_index < 0 || effect_index >= BbSettings::EffectCount) {
        return "";
    }
    const auto& group = kEffects[effect_index];
    switch (lang) {
    case BbSettings::LangPortuguese:
        return group.pt_br;
    case BbSettings::LangRussian:
        return group.ru;
    case BbSettings::LangEnglish:
    default:
        return group.en;
    }
}

} // namespace BbStrings
