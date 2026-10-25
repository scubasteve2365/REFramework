#pragma once

// PragmataVR: tuning and in-headset menu for Pragmata's 3D HUD panels.
//
// Pragmata draws its combat HUD (hacking panel, health, stamina, weapon wheel, tutorials) as
// World-mode GUI views seen through the game's GUI camera. REFramework feeds that camera the
// headset projection (see VR::hijack_camera), which brings the panels into view, but at the game's
// flat-screen layout scale.
//
// Build 11: each HUD element is tuned on its own (hacking circle, health, stamina, weapon wheel, tutorial box):
// size, x/y position and a per-eye gap, applied to each panel's GUI view just before it draws and restored right
// after (VR::on_pre_gui_draw_element / on_gui_draw_element). Size grows an element around its own center (found from
// the panel's GUI tree), so a bigger element stays where it is instead of sliding off toward the edge of the view.
// The hacking circle keeps the Build 3-6 behaviour it was tuned with, and is locked at Steve's values by default.
//
// Values live in reframework/data/pragmatavr_hud.ini. The VR mod menu edits them in the headset.
//   Gamepad (Build 10): click both thumbsticks = open/close. D-pad or left stick only navigates (Up/Down rows,
//            Left/Right to a value's INV box and between columns). A = edit the highlighted value (then Left/Right
//            change it, RB held = big steps, A or B = done), flip the highlighted INV box, toggle or run an action.
//            B = stop editing, or close. The game does not see the pad while the menu is open
//            (if it reads the pad through XInput, see pragmata_xinput_get_state in VR.cpp).
//   Keyboard: F1 open/close, arrows, Shift = big steps, Enter = A, Backspace = B.
// Pause = snapshot of both eye images. No other hotkeys.
//
// Menu drawing (Build 9): VR::on_frame draws the menu as one ImGui window (ImGui frames render into
// REFramework's IMGUI render target while the headset is active). Under OpenXR, D3D12Component::on_frame
// copies that window's rectangle into a dedicated swapchain, and OpenXR::end_frame submits it as a quad
// composition layer fixed in the room (stage space). The headset's compositor draws the quad with full
// perspective from each eye's real pose, so the menu cannot bend, tilt or follow the head. Its physical
// size comes from the window's pixel size, so it always fits every row. OpenVR keeps the Build 8 path
// (menu pasted into each eye image).
//
// Included from VR.cpp (after its includes) and from vr/D3D12Component.cpp.

#include <Windows.h>
#include <mmsystem.h>
#include <Xinput.h>

#pragma comment(lib, "winmm.lib")

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
#include <glm/gtc/quaternion.hpp>

namespace pragmatavr {

enum class EyeMode : int {
    Current = 0,
    Swapped = 1,
    Left = 2,
    Right = 3,
};

enum Group : int {
    GroupHack = 0,
    GroupHealth = 1,
    GroupStamina = 2,
    GroupWeapons = 3,
    GroupTutorial = 4,
    GroupCount = 5,
};

enum Field : int {
    FieldSize = 0,
    FieldX = 1,
    FieldY = 2,
    FieldGap = 3,
    FieldCount = 4,
};

inline constexpr std::array<const char*, GroupCount> GROUP_KEYS{"hack", "health", "stamina", "weapons", "tutorial"};
inline constexpr std::array<const char*, GroupCount> GROUP_LABELS{"Hacking circle", "Health", "Stamina", "Weapon wheel", "Tutorial box"};
inline constexpr std::array<const char*, GroupCount> GROUP_TITLES{"HACKING CIRCLE", "HEALTH", "STAMINA", "WEAPON WHEEL", "TUTORIAL BOX"};

// The ini format this build writes. Older files (Build 10 and before) had the hacking circle reset to neutral by the
// Build 8 fresh ini, so their hack.* values are ignored and the locked values below are used instead.
inline constexpr int INI_VERSION = 12;
// hud_fixed and hack_locked lines are ignored in files older than this (Build 11 wrote hud_fixed=0 as its default).
inline constexpr int INI_VERSION_FIXED_HUD = 12;
// hack.* lines are ignored in files older than this (Build 10 and before had them reset by Build 8).
inline constexpr int INI_VERSION_HACK = 11;

// Steve's hacking circle (Dream Air, per-panel layout, 2026-10-23). D:\Projects\PragmataVR\HACKING_CIRCLE_LOCKED.txt
inline constexpr float HACK_LOCKED_SIZE = 0.9322f;
inline constexpr float HACK_LOCKED_X = -275.0f;
inline constexpr float HACK_LOCKED_Y = -75.0f;
inline constexpr float HACK_LOCKED_GAP = 12.0f;
inline constexpr std::array<const char*, FieldCount> FIELD_KEYS{"size", "x", "y", "gap"};

struct GroupTune {
    float size{1.0f}; // multiplies the panel's own scale
    float x{0.0f};    // layout units (1920 x 1080 basis)
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
    bool hud_fixed{true};    // HUD fixed in the room (Build 12, Steve's standing preference). Off = follows the head
    bool lock_invert{false}; // flips the fixed-HUD correction if it turns out mirrored on some runtime
    bool hide_ghost{true};   // hide Pragmata's offset "aberration" layers (zz_pragmatavr_hide_ghost.lua reads this)
    bool scale_about_center{true}; // Size grows each element around its own center (off = around the panel origin)

    // HUD layout. 0 = per-element (Builds 2-6, and Build 11 onward). 1 = "game screen" (Builds 7-10): the whole 3D HUD
    // as one virtual screen. Steve found the single screen no better and it broke the hacking circle, so Build 11
    // always uses 0 (the ini value is ignored; the game-screen code is kept for reference).
    int layout_mode{0};
    std::array<float, 4> screen{70.0f, 1.5f, 0.0f, -8.0f}; // width (degrees), distance (m), left/right (deg), up/down (deg)
    std::array<bool, 4> screen_invert{};

    // Mod menu placement: a panel fixed in the room where it was opened.
    // distance (m), text size (1 = body text MENU_TEXT_DEG degrees tall, at any distance), up/down (deg),
    // click volume (0..1 of full scale)
    std::array<float, 4> menu{2.5f, 1.0f, -5.0f, 0.5f};
    std::array<bool, 4> menu_invert{};
    std::array<GroupTune, GroupCount> groups{};

    HudTune() {
        // The hacking circle starts locked at Steve's tuned values. Every other element starts neutral.
        auto& h = groups[GroupHack];
        h.size = HACK_LOCKED_SIZE;
        h.x = HACK_LOCKED_X;
        h.y = HACK_LOCKED_Y;
        h.gap = HACK_LOCKED_GAP;
        hack_locked = true;
    }
};

inline GroupTune default_group(int g) {
    return HudTune{}.groups[g];
}

// Which element a GUI GameObject belongs to. ui2160Gui is no longer tuned: the fix pack lists it with the hacking
// target brackets (ui3203), which sit on enemies in the world and must not be moved or scaled with the HUD.
inline int group_for(std::string_view name) {
    if (name == "ui3400Gui") return GroupHack;
    if (name == "ui2130Gui") return GroupHealth;
    if (name == "ui2140Gui") return GroupStamina;
    if (name == "ui2150Gui") return GroupWeapons;
    if (name == "ui2201Gui" || name == "ui2100Gui" || name == "ui2002_00Gui") return GroupTutorial;
    return -1;
}

inline thread_local size_t g_current_gui_hash = 0;
inline thread_local std::string g_current_gui_name{};

// Menu click sounds. Build 9 and earlier used Beep(), which has no volume control and was too loud. The same two tones
// (1400 Hz 30 ms "ok", 220 Hz 160 ms "limit / blocked") are now made here as small WAV images and played with
// PlaySound, at the menu's Click volume (menu.click_volume, default 0.5 = half of full scale).
inline std::atomic<int> g_click_volume_pct{50};

inline const std::vector<uint8_t>& click_wav(bool ok, int volume_pct) {
    // One cached image per tone and volume step. Never freed, so an asynchronous PlaySound never reads freed memory.
    static std::mutex mtx{};
    static std::unordered_map<int, std::unique_ptr<std::vector<uint8_t>>> cache{};

    std::scoped_lock _{mtx};
    const int key = (ok ? 1000 : 0) + volume_pct;
    auto it = cache.find(key);
    if (it != cache.end()) {
        return *it->second;
    }

    constexpr uint32_t rate = 44100;
    const float freq = ok ? 1400.0f : 220.0f;
    const uint32_t samples = rate * (ok ? 30u : 160u) / 1000u;
    const uint32_t fade = rate * 3u / 1000u; // 3 ms fade in and out so the tone does not click
    const float amp = 32767.0f * (float)volume_pct / 100.0f;
    const uint32_t data_bytes = samples * 2u;

    auto wav = std::make_unique<std::vector<uint8_t>>();
    auto& w = *wav;
    w.reserve(44 + data_bytes);
    const auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) w.push_back((uint8_t)(v >> (8 * i))); };
    const auto put16 = [&](uint16_t v) { w.push_back((uint8_t)v); w.push_back((uint8_t)(v >> 8)); };
    const auto tag = [&](const char* s) { w.insert(w.end(), s, s + 4); };

    tag("RIFF"); put32(36 + data_bytes); tag("WAVE");
    tag("fmt "); put32(16); put16(1); put16(1); put32(rate); put32(rate * 2); put16(2); put16(16);
    tag("data"); put32(data_bytes);

    for (uint32_t i = 0; i < samples; ++i) {
        float env = 1.0f;
        if (i < fade) env = (float)i / (float)fade;
        else if (samples - i < fade) env = (float)(samples - i) / (float)fade;
        const float s = std::sin(6.2831853f * freq * (float)i / (float)rate) * amp * env;
        put16((uint16_t)(int16_t)std::clamp(s, -32767.0f, 32767.0f));
    }

    const auto& ref = *wav;
    cache.emplace(key, std::move(wav));
    return ref;
}

inline void play_click(bool ok) {
    const int vol = std::clamp(g_click_volume_pct.load(), 0, 100);
    if (vol == 0) {
        return;
    }

    const auto& wav = click_wav(ok, vol);
    PlaySoundA((LPCSTR)wav.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

// One row of the VR mod menu. The menu has two columns; rows are listed left column first, then right.
struct MenuRow {
    enum class Kind { Header, Value, Toggle, Action };
    Kind kind{Kind::Header};
    int group{-1};
    int field{-1};
    int action{0};
    int column{0};
    int band{0}; // 0 = mod menu settings (top), 1 = game HUD settings (below)
    std::string label{};
    std::string hint{}; // footer help line while this row is selected (headers: small tag after the title)
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
    MenuScale = 1,
    MenuPitch = 2,
    MenuClickVolume = 3,
};
// menu.width (Build 8, meters) is gone: the panel's size now follows its text. Old ini lines for it are ignored.
inline constexpr std::array<const char*, 4> MENU_KEYS{"distance", "scale", "pitch", "click_volume"};
inline constexpr std::array<float, 4> MENU_MIN{0.8f, 0.5f, -40.0f, 0.0f};
inline constexpr std::array<float, 4> MENU_MAX{8.0f, 2.0f, 40.0f, 1.0f};
inline constexpr std::array<float, 4> MENU_STEP{0.1f, 0.05f, 1.0f, 0.1f};
inline constexpr std::array<float, 4> MENU_STEP_BIG{0.5f, 0.25f, 5.0f, 0.25f};

// Menu text: body text is drawn MENU_FONT_PX pixels tall into the menu image, and shown MENU_TEXT_DEG degrees tall
// (times menu.scale) in the headset, whatever the distance. 0.7 degrees is comfortable reading size in VR.
inline constexpr float MENU_FONT_PX = 30.0f;
inline constexpr float MENU_TEXT_DEG = 0.7f;

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
    ActionToggleScaleCenter,
    ActionSnapshotSeries,
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
        poll_snapshot_series();

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
    std::atomic<bool> menu_editing{false};   // A on a value row: Left / Right change it until A or B
    std::atomic<bool> menu_on_invert{false}; // highlight is on the row's INV box (A flips it)
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
        case MenuScale: return fmt::format("{:.0f}%", t.menu[1] * 100.0f);
        case MenuClickVolume: return t.menu[3] <= 0.001f ? std::string{"OFF"} : fmt::format("{:.0f}%", t.menu[3] * 100.0f);
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

        int column = 0;
        int band = 0;
        const auto add = [&](MenuRow::Kind kind, int group, int field, int action, std::string label, std::string hint) {
            MenuRow r{};
            r.kind = kind;
            r.group = group;
            r.field = field;
            r.action = action;
            r.column = column;
            r.band = band;
            r.label = std::move(label);
            r.hint = std::move(hint);
            m_rows.push_back(std::move(r));
        };

        const std::string value_hint = "A = edit this value";

        // ==== MOD MENU SETTINGS (top band) ====
        band = 0;
        column = 0;
        add(MenuRow::Kind::Header, -1, -1, ActionNone, "PLACEMENT", "FIXED WHERE IT OPENS");
        add(MenuRow::Kind::Value, GROUP_MENU, MenuDistance, ActionNone, "Distance", "A = edit. Text keeps the same size at any distance");
        add(MenuRow::Kind::Value, GROUP_MENU, MenuScale, ActionNone, "Text size", value_hint);
        add(MenuRow::Kind::Value, GROUP_MENU, MenuPitch, ActionNone, "Up / down", value_hint);
        add(MenuRow::Kind::Action, GROUP_MENU, -1, ActionResetMenu, "Reset menu placement", "A = default distance, size and height, and move the menu in front of you");

        column = 1;
        add(MenuRow::Kind::Header, -1, -1, ActionNone, "SOUND AND TOOLS", "");
        add(MenuRow::Kind::Value, GROUP_MENU, MenuClickVolume, ActionNone, "Click volume", "A = edit. Volume of the menu click sounds (0 = off)");
        add(MenuRow::Kind::Action, -1, -1, ActionSnapshot, "Take snapshot", "A = save both eye images to reframework\\data\\snapshots");
        add(MenuRow::Kind::Action, -1, -1, ActionSnapshotSeries, "Snapshot series",
            "A = close the menu, wait 3 s, then 5 snapshots 1 s apart (move your head to record HUD motion)");
        add(MenuRow::Kind::Action, -1, -1, ActionClose, "Close menu", "A = close. B or both thumbsticks also close the menu");

        // ==== GAME HUD SETTINGS (lower band) ====
        // Build 11: every HUD element has its own block. The game-screen "one panel" rows are gone.
        band = 1;
        const auto add_element = [&](int g) {
            add(MenuRow::Kind::Header, g, -1, ActionNone, GROUP_TITLES[g], g == GroupHack ? "YOUR TUNED VALUES" : "");

            if (g == GroupHack) {
                add(MenuRow::Kind::Toggle, g, -1, ActionToggleHackLock, "Lock", "A = lock or unlock the hacking circle values");
            }

            add(MenuRow::Kind::Value, g, FieldSize, ActionNone, "Size", "A = edit. Grows or shrinks this element where it is");
            add(MenuRow::Kind::Value, g, FieldX, ActionNone, "Left / right", value_hint);
            add(MenuRow::Kind::Value, g, FieldY, ActionNone, "Up / down", value_hint);
            add(MenuRow::Kind::Value, g, FieldGap, ActionNone, "Eye gap", "A = edit the double-image gap between eyes");
            add(MenuRow::Kind::Action, g, -1, ActionResetGroup, "Reset to defaults",
                g == GroupHack ? "A = back to your tuned values (blocked while locked)" : "A = reset this element to the game's own size and place");
        };

        column = 0;
        add(MenuRow::Kind::Header, -1, -1, ActionNone, "VIEW", "");
        add(MenuRow::Kind::Toggle, -1, -1, ActionToggleHudFixed, "HUD position", "A = HUD fixed in the room (default), or HUD follows your head");
        add(MenuRow::Kind::Action, -1, -1, ActionRecenter, "Recenter HUD and menu", "A = move the HUD and this menu in front of where you are looking");
        add(MenuRow::Kind::Value, GROUP_SCREEN, ScreenDistance, ActionNone, "HUD distance",
            "A = edit. How far away the room-fixed HUD sits (meters). Sizes look the same at any distance");
        add(MenuRow::Kind::Toggle, -1, -1, ActionToggleGhost, "Ghost layer", "A = show or hide the game's grey double image behind HUD boxes");
        add(MenuRow::Kind::Toggle, -1, -1, ActionToggleScaleCenter, "Size grows from",
            "A = element center (keeps it in place) or panel origin (old behaviour). Hacking circle always uses the old way");
        add_element(GroupHealth);
        add_element(GroupStamina);

        column = 1;
        add_element(GroupHack);
        add_element(GroupWeapons);
        add_element(GroupTutorial);

        // Navigation: each column's selectable rows top to bottom through both bands, with each row's height in its
        // band (in row units; headers are about 1.3 rows tall, the first one in a column about 0.9) for Left / Right.
        for (auto& n : m_nav) {
            n.clear();
        }
        std::array<std::array<float, 2>, 2> ys{};
        std::array<std::array<bool, 2>, 2> first{{{true, true}, {true, true}}};
        m_row_y.assign(m_rows.size(), 0.0f);
        for (int i = 0; i < (int)m_rows.size(); ++i) {
            const auto& r = m_rows[i];
            const int c = std::clamp(r.column, 0, 1);
            const int bd = std::clamp(r.band, 0, 1);
            if (r.kind == MenuRow::Kind::Header) {
                ys[bd][c] += first[bd][c] ? 0.93f : 1.33f;
                first[bd][c] = false;
                continue;
            }
            m_row_y[i] = ys[bd][c] + 0.5f;
            ys[bd][c] += 1.0f;
            m_nav[c].push_back(i);
        }

        menu_selected = m_nav[0].empty() ? 0 : m_nav[0].front();
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
        s += "# Menu: both thumbsticks or F1 open/close. D-pad / arrows navigate, A / Enter edits the highlighted value\n";
        s += "#   (Left/Right change it, RB / Shift = big steps, A or B = done), flips a highlighted INV box, or runs an action.\n";
        s += "#   B / Backspace = stop editing or close. Pause = snapshot of both eyes.\n";
        s += "# <element>.size / .x / .y / .gap: hack, health, stamina, weapons, tutorial. x/y/gap in layout units (1920x1080 basis).\n";
        s += "# scale_about_center = 1: size grows each element around its own center (hack always uses the panel origin).\n";
        s += "# <group>.<field>.invert = 1 flips the direction Left/Right changes that value in the menu.\n";
        s += "# hack_locked = 1 stops the menu changing the hacking circle.\n";
        s += fmt::format("ini_version={}\n", INI_VERSION);
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
        s += fmt::format("scale_about_center={}\n", t.scale_about_center ? 1 : 0);
        s += "# screen.distance = HUD distance in meters (the room-fixed HUD is shown as a panel this far in front of you).\n";
        s += fmt::format("screen.distance={:.2f}\n", t.screen[ScreenDistance]);
        s += "# menu.distance in meters, menu.scale = text size (1.00 = 100%), menu.pitch in degrees,\n";
        s += "# menu.click_volume = click sound level (0 = off, 0.50 = half, 1.00 = full).\n";
        for (int i = 0; i < (int)MENU_KEYS.size(); ++i) {
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

        // Read the whole file first: hack.* lines only count when the file was written by Build 11 or later.
        std::vector<std::string> lines{};
        int file_version = 0;
        while (std::getline(f, line)) {
            const auto k = trim(line.substr(0, line.find('=')));
            if (k == "ini_version" && line.find('=') != std::string::npos) {
                try { file_version = std::stoi(trim(line.substr(line.find('=') + 1))); } catch (...) {}
            }
            lines.push_back(line);
        }
        f.close();
        const bool old_file = file_version < INI_VERSION;
        const bool old_hack = file_version < INI_VERSION_HACK;
        const bool old_fixed = file_version < INI_VERSION_FIXED_HUD;
        if (old_file) {
            spdlog::info("[PragmataVR] pragmatavr_hud.ini v{} is from an older build:{}{} (file will be rewritten as v{})", file_version,
                old_hack ? " hacking circle set to the locked values, other elements neutral;" : "",
                old_fixed ? " HUD fixed in the room" : "", INI_VERSION);
        }

        for (auto& l : lines) {
            line = l;
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
                if (old_fixed && (key == "hud_fixed" || key == "hack_locked")) continue;
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
                else if (key == "scale_about_center") t->scale_about_center = val != 0.0f;
                // layout_mode is ignored from Build 11 on (always per-element). hud_fixed and hack_locked from an
                // older file are ignored too, so the HUD comes back exactly as the hacking circle was tuned.
                continue;
            }

            const auto group_key = key.substr(0, dot);
            auto rest = key.substr(dot + 1);
            const bool is_invert = rest.size() > 7 && rest.substr(rest.size() - 7) == ".invert";
            if (is_invert) {
                rest = rest.substr(0, rest.size() - 7);
            }

            if (group_key == "menu") {
                for (int i = 0; i < (int)MENU_KEYS.size(); ++i) {
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
                if (old_hack && g == GroupHack) {
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

        {
            std::string desc{};
            for (int g = 0; g < GroupCount; ++g) {
                const auto& gt = t->groups[g];
                desc += fmt::format(" {}(size={:.3f} x={:.0f} y={:.0f} gap={:.0f})", GROUP_KEYS[g], gt.size, gt.x, gt.y, gt.gap);
            }
            spdlog::info("[PragmataVR] Loaded pragmatavr_hud.ini v{}: hack_locked={} hud_fixed={} scale_about_center={}{}",
                file_version, t->hack_locked, t->hud_fixed, t->scale_about_center, desc);
        }

        g_click_volume_pct = (int)std::lround(t->menu[MenuClickVolume] * 100.0f);

        if (old_file) {
            write_file(*t); // upgrade the file to this build's format (also sets m_mtime)
        }

        std::scoped_lock _{m_mtx};
        m_tune = t;
        if (!old_file) {
            m_mtime = mtime;
        }
        m_loaded_once = true;
    }

    void commit(const std::shared_ptr<HudTune>& t) {
        g_click_volume_pct = (int)std::lround(t->menu[MenuClickVolume] * 100.0f);
        write_file(*t);
        std::scoped_lock _{m_mtx};
        m_tune = t;
    }

    std::shared_ptr<HudTune> copy_current() {
        std::scoped_lock _{m_mtx};
        return std::make_shared<HudTune>(*m_tune);
    }

    // Up / Down: next selectable row in the same column, through both bands, wrapping around.
    void move_vertical(int dir) {
        const int sel = menu_selected.load();
        const int c = sel >= 0 && sel < (int)m_rows.size() ? std::clamp(m_rows[sel].column, 0, 1) : 0;
        const auto& nav = m_nav[c];
        if (nav.empty()) {
            return;
        }

        auto it = std::find(nav.begin(), nav.end(), sel);
        int pos = it == nav.end() ? 0 : (int)(it - nav.begin());
        pos = (pos + dir + (int)nav.size()) % (int)nav.size();
        menu_selected = nav[pos];

        // Stay on the INV box when moving between value rows.
        if (m_rows[nav[pos]].kind != MenuRow::Kind::Value) {
            menu_on_invert = false;
        }
    }

    // Left / Right while navigating: row -> its INV box -> the other column (nearest row in the same band).
    // Returns false when there is nothing further that way.
    bool move_horizontal(int dir) {
        const int sel = menu_selected.load();
        if (sel < 0 || sel >= (int)m_rows.size()) {
            return false;
        }

        const auto& row = m_rows[sel];
        const bool is_value = row.kind == MenuRow::Kind::Value;
        const bool on_inv = menu_on_invert.load() && is_value;

        if (dir > 0 && is_value && !on_inv) {
            menu_on_invert = true;
            return true;
        }
        if (dir < 0 && on_inv) {
            menu_on_invert = false;
            return true;
        }

        const int target_col = row.column + dir;
        if (target_col < 0 || target_col > 1) {
            return false;
        }

        int best = -1;
        float best_d = 1e9f;
        for (const int i : m_nav[target_col]) {
            if (m_rows[i].band != row.band) {
                continue;
            }
            const float d = std::fabs(m_row_y[i] - m_row_y[sel]);
            if (d < best_d - 0.01f) {
                best_d = d;
                best = i;
            }
        }
        if (best < 0) {
            return false;
        }

        menu_selected = best;
        // Coming in from the right, land on the INV box (the nearest thing on that side).
        menu_on_invert = dir < 0 && m_rows[best].kind == MenuRow::Kind::Value;
        return true;
    }

    // Snapshot series (Build 13): 5 snapshots one second apart, starting 3 seconds after the menu closes, so head
    // movement can be recorded with the HUD showing and the menu out of the way.
    int m_series_left{0};
    std::chrono::steady_clock::time_point m_series_next{};

    void poll_snapshot_series() {
        if (m_series_left <= 0) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < m_series_next || snapshot_frames_left.load() > 0) {
            return;
        }
        request_snapshot();
        --m_series_left;
        m_series_next = now + std::chrono::seconds(1);
        if (m_series_left == 0) {
            spdlog::info("[PragmataVR] Snapshot series finished");
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
            menu_editing = false;
            menu_on_invert = false;
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

        const bool editing = menu_editing.load();

        switch (vk) {
        case VK_BACK:
            if (editing) {
                menu_editing = false; // B: stop editing, back to navigation
                play_click(true);
                return;
            }
            menu_open = false;
            menu_on_invert = false;
            block_pad_briefly();
            spdlog::info("[PragmataVR] Menu closed");
            play_click(true);
            return;
        case VK_UP:
        case VK_DOWN:
            if (editing) {
                play_click(false); // finish editing with A or B first
                return;
            }
            move_vertical(vk == VK_UP ? -1 : 1);
            play_click(true);
            return;
        case VK_LEFT:
        case VK_RIGHT:
            if (!editing) {
                play_click(move_horizontal(vk == VK_RIGHT ? 1 : -1));
                return;
            }
            break; // editing: change the value below
        default:
            break;
        }

        const int sel = menu_selected.load();
        if (sel < 0 || sel >= (int)m_rows.size()) {
            return;
        }

        const auto& row = m_rows[sel];
        auto t = copy_current();

        if (vk == VK_RETURN && editing) {
            menu_editing = false; // A: done editing
            play_click(true);
            return;
        }

        if (vk == VK_RETURN && row.kind == MenuRow::Kind::Value && !menu_on_invert.load()) {
            if (row.group == GroupHack && t->hack_locked) {
                play_click(false);
                return;
            }
            menu_editing = true; // A on a value: edit it with Left / Right
            play_click(true);
            return;
        }

        if (vk == VK_RETURN) {
            switch (row.kind) {
            case MenuRow::Kind::Value: {
                // The highlight is on this row's INV box: flip which way Left / Right moves the value.
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
                } else if (row.action == ActionToggleScaleCenter) {
                    t->scale_about_center = !t->scale_about_center;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: size grows from {}", t->scale_about_center ? "element center" : "panel origin");
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
                    const float volume = t->menu[MenuClickVolume];
                    t->menu = HudTune{}.menu;
                    t->menu[MenuClickVolume] = volume; // placement only
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
                } else if (row.action == ActionSnapshotSeries) {
                    m_series_left = 5;
                    m_series_next = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                    menu_open = false;
                    menu_editing = false;
                    menu_on_invert = false;
                    block_pad_briefly();
                    spdlog::info("[PragmataVR] Snapshot series: 5 snapshots starting in 3 s");
                    play_click(true);
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

        if ((vk != VK_LEFT && vk != VK_RIGHT) || row.kind != MenuRow::Kind::Value || !editing) {
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
    static constexpr float MAX_SIZE = 8.0f;
    static constexpr float MOVE_STEP = 10.0f;
    static constexpr float MOVE_STEP_BIG = 50.0f;
    static constexpr float MAX_MOVE = 4000.0f;
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
    std::array<std::vector<int>, 2> m_nav{}; // selectable rows per column, top to bottom
    std::vector<float> m_row_y{};             // row center height within its band, in row units
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

// Mod menu panel in tracking (stage) space for the OpenXR quad layer: `dist` meters in front of the anchor, tilted
// menu.pitch degrees, facing the player. Its size follows the menu image: body text MENU_FONT_PX pixels tall is shown
// MENU_TEXT_DEG * menu.scale degrees tall, so the panel always fits its text and never stretches it.
struct MenuQuadPlacement {
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 position{0.0f};
    float width_m{1.0f};
    float height_m{1.0f};
};

inline MenuQuadPlacement menu_quad_placement(const HudTune& t, const HudTuning::Pose& anchor, float image_w_px, float image_h_px) {
    const float dist = t.menu[MenuDistance];
    const float text_rad = glm::radians(MENU_TEXT_DEG * t.menu[MenuScale]);
    const float meters_per_px = (2.0f * dist * std::tan(text_rad * 0.5f)) / MENU_FONT_PX;

    const glm::mat4 r = anchor.rot * glm::rotate(glm::mat4{1.0f}, glm::radians(t.menu[MenuPitch]), glm::vec3{1.0f, 0.0f, 0.0f});

    MenuQuadPlacement out{};
    out.orientation = glm::normalize(glm::quat_cast(glm::mat3{r}));
    out.position = anchor.pos + glm::vec3{r * glm::vec4{0.0f, 0.0f, -dist, 0.0f}};
    out.width_m = image_w_px * meters_per_px;
    out.height_m = image_h_px * meters_per_px;
    return out;
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
