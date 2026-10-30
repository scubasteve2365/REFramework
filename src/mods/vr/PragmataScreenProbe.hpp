#pragma once

// PragmataVR Build 48: diagnostic only. Finds the game function that turns an enemy's world position into a screen
// position for the enemy health bars (ui3200Gui / ui3210Gui) and damage numbers (ui3201Gui).
//
// On Quest 3 those markers sit off the enemy: the game projects with its own symmetric camera while each eye is
// rendered with the headset's lopsided projection. The fix (a later build) is to hook that one function and answer
// with the headset's projection. This probe:
//   - lists every game method whose name looks like a world-to-screen conversion (once, at the first PrerenderGUI);
//   - hooks up to MAX_HOOKS of them with a counting pre-hook that calls the original (no arguments or results are
//     changed, no game object is touched);
//   - samples the callers (return addresses, resolved to method names by REFramework's MethodDatabase);
//   - every 10 seconds logs, for each method that was called, calls per second, the top callers, and whether enemy
//     bars / damage numbers were being drawn in that window.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

#include "HookManager.hpp"
#include "mods/MethodDatabase.hpp"
#include "sdk/RETypeDB.hpp"
#include "sdk/SceneManager.hpp"
#include "sdk/REManagedObject.hpp"

namespace pragmata_screen_probe {

inline constexpr size_t MAX_HOOKS = 48;
inline constexpr uint64_t CALLER_SAMPLE_EVERY = 16;

struct Probe {
    std::string name{};
    std::atomic<uint64_t> calls{0};
    std::mutex mtx{};
    std::unordered_map<uintptr_t, uint64_t> callers{};
};

inline std::array<Probe, MAX_HOOKS> g_probes{};
inline size_t g_probe_count{0};
inline bool g_installed{false};
inline std::chrono::steady_clock::time_point g_last_report{};

// Set from the draw callback (render threads) when these elements draw.
inline std::atomic<int64_t> g_enemy_bar_seen_ms{0};
inline std::atomic<int64_t> g_damage_seen_ms{0};

// Build 49: detailed samples of app.GuiUtil.transformWorldToScreen (the function Build 48 found behind the enemy bars
// and damage numbers: app.GuiBehaviorV2.applyPostureData calls it), to work out its units and the correction.
inline std::atomic<int64_t> g_detail_last_ms{0};
inline thread_local bool tl_detail_sample = false;
inline thread_local std::array<uintptr_t, 6> tl_detail_args{};

inline std::string floats_at(uintptr_t p, int n) {
    if (p < 0x10000 || p > 0x7FFFFFFFFFFFull || IsBadReadPtr((const void*)p, n * sizeof(float))) {
        return "-";
    }
    std::string out{};
    for (int i = 0; i < n; ++i) {
        out += fmt::format("{}{:.4f}", i ? "," : "", ((const float*)p)[i]);
    }
    return out;
}

inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

inline bool name_matches(const std::string& n) {
    static const std::array<const char*, 10> keys{"toscreen", "screenpos", "screenpoint", "toviewport", "worldtocanvas",
                                                   "tocanvas", "canvaspos", "worldposto", "screencoord", "projectpoint"};
    for (const auto* k : keys) {
        if (n.find(k) != std::string::npos) {
            return true;
        }
    }
    return false;
}

inline void install() {
    if (g_installed) {
        return;
    }
    g_installed = true;
    g_last_report = std::chrono::steady_clock::now();

    auto tdb = sdk::RETypeDB::get();
    if (tdb == nullptr) {
        spdlog::info("[PragmataVR] Screen probe: no type database");
        return;
    }

    struct Candidate {
        sdk::REMethodDefinition* m;
        std::string full;
        int priority;
    };
    std::vector<Candidate> found{};
    const auto n = tdb->get_num_methods();
    for (uint32_t i = 0; i < n; ++i) {
        auto m = tdb->get_method(i);
        if (m == nullptr) {
            continue;
        }
        const char* mn = m->get_name();
        if (mn == nullptr || !name_matches(lower(mn))) {
            continue;
        }
        auto dt = m->get_declaring_type();
        const std::string tn = dt != nullptr ? dt->get_full_name() : std::string{"?"};
        if (tn.rfind("System.", 0) == 0) {
            continue;
        }
        std::string params{};
        for (const auto* p : m->get_param_names()) {
            params += (params.empty() ? "" : ", ") + std::string{p != nullptr ? p : "?"};
        }
        const std::string full = tn + "." + mn + "(" + params + ")";
        // Game code first (app.*), then the engine's camera / view / GUI helpers.
        int prio = 3;
        if (tn.rfind("app.", 0) == 0) prio = 0;
        else if (tn.find("Camera") != std::string::npos || tn.find("SceneView") != std::string::npos) prio = 1;
        else if (tn.rfind("via.gui", 0) == 0) prio = 2;
        found.push_back({m, full, prio});
    }
    std::stable_sort(found.begin(), found.end(), [](const Candidate& a, const Candidate& b) { return a.priority < b.priority; });

    spdlog::info("[PragmataVR] Screen probe: {} candidate methods", found.size());
    for (const auto& c : found) {
        spdlog::info("[PragmataVR] Screen probe candidate: {}{}", c.full, c.m->get_function() == nullptr ? " (no code)" : "");
    }

    for (const auto& c : found) {
        if (g_probe_count >= MAX_HOOKS) {
            break;
        }
        if (c.m->get_function() == nullptr) {
            continue;
        }
        const size_t idx = g_probe_count;
        g_probes[idx].name = c.full;
        const bool detail = c.full.rfind("app.GuiUtil.transformWorldToScreen", 0) == 0;
        if (detail) {
            std::string pt{};
            for (auto t : c.m->get_param_types()) {
                pt += (pt.empty() ? "" : ", ") + (t != nullptr ? t->get_full_name() : std::string{"?"});
            }
            auto rt = c.m->get_return_type();
            spdlog::info("[PragmataVR] Screen probe detail: {} static={} params=({}) returns {}", c.full, c.m->is_static(), pt,
                         rt != nullptr ? rt->get_full_name() : std::string{"?"});
        }
        try {
            g_hookman.add(c.m,
                [idx, detail](std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>& arg_tys, uintptr_t ret_addr) {
                    if (detail) {
                        const auto t = now_ms();
                        auto last = g_detail_last_ms.load();
                        tl_detail_sample = t - last >= 2000 && g_detail_last_ms.compare_exchange_strong(last, t);
                        if (tl_detail_sample) {
                            for (size_t i = 0; i < tl_detail_args.size(); ++i) {
                                tl_detail_args[i] = i < args.size() ? args[i] : 0;
                            }
                            std::string a{};
                            for (size_t i = 0; i < args.size() && i < 6; ++i) {
                                a += fmt::format(" a{}={:x}[{}]", i, args[i], floats_at(args[i], 4));
                            }
                            std::string at{};
                            for (auto ty : arg_tys) {
                                at += (at.empty() ? "" : ",") + (ty != nullptr ? ty->get_full_name() : std::string{"?"});
                            }
                            float fov = -1.0f, aspect = -1.0f;
                            if (auto cam = sdk::get_primary_camera(); cam != nullptr) {
                                fov = sdk::call_object_func_easy<float>((REManagedObject*)cam, "get_FOV");
                                aspect = sdk::call_object_func_easy<float>((REManagedObject*)cam, "get_AspectRatio");
                            }
                            spdlog::info("[PragmataVR] Screen probe sample in: n={} types=({}){} caller {} | camera fov {:.3f} aspect {:.4f}",
                                         args.size(), at, a, MethodDatabase::get()->find_method(ret_addr), fov, aspect);
                        }
                    }
                    auto& p = g_probes[idx];
                    const auto k = p.calls.fetch_add(1, std::memory_order_relaxed);
                    if (k % CALLER_SAMPLE_EVERY == 0) {
                        std::scoped_lock _{p.mtx};
                        if (p.callers.size() < 64 || p.callers.count(ret_addr) != 0) {
                            p.callers[ret_addr]++;
                        }
                    }
                    return HookManager::PreHookResult::CALL_ORIGINAL;
                },
                [detail](uintptr_t& ret_val, sdk::RETypeDefinition* ret_ty, uintptr_t ret_addr) {
                    if (detail && tl_detail_sample) {
                        tl_detail_sample = false;
                        std::string a{};
                        for (size_t i = 0; i < tl_detail_args.size(); ++i) {
                            a += fmt::format(" a{}[{}]", i, floats_at(tl_detail_args[i], 4));
                        }
                        spdlog::info("[PragmataVR] Screen probe sample out: ret={:x}[{}] ret type {} | args after:{}", ret_val,
                                     floats_at(ret_val, 4), ret_ty != nullptr ? ret_ty->get_full_name() : std::string{"?"}, a);
                    }
                });
            ++g_probe_count;
            spdlog::info("[PragmataVR] Screen probe: hooked {}", c.full);
        } catch (...) {
            spdlog::info("[PragmataVR] Screen probe: could not hook {}", c.full);
        }
    }
}

inline void report() {
    if (!g_installed) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto secs = std::chrono::duration<double>(now - g_last_report).count();
    if (secs < 10.0) {
        return;
    }
    g_last_report = now;

    const auto t = now_ms();
    const bool bars = t - g_enemy_bar_seen_ms.load() < 10000;
    const bool damage = t - g_damage_seen_ms.load() < 10000;

    for (size_t i = 0; i < g_probe_count; ++i) {
        auto& p = g_probes[i];
        const auto calls = p.calls.exchange(0);
        std::vector<std::pair<uintptr_t, uint64_t>> callers{};
        {
            std::scoped_lock _{p.mtx};
            callers.assign(p.callers.begin(), p.callers.end());
            p.callers.clear();
        }
        if (calls == 0) {
            continue;
        }
        std::sort(callers.begin(), callers.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        std::string top{};
        for (size_t c = 0; c < callers.size() && c < 4; ++c) {
            std::string who{};
            try {
                who = MethodDatabase::get()->find_method(callers[c].first);
            } catch (...) {
            }
            top += fmt::format("{}{} x{}", top.empty() ? "" : "; ", who.empty() ? fmt::format("{:x}", callers[c].first) : who,
                               callers[c].second * CALLER_SAMPLE_EVERY);
        }
        spdlog::info("[PragmataVR] Screen probe: {:.0f}/s {} | enemy bars {} damage {} | callers: {}", calls / secs, p.name,
                     bars ? "ON" : "off", damage ? "ON" : "off", top);
    }
}

} // namespace pragmata_screen_probe
