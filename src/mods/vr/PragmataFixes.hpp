#pragma once

// PragmataVR Build 36: the community fix pack's Lua HUD fixes (and our ghost layer script), ported to C++.
//
// Build 35 telemetry showed the five Lua scripts in reframework/autorun cost 5 to 9 ms per frame in native stereo:
// every script asked the engine for the element's GameObject and name on every GUI element draw, in both eyes,
// behind the Lua lock. Here the name is read once per draw and compared by hash; only the few matching elements
// make further engine calls. Behaviour matches the scripts:
//
//   zz_pragmata_transform_probe.lua        ui3203Gui, ui2160Gui (hacking target brackets, decoration): not drawn
//   zz_pragmatavr_hide_ghost.lua           offset "aberration" layer hidden on ui2100/2110/2120/2130/2140/2150/2201
//                                          (follows hide_ghost in pragmatavr_hud.ini)
//   zzzzz_pragmata_label_probe.lua         ui3010Gui (location label): Overlay off, title panel alpha 1 when the title
//                                          text is the only visible child, colour restored after the draw
//   zzzzzz_pragmata_door_hack_inspect.lua  ui3500Gui, ui3510Gui: VFXtex effect textures hidden for the draw
//   zzzzzzzz_pragmata_hud_orange_test.lua  ui2130Gui: hackingGauge_VFX effect texture hidden for the draw
//
// It is its own Mod, registered right after ScriptRunner, so it runs at the same point the Lua scripts did.
// The .lua files must be moved out of autorun (they would do the same work again).

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <memory>
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

    bool on_pre_gui_draw_element(REComponent* gui_element, void* primitive_context) override {
        auto go = utility::re_component::get_game_object(gui_element);
        if (go == nullptr) {
            return true;
        }

        const auto name = utility::re_game_object::get_name(go);
        const auto h = utility::hash(name);

        switch (h) {
        case "ui3203Gui"_fnv:
        case "ui2160Gui"_fnv:
            log_once(h, fmt::format("{}: hacking target brackets / decoration hidden", name));
            return false; // not drawn, any eye, any view mode

        case "ui3010Gui"_fnv:
            label(gui_element, name);
            return true;

        case "ui3500Gui"_fnv:
        case "ui3510Gui"_fnv:
            hide_effects(gui_element, name, {L"main/layout/VFXtex/effecttexture00", L"main/layout/VFXtex/effecttexture01"});
            return true;

        case "ui2130Gui"_fnv:
            ghost(gui_element, name, h);
            hide_effects(gui_element, name, {L"main/layout/offset/over/hackingGauge_VFX/effecttexture_"});
            return true;

        case "ui2100Gui"_fnv:
        case "ui2110Gui"_fnv:
        case "ui2120Gui"_fnv:
        case "ui2140Gui"_fnv:
        case "ui2150Gui"_fnv:
        case "ui2201Gui"_fnv:
            ghost(gui_element, name, h);
            return true;

        default:
            return true;
        }
    }

    void on_gui_draw_element(REComponent* gui_element, void* primitive_context) override {
        auto it = s_pending.find(gui_element);
        if (it == s_pending.end()) {
            return;
        }

        auto ctx = sdk::get_thread_context();
        for (auto& v : it->second.visible) {
            sdk::call_object_func<void*>(v.node, "set_Visible", ctx, v.node, v.visible);
        }
        if (it->second.color_panel != nullptr) {
            sdk::call_object_func<void*>(it->second.color_panel, "set_ColorScale", ctx, it->second.color_panel, &it->second.color);
        }
        s_pending.erase(it);
    }

private:
    struct SavedVisible {
        REManagedObject* node{nullptr};
        bool visible{true};
    };

    struct Pending {
        std::vector<SavedVisible> visible{};
        REManagedObject* color_panel{nullptr};
        Vector4f color{};
    };

    // Per element, restored in on_gui_draw_element. Pre and post of one element run on the same thread.
    static inline thread_local std::unordered_map<REComponent*, Pending> s_pending{};
    static inline thread_local std::unordered_map<size_t, bool> s_logged{};
    // Ghost layer per GUI name: whether we hid it (so turning hide_ghost off shows it again).
    static inline thread_local std::unordered_map<size_t, bool> s_ghost_hidden{};

    static void log_once(size_t key, const std::string& msg) {
        if (s_logged.emplace(key, true).second) {
            spdlog::info("[PragmataVR] Fixes: {}", msg);
        }
    }

    static std::string type_name(REManagedObject* o) {
        auto td = utility::re_managed_object::get_type_definition(o);
        return td != nullptr ? std::string{td->get_full_name()} : std::string{};
    }

    // view:call('getObject', path) / element:call('getObject', path), as the Lua scripts did.
    static REManagedObject* get_object(REManagedObject* owner, std::wstring_view path, sdk::VMContext* ctx) {
        auto str = sdk::VM::create_managed_string(path);
        if (str == nullptr) {
            return nullptr;
        }
        return sdk::call_object_func<REManagedObject*>(owner, "getObject", ctx, owner, str);
    }

    // zz_pragmatavr_hide_ghost.lua
    static void ghost(REComponent* element, const std::string& name, size_t h) {
        static const std::unordered_map<size_t, std::vector<std::wstring>> paths{
            {"ui2100Gui"_fnv, {L"main/layout/offset/under", L"main/layout/under", L"main/under"}},
            {"ui2110Gui"_fnv, {L"main/layout/offset/under"}},
            {"ui2120Gui"_fnv, {L"main/layout/offset/under"}},
            {"ui2130Gui"_fnv, {L"main/layout/offset/under"}},
            {"ui2140Gui"_fnv, {L"main/layout/offset/under"}},
            {"ui2150Gui"_fnv, {L"main/layout/offset/under"}},
            {"ui2201Gui"_fnv, {L"main/layout/offset/under", L"main/layout/under", L"main/under"}},
        };

        const auto pit = paths.find(h);
        if (pit == paths.end()) {
            return;
        }

        // Looked up on every draw, like the script (the element and its layers can be recreated).
        auto ctx = sdk::get_thread_context();
        REManagedObject* layer = nullptr;
        std::wstring_view used{};
        for (const auto& p : pit->second) {
            layer = get_object((REManagedObject*)element, p, ctx);
            if (layer != nullptr) {
                used = p;
                break;
            }
        }
        log_once(h ^ 0x9e3779b97f4a7c15ull, layer != nullptr ? fmt::format("{}: ghost layer found at {}", name, utility::narrow(used))
                                                              : fmt::format("{}: no ghost layer at the known paths", name));
        if (layer == nullptr) {
            return;
        }

        auto& hidden_by_us = s_ghost_hidden[h];
        const bool hide = pragmatavr::HudTuning::get().tune()->hide_ghost;
        if (hide) {
            if (sdk::call_object_func<bool>(layer, "get_Visible", ctx, layer)) {
                sdk::call_object_func<void*>(layer, "set_Visible", ctx, layer, false);
            }
            hidden_by_us = true;
        } else if (hidden_by_us) {
            sdk::call_object_func<void*>(layer, "set_Visible", ctx, layer, true);
            hidden_by_us = false;
        }
    }

    // zzzzzz_pragmata_door_hack_inspect.lua and zzzzzzzz_pragmata_hud_orange_test.lua
    static void hide_effects(REComponent* element, const std::string& name, std::initializer_list<std::wstring_view> paths) {
        auto ctx = sdk::get_thread_context();
        auto view = sdk::call_object_func<REManagedObject*>((REManagedObject*)element, "get_View", ctx, (REManagedObject*)element);
        if (view == nullptr) {
            return;
        }

        auto& pending = s_pending[element];
        for (const auto p : paths) {
            auto node = get_object(view, p, ctx);
            const auto key = utility::hash(name) ^ utility::hash(utility::narrow(p));
            if (node == nullptr || type_name(node) != "via.gui.EffectTexture") {
                log_once(key, fmt::format("{}: expected EffectTexture missing at {}, skipped", name, utility::narrow(p)));
                continue;
            }
            if (sdk::call_object_func<bool>(node, "get_Visible", ctx, node)) {
                pending.visible.push_back({node, true});
                sdk::call_object_func<void*>(node, "set_Visible", ctx, node, false);
            }
            log_once(key, fmt::format("{}: effect texture {} hidden while drawing", name, utility::narrow(p)));
        }
    }

    // zzzzz_pragmata_label_probe.lua (V13)
    static void label(REComponent* element, const std::string& name) {
        auto ctx = sdk::get_thread_context();
        auto view = sdk::call_object_func<REManagedObject*>((REManagedObject*)element, "get_View", ctx, (REManagedObject*)element);
        if (view == nullptr) {
            return;
        }

        sdk::call_object_func<void*>(view, "set_Overlay", ctx, view, false);

        auto panel = get_object(view, L"main/layout/type/item", ctx);
        if (panel == nullptr || type_name(panel) != "via.gui.Panel") {
            log_once("ui3010 missing"_fnv, "ui3010Gui: title panel missing, no brightness edit");
            return;
        }

        // Only when the title text 'name' is the sole visible direct child (protects other content).
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

        const auto original = sdk::call_object_func_easy<Vector4f>(panel, "get_ColorScale");
        if (!std::isfinite(original.x) || !std::isfinite(original.y) || !std::isfinite(original.z) || !std::isfinite(original.w)) {
            return;
        }

        auto& pending = s_pending[element];
        if (pending.color_panel != nullptr) {
            return; // already applied for this draw
        }
        pending.color_panel = panel;
        pending.color = original;

        Vector4f brighter{original.x, original.y, original.z, 1.0f};
        sdk::call_object_func<void*>(panel, "set_ColorScale", ctx, panel, &brighter);
        log_once("ui3010 applied"_fnv, fmt::format("{}: title alpha set to 1 while drawing (colour restored after)", name));
    }
};
