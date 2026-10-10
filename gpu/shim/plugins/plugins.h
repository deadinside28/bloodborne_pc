// SPDX-License-Identifier: GPL-3.0-or-later
// bbport plugins: shared libraries with bbhost's plugin API (plugins/bbhost_plugin.h, version
// 11; github.com/droogie/bbhost, GPL-3.0-or-later), so a bbhost plugin (bbcoop, the seamless
// co-op) runs here unchanged. They load from <bb-probe dir>/plugins and $BB_DATA_DIR/plugins
// while the image is patched; a plugin flagged opt-in loads only with plugins.<name>=1 in
// bbport.ini, and its settings are <name>.<key> there. BB_PLUGINS=0 loads none.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// From bbgpu_patch_image: loads the plugins and runs both their phases (init, image), and
// places the game hooks they need (the frame hook first of all).
void plugins_start(unsigned char* image, std::uint64_t size);

// For the in-game menu (bbport_overlay.cpp).
struct PluginOption {
    std::uint32_t type = 0;  // BB_OPT_*, without BB_OPT_LIVE
    bool live = false;       // takes effect at once (else at the next start)
    std::string key, label, help, def, choices;
    double min = 0, max = 0;
};
struct PluginInfo {
    std::string name, title, version, description;
    std::uint32_t flags = 0;
    bool loaded = false;   // running in this session
    bool enabled = false;  // plugins.<name> (opt-in ones default off)
    std::vector<PluginOption> options;
};
// Every plugin file found (read without starting it), with whether it runs now.
std::vector<PluginInfo> plugins_list();
std::string plugins_option_value(const std::string& name, const PluginOption& o);
// A setting: written to bbport.ini, and for a running plugin handed to its on_option callback
// on the main thread at the next frame.
void plugins_set_option(const std::string& name, const std::string& key, const std::string& value);
void plugins_run_action(const std::string& name, const std::string& action);
void plugins_set_enabled(const std::string& name, bool on);
// A plugin's show_message, while it lasts: the text and how much of it is left (0..1).
bool plugins_banner(std::string* text, float* left);
