#pragma once

// PragmataVR: tuning and in-headset menu for Pragmata's 3D HUD panels.
//
// Pragmata draws its combat HUD (hacking panel, health, stamina, weapon wheel, tutorials) as
// World-mode GUI views seen through the game's GUI camera. REFramework feeds that camera the
// headset projection (see VR::hijack_camera), which brings the panels into view, but at the game's
// flat-screen layout scale.
//
// The panels are tuned in three groups (hacking circle, health/stamina/weapon wheel, tutorial):
// size, x/y position and a per-eye gap, applied to each panel's GUI view just before it draws and
// restored right after (VR::on_pre_gui_draw_element / on_gui_draw_element).
//
// Values live in reframework/data/pragmatavr_hud.ini. The VR mod menu (F1) edits them in the headset:
//   F1 open/close, Up/Down select, Left/Right change (hold Shift for big steps), Enter = toggle the
//   invert box on value rows or run the action on button rows, Backspace = close.
// Pause = snapshot of both eye images. No other hotkeys.
//
// Menu drawing: VR::on_frame draws an ImGui window (ImGui frames render into REFramework's IMGUI
// render target while the headset is active); D3D12Component::on_frame copies that window's rectangle
// into each eye image, centered on the eye's forward direction, before the eye is sent to the headset.
//
// Included from VR.cpp (after its includes) and from vr/D3D12Component.cpp.

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
    Current = 0,
    Swapped = 1,
    Left = 2,
    Right = 3,
};

enum Group : int {
    GroupHack = 0,
    GroupVitals = 1,
    GroupTutorial = 2,
    GroupCount = 3,
};

enum Field : int {
    FieldSize = 0,
    FieldX = 1,
    FieldY = 2,
    FieldGap = 3,
    FieldCount = 4,
};

inline constexpr std::array<const char*, GroupCount> GROUP_KEYS{"hack", "vitals", "tutorial"};
inline constexpr std::array<const char*, GroupCount> GROUP_LABELS{"Hacking circle", "Health / stamina / weapon wheel", "Tutorial box"};
inline constexpr std::array<const char*, FieldCount> FIELD_KEYS{"size", "x", "y", "gap"};

struct GroupTune {
    float size{1.0f}; // multiplies the panel's own scale
    float x{0.0f};    // layout units (1920 x 1080 basis). For vitals this is "spread": + moves outward
    float y{0.0f};    // layout units
    float gap{0.0f};  // per-eye horizontal shift: left eye +gap, right eye -gap
    std::array<bool, FieldCount> invert{};
};

struct HudTune {
    bool enabled{true};
    EyeMode eye_mode{EyeMode::Current};
    float zoom{1.0f};
    float depth_shift{0.0f};
    float y_sign{1.0f};
    bool log_elements{true};
    bool hack_locked{true};
    float menu_width{0.42f}; // menu width as a fraction of the eye image width
    std::array<GroupTune, GroupCount> groups{};

    HudTune() {
        // Steve's tuned hacking circle (Pimax Dream Air, 2026-10-23).
        groups[GroupHack].size = 0.9322f;
        groups[GroupHack].x = -275.0f;
        groups[GroupHack].y = -75.0f;
        groups[GroupHack].gap = 12.0f;
    }
};

inline GroupTune default_group(int g) {
    return HudTune{}.groups[g];
}

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

inline thread_local size_t g_current_gui_hash = 0;
inline thread_local std::string g_current_gui_name{};

inline void play_click(bool ok) {
    std::thread([ok]() {
        if (ok) {
            Beep(1400, 30);
        } else {
            Beep(220, 160);
        }
    }).detach();
}

// One row of the VR mod menu.
struct MenuRow {
    enum class Kind { Header, Value, Toggle, Action };
    Kind kind{Kind::Header};
    int group{-1};
    int field{-1};
    int action{0};
    std::string label{};
};

enum MenuAction : int {
    ActionNone = 0,
    ActionToggleHackLock,
    ActionResetGroup,
    ActionSnapshot,
    ActionClose,
};

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

    // Called once per presented frame. Edge-triggered, no key repeat, only while the game has focus.
    void poll_keys() {
        DWORD fg_pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &fg_pid);
        const bool focused = fg_pid == GetCurrentProcessId();

        const auto now = std::chrono::steady_clock::now();

        for (size_t i = 0; i < KEYS.size(); ++i) {
            const bool down = (GetAsyncKeyState(KEYS[i]) & 0x8000) != 0;
            const bool was_down = m_key_down[i];
            m_key_down[i] = down;

            if (!focused || !down || was_down) {
                continue;
            }

            if (now - m_last_press[i] < std::chrono::milliseconds(120)) {
                continue;
            }

            m_last_press[i] = now;
            const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            handle_key(KEYS[i], shift);
        }
    }

    // ---- Menu state, read by VR::on_frame (draw) and D3D12Component::on_frame (composite) ----
    std::atomic<bool> menu_open{false};
    std::atomic<int> menu_selected{0};
    std::atomic<float> menu_rect_x{0.0f};
    std::atomic<float> menu_rect_y{0.0f};
    std::atomic<float> menu_rect_w{0.0f};
    std::atomic<float> menu_rect_h{0.0f};
    std::atomic<bool> menu_rect_valid{false};

    const std::vector<MenuRow>& menu_rows() const { return m_rows; }

    // ---- Snapshot request state, consumed by D3D12Component::on_frame ----
    std::atomic<int> snapshot_frames_left{0};
    std::atomic<bool> snapshot_left_done{false};
    std::atomic<bool> snapshot_right_done{false};
    std::string snapshot_stem{};

    static std::filesystem::path data_dir() {
        return REFramework::get_persistent_dir("reframework/data");
    }

    // Text for one value row, e.g. "Size  0.93", used by the menu.
    static std::string format_value(const GroupTune& gt, int group, int field) {
        switch (field) {
        case FieldSize: return fmt::format("{:.2f}x", gt.size);
        case FieldX: return fmt::format("{:+.0f}", gt.x);
        case FieldY: return fmt::format("{:+.0f}", gt.y);
        case FieldGap: return fmt::format("{:+.0f}", gt.gap);
        default: return {};
        }
    }

private:
    static constexpr std::array<int, 8> KEYS{VK_F1, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_BACK, VK_PAUSE};

    HudTuning() {
        m_tune = std::make_shared<HudTune>();
        m_key_down.fill(false);
        build_rows();
    }

    void build_rows() {
        m_rows.clear();

        const auto add = [&](MenuRow::Kind kind, int group, int field, int action, std::string label) {
            MenuRow r{};
            r.kind = kind;
            r.group = group;
            r.field = field;
            r.action = action;
            r.label = std::move(label);
            m_rows.push_back(std::move(r));
        };

        for (int g = 0; g < GroupCount; ++g) {
            add(MenuRow::Kind::Header, g, -1, ActionNone, GROUP_LABELS[g]);

            if (g == GroupHack) {
                add(MenuRow::Kind::Toggle, g, -1, ActionToggleHackLock, "Locked (Enter to unlock)");
            }

            add(MenuRow::Kind::Value, g, FieldSize, ActionNone, "Size");
            add(MenuRow::Kind::Value, g, FieldX, ActionNone, g == GroupVitals ? "Toward / away from center" : "Left / right");
            add(MenuRow::Kind::Value, g, FieldY, ActionNone, "Up / down");
            add(MenuRow::Kind::Value, g, FieldGap, ActionNone, "Eye gap (double image)");
            add(MenuRow::Kind::Action, g, -1, ActionResetGroup, g == GroupHack ? "Restore locked-in values" : "Reset to defaults");
        }

        add(MenuRow::Kind::Header, -1, -1, ActionNone, "Other");
        add(MenuRow::Kind::Action, -1, -1, ActionSnapshot, "Take snapshot (both eyes)");
        add(MenuRow::Kind::Action, -1, -1, ActionClose, "Close menu");

        // Start on the first selectable row.
        for (int i = 0; i < (int)m_rows.size(); ++i) {
            if (m_rows[i].kind != MenuRow::Kind::Header) {
                menu_selected = i;
                break;
            }
        }
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
        s += "# The VR mod menu (F1) rewrites this file whenever a value changes.\n";
        s += "# Menu: F1 open/close, Up/Down select, Left/Right change (Shift = big steps), Enter = invert box or action,\n";
        s += "#   Backspace close. Pause = snapshot of both eyes.\n";
        s += "# <group>.size / .x / .y / .gap: hack, vitals, tutorial. x/y/gap in layout units (1920x1080 basis).\n";
        s += "# vitals.x is a spread: + moves health and weapon wheel outward, - toward the center.\n";
        s += "# <group>.<field>.invert = 1 flips the direction Left/Right changes that value in the menu.\n";
        s += "# hack_locked = 1 stops the menu changing the hacking circle.\n";
        s += fmt::format("enabled={}\n", t.enabled ? 1 : 0);
        s += fmt::format("eye_mode={}\n", (int)t.eye_mode);
        s += fmt::format("zoom={:.4f}\n", t.zoom);
        s += fmt::format("depth_shift={:.4f}\n", t.depth_shift);
        s += fmt::format("y_sign={:.0f}\n", t.y_sign);
        s += fmt::format("log_elements={}\n", t.log_elements ? 1 : 0);
        s += fmt::format("hack_locked={}\n", t.hack_locked ? 1 : 0);
        s += fmt::format("menu_width={:.3f}\n", t.menu_width);

        for (int g = 0; g < GroupCount; ++g) {
            const auto& gt = t.groups[g];
            s += fmt::format("{}.size={:.4f}\n", GROUP_KEYS[g], gt.size);
            s += fmt::format("{}.x={:.2f}\n", GROUP_KEYS[g], gt.x);
            s += fmt::format("{}.y={:.2f}\n", GROUP_KEYS[g], gt.y);
            s += fmt::format("{}.gap={:.2f}\n", GROUP_KEYS[g], gt.gap);

            for (int f = 0; f < FieldCount; ++f) {
                s += fmt::format("{}.{}.invert={}\n", GROUP_KEYS[g], FIELD_KEYS[f], gt.invert[f] ? 1 : 0);
            }
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
                else if (key == "hack_locked") t->hack_locked = val != 0.0f;
                else if (key == "menu_width") t->menu_width = std::clamp(val, 0.15f, 0.9f);
                continue;
            }

            const auto group_key = key.substr(0, dot);
            auto rest = key.substr(dot + 1);
            const bool is_invert = rest.size() > 7 && rest.substr(rest.size() - 7) == ".invert";
            if (is_invert) {
                rest = rest.substr(0, rest.size() - 7);
            }

            for (int g = 0; g < GroupCount; ++g) {
                if (group_key != GROUP_KEYS[g]) {
                    continue;
                }

                auto& gt = t->groups[g];

                for (int fi = 0; fi < FieldCount; ++fi) {
                    if (rest != FIELD_KEYS[fi]) {
                        continue;
                    }

                    if (is_invert) {
                        gt.invert[fi] = val != 0.0f;
                        continue;
                    }

                    switch (fi) {
                    case FieldSize: gt.size = std::clamp(val, MIN_SIZE, MAX_SIZE); break;
                    case FieldX: gt.x = std::clamp(val, -MAX_MOVE, MAX_MOVE); break;
                    case FieldY: gt.y = std::clamp(val, -MAX_MOVE, MAX_MOVE); break;
                    case FieldGap: gt.gap = std::clamp(val, -MAX_GAP, MAX_GAP); break;
                    default: break;
                    }
                }
            }
        }

        spdlog::info("[PragmataVR] Loaded pragmatavr_hud.ini: hack_locked={} hack(size={} x={} y={} gap={}) vitals(size={} x={} y={} gap={}) tutorial(size={} x={} y={} gap={})",
            t->hack_locked,
            t->groups[0].size, t->groups[0].x, t->groups[0].y, t->groups[0].gap,
            t->groups[1].size, t->groups[1].x, t->groups[1].y, t->groups[1].gap,
            t->groups[2].size, t->groups[2].x, t->groups[2].y, t->groups[2].gap);

        std::scoped_lock _{m_mtx};
        m_tune = t;
        m_mtime = mtime;
        m_loaded_once = true;
    }

    void commit(const std::shared_ptr<HudTune>& t) {
        write_file(*t);
        std::scoped_lock _{m_mtx};
        m_tune = t;
    }

    std::shared_ptr<HudTune> copy_current() {
        std::scoped_lock _{m_mtx};
        return std::make_shared<HudTune>(*m_tune);
    }

    void move_selection(int dir) {
        const int n = (int)m_rows.size();
        int i = menu_selected.load();

        for (int step = 0; step < n; ++step) {
            i = (i + dir + n) % n;
            if (m_rows[i].kind != MenuRow::Kind::Header) {
                menu_selected = i;
                return;
            }
        }
    }

    void request_snapshot() {
        if (snapshot_frames_left.load() > 0) {
            play_click(false);
            return;
        }

        const auto stamp = std::chrono::system_clock::now();
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(stamp.time_since_epoch()).count();
        snapshot_stem = fmt::format("snapshot_{}", secs);
        snapshot_left_done = false;
        snapshot_right_done = false;

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

        snapshot_frames_left = 8;
        spdlog::info("[PragmataVR] Snapshot requested ({})", snapshot_stem);
        play_click(true);
    }

    void handle_key(int vk, bool shift) {
        if (vk == VK_PAUSE) {
            request_snapshot();
            return;
        }

        if (vk == VK_F1) {
            menu_open = !menu_open.load();
            spdlog::info("[PragmataVR] Menu {}", menu_open.load() ? "opened" : "closed");
            play_click(true);
            return;
        }

        if (!menu_open.load()) {
            return;
        }

        switch (vk) {
        case VK_BACK:
            menu_open = false;
            spdlog::info("[PragmataVR] Menu closed");
            play_click(true);
            return;
        case VK_UP:
            move_selection(-1);
            play_click(true);
            return;
        case VK_DOWN:
            move_selection(1);
            play_click(true);
            return;
        default:
            break;
        }

        const int sel = menu_selected.load();
        if (sel < 0 || sel >= (int)m_rows.size()) {
            return;
        }

        const auto& row = m_rows[sel];
        auto t = copy_current();

        if (vk == VK_RETURN) {
            switch (row.kind) {
            case MenuRow::Kind::Value: {
                if (row.group == GroupHack && t->hack_locked) {
                    play_click(false);
                    return;
                }
                auto& inv = t->groups[row.group].invert[row.field];
                inv = !inv;
                commit(t);
                spdlog::info("[PragmataVR] Menu: {} {} invert = {}", GROUP_KEYS[row.group], FIELD_KEYS[row.field], inv);
                play_click(true);
                return;
            }
            case MenuRow::Kind::Toggle:
                if (row.action == ActionToggleHackLock) {
                    t->hack_locked = !t->hack_locked;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: hacking circle {}", t->hack_locked ? "locked" : "unlocked");
                    play_click(true);
                }
                return;
            case MenuRow::Kind::Action:
                if (row.action == ActionResetGroup) {
                    if (row.group == GroupHack && t->hack_locked) {
                        play_click(false);
                        return;
                    }
                    const auto inverts = t->groups[row.group].invert;
                    t->groups[row.group] = default_group(row.group);
                    t->groups[row.group].invert = inverts;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: reset {}", GROUP_KEYS[row.group]);
                    play_click(true);
                } else if (row.action == ActionSnapshot) {
                    request_snapshot();
                } else if (row.action == ActionClose) {
                    menu_open = false;
                    play_click(true);
                }
                return;
            default:
                return;
            }
        }

        if ((vk != VK_LEFT && vk != VK_RIGHT) || row.kind != MenuRow::Kind::Value) {
            return;
        }

        if (row.group == GroupHack && t->hack_locked) {
            play_click(false);
            return;
        }

        auto& gt = t->groups[row.group];
        int dir = vk == VK_RIGHT ? 1 : -1;
        if (gt.invert[row.field]) {
            dir = -dir;
        }

        bool ok = true;
        const auto step = [&](float& v, float delta, float lo, float hi) {
            const float nv = std::clamp(v + delta, lo, hi);
            ok = nv != v;
            v = nv;
        };

        switch (row.field) {
        case FieldSize: {
            const float factor = shift ? SIZE_STEP_BIG : SIZE_STEP;
            const float nv = std::clamp(dir > 0 ? gt.size * factor : gt.size / factor, MIN_SIZE, MAX_SIZE);
            ok = nv != gt.size;
            gt.size = nv;
            break;
        }
        case FieldX: step(gt.x, dir * (shift ? MOVE_STEP_BIG : MOVE_STEP), -MAX_MOVE, MAX_MOVE); break;
        case FieldY: step(gt.y, dir * (shift ? MOVE_STEP_BIG : MOVE_STEP), -MAX_MOVE, MAX_MOVE); break;
        case FieldGap: step(gt.gap, dir * (shift ? GAP_STEP_BIG : GAP_STEP), -MAX_GAP, MAX_GAP); break;
        default: return;
        }

        if (ok) {
            commit(t);
        }

        spdlog::info("[PragmataVR] Menu: {} {} -> size={:.3f} x={:.1f} y={:.1f} gap={:.1f}{}",
            GROUP_KEYS[row.group], FIELD_KEYS[row.field], gt.size, gt.x, gt.y, gt.gap, ok ? "" : " (limit)");
        play_click(ok);
    }

    static constexpr float SIZE_STEP = 1.04f;
    static constexpr float SIZE_STEP_BIG = 1.20f;
    static constexpr float MIN_SIZE = 0.2f;
    static constexpr float MAX_SIZE = 5.0f;
    static constexpr float MOVE_STEP = 10.0f;
    static constexpr float MOVE_STEP_BIG = 50.0f;
    static constexpr float MAX_MOVE = 2000.0f;
    static constexpr float GAP_STEP = 1.0f;
    static constexpr float GAP_STEP_BIG = 5.0f;
    static constexpr float MAX_GAP = 300.0f;

    std::mutex m_mtx{};
    std::shared_ptr<const HudTune> m_tune{};
    std::filesystem::file_time_type m_mtime{};
    std::chrono::steady_clock::time_point m_last_check{};
    bool m_loaded_once{false};
    std::unordered_map<size_t, bool> m_logged{};
    std::array<bool, KEYS.size()> m_key_down{};
    std::array<std::chrono::steady_clock::time_point, KEYS.size()> m_last_press{};
    std::vector<MenuRow> m_rows{};
};

inline void adjust_projection(Matrix4x4f& m, float zoom, float x, float y) {
    m[0][0] *= zoom;
    m[1][1] *= zoom;
    m[2][0] *= zoom;
    m[2][1] *= zoom;
    m[2][0] += x * m[2][3];
    m[2][1] += y * m[2][3];
}

} // namespace pragmatavr
