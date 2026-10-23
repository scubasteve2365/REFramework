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
// Values live in reframework/data/pragmatavr_hud.ini. The VR mod menu edits them in the headset.
//   Gamepad: click both thumbsticks = open/close, D-pad or left stick = move/change, RB held = big steps,
//            A = invert box / action, B = close. The game does not see the pad while the menu is open
//            (if it reads the pad through XInput, see pragmata_xinput_get_state in VR.cpp).
//   Keyboard: F1 open/close, arrows, Shift = big steps, Enter, Backspace.
// Pause = snapshot of both eye images. No other hotkeys.
//
// Menu drawing: VR::on_frame draws an ImGui window (ImGui frames render into REFramework's IMGUI
// render target while the headset is active); D3D12Component::on_frame copies that window's rectangle
// into each eye image, centered on the eye's forward direction, before the eye is sent to the headset.
//
// Included from VR.cpp (after its includes) and from vr/D3D12Component.cpp.

#include <Windows.h>
#include <Xinput.h>

#include <algorithm>
#include <cmath>
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
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

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
    bool hud_fixed{true};    // 3D HUD panels and the menu stay fixed in the room instead of following the head
    bool lock_invert{false}; // flips the fixed-HUD correction if it turns out mirrored on some runtime
    bool hide_ghost{true};   // hide Pragmata's offset "aberration" layers (zz_pragmatavr_hide_ghost.lua reads this)

    // HUD layout. 1 = "game screen": the game's own flat 16:9 HUD layout (its GUI camera projection) is shown as
    // one virtual screen floating in front of the player, like the DS3 VR HUD panel. 0 = per-panel (Builds 2-6).
    int layout_mode{1};
    std::array<float, 4> screen{70.0f, 2.0f, 0.0f, -8.0f}; // width (degrees), distance (m), left/right (deg), up/down (deg)
    std::array<bool, 4> screen_invert{};

    // Mod menu placement: a panel fixed in the room where it was opened.
    std::array<float, 3> menu{2.5f, 1.1f, -5.0f}; // distance (m), width (m), up/down (deg)
    std::array<bool, 3> menu_invert{};
    std::array<GroupTune, GroupCount> groups{};

    HudTune() {
        // Per-panel adjustments start neutral in the game-screen layout. Steve's per-panel hacking circle values
        // from 2026-10-23 (size 0.9322, x -275, y -75, gap 12) are kept in HACKING_CIRCLE_LOCKED.txt.
        hack_locked = false;
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

// Pseudo-group for the HUD screen rows, and its fields.
inline constexpr int GROUP_SCREEN = 100;
enum ScreenField : int {
    ScreenWidth = 0,
    ScreenDistance = 1,
    ScreenYaw = 2,
    ScreenPitch = 3,
};
inline constexpr std::array<const char*, 4> SCREEN_KEYS{"width", "distance", "yaw", "pitch"};
inline constexpr std::array<float, 4> SCREEN_MIN{20.0f, 0.5f, -60.0f, -45.0f};
inline constexpr std::array<float, 4> SCREEN_MAX{140.0f, 10.0f, 60.0f, 45.0f};
inline constexpr std::array<float, 4> SCREEN_STEP{2.0f, 0.1f, 1.0f, 1.0f};
inline constexpr std::array<float, 4> SCREEN_STEP_BIG{10.0f, 0.5f, 5.0f, 5.0f};

inline constexpr int GROUP_MENU = 101;
enum MenuField : int {
    MenuDistance = 0,
    MenuWidth = 1,
    MenuPitch = 2,
};
inline constexpr std::array<const char*, 3> MENU_KEYS{"distance", "width", "pitch"};
inline constexpr std::array<float, 3> MENU_MIN{0.8f, 0.3f, -40.0f};
inline constexpr std::array<float, 3> MENU_MAX{8.0f, 4.0f, 40.0f};
inline constexpr std::array<float, 3> MENU_STEP{0.1f, 0.05f, 1.0f};
inline constexpr std::array<float, 3> MENU_STEP_BIG{0.5f, 0.25f, 5.0f};

enum MenuAction : int {
    ActionNone = 0,
    ActionToggleLayout,
    ActionResetScreen,
    ActionResetMenu,
    ActionToggleHudFixed,
    ActionRecenter,
    ActionToggleLockInvert,
    ActionToggleGhost,
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

    // Gamepad menu input. get_state points at the real XInputGetState (the hook trampoline when the
    // game's XInput is hooked), so the menu still sees the pad while the game is being fed zeros.
    using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    std::atomic<XInputGetStateFn> xinput_get_state{nullptr};

    // True while the game should not see the pad: menu open, and a short time after it closes so the
    // closing press does not leak into the game.
    bool should_block_pad() const {
        if (menu_open.load()) {
            return true;
        }
        return std::chrono::steady_clock::now() < m_block_pad_until.load();
    }

    void poll_pad() {
        auto get_state = xinput_get_state.load();
        if (get_state == nullptr) {
            return;
        }

        XINPUT_STATE st{};
        bool have = false;
        for (DWORD i = 0; i < 4; ++i) {
            if (get_state(i, &st) == ERROR_SUCCESS) {
                have = true;
                break;
            }
        }

        WORD buttons = 0;
        float lx = 0.0f;
        float ly = 0.0f;
        if (have) {
            buttons = st.Gamepad.wButtons;
            lx = st.Gamepad.sThumbLX / 32767.0f;
            ly = st.Gamepad.sThumbLY / 32767.0f;
        }

        const WORD prev = m_pad_prev;
        m_pad_prev = buttons;
        const auto pressed = [&](WORD b) { return (buttons & b) != 0 && (prev & b) == 0; };

        // Left stick as a second D-pad: one step each time it is pushed past 60% and released.
        int stick_dir = 0; // 1 up, 2 down, 3 left, 4 right
        if (ly > 0.6f) stick_dir = 1;
        else if (ly < -0.6f) stick_dir = 2;
        else if (lx < -0.6f) stick_dir = 3;
        else if (lx > 0.6f) stick_dir = 4;
        const int prev_stick = m_stick_prev;
        if (std::fabs(lx) < 0.3f && std::fabs(ly) < 0.3f) {
            m_stick_prev = 0;
        } else if (stick_dir != 0) {
            m_stick_prev = stick_dir;
        }
        const bool stick_new = stick_dir != 0 && prev_stick == 0;

        const auto now = std::chrono::steady_clock::now();
        const bool both_sticks = (buttons & XINPUT_GAMEPAD_LEFT_THUMB) && (buttons & XINPUT_GAMEPAD_RIGHT_THUMB);
        const bool both_sticks_prev = (prev & XINPUT_GAMEPAD_LEFT_THUMB) && (prev & XINPUT_GAMEPAD_RIGHT_THUMB);

        if (both_sticks && !both_sticks_prev && now - m_last_chord > std::chrono::milliseconds(300)) {
            m_last_chord = now;
            handle_key(VK_F1, false);
            return;
        }

        if (!menu_open.load()) {
            return;
        }

        const bool big = (buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;

        if (pressed(XINPUT_GAMEPAD_DPAD_UP) || (stick_new && stick_dir == 1)) handle_key(VK_UP, big);
        if (pressed(XINPUT_GAMEPAD_DPAD_DOWN) || (stick_new && stick_dir == 2)) handle_key(VK_DOWN, big);
        if (pressed(XINPUT_GAMEPAD_DPAD_LEFT) || (stick_new && stick_dir == 3)) handle_key(VK_LEFT, big);
        if (pressed(XINPUT_GAMEPAD_DPAD_RIGHT) || (stick_new && stick_dir == 4)) handle_key(VK_RIGHT, big);
        if (pressed(XINPUT_GAMEPAD_A)) handle_key(VK_RETURN, big);
        if (pressed(XINPUT_GAMEPAD_B)) handle_key(VK_BACK, big);
    }

    // ---- Head tracking for the fixed HUD and the menu ----
    // update_head is called once per presented frame with the HMD pose in tracking space.
    // The game-screen HUD and the mod menu are real planes in tracking space (anchor pose + offsets), seen from each
    // eye's actual pose, so they stay put when the head turns, tilts or moves.
    std::atomic<bool> recenter_requested{false};
    std::atomic<bool> recenter_if_far{false};
    std::atomic<bool> menu_place_requested{false};

    struct Pose {
        glm::mat4 rot{1.0f};
        glm::vec3 pos{0.0f};
    };

    struct HeadState {
        Pose head{};
        Pose hud{};
        Pose menu{};
        bool valid{false};
    };

    static Matrix4x4f yaw_only(const Matrix4x4f& r) {
        const glm::vec4 f = r * glm::vec4{0.0f, 0.0f, -1.0f, 0.0f};
        const float yaw = std::atan2(-f.x, -f.z);
        return glm::rotate(glm::mat4{1.0f}, yaw, glm::vec3{0.0f, 1.0f, 0.0f});
    }

    void update_head(const Matrix4x4f& hmd_rotation, const glm::vec3& hmd_position) {
        glm::mat4 hmd = hmd_rotation;
        hmd[3] = glm::vec4{0.0f, 0.0f, 0.0f, 1.0f};

        std::scoped_lock _{m_head_mtx};
        m_state.head = Pose{hmd, hmd_position};

        bool recenter = recenter_requested.exchange(false) || !m_anchor_valid;

        if (recenter_if_far.exchange(false) && m_anchor_valid) {
            const glm::vec4 head_fwd = yaw_only(hmd) * glm::vec4{0.0f, 0.0f, -1.0f, 0.0f};
            const glm::vec4 anchor_fwd = m_state.hud.rot * glm::vec4{0.0f, 0.0f, -1.0f, 0.0f};
            if (glm::dot(glm::vec3{head_fwd}, glm::vec3{anchor_fwd}) < 0.7f) { // more than about 45 degrees away
                recenter = true;
            }
        }

        if (recenter) {
            m_state.hud = Pose{yaw_only(hmd), hmd_position};
            m_anchor_valid = true;
            spdlog::info("[PragmataVR] HUD recentered");
        }

        if (menu_place_requested.exchange(false) || !m_menu_valid) {
            m_state.menu = Pose{yaw_only(hmd), hmd_position};
            m_menu_valid = true;
        }

        m_state.valid = true;
        m_lock = glm::transpose(hmd) * m_state.hud.rot; // per-panel layout (rotation only)
    }

    // Current poses. When the HUD follows the head, its anchor is the head itself.
    HeadState head_state() {
        const auto t = tune();
        std::scoped_lock _{m_head_mtx};
        HeadState st = m_state;
        if (!t->hud_fixed) {
            st.hud = st.head;
        }
        return st;
    }

    // Per-panel layout only: view-space rotation head^-1 * anchor (identity when following the head).
    Matrix4x4f lock_rotation() {
        const auto t = tune();
        if (!t->hud_fixed) {
            return glm::mat4{1.0f};
        }

        std::scoped_lock _{m_head_mtx};
        return t->lock_invert ? glm::transpose(m_lock) : m_lock;
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

    static std::string format_menu(const HudTune& t, int field) {
        switch (field) {
        case MenuDistance: return fmt::format("{:.1f} m", t.menu[0]);
        case MenuWidth: return fmt::format("{:.2f} m", t.menu[1]);
        case MenuPitch: return fmt::format("{:+.0f} deg", t.menu[2]);
        default: return {};
        }
    }

    static std::string format_screen(const HudTune& t, int field) {
        switch (field) {
        case ScreenWidth: return fmt::format("{:.0f} deg", t.screen[0]);
        case ScreenDistance: return fmt::format("{:.1f} m", t.screen[1]);
        case ScreenYaw: return fmt::format("{:+.0f} deg", t.screen[2]);
        case ScreenPitch: return fmt::format("{:+.0f} deg", t.screen[3]);
        default: return {};
        }
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

        add(MenuRow::Kind::Header, -1, -1, ActionNone, "HUD screen (the game's whole HUD as one panel)");
        add(MenuRow::Kind::Toggle, -1, -1, ActionToggleLayout, "HUD layout");
        add(MenuRow::Kind::Value, GROUP_SCREEN, ScreenWidth, ActionNone, "Size (width)");
        add(MenuRow::Kind::Value, GROUP_SCREEN, ScreenDistance, ActionNone, "Distance");
        add(MenuRow::Kind::Value, GROUP_SCREEN, ScreenYaw, ActionNone, "Left / right");
        add(MenuRow::Kind::Value, GROUP_SCREEN, ScreenPitch, ActionNone, "Up / down");
        add(MenuRow::Kind::Action, GROUP_SCREEN, -1, ActionResetScreen, "Reset HUD screen to defaults");

        add(MenuRow::Kind::Header, -1, -1, ActionNone, "Mod menu (fixed where it opens)");
        add(MenuRow::Kind::Value, GROUP_MENU, MenuDistance, ActionNone, "Menu distance");
        add(MenuRow::Kind::Value, GROUP_MENU, MenuWidth, ActionNone, "Menu size (width)");
        add(MenuRow::Kind::Value, GROUP_MENU, MenuPitch, ActionNone, "Menu up / down");
        add(MenuRow::Kind::Action, GROUP_MENU, -1, ActionResetMenu, "Reset menu placement");

        add(MenuRow::Kind::Header, -1, -1, ActionNone, "View");
        add(MenuRow::Kind::Toggle, -1, -1, ActionToggleHudFixed, "HUD position");
        add(MenuRow::Kind::Action, -1, -1, ActionRecenter, "Recenter HUD in front of me");
        add(MenuRow::Kind::Toggle, -1, -1, ActionToggleLockInvert, "Fixed HUD direction");
        add(MenuRow::Kind::Toggle, -1, -1, ActionToggleGhost, "Ghost / double layer");

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
        s += "# The VR mod menu (both thumbsticks or F1) rewrites this file whenever a value changes.\n";
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
        s += fmt::format("hud_fixed={}\n", t.hud_fixed ? 1 : 0);
        s += fmt::format("lock_invert={}\n", t.lock_invert ? 1 : 0);
        s += fmt::format("hide_ghost={}\n", t.hide_ghost ? 1 : 0);
        s += "# layout_mode: 1 = game screen (whole HUD as one panel), 0 = per-panel.\n";
        s += "# screen.width in degrees, screen.distance in meters, screen.yaw / screen.pitch in degrees.\n";
        s += fmt::format("layout_mode={}\n", t.layout_mode);
        for (int i = 0; i < 4; ++i) {
            s += fmt::format("screen.{}={:.2f}\n", SCREEN_KEYS[i], t.screen[i]);
            s += fmt::format("screen.{}.invert={}\n", SCREEN_KEYS[i], t.screen_invert[i] ? 1 : 0);
        }
        s += "# menu.distance and menu.width in meters, menu.pitch in degrees.\n";
        for (int i = 0; i < 3; ++i) {
            s += fmt::format("menu.{}={:.2f}\n", MENU_KEYS[i], t.menu[i]);
            s += fmt::format("menu.{}.invert={}\n", MENU_KEYS[i], t.menu_invert[i] ? 1 : 0);
        }

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
                else if (key == "hud_fixed") t->hud_fixed = val != 0.0f;
                else if (key == "lock_invert") t->lock_invert = val != 0.0f;
                else if (key == "hide_ghost") t->hide_ghost = val != 0.0f;
                else if (key == "layout_mode") t->layout_mode = val != 0.0f ? 1 : 0;
                continue;
            }

            const auto group_key = key.substr(0, dot);
            auto rest = key.substr(dot + 1);
            const bool is_invert = rest.size() > 7 && rest.substr(rest.size() - 7) == ".invert";
            if (is_invert) {
                rest = rest.substr(0, rest.size() - 7);
            }

            if (group_key == "menu") {
                for (int i = 0; i < 3; ++i) {
                    if (rest == MENU_KEYS[i]) {
                        if (is_invert) t->menu_invert[i] = val != 0.0f;
                        else t->menu[i] = std::clamp(val, MENU_MIN[i], MENU_MAX[i]);
                    }
                }
                continue;
            }

            if (group_key == "screen") {
                for (int i = 0; i < 4; ++i) {
                    if (rest == SCREEN_KEYS[i]) {
                        if (is_invert) t->screen_invert[i] = val != 0.0f;
                        else t->screen[i] = std::clamp(val, SCREEN_MIN[i], SCREEN_MAX[i]);
                    }
                }
                continue;
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

    void block_pad_briefly() {
        m_block_pad_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
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
            if (!menu_open.load()) {
                block_pad_briefly();
            } else {
                recenter_if_far = true;
                menu_place_requested = true;
            }
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
            block_pad_briefly();
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
                if (row.group == GROUP_MENU) {
                    auto& minv = t->menu_invert[row.field];
                    minv = !minv;
                    commit(t);
                    play_click(true);
                    return;
                }
                if (row.group == GROUP_SCREEN) {
                    auto& inv = t->screen_invert[row.field];
                    inv = !inv;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: screen {} invert = {}", SCREEN_KEYS[row.field], inv);
                    play_click(true);
                    return;
                }
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
                if (row.action == ActionToggleLayout) {
                    t->layout_mode = t->layout_mode == 1 ? 0 : 1;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: HUD layout {}", t->layout_mode == 1 ? "game screen" : "per-panel");
                    play_click(true);
                } else if (row.action == ActionToggleHudFixed) {
                    t->hud_fixed = !t->hud_fixed;
                    commit(t);
                    recenter_requested = true;
                    spdlog::info("[PragmataVR] Menu: HUD {}", t->hud_fixed ? "fixed in room" : "follows head");
                    play_click(true);
                } else if (row.action == ActionToggleLockInvert) {
                    t->lock_invert = !t->lock_invert;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: fixed HUD direction {}", t->lock_invert ? "flipped" : "normal");
                    play_click(true);
                } else if (row.action == ActionToggleGhost) {
                    t->hide_ghost = !t->hide_ghost;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: ghost layer {}", t->hide_ghost ? "hidden" : "shown");
                    play_click(true);
                } else if (row.action == ActionToggleHackLock) {
                    t->hack_locked = !t->hack_locked;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: hacking circle {}", t->hack_locked ? "locked" : "unlocked");
                    play_click(true);
                }
                return;
            case MenuRow::Kind::Action:
                if (row.action == ActionResetMenu) {
                    const auto inverts = t->menu_invert;
                    t->menu = HudTune{}.menu;
                    t->menu_invert = inverts;
                    commit(t);
                    menu_place_requested = true;
                    spdlog::info("[PragmataVR] Menu: reset menu placement");
                    play_click(true);
                } else if (row.action == ActionResetScreen) {
                    const auto inverts = t->screen_invert;
                    t->screen = HudTune{}.screen;
                    t->screen_invert = inverts;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: reset HUD screen");
                    play_click(true);
                } else if (row.action == ActionResetGroup) {
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
                } else if (row.action == ActionRecenter) {
                    recenter_requested = true;
                    menu_place_requested = true;
                    spdlog::info("[PragmataVR] Menu: recenter HUD");
                    play_click(true);
                } else if (row.action == ActionSnapshot) {
                    request_snapshot();
                } else if (row.action == ActionClose) {
                    menu_open = false;
                    block_pad_briefly();
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

        if (row.group == GROUP_MENU) {
            int mdir = vk == VK_RIGHT ? 1 : -1;
            if (t->menu_invert[row.field]) {
                mdir = -mdir;
            }
            auto& v = t->menu[row.field];
            const float nv = std::clamp(v + mdir * (shift ? MENU_STEP_BIG[row.field] : MENU_STEP[row.field]),
                MENU_MIN[row.field], MENU_MAX[row.field]);
            const bool mok = nv != v;
            v = nv;
            if (mok) {
                commit(t);
            }
            play_click(mok);
            return;
        }

        if (row.group == GROUP_SCREEN) {
            int sdir = vk == VK_RIGHT ? 1 : -1;
            if (t->screen_invert[row.field]) {
                sdir = -sdir;
            }
            auto& v = t->screen[row.field];
            const float nv = std::clamp(v + sdir * (shift ? SCREEN_STEP_BIG[row.field] : SCREEN_STEP[row.field]),
                SCREEN_MIN[row.field], SCREEN_MAX[row.field]);
            const bool sok = nv != v;
            v = nv;
            if (sok) {
                commit(t);
            }
            spdlog::info("[PragmataVR] Menu: screen width={:.0f} distance={:.2f} yaw={:.0f} pitch={:.0f}{}",
                t->screen[0], t->screen[1], t->screen[2], t->screen[3], sok ? "" : " (limit)");
            play_click(sok);
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
    std::mutex m_head_mtx{};
    HeadState m_state{};
    Matrix4x4f m_lock{1.0f};
    bool m_anchor_valid{false};
    bool m_menu_valid{false};
    WORD m_pad_prev{0};
    int m_stick_prev{0};
    std::chrono::steady_clock::time_point m_last_chord{};
    std::atomic<std::chrono::steady_clock::time_point> m_block_pad_until{};
};

// A flat panel in tracking space: center, and right/up vectors scaled to the panel's half width / half height.
struct Plane {
    glm::vec3 center{0.0f};
    glm::vec3 right{0.0f};
    glm::vec3 up{0.0f};
};

// Panel `dist` meters from the anchor, turned yaw_deg right and pitch_deg up from the anchor's forward direction.
inline Plane make_plane(const HudTuning::Pose& anchor, float yaw_deg, float pitch_deg, float dist, float half_w, float half_h) {
    const glm::mat4 aim = glm::rotate(glm::mat4{1.0f}, -glm::radians(yaw_deg), glm::vec3{0.0f, 1.0f, 0.0f}) *
                          glm::rotate(glm::mat4{1.0f}, glm::radians(pitch_deg), glm::vec3{1.0f, 0.0f, 0.0f});
    const glm::mat4 r = anchor.rot * aim;

    Plane p{};
    p.center = anchor.pos + glm::vec3{r * glm::vec4{0.0f, 0.0f, -dist, 0.0f}};
    p.right = glm::vec3{r * glm::vec4{1.0f, 0.0f, 0.0f, 0.0f}} * half_w;
    p.up = glm::vec3{r * glm::vec4{0.0f, 1.0f, 0.0f, 0.0f}} * half_h;
    return p;
}

// Tracking space -> this eye's view space.
inline glm::mat4 eye_view(const HudTuning::Pose& head, const Matrix4x4f& eye_to_head) {
    const glm::mat4 head_world = glm::translate(glm::mat4{1.0f}, head.pos) * head.rot;
    return glm::inverse(head_world * glm::mat4{eye_to_head});
}

// Game-screen HUD: the game's flat GUI clip space (x, y in -1..1 over its own 16:9 layout) is mapped onto `plane`
// and projected into this eye: point = center + right * (x/w) + up * (y/w). As a matrix on flat clip coordinates
// that is B = [right, up, 0, center], so out = eye_proj * view * B * flat. Returns false if the panel's center is
// behind this eye.
inline bool screen_projection(Matrix4x4f& out, const Matrix4x4f& flat, const Matrix4x4f& eye_proj, const glm::mat4& view, const Plane& plane) {
    const glm::vec4 c = eye_proj * view * glm::vec4{plane.center, 1.0f};
    if (c.w <= 1e-4f) {
        return false;
    }

    glm::mat4 B{0.0f};
    B[0] = glm::vec4{plane.right, 0.0f};
    B[1] = glm::vec4{plane.up, 0.0f};
    B[2] = glm::vec4{0.0f};
    B[3] = glm::vec4{plane.center, 1.0f};

    out = eye_proj * view * B * glm::mat4{flat};
    return true;
}

// Game-screen HUD plane from the current settings and anchor.
inline Plane hud_plane(const HudTune& t, const HudTuning::Pose& anchor, const Matrix4x4f& flat) {
    const float dist = t.screen[ScreenDistance];
    const float half_w = dist * std::tan(glm::radians(t.screen[ScreenWidth]) * 0.5f);
    const float flat_aspect = flat[0][0] != 0.0f ? std::fabs(flat[1][1] / flat[0][0]) : (16.0f / 9.0f);
    return make_plane(anchor, t.screen[ScreenYaw], t.screen[ScreenPitch], dist, half_w, half_w / flat_aspect);
}

inline void adjust_projection(Matrix4x4f& m, float zoom, float x, float y) {
    m[0][0] *= zoom;
    m[1][1] *= zoom;
    m[2][0] *= zoom;
    m[2][1] *= zoom;
    m[2][0] += x * m[2][3];
    m[2][1] += y * m[2][3];
}

} // namespace pragmatavr
