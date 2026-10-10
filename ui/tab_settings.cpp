// SPDX-License-Identifier: GPL-2.0-or-later
#include "tab_settings.h"
#include "bbport_settings.h"
#include "bbport_strings.h"

#include <imgui.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>

namespace BbOverlay {
void RenderGraphicsSettings();
}

namespace UI {

void TabSettings::Render() {
    BbOverlay::RenderGraphicsSettings();
}

} // namespace UI
