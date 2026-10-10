// SPDX-License-Identifier: GPL-2.0-or-later
#include "tab_scanner.h"
#include "ui_strings.h"
#include "mem_scanner.h"
#include "mem_editor.h"
#include "tab_hexview.h"
#include "breakpoint.h"
#include "ui_manager.h"

#include <imgui.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace UI {

static char value_input[128] = "1";
static int selected_type = 2; // Default u32
static int selected_scope = 0; // Default ExecutableOnly
static int selected_comp = 0; // Default Exact

void TabScanner::Render() {
    auto& scanner = Debugger::MemoryScanner::Get();
    auto& editor = Debugger::MemoryEditor::Get();

    ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.55f, 1.0f), "%s",
                       L("Memory Scanner", "Scanner de Memoria", "Сканер памяти"));
    ImGui::Separator();

    if (ImGui::BeginTable("ScanControls", 2, ImGuiTableFlags_None)) {
        ImGui::TableSetupColumn("Inputs", ImGuiTableColumnFlags_WidthFixed, 360.0f);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();

        // Left: Inputs
        ImGui::TableSetColumnIndex(0);
        ImGui::SetNextItemWidth(170.0f);
        double val = std::strtod(value_input, nullptr);
        if (ImGui::InputText(L("Value", "Valor", "Значение"), value_input, sizeof(value_input), ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (scanner.GetMatchCount() > 0) {
                scanner.NextScan(static_cast<Debugger::ScanComparison>(selected_comp), val);
            } else {
                scanner.StartFirstScan(static_cast<Debugger::DataType>(selected_type),
                                       static_cast<Debugger::ScanComparison>(selected_comp),
                                       val,
                                       static_cast<Debugger::ScanScope>(selected_scope));
            }
        }
        ImGui::SetItemTooltip("%s", L("Press Enter to run Scan", "Pressione Enter para escanear", "Нажмите Enter для поиска"));

        const char* type_names[] = {
            "1 Byte (u8)", "2 Bytes (u16)", "4 Bytes (u32)", "8 Bytes (u64)",
            "1 Byte (i8)", "2 Bytes (i16)", "4 Bytes (i32)", "8 Bytes (i64)",
            "Float", "Double"
        };
        ImGui::SetNextItemWidth(170.0f);
        ImGui::Combo(L("Type", "Tipo", "Тип"), &selected_type, type_names, IM_ARRAYSIZE(type_names));

        const char* scope_names_en[] = {
            "Executable & Data (<256MB, Fast)",
            "Guest Memory (PS4 Heap)",
            "Full Process"
        };
        const char* scope_names_pt[] = {
            "Executavel e .data (<256MB, Rapido)",
            "Memoria Guest (Heap PS4)",
            "Processo Completo"
        };
        const char* scope_names_ru[] = {
            "Исполняемый файл и данные (<256 МБ)",
            "Память PS4 (Heap)",
            "Весь процесс"
        };
        const char** scope_names = GetCurrentLang() == BbSettings::LangPortuguese ? scope_names_pt
                                 : GetCurrentLang() == BbSettings::LangRussian    ? scope_names_ru
                                                                                 : scope_names_en;
        ImGui::SetNextItemWidth(230.0f);
        ImGui::Combo(L("Scope", "Regiao", "Область"), &selected_scope, scope_names, 3);

        const char* comp_names_en[] = {
            "Exact Value", "Changed", "Unchanged", "Increased", "Decreased"
        };
        const char* comp_names_pt[] = {
            "Valor Exato", "Mudou", "Nao Mudou", "Aumentou", "Diminuiu"
        };
        const char* comp_names_ru[] = {
            "Точное значение", "Изменилось", "Не изменилось", "Увеличилось", "Уменьшилось"
        };
        const char** comp_names = GetCurrentLang() == BbSettings::LangPortuguese ? comp_names_pt
                                : GetCurrentLang() == BbSettings::LangRussian    ? comp_names_ru
                                                                                : comp_names_en;
        ImGui::SetNextItemWidth(170.0f);
        ImGui::Combo(L("Condition", "Condicao", "Тип поиска"), &selected_comp, comp_names, 5);

        // Right: Scan buttons
        ImGui::TableSetColumnIndex(1);
        val = std::strtod(value_input, nullptr);
        const bool is_scanning = scanner.IsScanning();

        ImGui::BeginDisabled(is_scanning);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.48f, 0.32f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.62f, 0.40f, 1.0f));
        if (ImGui::Button(L("First Scan", "Primeiro Scan", "Первый поиск"), ImVec2(125, 30))) {
            scanner.StartFirstScan(static_cast<Debugger::DataType>(selected_type),
                                   static_cast<Debugger::ScanComparison>(selected_comp),
                                   val,
                                   static_cast<Debugger::ScanScope>(selected_scope));
        }
        ImGui::PopStyleColor(2);

        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.38f, 0.58f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.50f, 0.74f, 1.0f));
        if (ImGui::Button(L("Next Scan", "Proximo Scan", "След. поиск"), ImVec2(125, 30))) {
            scanner.NextScan(static_cast<Debugger::ScanComparison>(selected_comp), val);
        }
        ImGui::PopStyleColor(2);
        ImGui::EndDisabled();

        ImGui::Spacing();
        if (is_scanning) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.20f, 0.20f, 0.90f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.90f, 0.25f, 0.25f, 1.0f));
            if (ImGui::Button(L("Cancel", "Cancelar", "Отмена"), ImVec2(125, 26))) {
                scanner.CancelScan();
            }
            ImGui::PopStyleColor(2);
        } else {
            if (ImGui::Button(L("Reset", "Resetar Busca", "Сброс"), ImVec2(125, 26))) {
                scanner.Reset();
            }
        }

        ImGui::EndTable();
    }

    if (scanner.IsScanning()) {
        ImGui::Spacing();
        const float p = scanner.GetProgress();
        char overlay[64];
        std::snprintf(overlay, sizeof(overlay), "%s (%.0f%%)",
                      L("Scanning...", "Escaneando...", "Сканирование..."),
                      p * 100.0f);

        // Progress bar with generous height (26px) and high-contrast color scheme
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.20f, 0.55f, 0.82f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.08f, 0.10f, 0.14f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.30f, 0.42f, 0.58f, 0.70f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

        ImGui::ProgressBar(p, ImVec2(-1.0f, 26.0f), "");

        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);

        const ImVec2 rect_min = ImGui::GetItemRectMin();
        const ImVec2 rect_max = ImGui::GetItemRectMax();
        const ImVec2 text_size = ImGui::CalcTextSize(overlay);
        const ImVec2 text_pos(
            rect_min.x + (rect_max.x - rect_min.x - text_size.x) * 0.5f,
            rect_min.y + (rect_max.y - rect_min.y - text_size.y) * 0.5f
        );

        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        // 4-point halo + shadow for crisp legibility over any background/fill
        const ImU32 shadow_col = IM_COL32(0, 0, 0, 240);
        draw_list->AddText(ImVec2(text_pos.x - 1, text_pos.y), shadow_col, overlay);
        draw_list->AddText(ImVec2(text_pos.x + 1, text_pos.y), shadow_col, overlay);
        draw_list->AddText(ImVec2(text_pos.x, text_pos.y - 1), shadow_col, overlay);
        draw_list->AddText(ImVec2(text_pos.x, text_pos.y + 1), shadow_col, overlay);
        draw_list->AddText(ImVec2(text_pos.x + 1, text_pos.y + 1), shadow_col, overlay);
        draw_list->AddText(text_pos, IM_COL32(245, 248, 252, 255), overlay);
        ImGui::Spacing();
    }

    ImGui::Spacing();
    const size_t count = scanner.GetMatchCount();
    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.0f, 1.0f),
                       "%s", L("Search Results:", "Resultados da Busca:", "Результаты поиска:"));
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu %s)", count, L("matches", "encontrados", "найдено"));
    ImGui::Separator();

    if (count > 0) {
        static int current_page = 0;
        constexpr size_t PAGE_SIZE = 50;
        const int total_pages = static_cast<int>((count + PAGE_SIZE - 1) / PAGE_SIZE);
        if (current_page >= total_pages) current_page = total_pages - 1;
        if (current_page < 0) current_page = 0;

        ImGui::Text(L("Page %d of %d", "Pagina %d de %d", "Страница %d из %d"), current_page + 1, total_pages);
        ImGui::SameLine();
        if (ImGui::Button(L("< Previous", "< Anterior", "< Назад")) && current_page > 0) current_page--;
        ImGui::SameLine();
        if (ImGui::Button(L("Next >", "Proxima >", "Вперед >")) && current_page + 1 < total_pages) current_page++;

        if (ImGui::BeginTable("ScanResults", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 310))) {
            ImGui::TableSetupColumn(L("Address", "Endereco", "Адрес"), ImGuiTableColumnFlags_WidthFixed, 140.0f);
            ImGui::TableSetupColumn(L("Current Value", "Valor Atual", "Текущее"), ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn(L("Previous Value", "Valor Anterior", "Предыдущее"), ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn(L("Action", "Acao", "Действие"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            const auto scan_type = scanner.GetCurrentType();
            const auto matches = scanner.GetMatches(current_page * PAGE_SIZE, PAGE_SIZE);
            for (size_t i = 0; i < matches.size(); ++i) {
                const auto& m = matches[i];
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(ImVec4(0.40f, 0.80f, 1.0f, 1.0f), "0x%012llx",
                                   static_cast<unsigned long long>(m.address));

                double cur_num = 0;
                std::string cur_str;
                Debugger::MemoryScanner::ReadFormatted(m.address, scan_type, cur_num, cur_str);

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", cur_str.c_str());

                char prev_str[64];
                switch (scan_type) {
                case Debugger::DataType::U8: std::snprintf(prev_str, sizeof(prev_str), "%u", static_cast<uint8_t>(m.prev_raw)); break;
                case Debugger::DataType::I8: std::snprintf(prev_str, sizeof(prev_str), "%d", static_cast<int8_t>(m.prev_raw)); break;
                case Debugger::DataType::U16: std::snprintf(prev_str, sizeof(prev_str), "%u", static_cast<uint16_t>(m.prev_raw)); break;
                case Debugger::DataType::I16: std::snprintf(prev_str, sizeof(prev_str), "%d", static_cast<int16_t>(m.prev_raw)); break;
                case Debugger::DataType::U32: std::snprintf(prev_str, sizeof(prev_str), "%u", static_cast<uint32_t>(m.prev_raw)); break;
                case Debugger::DataType::I32: std::snprintf(prev_str, sizeof(prev_str), "%d", static_cast<int32_t>(m.prev_raw)); break;
                case Debugger::DataType::U64: std::snprintf(prev_str, sizeof(prev_str), "%llu", static_cast<unsigned long long>(m.prev_raw)); break;
                case Debugger::DataType::I64: std::snprintf(prev_str, sizeof(prev_str), "%lld", static_cast<long long>(m.prev_raw)); break;
                case Debugger::DataType::Float: {
                    float fval = 0.0f;
                    std::memcpy(&fval, &m.prev_raw, sizeof(fval));
                    std::snprintf(prev_str, sizeof(prev_str), "%.3f", fval);
                    break;
                }
                case Debugger::DataType::Double: {
                    double dval = 0.0;
                    std::memcpy(&dval, &m.prev_raw, sizeof(dval));
                    std::snprintf(prev_str, sizeof(prev_str), "%.4f", dval);
                    break;
                }
                }
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%s", prev_str);

                ImGui::TableSetColumnIndex(3);
                ImGui::PushID(static_cast<int>(i));

                // 1. Add to Watchlist
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.35f, 0.75f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.60f, 0.45f, 0.95f));
                if (ImGui::SmallButton(L("+ Watch", "+ Watch", "+ Список"))) {
                    char label[32];
                    std::snprintf(label, sizeof(label), "Item %zu", editor.GetCount() + 1);
                    editor.AddWatch(label, m.address, scan_type);
                    UiManager::SetStatus(L("Address added to Watchlist!", "Endereco adicionado a Watchlist!", "Адрес добавлен в таблицу!"));
                }
                ImGui::PopStyleColor(2);

                // 2. Jump to Hex View
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.32f, 0.46f, 0.75f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.44f, 0.64f, 0.95f));
                if (ImGui::SmallButton("Hex")) {
                    TabHexView::NavigateTo(m.address);
                    UiManager::RequestTab(TabId::HexView);
                }
                ImGui::PopStyleColor(2);

                // 3. Attach Who Writes?
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.36f, 0.26f, 0.50f, 0.75f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.48f, 0.35f, 0.68f, 0.95f));
                if (ImGui::SmallButton(L("Who Writes?", "Quem Escreve?", "Кто пишет?"))) {
                    Debugger::BreakpointManager::Get().SetWriteWatchpoint(m.address);
                    UiManager::RequestTab(TabId::Debugger);
                }
                ImGui::PopStyleColor(2);

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
}

} // namespace UI
