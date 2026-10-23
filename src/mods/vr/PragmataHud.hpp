#pragma once

// PragmataVR: tuning for Pragmata's 3D HUD panels.
//
// Pragmata draws its combat HUD (hacking panel, health, stamina, weapon wheel, tutorials) as
// World-mode GUI views seen through the game's GUI camera. REFramework feeds that camera the
// headset projection (see VR::hijack_camera), which brings the panels into view, but at the game's
// flat-screen layout scale.
//
// Build 3 tunes the panels in three groups (hacking circle, health/stamina/weapon wheel, tutorial):
//   size, x/y position and a per-eye gap, applied to each panel's GUI view just before it draws and
//   restored right after. Values live in reframework/data/pragmatavr_hud.ini, which is re-read when
//   it changes and rewritten when a hotkey changes a value.
// Hotkeys (game window focused, no key repeat):
//   F5/F6 eye gap, F7/F8 size, F9/F10 left/right, F11/F12 up/down
//   no modifier = hacking circle, Shift = health/stamina/weapon wheel, Ctrl = tutorial
//   Pause = snapshot of both eyes, Shift+Pause = reset everything to defaults
// A short click confirms each press; a low buzz means a limit was reached.
//
// Included from VR.cpp (after its includes) and from vr/D3D12Component.cpp (snapshot only).

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

namespace pragmatavr {

enum class EyeMode : int {
    Current = 0, // the eye REFramework says is being rendered
    Swapped = 1, // the other eye
    Left = 2,    // always the left eye's projection
    Right = 3,   // always the right eye's projection
};

enum Group : int {
    GroupHack = 0,
    GroupVitals = 1,
    GroupTutorial = 2,
    GroupCount = 3,
};

inline constexpr std::array<const char*, GroupCount> GROUP_KEYS{"hack", "vitals", "tutorial"};
inline constexpr std::array<const char*, GroupCount> GROUP_LABELS{"hacking circle", "health/stamina/weapon wheel", "tutorial"};

struct GroupTune {
    float size{1.0f}; // multiplies the panel's own scale
    float x{0.0f};    // layout units (1920 x 1080 basis). For vitals this is "spread": + moves outward
    float y{0.0f};    // layout units, + is up
    float gap{0.0f};  // per-eye horizontal shift: left eye +gap, right eye -gap (+ pulls the images together)
};

struct HudTune {
    bool enabled{true};
    EyeMode eye_mode{EyeMode::Current};
    float zoom{1.0f};        // whole GUI camera projection zoom (all 3D panels)
    float depth_shift{0.0f}; // whole GUI camera projection per-eye shift, NDC units
    float y_sign{1.0f};      // flips the meaning of up/down if the game's layout y runs the other way
    bool log_elements{true};
    std::array<GroupTune, GroupCount> groups{};
};

// Which group a GUI GameObject belongs to, and its side for the vitals "spread" (-1 left, +1 right).
inline int group_for(std::string_view name, float& side) {
    side = 0.0f;
    if (name == "ui3400Gui") return GroupHack;
    if (name == "ui2130Gui") { side = -1.0f; return GroupVitals; } // health, lower left
    if (name == "ui2150Gui") { side = 1.0f; return GroupVitals; }  // weapon wheel, lower right
    if (name == "ui2140Gui" || name == "ui2160Gui") return GroupVitals; // stamina, HUD frame
    if (name == "ui2201Gui" || name == "ui2100Gui" || name == "ui2002_00Gui") return GroupTutorial;
    return -1;
}

// Set in VR::on_pre_gui_draw_element, cleared in VR::on_gui_draw_element.
inline thread_local size_t g_current_gui_hash = 0;
inline thread_local std::string g_current_gui_name{};

inline void play_click(bool ok) {
    std::thread([ok]() {
        if (ok) {
            Beep(1400, 35);
        } else {
            Beep(220, 180);
        }
    }).detach();
}

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

    void note_element(size_t hash, const std::string& name) {
        std::scoped_lock _{m_mtx};

        if (m_logged.emplace(hash, true).second) {
            spdlog::info("[PragmataVR] GUI camera projection requested by: {}", hash == 0 ? std::string{"(outside a GUI element draw)"} : name);
        }
    }

    // Called once per presented frame. Edge-triggered, ignores key repeat, only while the game has focus.
    void poll_keys() {
        DWORD fg_pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &fg_pid);
        const bool focused = fg_pid == GetCurrentProcessId();

        static constexpr std::array<int, 9> keys{VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12, VK_PAUSE};
        const auto now = std::chrono::steady_clock::now();

        for (size_t i = 0; i < keys.size(); ++i) {
            const bool down = (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
            const bool was_down = m_key_down[i];
            m_key_down[i] = down;

            if (!focused || !down || was_down) {
                continue;
            }

            if (now - m_last_press[i] < std::chrono::milliseconds(150)) {
                continue;
            }

            m_last_press[i] = now;

            const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            handle_key(keys[i], shift, ctrl);
        }
    }

    // Snapshot request state, consumed by D3D12Component::on_frame.
    std::atomic<int> snapshot_frames_left{0};
    std::atomic<bool> snapshot_left_done{false};
    std::atomic<bool> snapshot_right_done{false};
    std::string snapshot_stem{};

    static std::filesystem::path data_dir() {
        return REFramework::get_persistent_dir("reframework/data");
    }

private:
    HudTuning() {
        m_tune = std::make_shared<HudTune>();
        m_key_down.fill(false);
    }

    static std::filesystem::path path() {
        return data_dir() / "pragmatavr_hud.ini";
    }

    static std::string trim(const std::string& s) {
        const auto b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) {
            return {};
        }
        const auto e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    static std::string to_text(const HudTune& t) {
        std::string s{};
        s += "# PragmataVR 3D HUD tuning. Saved changes apply within about a second, no restart needed.\n";
        s += "# The hotkeys rewrite this file, so edits made while the game runs are kept only until the next key press.\n";
        s += "# Hotkeys: F5/F6 eye gap, F7/F8 size, F9/F10 left/right, F11/F12 up/down.\n";
        s += "#   No modifier = hacking circle, Shift = health/stamina/weapon wheel, Ctrl = tutorial.\n";
        s += "#   Pause = snapshot of both eyes, Shift+Pause = reset to defaults.\n";
        s += "# enabled: 1 = use these settings, 0 = plain headset projection, no panel changes.\n";
        s += "# eye_mode: 0 = current eye, 1 = swapped eyes, 2 = left eye for both, 3 = right eye for both.\n";
        s += "# zoom / depth_shift: whole 3D HUD projection (all panels together).\n";
        s += "# <group>.size / .x / .y / .gap: hack, vitals, tutorial. x/y in layout units (1920x1080 basis).\n";
        s += "# vitals.x is a spread: + moves health and weapon wheel outward, - moves them toward the center.\n";
        s += "# gap: + moves the left eye image right and the right eye image left.\n";
        s += fmt::format("enabled={}\n", t.enabled ? 1 : 0);
        s += fmt::format("eye_mode={}\n", (int)t.eye_mode);
        s += fmt::format("zoom={:.4f}\n", t.zoom);
        s += fmt::format("depth_shift={:.4f}\n", t.depth_shift);
        s += fmt::format("y_sign={:.0f}\n", t.y_sign);
        s += fmt::format("log_elements={}\n", t.log_elements ? 1 : 0);

        for (int g = 0; g < GroupCount; ++g) {
            const auto& gt = t.groups[g];
            s += fmt::format("{}.size={:.4f}\n", GROUP_KEYS[g], gt.size);
            s += fmt::format("{}.x={:.2f}\n", GROUP_KEYS[g], gt.x);
            s += fmt::format("{}.y={:.2f}\n", GROUP_KEYS[g], gt.y);
            s += fmt::format("{}.gap={:.2f}\n", GROUP_KEYS[g], gt.gap);
        }

        return s;
    }

    void write_file(const HudTune& t) {
        const auto p = path();
        std::error_code ec{};
        std::filesystem::create_directories(p.parent_path(), ec);

        const auto tmp = p.string() + ".tmp";
        {
            std::ofstream f{tmp, std::ios::trunc};
            if (!f) {
                spdlog::error("[PragmataVR] Could not write {}", tmp);
                return;
            }
            f << to_text(t);
        }

        std::filesystem::rename(tmp, p, ec);
        if (ec) {
            spdlog::error("[PragmataVR] Could not replace {}: {}", p.string(), ec.message());
            return;
        }

        std::scoped_lock _{m_mtx};
        m_mtime = std::filesystem::last_write_time(p, ec);
        m_loaded_once = true;
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
            write_file(HudTune{});
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
                else if (key == "y_sign") t->y_sign = val < 0.0f ? -1.0f : 1.0f;
                else if (key == "log_elements") t->log_elements = val != 0.0f;
                continue;
            }

            const auto group_key = key.substr(0, dot);
            const auto field = key.substr(dot + 1);

            for (int g = 0; g < GroupCount; ++g) {
                if (group_key != GROUP_KEYS[g]) {
                    continue;
                }

                auto& gt = t->groups[g];
                if (field == "size") gt.size = std::clamp(val, MIN_SIZE, MAX_SIZE);
                else if (field == "x") gt.x = std::clamp(val, -MAX_MOVE, MAX_MOVE);
                else if (field == "y") gt.y = std::clamp(val, -MAX_MOVE, MAX_MOVE);
                else if (field == "gap") gt.gap = std::clamp(val, -MAX_GAP, MAX_GAP);
            }
        }

        spdlog::info("[PragmataVR] Loaded pragmatavr_hud.ini: enabled={} eye_mode={} zoom={} depth_shift={} hack(size={} x={} y={} gap={}) vitals(size={} x={} y={} gap={}) tutorial(size={} x={} y={} gap={})",
            t->enabled, (int)t->eye_mode, t->zoom, t->depth_shift,
            t->groups[0].size, t->groups[0].x, t->groups[0].y, t->groups[0].gap,
            t->groups[1].size, t->groups[1].x, t->groups[1].y, t->groups[1].gap,
            t->groups[2].size, t->groups[2].x, t->groups[2].y, t->groups[2].gap);

        std::scoped_lock _{m_mtx};
        m_tune = t;
        m_mtime = mtime;
        m_loaded_once = true;
    }

    void handle_key(int vk, bool shift, bool ctrl) {
        if (vk == VK_PAUSE) {
            if (shift) {
                auto t = std::make_shared<HudTune>();
                write_file(*t);
                {
                    std::scoped_lock _{m_mtx};
                    m_tune = t;
                }
                spdlog::info("[PragmataVR] Hotkey: reset all HUD tuning to defaults");
                play_click(true);
                return;
            }

            if (snapshot_frames_left.load() > 0) {
                play_click(false);
                return;
            }

            const auto stamp = std::chrono::system_clock::now();
            const auto secs = std::chrono::duration_cast<std::chrono::seconds>(stamp.time_since_epoch()).count();
            snapshot_stem = fmt::format("snapshot_{}", secs);
            snapshot_left_done = false;
            snapshot_right_done = false;
            snapshot_frames_left = 8; // capture both eyes within the next few frames

            std::error_code ec{};
            std::filesystem::create_directories(data_dir() / "snapshots", ec);
            {
                std::ofstream f{data_dir() / "snapshots" / (snapshot_stem + "_settings.txt")};
                std::shared_ptr<const HudTune> cur{};
                {
                    std::scoped_lock _{m_mtx};
                    cur = m_tune;
                }
                f << to_text(*cur);
            }

            spdlog::info("[PragmataVR] Hotkey: snapshot requested ({})", snapshot_stem);
            play_click(true);
            return;
        }

        const int group = ctrl ? GroupTutorial : (shift ? GroupVitals : GroupHack);

        std::shared_ptr<const HudTune> cur{};
        {
            std::scoped_lock _{m_mtx};
            cur = m_tune;
        }

        auto t = std::make_shared<HudTune>(*cur);
        auto& gt = t->groups[group];
        bool ok = true;
        const char* what = "";

        const auto step = [&](float& v, float delta, float lo, float hi) {
            const float nv = std::clamp(v + delta, lo, hi);
            ok = nv != v;
            v = nv;
        };

        switch (vk) {
        case VK_F5: step(gt.gap, GAP_STEP, -MAX_GAP, MAX_GAP); what = "eye gap +"; break;
        case VK_F6: step(gt.gap, -GAP_STEP, -MAX_GAP, MAX_GAP); what = "eye gap -"; break;
        case VK_F7: {
            const float nv = std::clamp(gt.size / SIZE_STEP, MIN_SIZE, MAX_SIZE);
            ok = nv != gt.size;
            gt.size = nv;
            what = "smaller";
            break;
        }
        case VK_F8: {
            const float nv = std::clamp(gt.size * SIZE_STEP, MIN_SIZE, MAX_SIZE);
            ok = nv != gt.size;
            gt.size = nv;
            what = "bigger";
            break;
        }
        case VK_F9: step(gt.x, -MOVE_STEP, -MAX_MOVE, MAX_MOVE); what = group == GroupVitals ? "toward center" : "left"; break;
        case VK_F10: step(gt.x, MOVE_STEP, -MAX_MOVE, MAX_MOVE); what = group == GroupVitals ? "away from center" : "right"; break;
        case VK_F11: step(gt.y, MOVE_STEP, -MAX_MOVE, MAX_MOVE); what = "up"; break;
        case VK_F12: step(gt.y, -MOVE_STEP, -MAX_MOVE, MAX_MOVE); what = "down"; break;
        default: return;
        }

        if (ok) {
            write_file(*t);
            std::scoped_lock _{m_mtx};
            m_tune = t;
        }

        spdlog::info("[PragmataVR] Hotkey: {} {} -> size={:.3f} x={:.1f} y={:.1f} gap={:.1f}{}",
            GROUP_LABELS[group], what, gt.size, gt.x, gt.y, gt.gap, ok ? "" : " (limit)");
        play_click(ok);
    }

    static constexpr float SIZE_STEP = 1.08f;
    static constexpr float MIN_SIZE = 0.2f;
    static constexpr float MAX_SIZE = 5.0f;
    static constexpr float MOVE_STEP = 25.0f;
    static constexpr float MAX_MOVE = 2000.0f;
    static constexpr float GAP_STEP = 3.0f;
    static constexpr float MAX_GAP = 300.0f;

    std::mutex m_mtx{};
    std::shared_ptr<const HudTune> m_tune{};
    std::filesystem::file_time_type m_mtime{};
    std::chrono::steady_clock::time_point m_last_check{};
    bool m_loaded_once{false};
    std::unordered_map<size_t, bool> m_logged{};
    std::array<bool, 9> m_key_down{};
    std::array<std::chrono::steady_clock::time_point, 9> m_last_press{};
};

// Applies zoom and offset to a perspective projection matrix (glm, column-major).
inline void adjust_projection(Matrix4x4f& m, float zoom, float x, float y) {
    m[0][0] *= zoom;
    m[1][1] *= zoom;
    m[2][0] *= zoom;
    m[2][1] *= zoom;
    m[2][0] += x * m[2][3];
    m[2][1] += y * m[2][3];
}

} // namespace pragmatavr
