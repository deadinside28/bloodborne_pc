// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: RenderDoc frame captures on a trigger file (BB_RENDERDOC_TRIGGER).
#pragma once

namespace BbRenderDoc {
/// Once a frame (the presenter): a capture of the next frame when the trigger file appears.
void Poll();
} // namespace BbRenderDoc
