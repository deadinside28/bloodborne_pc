// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the in-game menu's language (bbport.ini menu_language: en or ru).
#pragma once

namespace BbI18n {

/// `english` in the menu language: its Russian text with menu_language=ru, else itself.
const char* Tr(const char* english);

} // namespace BbI18n
