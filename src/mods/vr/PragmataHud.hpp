#pragma once

// PragmataVR: live-tunable placement for Pragmata's 3D HUD panels.
//
// Pragmata draws its combat HUD (hacking panel, health, stamina, weapon wheel, tutorials) as
// World-mode GUI views seen through the game's GUI camera. REFramework now feeds that camera the
// headset projection (see VR::hijack_camera), so the panels land in view, but at the game's own
// layout scale, which is tuned for a narrow flat-screen field of view.
//
// This file reads reframework/data/pragmatavr_hud.ini and adjusts the GUI camera projection per
// HUD element: zoom (size), x/y (screen offset) and which eye's projection to use. The file is
// re-read whenever it changes, so values can be tuned while the game is running.
//
// Only included from VR.cpp, after its own includes (needs glm, spdlog, utility::hash, REFramework).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

namespace pragmatavr {

enum class EyeMode : int {
    Current = 0, // the eye REFramework says is being rendered
    Swapped = 1, // the other eye (tests a left/right mix-up)
    Left = 2,    // always the left eye's projection
    Right = 3,   // always the right eye's projection
};

struct ElementTune {
    float zoom{1.0f};
    float x{0.0f};
    float y{0.0f};
};

struct HudTune {
    bool enabled{true};
    EyeMode eye_mode{EyeMode::Current};
    float zoom{1.0f};        // applies to every 3D HUD panel, multiplied by the element's own zoom
    float depth_shift{0.0f}; // per-eye horizontal shift in screen units; positive brings the HUD nearer
    bool log_elements{true};
    std::unordered_map<size_t, ElementTune> elements{};
};

// Set in VR::on_pre_gui_draw_element, cleared in VR::on_gui_draw_element.
inline thread_local size_t g_current_gui_hash = 0;
inline thread_local std::string g_current_gui_name{};

class HudTuning {
public:
    static HudTuning& get() {
        static HudTuning instance{};
        return instance;
    }

    std::shared_ptr<const HudTune> tune() {
        maybe_reload();
        std::scoped_lock _{m_mtx};
        return m_tune;
    }

    // Logs each element name the first time its GUI camera projection is requested.
    void note_element(size_t hash, const std::string& name) {
        std::scoped_lock _{m_mtx};

        if (m_logged.emplace(hash, true).second) {
            spdlog::info("[PragmataVR] GUI camera projection requested by: {}", hash == 0 ? std::string{"(outside a GUI element draw)"} : name);
        }
    }

private:
    HudTuning() {
        m_tune = std::make_shared<HudTune>();
    }

    static std::filesystem::path path() {
        return REFramework::get_persistent_dir("reframework/data/pragmatavr_hud.ini");
    }

    static std::string trim(const std::string& s) {
        const auto b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) {
            return {};
        }
        const auto e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    void write_default_file(const std::filesystem::path& p) {
        std::error_code ec{};
        std::filesystem::create_directories(p.parent_path(), ec);

        std::ofstream f{p};
        if (!f) {
            return;
        }

        f << "# PragmataVR 3D HUD tuning. Saved changes apply within about a second, no restart needed.\n"
          << "# enabled: 1 = use these settings, 0 = plain headset projection.\n"
          << "# eye_mode: 0 = current eye, 1 = swapped eyes, 2 = left eye for both, 3 = right eye for both.\n"
          << "# zoom: size of all 3D HUD panels (2.0 = twice as big).\n"
          << "# depth_shift: positive pulls the 3D HUD nearer, negative pushes it farther (try 0.01 steps).\n"
          << "# Per panel: <name>.zoom, <name>.x, <name>.y (x/y in screen units, 0.1 = a twentieth of the view).\n"
          << "enabled=1\n"
          << "eye_mode=0\n"
          << "zoom=1.0\n"
          << "depth_shift=0.0\n"
          << "log_elements=1\n";
    }

    void maybe_reload() {
        const auto now = std::chrono::steady_clock::now();

        {
            std::scoped_lock _{m_mtx};
            if (now - m_last_check < std::chrono::milliseconds(500)) {
                return;
            }
            m_last_check = now;
        }

        const auto p = path();
        std::error_code ec{};

        if (!std::filesystem::exists(p, ec)) {
            write_default_file(p);
        }

        const auto mtime = std::filesystem::last_write_time(p, ec);
        if (ec) {
            return;
        }

        {
            std::scoped_lock _{m_mtx};
            if (m_loaded_once && mtime == m_mtime) {
                return;
            }
        }

        auto t = std::make_shared<HudTune>();
        std::ifstream f{p};
        std::string line{};

        while (std::getline(f, line)) {
            const auto hash_pos = line.find('#');
            if (hash_pos != std::string::npos) {
                line = line.substr(0, hash_pos);
            }

            const auto eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }

            const auto key = trim(line.substr(0, eq));
            const auto val_str = trim(line.substr(eq + 1));
            if (key.empty() || val_str.empty()) {
                continue;
            }

            float val = 0.0f;
            try {
                val = std::stof(val_str);
            } catch (...) {
                spdlog::warn("[PragmataVR] pragmatavr_hud.ini: bad value for {}: {}", key, val_str);
                continue;
            }

            const auto dot = key.find('.');
            if (dot == std::string::npos) {
                if (key == "enabled") t->enabled = val != 0.0f;
                else if (key == "eye_mode") t->eye_mode = (EyeMode)std::clamp((int)val, 0, 3);
                else if (key == "zoom") t->zoom = std::clamp(val, 0.05f, 20.0f);
                else if (key == "depth_shift") t->depth_shift = std::clamp(val, -1.0f, 1.0f);
                else if (key == "log_elements") t->log_elements = val != 0.0f;
                continue;
            }

            const auto name = key.substr(0, dot);
            const auto field = key.substr(dot + 1);
            auto& e = t->elements[utility::hash(name)];

            if (field == "zoom") e.zoom = std::clamp(val, 0.05f, 20.0f);
            else if (field == "x") e.x = std::clamp(val, -5.0f, 5.0f);
            else if (field == "y") e.y = std::clamp(val, -5.0f, 5.0f);
        }

        spdlog::info("[PragmataVR] Loaded pragmatavr_hud.ini: enabled={} eye_mode={} zoom={} depth_shift={} panels={}",
            t->enabled, (int)t->eye_mode, t->zoom, t->depth_shift, t->elements.size());

        std::scoped_lock _{m_mtx};
        m_tune = t;
        m_mtime = mtime;
        m_loaded_once = true;
    }

    std::mutex m_mtx{};
    std::shared_ptr<const HudTune> m_tune{};
    std::filesystem::file_time_type m_mtime{};
    std::chrono::steady_clock::time_point m_last_check{};
    bool m_loaded_once{false};
    std::unordered_map<size_t, bool> m_logged{};
};

// Applies zoom and offset to a perspective projection matrix (glm, column-major).
// Clip x = m[0][0]*x + m[2][0]*z, w = m[2][3]*z. Scaling the x/y terms zooms about the
// frustum's own center; adding d * m[2][3] to m[2][0] shifts the result by d in NDC.
inline void adjust_projection(Matrix4x4f& m, float zoom, float x, float y) {
    m[0][0] *= zoom;
    m[1][1] *= zoom;
    m[2][0] *= zoom;
    m[2][1] *= zoom;
    m[2][0] += x * m[2][3];
    m[2][1] += y * m[2][3];
}

} // namespace pragmatavr
