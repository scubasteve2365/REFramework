#pragma once

// PragmataVR Build 68: Max detail (EXPERIMENTAL tab, off by default, ini max_detail).
//
// Steve: some textures stay low resolution in VR and never sharpen, and releasing the streamed textures does not fix it,
// so the streaming floor of Sharp textures (Build 59) is not what keeps them low. This switch goes after the game's own
// detail choices instead, all of them reversible (the game's values are put back when it is turned off):
//   1. via.render.Renderer.DisableLod = true: every mesh drawn at its highest level of detail (a lower level of detail
//      usually comes with smaller textures).
//   2. via.render.Renderer.IgnoreFixedMipNum = true: textures the game pins to a fixed number of mip levels get all of
//      them.
//   3. app.StreamingTextureControllerApp.calcStreamingPriority: every answer raised to the highest priority the game has
//      given any texture controller so far (so all of them are treated like its most important one). The values seen
//      are logged, to learn how the game ranks them.
// The settings are checked twice a second (the game may write its own values back), and the related values are logged
// once at start and with every change, so a log shows what the game uses.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "HookManager.hpp"
#include "sdk/RETypeDB.hpp"
#include "sdk/REContext.hpp"

#include "PragmataTexDiag.hpp"

namespace pragmata_max_detail {

inline std::atomic<bool> g_on{false};
inline std::atomic<uint32_t> g_prio_max{0};       // highest priority the game returned so far
inline std::atomic<uint32_t> g_prio_min{UINT32_MAX};
inline std::atomic<uint64_t> g_prio_calls{0};
inline std::atomic<uint64_t> g_prio_raised{0};
inline bool g_hooked{false};

// A static bool property of via.render.Renderer (get_X / set_X). Returns false when it is not there.
inline bool get_renderer_bool(const char* name, bool& out) {
    auto t = sdk::find_type_definition("via.render.Renderer");
    auto m = t != nullptr ? t->get_method((std::string{"get_"} + name).c_str()) : nullptr;
    if (m == nullptr) {
        return false;
    }
    out = m->call<bool>(sdk::get_thread_context());
    return true;
}

inline bool set_renderer_bool(const char* name, bool v) {
    auto t = sdk::find_type_definition("via.render.Renderer");
    auto m = t != nullptr ? t->get_method((std::string{"set_"} + name).c_str()) : nullptr;
    if (m == nullptr) {
        return false;
    }
    m->call<void*>(sdk::get_thread_context(), v);
    return true;
}

// Build 69: a static int property of via.render.Renderer.
inline bool get_renderer_int(const char* name, int32_t& out) {
    auto t = sdk::find_type_definition("via.render.Renderer");
    auto m = t != nullptr ? t->get_method((std::string{"get_"} + name).c_str()) : nullptr;
    if (m == nullptr) {
        return false;
    }
    out = m->call<int32_t>(sdk::get_thread_context());
    return true;
}

inline bool set_renderer_int(const char* name, int32_t v) {
    auto t = sdk::find_type_definition("via.render.Renderer");
    auto m = t != nullptr ? t->get_method((std::string{"set_"} + name).c_str()) : nullptr;
    if (m == nullptr) {
        return false;
    }
    m->call<void*>(sdk::get_thread_context(), v);
    return true;
}

// Build 69: StreamingResidentTextureTargetSize read 4096 on Steve's PC (likely MB of streamed texture data the game aims
// to keep). With Max detail on it is raised to RESIDENT_TARGET so more full size textures can stay loaded.
inline constexpr int32_t RESIDENT_TARGET = 16384;

// The detail related values the game uses (logged; static properties, no instance needed).
inline std::string state() {
    std::string out{};
    const auto stat = [&](const char* type, const char* method) {
        auto t = sdk::find_type_definition(type);
        auto m = t != nullptr ? t->get_method(method) : nullptr;
        if (m == nullptr) {
            return;
        }
        std::vector<void*> none{};
        const auto r = m->invoke(nullptr, std::span<void*>(none));
        if (!r.exception_thrown) {
            auto rt = m->get_return_type();
            out += fmt::format(" {}={}", std::string_view{method}.substr(4),
                               pragmata_tex_diag::value_of(r, rt != nullptr ? rt->get_full_name() : std::string{}));
        }
    };
    stat("via.render.Renderer", "get_DisableLod");
    stat("via.render.Renderer", "get_IgnoreFixedMipNum");
    stat("via.render.Renderer", "get_DisableStreamingTexture");
    stat("via.render.Renderer", "get_UseLegacyCalculateLod");
    stat("via.render.Renderer", "get_MipmapBiasReadjustRatio");
    stat("via.render.Renderer", "get_StreamingResidentTextureTargetSize");
    stat("via.render.MeshRenderer", "get_LodBias");
    stat("via.render.MeshRenderer", "get_LodRate2");
    stat("via.render.MeshRenderer", "get_StreamingLoadLodRate");
    stat("via.render.MeshRenderer", "get_ForceLodBiasInManualMode");
    stat("via.render.StreamingManager", "get_StreamingPriorityHigher");
    stat("via.render.StreamingManager", "get_BreadthFirstStreaming");
    const auto mx = g_prio_max.load();
    const auto mn = g_prio_min.load();
    out += fmt::format(" | texture controller priority: {} answers, game range {}..{}, raised {}", g_prio_calls.load(),
                       mn == UINT32_MAX ? 0u : mn, mx, g_prio_raised.load());
    return out;
}

inline void install_hook() {
    if (g_hooked) {
        return;
    }
    g_hooked = true;
    auto t = sdk::find_type_definition("app.StreamingTextureControllerApp");
    auto m = t != nullptr ? t->get_method("calcStreamingPriority") : nullptr;
    if (m == nullptr) {
        spdlog::info("[PragmataVR] Max detail: app.StreamingTextureControllerApp.calcStreamingPriority not found (priority part off)");
        return;
    }
    g_hookman.add(m,
        [](std::vector<uintptr_t>&, std::vector<sdk::RETypeDefinition*>&, uintptr_t) {
            return HookManager::PreHookResult::CALL_ORIGINAL;
        },
        [](uintptr_t& ret_val, sdk::RETypeDefinition*, uintptr_t) {
            const uint32_t v = (uint32_t)ret_val;
            g_prio_calls.fetch_add(1, std::memory_order_relaxed);
            uint32_t cur = g_prio_max.load(std::memory_order_relaxed);
            while (v > cur && !g_prio_max.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
            }
            cur = g_prio_min.load(std::memory_order_relaxed);
            while (v < cur && !g_prio_min.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
            }
            if (g_on.load(std::memory_order_relaxed)) {
                const uint32_t top = g_prio_max.load(std::memory_order_relaxed);
                if (v < top) {
                    ret_val = (ret_val & ~(uintptr_t)0xFFFFFFFFull) | (uintptr_t)top;
                    g_prio_raised.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    spdlog::info("[PragmataVR] Max detail: hooked app.StreamingTextureControllerApp.calcStreamingPriority");
}

// Main thread, twice a second.
inline void apply(bool on) {
    static bool init = false;
    static bool have_orig = false;
    static bool orig_disable_lod = false;
    static bool orig_ignore_fixed = false;
    static int32_t orig_resident = 0;
    static bool have_resident = false;
    static bool applied = false;
    static int64_t last_state_log = 0;
    if (!init) {
        init = true;
        install_hook();
        spdlog::info("[PragmataVR] Max detail: game values at start:{}", state());
    }
    g_on.store(on, std::memory_order_relaxed);
    if (!have_orig) {
        bool a = false, b = false;
        if (get_renderer_bool("DisableLod", a) && get_renderer_bool("IgnoreFixedMipNum", b)) {
            have_orig = true;
            orig_disable_lod = a;
            orig_ignore_fixed = b;
        }
    }
    if (!have_resident) {
        int32_t r = 0;
        if (get_renderer_int("StreamingResidentTextureTargetSize", r) && r > 0) {
            have_resident = true;
            orig_resident = r;
        }
    }
    if (on) {
        bool lod = false, fixed = false;
        bool changed = false;
        if (get_renderer_bool("DisableLod", lod) && !lod) {
            changed |= set_renderer_bool("DisableLod", true);
        }
        if (get_renderer_bool("IgnoreFixedMipNum", fixed) && !fixed) {
            changed |= set_renderer_bool("IgnoreFixedMipNum", true);
        }
        int32_t resident = 0;
        if (have_resident && get_renderer_int("StreamingResidentTextureTargetSize", resident) && resident < (std::max)(RESIDENT_TARGET, orig_resident)) {
            changed |= set_renderer_int("StreamingResidentTextureTargetSize", (std::max)(RESIDENT_TARGET, orig_resident));
        }
        if (changed || !applied) {
            applied = true;
            static int logs = 0;
            if (++logs <= 10) {
                spdlog::info("[PragmataVR] Max detail: on:{}", state());
            }
        }
        // Every 30 s while on: the values and the priority counts.
        const int64_t now = pragmata_tex_diag::now_ms();
        if (now - last_state_log > 30000) {
            last_state_log = now;
            spdlog::info("[PragmataVR] Max detail: state:{}", state());
        }
    } else if (applied) {
        applied = false;
        if (have_orig) {
            set_renderer_bool("DisableLod", orig_disable_lod);
            set_renderer_bool("IgnoreFixedMipNum", orig_ignore_fixed);
        }
        if (have_resident) {
            set_renderer_int("StreamingResidentTextureTargetSize", orig_resident);
        }
        spdlog::info("[PragmataVR] Max detail: off (game values back):{}", state());
    }
}

} // namespace pragmata_max_detail
