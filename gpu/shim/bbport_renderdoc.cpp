// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: frame captures with RenderDoc's in-application API, where its layer is loaded already
// (on Android: the GPU debug layer settings for the app, VK_LAYER_RENDERDOC_Capture from
// org.renderdoc.renderdoccmd.arm64). BB_RENDERDOC_TRIGGER=<file>: when the file appears, the next
// frame is captured into BB_RENDERDOC_DIR (default: the trigger's folder).
#include "bbport_renderdoc.h"

#include <cstdio>
#include <string>
#include <dlfcn.h>

#include "shim/bb_env.h"
#include "third_party/renderdoc/renderdoc_app.h"

namespace BbRenderDoc {
namespace {

RENDERDOC_API_1_6_0* Api(const char* trigger) {
    static RENDERDOC_API_1_6_0* api = [trigger]() -> RENDERDOC_API_1_6_0* {
        // the layer as it is loaded: never loaded from here
        for (const char* name : {"libVkLayer_GLES_RenderDoc.so", "librenderdoc.so"}) {
            void* lib = dlopen(name, RTLD_NOW | RTLD_NOLOAD);
            if (!lib) {
                continue;
            }
            const auto get = reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(lib, "RENDERDOC_GetAPI"));
            RENDERDOC_API_1_6_0* found = nullptr;
            if (get && get(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void**>(&found)) == 1 &&
                found) {
                std::string dir;
                if (const char* env = bb_env("BB_RENDERDOC_DIR"); env && *env) {
                    dir = env;
                } else {
                    dir = trigger;
                    dir = dir.substr(0, dir.find_last_of('/'));
                }
                found->SetCaptureFilePathTemplate((dir + "/bbport").c_str());
                std::printf("RenderDoc: in-application API from %s; captures go to %s\n", name,
                            dir.c_str());
                return found;
            }
        }
        std::printf("RenderDoc: no layer loaded (BB_RENDERDOC_TRIGGER needs RenderDoc's layer)\n");
        return nullptr;
    }();
    return api;
}

} // namespace

void Poll() {
    static const char* trigger = bb_env("BB_RENDERDOC_TRIGGER");
    if (!trigger || !*trigger) {
        return;
    }
    RENDERDOC_API_1_6_0* api = Api(trigger);
    if (api && std::remove(trigger) == 0) {
        api->TriggerCapture();
        std::printf("RenderDoc: capturing the next frame (%u captured so far)\n",
                    api->GetNumCaptures());
    }
}

} // namespace BbRenderDoc
