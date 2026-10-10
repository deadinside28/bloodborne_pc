// SPDX-License-Identifier: GPL-2.0-or-later
#include "tab_hexview.h"
#include "ui_strings.h"
#include "mem_scanner.h"
#include "mem_editor.h"
#include "breakpoint.h"
#include "cutscene_detector.h"
#include "ui_manager.h"
#include "gpu/bbgpu.h"

#include <imgui.h>
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <vector>
#include <string>
#include <chrono>

namespace UI {

static uintptr_t ResolveAddress(uintptr_t addr) {
    const uintptr_t img = bbgpu_get_guest_image_base();
    if (img != 0 && addr >= 0x00400000 && addr < 0x20000000) {
        return img + (addr - 0x00400000);
    }
    return addr;
}

static char addr_input[64] = "0x00400000";
static uintptr_t current_addr = 0x00400000;
static uintptr_t selected_addr = 0x00400000;
static bool has_selection = false;

static std::vector<uintptr_t> history = {0x00400000};
static size_t history_idx = 0;

static std::string status_msg;
static std::chrono::steady_clock::time_point status_time{};

static void CopyToClipboard(const char* text, const char* desc) {
    ImGui::SetClipboardText(text);
    SDL_SetClipboardText(text);
    status_msg = std::string(desc) + " " + L("copied to clipboard!", "copiado para a area de transferencia!", "скопировано в буфер!");
    status_time = std::chrono::steady_clock::now();
}

void TabHexView::NavigateTo(uintptr_t addr) {
    current_addr = ResolveAddress(addr);
    selected_addr = current_addr;
    has_selection = true;
    std::snprintf(addr_input, sizeof(addr_input), "0x%llx", static_cast<unsigned long long>(current_addr));
    if (history.empty() || history[history_idx] != current_addr) {
        if (history_idx + 1 < history.size()) {
            history.erase(history.begin() + history_idx + 1, history.end());
        }
        history.push_back(current_addr);
        history_idx = history.size() - 1;
    }
}

void TabHexView::Render() {
    ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.55f, 1.0f), "%s",
                       L("Memory Hex Inspector", "Visualizador e Editor Hexadecimal", "Hex-инспектор памяти"));
    ImGui::Separator();

    // 1. Navigation Toolbar
    ImGui::BeginGroup();
    // History Back / Forward
    const bool can_back = history_idx > 0;
    const bool can_fwd = history_idx + 1 < history.size();
    if (!can_back) ImGui::BeginDisabled();
    if (ImGui::Button("<##back", ImVec2(28, 0))) {
        if (can_back) {
            history_idx--;
            current_addr = history[history_idx];
            selected_addr = current_addr;
            std::snprintf(addr_input, sizeof(addr_input), "0x%llx", static_cast<unsigned long long>(current_addr));
        }
    }
    if (!can_back) ImGui::EndDisabled();

    ImGui::SameLine();
    if (!can_fwd) ImGui::BeginDisabled();
    if (ImGui::Button(">##fwd", ImVec2(28, 0))) {
        if (can_fwd) {
            history_idx++;
            current_addr = history[history_idx];
            selected_addr = current_addr;
            std::snprintf(addr_input, sizeof(addr_input), "0x%llx", static_cast<unsigned long long>(current_addr));
        }
    }
    if (!can_fwd) ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::InputText(L("Address##goto", "Endereco##goto", "Адрес##goto"), addr_input, sizeof(addr_input), ImGuiInputTextFlags_EnterReturnsTrue)) {
        NavigateTo(std::strtoull(addr_input, nullptr, 16));
    }
    ImGui::SameLine();
    if (ImGui::Button(L("Go##btn", "Ir##btn", "Перейти##btn"), ImVec2(36, 0))) {
        NavigateTo(std::strtoull(addr_input, nullptr, 16));
    }
    ImGui::SameLine();
    if (ImGui::Button(L("Paste##hex", "Colar##hex", "Вставить##hex"))) {
        const char* clip = SDL_GetClipboardText();
        if (clip && clip[0]) {
            std::snprintf(addr_input, sizeof(addr_input), "%s", clip);
            NavigateTo(std::strtoull(addr_input, nullptr, 16));
        }
    }

    // Step offsets
    ImGui::SameLine();
    if (ImGui::Button("-4K") && current_addr >= 4096) {
        NavigateTo(current_addr - 4096);
    }
    ImGui::SameLine();
    if (ImGui::Button("-256B") && current_addr >= 256) {
        NavigateTo(current_addr - 256);
    }
    ImGui::SameLine();
    if (ImGui::Button("+256B")) {
        NavigateTo(current_addr + 256);
    }
    ImGui::SameLine();
    if (ImGui::Button("+4K")) {
        NavigateTo(current_addr + 4096);
    }

    // Presets
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::SmallButton("eboot")) {
        NavigateTo(0x00400000);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Heap")) {
        NavigateTo(0x1000000000ULL);
    }
    ImGui::EndGroup();

    // 2. Quick Jump Presets & Actions
    ImGui::BeginGroup();
    struct JumpPreset { const char* label; uintptr_t address; };
    const JumpPreset presets[] = {
        {L("eboot.bin Base (ELF)", "Base eboot.bin (ELF)", "База eboot.bin (ELF)"), 0x00400000},
        {L("Main Game Code", "Codigo Principal", "Основной код игры"), 0x02000000},
        {L("Globals and Params (.data)", "Parametros e Globais (.data)", "Глобальные данные (.data)"), 0x05400000},
        {L("PS4 Direct Memory (Heap)", "Memoria Direta PS4 (Heap)", "Прямая память PS4 (Heap)"), 0x1000000000},
    };
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::BeginCombo(L("Quick Jump", "Salto Rapido", "Быстрый переход"),
                          L("Game Locations...", "Locais do Jogo...", "Области игры..."))) {
        for (const auto& p : presets) {
            if (ImGui::Selectable(p.label)) {
                NavigateTo(p.address);
            }
        }
        auto& cs = Debugger::CutsceneDetector::Get();
        if (cs.IsEnabled() && cs.GetAddress() != 0) {
            char cs_label[64];
            std::snprintf(cs_label, sizeof(cs_label), "%s (0x%llx)",
                          L("Cutscene Flag", "Flag de Cutscene", "Флаг кат-сцены"),
                          static_cast<unsigned long long>(cs.GetAddress()));
            if (ImGui::Selectable(cs_label)) {
                NavigateTo(cs.GetAddress());
            }
        }
        ImGui::EndCombo();
    }

    // Copy Toolbar
    ImGui::SameLine(360.0f);
    char cur_addr_hex[32];
    std::snprintf(cur_addr_hex, sizeof(cur_addr_hex), "0x%llx",
                  static_cast<unsigned long long>(has_selection ? selected_addr : current_addr));

    if (ImGui::Button(L("Copy Address", "Copiar Endereco", "Копировать адрес"))) {
        CopyToClipboard(cur_addr_hex, L("Address", "Endereco", "Адрес"));
    }
    ImGui::SameLine();
    if (ImGui::Button(L("Copy 16 Bytes", "Copiar 16 Bytes", "Копировать 16 байт"))) {
        uint8_t copy_buf[16] = {0};
        Debugger::MemoryScanner::ReadMemory(has_selection ? selected_addr : current_addr, copy_buf, 16);
        char hex_str[64];
        std::snprintf(hex_str, sizeof(hex_str),
                      "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                      copy_buf[0], copy_buf[1], copy_buf[2], copy_buf[3],
                      copy_buf[4], copy_buf[5], copy_buf[6], copy_buf[7],
                      copy_buf[8], copy_buf[9], copy_buf[10], copy_buf[11],
                      copy_buf[12], copy_buf[13], copy_buf[14], copy_buf[15]);
        CopyToClipboard(hex_str, L("16 Bytes Hex", "16 Bytes Hex", "16 байт Hex"));
    }
    ImGui::SameLine();
    if (ImGui::Button(L("Copy ASCII", "Copiar ASCII", "Копировать ASCII"))) {
        uint8_t copy_buf[16] = {0};
        Debugger::MemoryScanner::ReadMemory(has_selection ? selected_addr : current_addr, copy_buf, 16);
        char asc_str[17] = {0};
        for (int i = 0; i < 16; ++i) {
            asc_str[i] = std::isprint(copy_buf[i]) ? copy_buf[i] : '.';
        }
        CopyToClipboard(asc_str, L("ASCII Text", "Texto ASCII", "Текст ASCII"));
    }
    ImGui::EndGroup();

    // Status / Feedback message
    if (!status_msg.empty()) {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - status_time).count();
        if (elapsed < 3) {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "%s", status_msg.c_str());
        } else {
            status_msg.clear();
        }
    }

    ImGui::Spacing();

    const uintptr_t img = bbgpu_get_guest_image_base();
    const uint64_t img_sz = bbgpu_get_guest_image_size();
    if (img != 0 && current_addr >= img && current_addr < img + img_sz) {
        const uintptr_t ps4_vaddr = 0x00400000 + (current_addr - img);
        ImGui::TextColored(ImVec4(0.92f, 0.85f, 0.45f, 1.0f), "%s: 0x%08llx (%s: 0x%lx)",
                           L("PS4 Virtual Address", "Endereco Virtual PS4", "Виртуальный адрес PS4"),
                           static_cast<unsigned long long>(ps4_vaddr),
                           L("eboot offset", "offset eboot", "смещение eboot"),
                           static_cast<unsigned long>(current_addr - img));
    }

    // 3. Memory Hex Grid
    constexpr size_t VIEW_SIZE = 256;
    uint8_t buffer[VIEW_SIZE] = {0};
    const bool readable = Debugger::MemoryScanner::ReadMemory(current_addr, buffer, VIEW_SIZE);

    if (!readable) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s",
                           L("Inaccessible address or unallocated memory.",
                             "Endereco inacessivel ou memoria nao alocada.",
                             "Недоступный адрес или невыделенная память."));
        return;
    }

    if (ImGui::BeginTable("HexTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 280))) {
        ImGui::TableSetupColumn(L("Address (Click to Select)", "Endereco (Clique p/ Selecionar)", "Адрес (выбрать)"),
                                ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn("00 01 02 03  04 05 06 07", ImGuiTableColumnFlags_WidthFixed, 195.0f);
        ImGui::TableSetupColumn("08 09 0A 0B  0C 0D 0E 0F", ImGuiTableColumnFlags_WidthFixed, 195.0f);
        ImGui::TableSetupColumn("ASCII", ImGuiTableColumnFlags_WidthFixed, 145.0f);
        ImGui::TableHeadersRow();

        for (size_t row = 0; row < VIEW_SIZE; row += 16) {
            const uintptr_t row_addr = current_addr + row;
            ImGui::TableNextRow();

            // 1. Address / Selectable
            ImGui::TableSetColumnIndex(0);
            char row_label[32];
            std::snprintf(row_label, sizeof(row_label), "0x%012llx", static_cast<unsigned long long>(row_addr));

            const bool is_selected = has_selection && (selected_addr == row_addr);
            if (ImGui::Selectable(row_label, is_selected, ImGuiSelectableFlags_SpanAllColumns)) {
                selected_addr = row_addr;
                has_selection = true;
            }

            // Right-Click Context Menu
            if (ImGui::BeginPopupContextItem()) {
                selected_addr = row_addr;
                has_selection = true;

                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "0x%012llx",
                                   static_cast<unsigned long long>(row_addr));
                ImGui::Separator();

                if (ImGui::MenuItem(L("Copy Address", "Copiar Endereco", "Копировать адрес"))) {
                    char hex_addr[32];
                    std::snprintf(hex_addr, sizeof(hex_addr), "0x%llx", static_cast<unsigned long long>(row_addr));
                    CopyToClipboard(hex_addr, L("Address", "Endereco", "Адрес"));
                }
                if (ImGui::MenuItem(L("Copy Hex Bytes", "Copiar Bytes Hex", "Копировать байты Hex"))) {
                    char hex_row[64];
                    std::snprintf(hex_row, sizeof(hex_row),
                                  "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                                  buffer[row + 0], buffer[row + 1], buffer[row + 2], buffer[row + 3],
                                  buffer[row + 4], buffer[row + 5], buffer[row + 6], buffer[row + 7],
                                  buffer[row + 8], buffer[row + 9], buffer[row + 10], buffer[row + 11],
                                  buffer[row + 12], buffer[row + 13], buffer[row + 14], buffer[row + 15]);
                    CopyToClipboard(hex_row, L("Hex Bytes", "Bytes Hex", "Байты Hex"));
                }
                if (ImGui::MenuItem(L("Copy ASCII", "Copiar ASCII", "Копировать ASCII"))) {
                    char asc_row[17] = {0};
                    for (int i = 0; i < 16; ++i) {
                        asc_row[i] = std::isprint(buffer[row + i]) ? buffer[row + i] : '.';
                    }
                    CopyToClipboard(asc_row, L("ASCII", "ASCII", "ASCII"));
                }
                ImGui::Separator();
                if (ImGui::MenuItem(L("Add to Watchlist", "Adicionar a Watchlist", "Добавить в таблицу"))) {
                    Debugger::MemoryEditor::Get().AddWatch("Hex Item", row_addr, Debugger::DataType::U32);
                    UiManager::SetStatus(L("Address added to Watchlist!", "Endereco adicionado a Watchlist!", "Адрес добавлен в таблицу!"));
                }
                if (ImGui::MenuItem(L("Watch Writes ('Who Writes?')", "Monitorar com Watchpoint ('Quem Escreve?')", "Кто пишет по адресу?"))) {
                    Debugger::BreakpointManager::Get().SetWriteWatchpoint(row_addr);
                    UiManager::RequestTab(TabId::Debugger);
                    UiManager::SetStatus(L("Watchpoint attached! Switched to Debugger.", "Watchpoint ativado! Mudando para o Depurador.", "Точка наблюдения установлена!"));
                }
                if (ImGui::MenuItem(L("Set as Cutscene Flag", "Definir como Flag de Cutscene", "Назначить флагом кат-сцены"))) {
                    Debugger::CutsceneDetector::Get().SetAddress(row_addr);
                    Debugger::CutsceneDetector::Get().SetEnabled(true);
                    UiManager::SetStatus(L("Cutscene flag assigned!", "Flag de cutscene definida!", "Флаг кат-сцены назначен!"));
                }
                ImGui::EndPopup();
            }

            // 2. First 8 bytes
            ImGui::TableSetColumnIndex(1);
            char hex1[64];
            std::snprintf(hex1, sizeof(hex1), "%02X %02X %02X %02X  %02X %02X %02X %02X",
                          buffer[row + 0], buffer[row + 1], buffer[row + 2], buffer[row + 3],
                          buffer[row + 4], buffer[row + 5], buffer[row + 6], buffer[row + 7]);
            ImGui::TextUnformatted(hex1);

            // 3. Second 8 bytes
            ImGui::TableSetColumnIndex(2);
            char hex2[64];
            std::snprintf(hex2, sizeof(hex2), "%02X %02X %02X %02X  %02X %02X %02X %02X",
                          buffer[row + 8], buffer[row + 9], buffer[row + 10], buffer[row + 11],
                          buffer[row + 12], buffer[row + 13], buffer[row + 14], buffer[row + 15]);
            ImGui::TextUnformatted(hex2);

            // 4. ASCII representation
            ImGui::TableSetColumnIndex(3);
            char ascii[32] = {0};
            for (size_t c = 0; c < 16; ++c) {
                const char ch = static_cast<char>(buffer[row + c]);
                ascii[c] = (std::isprint(static_cast<unsigned char>(ch))) ? ch : '.';
            }
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "| %s |", ascii);
        }
        ImGui::EndTable();
    }

    // 4. Selection Inspector & Editor Bar
    if (has_selection) {
        ImGui::Spacing();
        uint8_t sel_bytes[8] = {0};
        Debugger::MemoryScanner::ReadMemory(selected_addr, sel_bytes, sizeof(sel_bytes));

        const uint8_t  v_u8  = sel_bytes[0];
        const uint16_t v_u16 = *reinterpret_cast<const uint16_t*>(sel_bytes);
        const uint32_t v_u32 = *reinterpret_cast<const uint32_t*>(sel_bytes);
        const int32_t  v_i32 = *reinterpret_cast<const int32_t*>(sel_bytes);
        const float    v_flt = *reinterpret_cast<const float*>(sel_bytes);
        const double   v_dbl = *reinterpret_cast<const double*>(sel_bytes);

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.13f, 0.15f, 0.20f, 0.70f));
        if (ImGui::BeginChild("DataInspectorCard", ImVec2(0, 68), true)) {
            ImGui::TextColored(ImVec4(0.35f, 0.85f, 1.0f, 1.0f), "%s: 0x%012llx",
                               L("Data Inspector", "Inspetor de Dados", "Инспектор данных"),
                               static_cast<unsigned long long>(selected_addr));
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            ImGui::Text("u8: %u  |  u16: %u  |  u32: %u  |  i32: %d  |  Float: %.3f  |  Double: %.4f",
                        v_u8, v_u16, v_u32, v_i32, v_flt, v_dbl);

            static char write_val_buf[32] = "0";
            static int write_type_idx = 2; // u32
            const char* w_types[] = { "u8", "u16", "u32", "u64", "Float" };

            ImGui::SetNextItemWidth(90.0f);
            ImGui::InputText("##write_val", write_val_buf, sizeof(write_val_buf));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(65.0f);
            ImGui::Combo("##write_type", &write_type_idx, w_types, IM_ARRAYSIZE(w_types));
            ImGui::SameLine();
            if (ImGui::Button(L("Write", "Gravar", "Запись"))) {
                double num = std::strtod(write_val_buf, nullptr);
                Debugger::DataType dt = Debugger::DataType::U32;
                if (write_type_idx == 0) dt = Debugger::DataType::U8;
                else if (write_type_idx == 1) dt = Debugger::DataType::U16;
                else if (write_type_idx == 2) dt = Debugger::DataType::U32;
                else if (write_type_idx == 3) dt = Debugger::DataType::U64;
                else if (write_type_idx == 4) dt = Debugger::DataType::Float;
                Debugger::MemoryScanner::WriteFormatted(selected_addr, dt, num);
                UiManager::SetStatus(L("Value written to memory!", "Valor gravado na memoria!", "Значение записано в память!"));
            }
            ImGui::SameLine();
            if (ImGui::Button(L("+ Watchlist", "+ Watchlist", "+ Список"))) {
                char label[32];
                std::snprintf(label, sizeof(label), "Hex 0x%llx", static_cast<unsigned long long>(selected_addr));
                Debugger::MemoryEditor::Get().AddWatch(label, selected_addr, Debugger::DataType::U32);
                UiManager::SetStatus(L("Address added to Watchlist!", "Endereco adicionado a Watchlist!", "Адрес добавлен в таблицу!"));
            }
            ImGui::SameLine();
            if (ImGui::Button(L("Who Writes?", "Quem Escreve?", "Кто пишет?"))) {
                Debugger::BreakpointManager::Get().SetWriteWatchpoint(selected_addr);
                UiManager::RequestTab(TabId::Debugger);
                UiManager::SetStatus(L("Watchpoint attached! Switched to Debugger.", "Watchpoint ativado! Mudando para o Depurador.", "Точка наблюдения установлена!"));
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
}

} // namespace UI
