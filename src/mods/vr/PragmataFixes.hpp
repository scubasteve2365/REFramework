#pragma once

// PragmataVR Build 36: the community fix pack's Lua HUD fixes (and our ghost layer script), ported to C++.
// Build 46: rewritten after the crash hunt of 2026-10-30.
//
// Builds 36 to 45 made these changes from the GUI draw callbacks, on the render threads, on every frame: hide a HUD
// piece before the element drew and show it again right after, look pieces up by name (a new managed string each
// time) and change the location label's colour. Steve's flat tests with VR off pinned the random worker-thread
// crashes (animation, collision, navigation, hacking jobs, all through corrupted pointers) on this code: PureDark's
// original DLL and Build 35 (no port) ran the hacking loop clean; Builds 36 to 40 crashed within a couple of hacks,
// even with the HUD code off. The hacking gauge and door hack effects it toggled are animated by the game's own
// workers during a hack, which is where the crashes clustered.
//
// Now:
//   - All changes happen on the main thread, in the application entry just before PrerenderGUI (after the game's
//     own update and behaviour code for the frame, before the GUI is prepared for drawing). Nothing is written from
//     the render threads.
//   - Each change is made once and left in place (hide only if visible, never hide and restore around a draw).
//     Turning a fix off shows again what this fix hid.
//   - Name lookups use managed strings created once and kept alive (add_ref), not new strings per lookup.
//   - The draw callback only notes which HUD elements are being drawn (no engine calls besides reading the name it
//     already read) and skips the target brackets, which changes nothing in the game.
//   - Every fix has its own switch in pragmatavr_hud.ini (fix_brackets, hide_ghost, fix_label, fix_door_vfx,
//     fix_gauge_vfx, hide_hack_backdrop).
//
// The fixes (behaviour of the original scripts):
//   zz_pragmata_transform_probe.lua        ui3203Gui, ui2160Gui (hacking target brackets, decoration): not drawn
//   zz_pragmatavr_hide_ghost.lua           offset "aberration" layer hidden on ui2100/2110/2120/2130/2140/2150/2201
//   zzzzz_pragmata_label_probe.lua         ui3010Gui (location label): Overlay off, title panel alpha 1 when the title
//                                          text is the only visible child
//   zzzzzz_pragmata_door_hack_inspect.lua  ui3500Gui, ui3510Gui: VFXtex effect textures hidden
//   zzzzzzzz_pragmata_hud_orange_test.lua  ui2130Gui: hackingGauge_VFX effect texture hidden
//   Build 42                               ui3500Gui, ui3510Gui: dimming overlays PNL_1, PNL_, rect_1, rect_ hidden

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

#include "Mod.hpp"
#include "sdk/REComponent.hpp"
#include "sdk/REContext.hpp"
#include "sdk/REGameObject.hpp"
#include "sdk/REManagedObject.hpp"
#include "sdk/REString.hpp"
#include "sdk/RETypeDB.hpp"
#include "sdk/SceneManager.hpp"
#include "sdk/Math.hpp"
#include "utility/String.hpp"

#include "PragmataHud.hpp"

class PragmataFixes : public Mod {
public:
    static std::shared_ptr<PragmataFixes>& get() {
        static auto inst = std::make_shared<PragmataFixes>();
        return inst;
    }

    std::string_view get_name() const override { return "PragmataFixes"; }

    // Render threads: read the name (as before), note the element as drawn, skip the brackets. No writes.
    bool on_pre_gui_draw_element(REComponent* gui_element, void* primitive_context) override {
        auto go = utility::re_component::get_game_object(gui_element);
        if (go == nullptr) {
            return true;
        }

        const auto name = utility::re_game_object::get_name(go);
        const auto h = utility::hash(name);

        if (h == "ui3203Gui"_fnv || h == "ui2160Gui"_fnv) {
            if (pragmatavr::HudTuning::get().tune()->fix_brackets) {
                log_once(h, fmt::format("{}: hacking target brackets / decoration not drawn", name));
                return false;
            }
            return true;
        }

        for (size_t i = 0; i < TARGETS.size(); ++i) {
            if (TARGETS[i].hash == h) {
                s_seen_ms[i].store(now_ms(), std::memory_order_relaxed);
                break;
            }
        }
        return true;
    }

    // Main thread, once per frame, before the GUI is prepared for drawing.
    void on_pre_application_entry(void* entry, const char* name, size_t hash) override {
        if (hash != "PrerenderGUI"_fnv) {
            return;
        }
        if (!s_announced) {
            s_announced = true;
            spdlog::info("[PragmataVR] Fixes: HUD fixes run on the main thread before PrerenderGUI (Build 46)");
        }
        apply();
    }

private:
    enum Kind : uint32_t {
        KindGhost = 1u << 0,
        KindGauge = 1u << 1,
        KindDoor = 1u << 2,
        KindLabel = 1u << 3,
    };

    struct Target {
        const char* name;
        size_t hash;
        uint32_t kinds;
    };

    static inline const std::array<Target, 10> TARGETS{{
        {"ui2100Gui", "ui2100Gui"_fnv, KindGhost},
        {"ui2110Gui", "ui2110Gui"_fnv, KindGhost},
        {"ui2120Gui", "ui2120Gui"_fnv, KindGhost},
        {"ui2130Gui", "ui2130Gui"_fnv, KindGhost | KindGauge},
        {"ui2140Gui", "ui2140Gui"_fnv, KindGhost},
        {"ui2150Gui", "ui2150Gui"_fnv, KindGhost},
        {"ui2201Gui", "ui2201Gui"_fnv, KindGhost},
        {"ui3500Gui", "ui3500Gui"_fnv, KindDoor},
        {"ui3510Gui", "ui3510Gui"_fnv, KindDoor},
        {"ui3010Gui", "ui3010Gui"_fnv, KindLabel},
    }};

    // Only elements drawn within this window are looked up (absent HUD screens cost nothing).
    static constexpr int64_t SEEN_WINDOW_MS = 2000;

    static inline std::array<std::atomic<int64_t>, 10> s_seen_ms{};
    static inline bool s_announced{false};

    // Main thread only.
    static inline std::unordered_map<size_t, bool> s_hidden_by_us{};
    static inline std::unordered_map<std::wstring, SystemString*> s_strings{};

    static inline std::mutex s_logged_mtx{};
    static inline std::unordered_map<size_t, bool> s_logged{};

    static int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    static void log_once(size_t key, const std::string& msg) {
        std::scoped_lock _{s_logged_mtx};
        if (s_logged.emplace(key, true).second) {
            spdlog::info("[PragmataVR] Fixes: {}", msg);
        }
    }

    static std::string type_name(REManagedObject* o) {
        auto td = utility::re_managed_object::get_type_definition(o);
        return td != nullptr ? std::string{td->get_full_name()} : std::string{};
    }

    // A managed string created once and kept alive, so lookups do not allocate every frame.
    static SystemString* managed(std::wstring_view s) {
        auto it = s_strings.find(std::wstring{s});
        if (it != s_strings.end()) {
            return it->second;
        }
        auto str = sdk::VM::create_managed_string(s);
        if (str != nullptr) {
            utility::re_managed_object::add_ref((REManagedObject*)str);
        }
        s_strings.emplace(std::wstring{s}, str);
        return str;
    }

    // view:call('getObject', path) / element:call('getObject', path), as the Lua scripts did.
    static REManagedObject* get_object(REManagedObject* owner, std::wstring_view path, sdk::VMContext* ctx) {
        auto str = managed(path);
        if (str == nullptr) {
            return nullptr;
        }
        return sdk::call_object_func<REManagedObject*>(owner, "getObject", ctx, owner, str);
    }

    static REGameObject* find_game_object(REManagedObject* scene, const char* name, sdk::VMContext* ctx) {
        static auto scene_t = sdk::find_type_definition("via.Scene");
        static auto find_method = scene_t != nullptr ? scene_t->get_method("findGameObject(System.String)") : nullptr;
        if (find_method == nullptr) {
            log_once("no findGameObject"_fnv, "via.Scene.findGameObject not found, HUD fixes inactive");
            return nullptr;
        }
        auto str = managed(utility::widen(name));
        if (str == nullptr) {
            return nullptr;
        }
        return find_method->call<REGameObject*>(ctx, scene, str);
    }

    static void apply() {
        const auto tune = pragmatavr::HudTuning::get().tune();
        const auto now = now_ms();

        auto scene = sdk::get_current_scene();
        if (scene == nullptr) {
            return;
        }
        auto ctx = sdk::get_thread_context();

        for (size_t i = 0; i < TARGETS.size(); ++i) {
            const auto& tg = TARGETS[i];
            const auto seen = s_seen_ms[i].load(std::memory_order_relaxed);
            if (seen == 0 || now - seen > SEEN_WINDOW_MS) {
                continue;
            }

            auto go = find_game_object(scene, tg.name, ctx);
            if (go == nullptr || go->transform == nullptr) {
                log_once(tg.hash ^ 0x51ed270b27f3ull, fmt::format("{}: drawn but not found in the scene, skipped", tg.name));
                continue;
            }
            auto gui = utility::re_component::find<::REComponent>((::REComponent*)go->transform, "via.gui.GUI");
            if (gui == nullptr) {
                continue;
            }
            auto view = sdk::call_object_func<REManagedObject*>((REManagedObject*)gui, "get_View", ctx, (REManagedObject*)gui);

            if (tg.kinds & KindGhost) {
                ghost((REManagedObject*)gui, tg, tune->hide_ghost, ctx);
            }
            if (view == nullptr) {
                continue;
            }
            if (tg.kinds & KindGauge) {
                set_hidden(view, tg, L"main/layout/offset/over/hackingGauge_VFX/effecttexture_", "via.gui.EffectTexture", tune->fix_gauge_vfx, ctx);
            }
            if (tg.kinds & KindDoor) {
                set_hidden(view, tg, L"main/layout/VFXtex/effecttexture00", "via.gui.EffectTexture", tune->fix_door_vfx, ctx);
                set_hidden(view, tg, L"main/layout/VFXtex/effecttexture01", "via.gui.EffectTexture", tune->fix_door_vfx, ctx);
                static const std::array<const wchar_t*, 4> backdrop{L"main/layout/PNL_1", L"main/layout/PNL_", L"main/layout/rect_1", L"main/layout/rect_"};
                for (const auto* p : backdrop) {
                    set_hidden(view, tg, p, nullptr, tune->hide_hack_backdrop, ctx);
                }
            }
            if ((tg.kinds & KindLabel) && tune->fix_label) {
                label(view, tg, ctx);
            }
        }
    }

    // Hide one piece while `hide` is on (only if it is visible); show it again once `hide` is off, if this fix hid it.
    static void set_hidden(REManagedObject* owner, const Target& tg, std::wstring_view path, const char* want_type, bool hide,
                           sdk::VMContext* ctx) {
        const auto key = tg.hash ^ utility::hash(utility::narrow(path));
        auto& ours = s_hidden_by_us[key];
        if (!hide && !ours) {
            return;
        }

        auto node = get_object(owner, path, ctx);
        if (node == nullptr || (want_type != nullptr && type_name(node) != want_type)) {
            log_once(key, fmt::format("{}: {} not found, skipped", tg.name, utility::narrow(path)));
            return;
        }

        if (hide) {
            if (sdk::call_object_func<bool>(node, "get_Visible", ctx, node)) {
                sdk::call_object_func<void*>(node, "set_Visible", ctx, node, false);
                ours = true;
                log_once(key ^ 0x9e3779b97f4a7c15ull, fmt::format("{}: {} hidden", tg.name, utility::narrow(path)));
            }
        } else {
            sdk::call_object_func<void*>(node, "set_Visible", ctx, node, true);
            ours = false;
        }
    }

    // zz_pragmatavr_hide_ghost.lua: the offset "aberration" layer, at the first path that exists.
    static void ghost(REManagedObject* gui, const Target& tg, bool hide, sdk::VMContext* ctx) {
        static const std::array<const wchar_t*, 3> paths{L"main/layout/offset/under", L"main/layout/under", L"main/under"};
        const auto key = tg.hash ^ 0x6a09e667f3bcc909ull;
        auto& ours = s_hidden_by_us[key];
        if (!hide && !ours) {
            return;
        }

        REManagedObject* layer = nullptr;
        const wchar_t* used = nullptr;
        for (const auto* p : paths) {
            layer = get_object(gui, p, ctx);
            if (layer != nullptr) {
                used = p;
                break;
            }
        }
        log_once(key, layer != nullptr ? fmt::format("{}: ghost layer at {}", tg.name, utility::narrow(used))
                                       : fmt::format("{}: no ghost layer at the known paths", tg.name));
        if (layer == nullptr) {
            return;
        }

        if (hide) {
            if (sdk::call_object_func<bool>(layer, "get_Visible", ctx, layer)) {
                sdk::call_object_func<void*>(layer, "set_Visible", ctx, layer, false);
            }
            ours = true;
        } else {
            sdk::call_object_func<void*>(layer, "set_Visible", ctx, layer, true);
            ours = false;
        }
    }

    // zzzzz_pragmata_label_probe.lua (V13): Overlay off; title panel alpha 1 when the title text is its only visible child.
    static void label(REManagedObject* view, const Target& tg, sdk::VMContext* ctx) {
        if (sdk::call_object_func<bool>(view, "get_Overlay", ctx, view)) {
            sdk::call_object_func<void*>(view, "set_Overlay", ctx, view, false);
        }

        auto panel = get_object(view, L"main/layout/type/item", ctx);
        if (panel == nullptr || type_name(panel) != "via.gui.Panel") {
            log_once("ui3010 missing"_fnv, "ui3010Gui: title panel missing, no brightness edit");
            return;
        }

        bool found = false;
        auto child = sdk::call_object_func<REManagedObject*>(panel, "get_Child", ctx, panel);
        for (int count = 0; child != nullptr; ++count) {
            if (count >= 32) {
                return;
            }
            if (sdk::call_object_func<bool>(child, "get_Visible", ctx, child)) {
                auto cname = sdk::call_object_func<SystemString*>(child, "get_Name", ctx, child);
                const auto cn = cname != nullptr ? utility::re_string::get_string(cname) : std::string{};
                if (cn != "name" || type_name(child) != "via.gui.Text") {
                    return;
                }
                found = true;
            }
            child = sdk::call_object_func<REManagedObject*>(child, "get_Next", ctx, child);
        }
        if (!found) {
            return;
        }

        const auto c = sdk::call_object_func_easy<Vector4f>(panel, "get_ColorScale");
        if (!std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z) || !std::isfinite(c.w) || c.w >= 1.0f) {
            return;
        }
        Vector4f brighter{c.x, c.y, c.z, 1.0f};
        sdk::call_object_func<void*>(panel, "set_ColorScale", ctx, panel, &brighter);
        log_once("ui3010 applied"_fnv, fmt::format("{}: title alpha set to 1", tg.name));
    }
};
