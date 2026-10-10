// SPDX-License-Identifier: GPL-2.0-or-later
#include "tab_debugger.h"
#include "ui_strings.h"
#include "breakpoint.h"
#include "mem_scanner.h"
#include "tab_hexview.h"
#include "ui_manager.h"
#include "common/decoder.h"

#include <imgui.h>
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

namespace UI {

static char bp_addr_input[32] = "0x00400000";
static char disasm_addr_input[32] = "0x00400000";
static uintptr_t disasm_current_addr = 0x00400000;
static std::string patch_status_msg;
static std::unordered_map<uintptr_t, std::vector<uint8_t>> patched_instructions;

static Debugger::CpuRegisters selected_regs{};
static bool has_selected_regs = false;

void TabDebugger::SetDisasmAddress(uintptr_t address) {
    disasm_current_addr = address;
    std::snprintf(disasm_addr_input, sizeof(disasm_addr_input), "0x%llx", static_cast<unsigned long long>(address));
}

void TabDebugger::SetBreakpointAddress(uintptr_t address) {
    std::snprintf(bp_addr_input, sizeof(bp_addr_input), "0x%llx", static_cast<unsigned long long>(address));
}

void TabDebugger::Render() {
    auto& bp = Debugger::BreakpointManager::Get();

    ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.55f, 1.0f), "%s",
                       L("Debugger, Disassembler & Breakpoints",
                         "Depurador, Desassemblador & Breakpoints",
                         "Отладчик, дизассемблер и точки останова"));
    ImGui::Separator();

    // 1. Execution Control
    ImGui::Text("%s", L("Execution Control:", "Controle de Execucao:", "Управление выполнением:"));
    ImGui::SameLine();
    const bool paused = bp.IsPaused();
    const char* pause_btn = paused ? L("Resume Game", "Retomar Jogo", "Возобновить")
                                   : L("Pause Game", "Pausar Jogo", "Пауза");

    if (paused) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.45f, 0.15f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.55f, 0.20f, 1.0f));
    }
    if (ImGui::Button(pause_btn, ImVec2(130, 26))) {
        bp.SetPaused(!paused);
    }
    if (paused) {
        ImGui::PopStyleColor(2);
    }

    ImGui::SameLine();
    if (ImGui::Button(L("Step 1 Frame", "Avancar 1 Frame", "Кадр вперед"), ImVec2(130, 26))) {
        bp.StepFrame();
    }
    ImGui::SameLine();
    bool auto_pause = bp.GetAutoPauseOnHit();
    if (ImGui::Checkbox(L("Auto-pause game on hit",
                          "Pausar jogo ao detectar escrita",
                          "Пауза при записи"), &auto_pause)) {
        bp.SetAutoPauseOnHit(auto_pause);
    }
    ImGui::Spacing();

    // 2. Memory Write Watchpoint
    ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.35f, 1.0f), "%s",
                       L("Write Watchpoint ('Find What Writes to this Address'):",
                         "Watchpoint de Escrita ('Quem Escreve Neste Endereco?'):",
                         "Точка наблюдения записи ('Кто пишет сюда?'):"));
    if (bp.HasActiveWatchpoint()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.26f, 0.18f, 0.50f));
        if (ImGui::BeginChild("WatchBanner", ImVec2(0, 52), true, ImGuiWindowFlags_NoScrollbar)) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(ImVec4(0.35f, 1.0f, 0.55f, 1.0f),
                               "[*] %s: 0x%012llx",
                               L("Active Watchpoint", "Watchpoint Ativo", "Активная точка"),
                               static_cast<unsigned long long>(bp.GetWatchedAddress()));
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.20f, 0.20f, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.25f, 0.25f, 1.0f));
            if (ImGui::SmallButton(L("Stop Watching", "Parar Monitoramento", "Остановить"))) {
                bp.ClearWriteWatchpoint();
            }
            ImGui::PopStyleColor(2);

            ImGui::Text("%s: 0x%016llx  |  %s: %llu",
                        L("Live Value", "Valor ao Vivo", "Текущее значение"),
                        static_cast<unsigned long long>(bp.GetLastWatchedValue()),
                        L("Changes Detected", "Alteracoes Detectadas", "Изменений"),
                        static_cast<unsigned long long>(bp.GetValueChangeCount()));
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("%s", L("No active watchpoint. Go to 'Watchlist' and click 'Who Writes?'.",
                                    "Nenhum watchpoint ativo. Va na aba 'Watchlist' e clique em 'Quem Escreve?'.",
                                    "Нет активных точек наблюдения. Перейдите в 'Таблицу' и нажмите 'Кто пишет?'."));
    }

    // 3. Captured Hits Table
    auto hits = bp.GetHits();
    uint64_t total_actions = 0;
    for (const auto& h : hits) {
        total_actions += h.count;
    }

    ImGui::Spacing();
    ImGui::Text(L("Captured Writes (Unique: %zu | Hits: %llu):",
                  "Escritas Capturadas (Unicas: %zu | Hits: %llu):",
                  "Инструкции записи (Уникальных: %zu | Всего: %llu):"),
                hits.size(), static_cast<unsigned long long>(total_actions));
    ImGui::SameLine(ImGui::GetWindowWidth() - 120.0f);
    if (ImGui::SmallButton(L("Clear Logs", "Limpar Registros", "Очистить"))) {
        bp.ClearHits();
        has_selected_regs = false;
        UiManager::SetStatus(L("Logs cleared", "Registros limpos", "Логи очищены"));
    }

    if (ImGui::BeginTable("HitsTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable, ImVec2(0, 150))) {
        ImGui::TableSetupColumn(L("Count", "Contagem", "Счетчик"), ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, 75.0f);
        ImGui::TableSetupColumn(L("Instruction (RIP)", "Instrucao (RIP)", "Инструкция (RIP)"), ImGuiTableColumnFlags_WidthFixed, 145.0f);
        ImGui::TableSetupColumn(L("Disassembly", "Desassembly", "Дизассемблирование"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(L("Thread ID", "Thread ID", "ID потока"), ImGuiTableColumnFlags_WidthFixed, 75.0f);
        ImGui::TableSetupColumn(L("Timestamp", "Timestamp", "Время"), ImGuiTableColumnFlags_WidthFixed, 85.0f);
        ImGui::TableSetupColumn(L("Actions", "Acoes", "Действия"), ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableHeadersRow();

        ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs();
        if (sort_specs && sort_specs->SpecsCount > 0) {
            const auto& spec = sort_specs->Specs[0];
            std::sort(hits.begin(), hits.end(), [&](const Debugger::WatchpointHit& a, const Debugger::WatchpointHit& b) {
                bool ascending = (spec.SortDirection == ImGuiSortDirection_Ascending);
                switch (spec.ColumnIndex) {
                case 0: return ascending ? (a.count < b.count) : (a.count > b.count);
                case 1: return ascending ? (a.rip < b.rip) : (a.rip > b.rip);
                case 2: return ascending ? (a.disassembly < b.disassembly) : (a.disassembly > b.disassembly);
                case 3: return ascending ? (a.thread_id < b.thread_id) : (a.thread_id > b.thread_id);
                case 4: return ascending ? (a.timestamp < b.timestamp) : (a.timestamp > b.timestamp);
                default: return false;
                }
            });
        } else {
            // Default sort: highest count first
            std::sort(hits.begin(), hits.end(), [](const Debugger::WatchpointHit& a, const Debugger::WatchpointHit& b) {
                return a.count > b.count;
            });
        }

        for (size_t i = 0; i < hits.size(); ++i) {
            const auto& hit = hits[i];
            ImGui::TableNextRow();

            // Column 0: Count (highlighted in gold/yellow)
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.25f, 1.0f), "%llu x", static_cast<unsigned long long>(hit.count));

            // Column 1: Instruction RIP (click to copy)
            ImGui::TableSetColumnIndex(1);
            char rip_str[32];
            std::snprintf(rip_str, sizeof(rip_str), "0x%012llx", static_cast<unsigned long long>(hit.rip));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
            if (ImGui::Selectable(rip_str, false, ImGuiSelectableFlags_None)) {
                ImGui::SetClipboardText(rip_str);
                UiManager::SetStatus(std::string("Copied: ") + rip_str);
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", L("Click to copy address", "Clique para copiar o endereco", "Нажмите для копирования"));
            }

            // Column 2: Disassembly
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%s", hit.disassembly.c_str());

            // Column 3: Thread ID
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%d", hit.thread_id);

            // Column 4: Timestamp
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%llu ms", static_cast<unsigned long long>(hit.timestamp % 1000000));

            // Column 5: Actions
            ImGui::TableSetColumnIndex(5);
            ImGui::PushID(static_cast<int>(i));

            // 1. Disasm button
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.38f, 0.58f, 0.75f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.48f, 0.72f, 0.95f));
            if (ImGui::SmallButton(L("Disasm", "Desmontar", "Дизасм"))) {
                SetDisasmAddress(hit.rip);
                if (hit.registers.valid) {
                    selected_regs = hit.registers;
                    has_selected_regs = true;
                }
            }
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", L("Navigate Disassembler to this RIP",
                                          "Navegar Desassemblador para este RIP",
                                          "Перейти в дизассемблер"));
            }

            // 2. Regs button
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f, 0.25f, 0.55f, 0.75f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.48f, 0.35f, 0.72f, 0.95f));
            if (ImGui::SmallButton(L("Regs", "Regs", "Рег"))) {
                if (hit.registers.valid) {
                    selected_regs = hit.registers;
                    has_selected_regs = true;
                }
            }
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", L("Inspect captured CPU registers for this hit",
                                          "Inspecionar registradores da CPU para este hit",
                                          "Посмотреть регистры CPU"));
            }

            // 3. Hex button
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.50f, 0.80f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.58f, 0.65f, 0.95f));
            if (ImGui::SmallButton("Hex")) {
                TabHexView::NavigateTo(hit.rip);
                UiManager::RequestTab(TabId::HexView);
            }
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", L("Inspect instruction bytes in Hex View",
                                          "Inspecionar bytes da instrucao no Hex View",
                                          "Открыть в Hex"));
            }

            // 4. NOP / Restore button
            ImGui::SameLine();
            if (patched_instructions.find(hit.rip) != patched_instructions.end()) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.50f, 0.40f, 0.85f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.65f, 0.50f, 1.0f));
                if (ImGui::SmallButton(L("Restore", "Restaurar", "Вернуть"))) {
                    auto& orig = patched_instructions[hit.rip];
                    if (Debugger::MemoryScanner::WriteMemory(hit.rip, orig.data(), orig.size())) {
                        char msg[80];
                        std::snprintf(msg, sizeof(msg), "Restored instruction at 0x%012llx",
                                      static_cast<unsigned long long>(hit.rip));
                        patch_status_msg = msg;
                        UiManager::SetStatus(msg);
                        patched_instructions.erase(hit.rip);
                    }
                }
                ImGui::PopStyleColor(2);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", L("Restore original instruction bytes",
                                              "Restaurar bytes originais da instrucao",
                                              "Восстановить исходные байты"));
                }
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.60f, 0.20f, 0.20f, 0.85f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.80f, 0.25f, 0.25f, 1.0f));
                if (ImGui::SmallButton("NOP")) {
                    uint8_t code_buf[16] = {0};
                    if (Debugger::MemoryScanner::ReadMemory(hit.rip, code_buf, sizeof(code_buf))) {
                        ZydisDecodedInstruction inst;
                        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
                        if (ZYAN_SUCCESS(Common::Decoder::Instance()->decodeInstruction(inst, operands, code_buf, sizeof(code_buf)))) {
                            const size_t len = (inst.length > 0 && inst.length <= 15) ? inst.length : 1;
                            std::vector<uint8_t> orig(code_buf, code_buf + len);
                            std::vector<uint8_t> nops(len, 0x90);
                            if (Debugger::MemoryScanner::WriteMemory(hit.rip, nops.data(), len)) {
                                patched_instructions[hit.rip] = orig;
                                char msg[80];
                                std::snprintf(msg, sizeof(msg), "NOPed %zu byte(s) at 0x%012llx",
                                              len, static_cast<unsigned long long>(hit.rip));
                                patch_status_msg = msg;
                                UiManager::SetStatus(msg);
                            }
                        }
                    }
                }
                ImGui::PopStyleColor(2);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", L("NOP (disable) this instruction",
                                              "Substituir instrucao por NOP (desativar)",
                                              "Отключить инструкцию (NOP)"));
                }
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Separator();

    // 4. CPU Registers Panel (Collapsible)
    if (ImGui::CollapsingHeader(L("CPU Registers (x86-64)", "Registradores da CPU (x86-64)", "Регистры CPU (x86-64)"), ImGuiTreeNodeFlags_DefaultOpen)) {
        auto regs = has_selected_regs ? selected_regs : bp.GetLastRegisters();
        if (!regs.valid) {
            ImGui::TextDisabled("%s", L("No register snapshot available. Registers are captured when watchpoint triggers.",
                                        "Nenhum snapshot de registradores disponivel. Registradores sao capturados nos watchpoints.",
                                        "Нет данных регистров. Регистры сохраняются при срабатывании точек останова."));
        } else {
            if (has_selected_regs) {
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s",
                                   L("[Viewing snapshot from selected Hit]",
                                     "[Visualizando snapshot do Hit selecionado]",
                                     "[Просмотр снимка выбранного события]"));
                ImGui::SameLine();
                if (ImGui::SmallButton(L("Show Latest", "Mostrar Recente", "Последние"))) {
                    has_selected_regs = false;
                }
            }
            if (ImGui::BeginTable("RegTable", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("RAX: 0x%016llx", static_cast<unsigned long long>(regs.rax));
                ImGui::TableSetColumnIndex(1); ImGui::Text("RBX: 0x%016llx", static_cast<unsigned long long>(regs.rbx));
                ImGui::TableSetColumnIndex(2); ImGui::Text("RCX: 0x%016llx", static_cast<unsigned long long>(regs.rcx));
                ImGui::TableSetColumnIndex(3); ImGui::Text("RDX: 0x%016llx", static_cast<unsigned long long>(regs.rdx));

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("RSI: 0x%016llx", static_cast<unsigned long long>(regs.rsi));
                ImGui::TableSetColumnIndex(1); ImGui::Text("RDI: 0x%016llx", static_cast<unsigned long long>(regs.rdi));
                ImGui::TableSetColumnIndex(2); ImGui::Text("RBP: 0x%016llx", static_cast<unsigned long long>(regs.rbp));
                ImGui::TableSetColumnIndex(3); ImGui::Text("RSP: 0x%016llx", static_cast<unsigned long long>(regs.rsp));

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("R8 : 0x%016llx", static_cast<unsigned long long>(regs.r8));
                ImGui::TableSetColumnIndex(1); ImGui::Text("R9 : 0x%016llx", static_cast<unsigned long long>(regs.r9));
                ImGui::TableSetColumnIndex(2); ImGui::Text("R10: 0x%016llx", static_cast<unsigned long long>(regs.r10));
                ImGui::TableSetColumnIndex(3); ImGui::Text("R11: 0x%016llx", static_cast<unsigned long long>(regs.r11));

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("R12: 0x%016llx", static_cast<unsigned long long>(regs.r12));
                ImGui::TableSetColumnIndex(1); ImGui::Text("R13: 0x%016llx", static_cast<unsigned long long>(regs.r13));
                ImGui::TableSetColumnIndex(2); ImGui::Text("R14: 0x%016llx", static_cast<unsigned long long>(regs.r14));
                ImGui::TableSetColumnIndex(3); ImGui::Text("R15: 0x%016llx", static_cast<unsigned long long>(regs.r15));

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "RIP: 0x%016llx", static_cast<unsigned long long>(regs.rip));
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("RFL: 0x%08llx", static_cast<unsigned long long>(regs.rflags));
                ImGui::TableSetColumnIndex(2);
                if (ImGui::SmallButton(L("Copy Regs", "Copiar Regs", "Копировать"))) {
                    char reg_text[512];
                    std::snprintf(reg_text, sizeof(reg_text),
                                  "RAX=%016llx RBX=%016llx RCX=%016llx RDX=%016llx\n"
                                  "RSI=%016llx RDI=%016llx RBP=%016llx RSP=%016llx\n"
                                  "R8 =%016llx R9 =%016llx R10=%016llx R11=%016llx\n"
                                  "R12=%016llx R13=%016llx R14=%016llx R15=%016llx\n"
                                  "RIP=%016llx RFL=%08llx\n",
                                  static_cast<unsigned long long>(regs.rax), static_cast<unsigned long long>(regs.rbx),
                                  static_cast<unsigned long long>(regs.rcx), static_cast<unsigned long long>(regs.rdx),
                                  static_cast<unsigned long long>(regs.rsi), static_cast<unsigned long long>(regs.rdi),
                                  static_cast<unsigned long long>(regs.rbp), static_cast<unsigned long long>(regs.rsp),
                                  static_cast<unsigned long long>(regs.r8),  static_cast<unsigned long long>(regs.r9),
                                  static_cast<unsigned long long>(regs.r10), static_cast<unsigned long long>(regs.r11),
                                  static_cast<unsigned long long>(regs.r12), static_cast<unsigned long long>(regs.r13),
                                  static_cast<unsigned long long>(regs.r14), static_cast<unsigned long long>(regs.r15),
                                  static_cast<unsigned long long>(regs.rip), static_cast<unsigned long long>(regs.rflags));
                    ImGui::SetClipboardText(reg_text);
                }
                ImGui::TableSetColumnIndex(3);
                ImGui::EndTable();
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();

    // 5. Interactive x86-64 Disassembler
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s",
                       L("x86-64 Disassembler (Live View):",
                         "Desassemblador x86-64 (Visao em Tempo Real):",
                         "Дизассемблер x86-64 (В реальном времени):"));

    ImGui::SetNextItemWidth(170.0f);
    if (ImGui::InputText("##DisasmAddr", disasm_addr_input, sizeof(disasm_addr_input), ImGuiInputTextFlags_EnterReturnsTrue)) {
        disasm_current_addr = std::strtoull(disasm_addr_input, nullptr, 16);
    }
    ImGui::SameLine();
    if (ImGui::Button(L("Go", "Ir", "Перейти"))) {
        disasm_current_addr = std::strtoull(disasm_addr_input, nullptr, 16);
    }
    ImGui::SameLine();
    if (ImGui::Button(L("Paste##disasm", "Colar##disasm", "Вставить##disasm"))) {
        const char* clip = SDL_GetClipboardText();
        if (clip && clip[0]) {
            std::snprintf(disasm_addr_input, sizeof(disasm_addr_input), "%s", clip);
            disasm_current_addr = std::strtoull(disasm_addr_input, nullptr, 16);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(L("Jump to RIP", "Pular para RIP", "К RIP"))) {
        const uintptr_t rip = bp.GetLastHitRip();
        if (rip) {
            SetDisasmAddress(rip);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("-32B")) {
        if (disasm_current_addr >= 32) {
            SetDisasmAddress(disasm_current_addr - 32);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("+32B")) {
        SetDisasmAddress(disasm_current_addr + 32);
    }

    if (!patch_status_msg.empty()) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", patch_status_msg.c_str());
    }

    // Read 256 bytes for disassembly window
    uint8_t code_chunk[256];
    const bool read_ok = Debugger::MemoryScanner::ReadMemory(disasm_current_addr, code_chunk, sizeof(code_chunk));
    if (!read_ok) {
        ImGui::TextDisabled("%s (0x%012llx)",
                            L("Cannot read memory at this address (unmapped or protected page)",
                              "Nao foi possivel ler a memoria neste endereco (pagina nao mapeada ou protegida)",
                              "Невозможно прочитать память по этому адресу"),
                            static_cast<unsigned long long>(disasm_current_addr));
    } else {
        if (ImGui::BeginTable("DisasmTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 230))) {
            ImGui::TableSetupColumn("BP", ImGuiTableColumnFlags_WidthFixed, 36.0f);
            ImGui::TableSetupColumn(L("Address", "Endereco", "Адрес"), ImGuiTableColumnFlags_WidthFixed, 150.0f);
            ImGui::TableSetupColumn(L("Bytes", "Bytes", "Байты"), ImGuiTableColumnFlags_WidthFixed, 140.0f);
            ImGui::TableSetupColumn(L("Instruction", "Instrucao", "Инструкция"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(L("Actions", "Acoes", "Действия"), ImGuiTableColumnFlags_WidthFixed, 150.0f);
            ImGui::TableHeadersRow();

            uintptr_t cur_pc = disasm_current_addr;
            size_t offset = 0;
            const uintptr_t last_rip = bp.GetLastHitRip();
            int inst_count = 0;

            while (offset < sizeof(code_chunk) && inst_count < 28) {
                ZydisDecodedInstruction inst;
                ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
                const size_t rem_size = sizeof(code_chunk) - offset;
                const ZyanStatus status = Common::Decoder::Instance()->decodeInstruction(
                    inst, operands, code_chunk + offset, rem_size);

                if (!ZYAN_SUCCESS(status)) {
                    // Unknown / unaligned byte
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextDisabled(" . ");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("0x%012llx", static_cast<unsigned long long>(cur_pc));
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextDisabled("%02X", code_chunk[offset]);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextDisabled("db 0x%02X", code_chunk[offset]);
                    ImGui::TableSetColumnIndex(4);

                    offset += 1;
                    cur_pc += 1;
                    inst_count++;
                    continue;
                }

                const size_t inst_len = (inst.length > 0 && inst.length <= 15) ? inst.length : 1;
                const std::string disasm_text = Common::Decoder::Instance()->disassembleInst(inst, operands, cur_pc);

                // Format raw bytes
                char hex_bytes[48];
                int pos = 0;
                for (size_t b = 0; b < inst_len && b < 5; ++b) {
                    pos += std::snprintf(hex_bytes + pos, sizeof(hex_bytes) - pos, "%02X ", code_chunk[offset + b]);
                }
                if (inst_len > 5) {
                    std::snprintf(hex_bytes + pos, sizeof(hex_bytes) - pos, "..");
                }

                const bool is_hit_rip = (cur_pc == last_rip);
                const bool has_bp = bp.HasBreakpoint(cur_pc);

                ImGui::TableNextRow();
                if (is_hit_rip) {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImColor(70, 60, 20, 180));
                }

                // Column 0: BP
                ImGui::TableSetColumnIndex(0);
                if (has_bp) {
                    ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "[BP]");
                } else {
                    ImGui::TextDisabled(" . ");
                }

                // Column 1: Address (click to copy)
                ImGui::TableSetColumnIndex(1);
                char addr_str[32];
                std::snprintf(addr_str, sizeof(addr_str), "0x%012llx", static_cast<unsigned long long>(cur_pc));
                if (is_hit_rip) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.3f, 1.0f));
                }
                if (ImGui::Selectable(addr_str, false, ImGuiSelectableFlags_None)) {
                    ImGui::SetClipboardText(addr_str);
                    SDL_SetClipboardText(addr_str);
                    UiManager::SetStatus(std::string("Copied: ") + addr_str);
                }
                if (is_hit_rip) {
                    ImGui::PopStyleColor();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", L("Click to copy address", "Clique para copiar o endereco", "Нажмите для копирования"));
                }

                // Column 2: Bytes
                ImGui::TableSetColumnIndex(2);
                ImGui::TextDisabled("%s", hex_bytes);

                // Column 3: Instruction
                ImGui::TableSetColumnIndex(3);
                if (inst.meta.category == ZYDIS_CATEGORY_CALL ||
                    inst.meta.category == ZYDIS_CATEGORY_COND_BR ||
                    inst.meta.category == ZYDIS_CATEGORY_UNCOND_BR) {
                    ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f), "%s", disasm_text.c_str());
                } else {
                    ImGui::Text("%s", disasm_text.c_str());
                }

                // Column 4: Actions (BP, NOP/Restore)
                ImGui::TableSetColumnIndex(4);
                ImGui::PushID(reinterpret_cast<void*>(cur_pc));
                const char* bp_label = has_bp ? "Del BP" : "+ BP";
                if (ImGui::SmallButton(bp_label)) {
                    if (has_bp) {
                        bp.RemoveBreakpoint(cur_pc);
                        UiManager::SetStatus(L("Breakpoint removed", "Breakpoint removido", "Точка останова удалена"));
                    } else {
                        bp.AddBreakpoint(cur_pc, "Disasm BP");
                        UiManager::SetStatus(L("Breakpoint added", "Breakpoint adicionado", "Точка останова добавлена"));
                    }
                }
                ImGui::SameLine();
                if (patched_instructions.find(cur_pc) != patched_instructions.end()) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.50f, 0.40f, 0.85f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.65f, 0.50f, 1.0f));
                    if (ImGui::SmallButton(L("Restore", "Restaurar", "Вернуть"))) {
                        auto& orig = patched_instructions[cur_pc];
                        if (Debugger::MemoryScanner::WriteMemory(cur_pc, orig.data(), orig.size())) {
                            char msg[80];
                            std::snprintf(msg, sizeof(msg), "Restored original bytes at 0x%012llx",
                                          static_cast<unsigned long long>(cur_pc));
                            patch_status_msg = msg;
                            UiManager::SetStatus(msg);
                            patched_instructions.erase(cur_pc);
                        }
                    }
                    ImGui::PopStyleColor(2);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", L("Restore original instruction bytes",
                                                  "Restaurar bytes originais da instrucao",
                                                  "Восстановить исходные байты"));
                    }
                } else {
                    if (ImGui::SmallButton("NOP")) {
                        std::vector<uint8_t> orig(code_chunk + offset, code_chunk + offset + inst_len);
                        std::vector<uint8_t> nops(inst_len, 0x90);
                        if (Debugger::MemoryScanner::WriteMemory(cur_pc, nops.data(), inst_len)) {
                            patched_instructions[cur_pc] = orig;
                            char msg[80];
                            std::snprintf(msg, sizeof(msg), "Patched %zu byte(s) with NOP at 0x%012llx",
                                          inst_len, static_cast<unsigned long long>(cur_pc));
                            patch_status_msg = msg;
                            UiManager::SetStatus(msg);
                        }
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", L("NOP (disable) this instruction",
                                                  "Substituir instrucao por NOP (desativar)",
                                                  "Отключить инструкцию (NOP)"));
                    }
                }
                ImGui::PopID();

                offset += inst_len;
                cur_pc += inst_len;
                inst_count++;
            }
            ImGui::EndTable();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();

    // 6. Software Breakpoints List
    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "%s",
                       L("Software Breakpoints (Code Execution):",
                         "Breakpoints de Codigo (Software Breakpoints):",
                         "Точки останова кода (Software Breakpoints):"));
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputText("##bp_addr_input", bp_addr_input, sizeof(bp_addr_input));
    ImGui::SameLine();
    if (ImGui::Button(L("Paste##bp", "Colar##bp", "Вставить##bp"))) {
        const char* clip = SDL_GetClipboardText();
        if (clip && clip[0]) {
            std::snprintf(bp_addr_input, sizeof(bp_addr_input), "%s", clip);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(L("Add Breakpoint", "Adicionar Breakpoint", "Добавить"))) {
        const uintptr_t addr = std::strtoull(bp_addr_input, nullptr, 16);
        bp.AddBreakpoint(addr, "BP manual");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", L("Hex Address", "Endereco Hex", "Hex адрес"));

    const auto bps = bp.GetBreakpoints();
    for (const auto& b : bps) {
        ImGui::BulletText("0x%012llx: %s (%s: 0x%02X)",
                          static_cast<unsigned long long>(b.address),
                          b.label.c_str(),
                          L("Original", "Original", "Оригинал"),
                          b.original_byte);
        ImGui::SameLine();
        ImGui::PushID(reinterpret_cast<void*>(b.address));
        if (ImGui::SmallButton(L("Remove", "Remover", "Удалить"))) {
            bp.RemoveBreakpoint(b.address);
        }
        ImGui::PopID();
    }
}

} // namespace UI
