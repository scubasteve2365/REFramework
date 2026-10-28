#pragma once

// PragmataVR Build 35: performance telemetry written to re2_framework_log.txt every 10 seconds.
// No on-screen display (Steve uses fpsVR). Turned off with perf_log=0 in pragmatavr_hud.ini or the
// PERFORMANCE section of the mod menu.
//
// What is measured (all CPU wall-clock time):
//   - game frames per second and frame time (interval between main-pass BeginRendering calls)
//   - time blocked in xrWaitFrame (near zero = the CPU is late, the runtime is waiting on us)
//   - every via.Application stage (UpdateScene, BeginRendering, ...) split into the game's own time and the time
//     REFramework's mods spend in their callbacks, separately for the main pass and the right-eye re-run
//   - the right-eye re-run itself (waiting for the render thread, then re-running the render chain)
//   - GUI element draws: count, engine draw time, and mod callback time (VR, Lua scripts)
//   - VR::on_present on the render thread (eye submission, copies)
//
// Nested time is never counted twice: a callback that runs other stages (the right-eye re-run happens inside
// VR's EndRendering callback) is charged only its own time; the nested stages are charged to themselves.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

namespace pragmatavr::perf {

inline std::atomic<bool> g_enabled{true};

inline int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Time spent in measured scopes nested inside the current one, on this thread.
inline thread_local int64_t tl_child_ns = 0;

// Runs f and returns its own time in ns (minus time charged to measured scopes nested inside it).
template <class F>
inline int64_t timed(F&& f) {
    const auto saved = tl_child_ns;
    tl_child_ns = 0;
    const auto t0 = now_ns();
    f();
    const auto elapsed = now_ns() - t0;
    const auto self = elapsed - tl_child_ns;
    tl_child_ns = saved + elapsed;
    return self;
}

struct Acc {
    int64_t total{0};
    int64_t worst{0};
    uint64_t count{0};

    void add(int64_t v) {
        total += v;
        worst = (std::max)(worst, v);
        ++count;
    }
};

struct Stage {
    Acc game{};
    Acc mods{};
};

struct ModAcc {
    std::string name{};
    Acc acc{};
};

// One mod's time in one callback round (mods are indexed by their position in REFramework's mod list).
struct ModSample {
    const char* name{nullptr};
    int64_t ns{0};
};

class Stats {
public:
    static Stats& get() {
        static Stats s{};
        return s;
    }

    // ---- recorders ----

    // One via.Application stage call. pass 0 = main pass, 1 = right-eye re-run. samples[i] = mod i (pre + post).
    void stage(int pass, const char* name, int64_t game_ns, int64_t mods_ns, const ModSample* samples, size_t n) {
        std::scoped_lock _{m_mtx};
        auto& s = m_stages[pass & 1][name];
        s.game.add(game_ns);
        s.mods.add(mods_ns);
        add_mods(m_mod_entry, samples, n);
    }

    // One GUI element draw: the engine's draw time and each mod's callback time (pre + post).
    void gui(int64_t draw_ns, const ModSample* samples, size_t n) {
        std::scoped_lock _{m_mtx};
        m_gui_engine.add(draw_ns);
        add_mods(m_mod_gui, samples, n);
    }

    // A main-pass BeginRendering started: one game frame.
    void frame_start() {
        const auto t = now_ns();
        std::scoped_lock _{m_mtx};
        if (m_last_frame_start != 0) {
            const auto dt = t - m_last_frame_start;
            if (dt > 0 && dt < 1'000'000'000) {
                m_frame_ms.push_back((float)((double)dt / 1.0e6));
            }
        }
        m_last_frame_start = t;
    }

    void xr_wait(int64_t ns, int64_t display_period_ns) {
        std::scoped_lock _{m_mtx};
        m_xr_wait.add(ns);
        if (display_period_ns > 0) {
            m_display_period.add(display_period_ns);
        }
    }

    void rerun(int64_t wait_ns, int64_t chain_ns) {
        std::scoped_lock _{m_mtx};
        m_rerun_wait.add(wait_ns);
        m_rerun_chain.add(chain_ns);
    }

    void present(int64_t ns) {
        const auto t = now_ns();
        std::scoped_lock _{m_mtx};
        m_present.add(ns);
        if (m_last_present != 0) {
            m_present_interval.add(t - m_last_present);
        }
        m_last_present = t;
    }

    // ---- report ----

    // Called every present. Writes the summary when 10 s have passed. mode = "NATIVE" or "AFW".
    void maybe_report(const char* mode) {
        const auto t = now_ns();
        {
            std::scoped_lock _{m_mtx};
            if (m_window_start == 0) {
                m_window_start = t;
                return;
            }
            if (t - m_window_start < 10'000'000'000LL) {
                return;
            }
        }

        // Take the window's data and start a new one.
        std::unordered_map<const char*, Stage> stages[2];
        std::vector<ModAcc> w_mod_entry, w_mod_gui;
        std::vector<float> frame_ms;
        Acc w_gui, w_xr_wait, w_display_period, w_rerun_wait, w_rerun_chain, w_present, w_present_interval;
        double seconds = 0.0;
        {
            std::scoped_lock _{m_mtx};
            seconds = (double)(t - m_window_start) / 1.0e9;
            stages[0].swap(m_stages[0]);
            stages[1].swap(m_stages[1]);
            w_mod_entry = m_mod_entry;
            for (auto& m : m_mod_entry) m.acc = {};
            w_mod_gui = m_mod_gui;
            for (auto& m : m_mod_gui) m.acc = {};
            frame_ms.swap(m_frame_ms);
            w_gui = m_gui_engine; m_gui_engine = {};
            w_xr_wait = m_xr_wait; m_xr_wait = {};
            w_display_period = m_display_period; m_display_period = {};
            w_rerun_wait = m_rerun_wait; m_rerun_wait = {};
            w_rerun_chain = m_rerun_chain; m_rerun_chain = {};
            w_present = m_present; m_present = {};
            w_present_interval = m_present_interval; m_present_interval = {};
            m_window_start = t;
        }

        const auto frames = (double)(std::max<size_t>)(frame_ms.size(), 1);
        const auto per_frame = [&](int64_t ns) { return (double)ns / 1.0e6 / frames; };
        const auto ms = [](int64_t ns) { return (double)ns / 1.0e6; };
        const auto avg = [](const Acc& a) { return a.count ? (double)a.total / 1.0e6 / (double)a.count : 0.0; };

        // 1. Frame pacing.
        double f_avg = 0.0, f_p95 = 0.0, f_worst = 0.0;
        if (!frame_ms.empty()) {
            double sum = 0.0;
            for (auto v : frame_ms) sum += v;
            f_avg = sum / (double)frame_ms.size();
            auto sorted = frame_ms;
            std::sort(sorted.begin(), sorted.end());
            f_p95 = sorted[(std::min)(sorted.size() - 1, (size_t)((double)sorted.size() * 0.95))];
            f_worst = sorted.back();
        }
        const double hz = w_display_period.count ? 1.0e9 / ((double)w_display_period.total / (double)w_display_period.count) : 0.0;

        spdlog::info("[PragmataVR] Perf, last {:.1f} s ({}): {} game frames ({:.1f} per s), frame time avg {:.2f} ms, 95% {:.2f} ms, worst {:.2f} ms; "
                     "{} presents; xrWaitFrame blocked {:.2f} ms per frame (worst {:.2f} ms); headset {:.1f} Hz",
            seconds, mode, frame_ms.size(), (double)frame_ms.size() / (std::max)(seconds, 0.001), f_avg, f_p95, f_worst,
            w_present_interval.count + 1, per_frame(w_xr_wait.total), ms(w_xr_wait.worst), hz);

        // 2. Main thread split.
        int64_t game0 = 0, mods0 = 0, game1 = 0, mods1 = 0, wait_rendering = 0;
        for (auto& [name, s] : stages[0]) {
            game0 += s.game.total;
            mods0 += s.mods.total;
            if (std::string_view{name} == "WaitRendering") wait_rendering += s.game.total;
        }
        for (auto& [name, s] : stages[1]) {
            game1 += s.game.total;
            mods1 += s.mods.total;
        }

        spdlog::info("[PragmataVR] Perf main thread per frame: main pass {:.2f} ms (game {:.2f}, of which WaitRendering {:.2f}; mods {:.2f}); "
                     "right-eye re-run {:.2f} ms (waiting for the render thread {:.2f}, re-running the chain {:.2f}: game {:.2f}, mods {:.2f})",
            per_frame(game0 + mods0), per_frame(game0), per_frame(wait_rendering), per_frame(mods0),
            per_frame(w_rerun_wait.total + w_rerun_chain.total), per_frame(w_rerun_wait.total), per_frame(w_rerun_chain.total),
            per_frame(game1), per_frame(mods1));

        // 3. Top stages per pass.
        for (int pass = 0; pass < 2; ++pass) {
            if (stages[pass].empty()) {
                continue;
            }

            std::vector<std::pair<const char*, Stage>> list(stages[pass].begin(), stages[pass].end());
            std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
                return a.second.game.total + a.second.mods.total > b.second.game.total + b.second.mods.total;
            });

            std::string line{};
            const size_t n = (std::min<size_t>)(list.size(), 12);
            for (size_t i = 0; i < n; ++i) {
                const auto& [name, s] = list[i];
                line += fmt::format("{}{} {:.2f}+{:.2f} ({:.2f})", i ? ", " : "", name, per_frame(s.game.total), per_frame(s.mods.total),
                    ms((std::max)(s.game.worst, s.mods.worst)));
            }
            spdlog::info("[PragmataVR] Perf stages, {} (ms per frame, game + mods, worst call): {}",
                pass == 0 ? "main pass" : "right-eye re-run", line);
        }

        // 4. Mod callback time.
        const auto mod_line = [&](const std::vector<ModAcc>& m) {
            std::vector<ModAcc> list{};
            for (auto& v : m) if (v.acc.total > 0) list.push_back(v);
            std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.acc.total > b.acc.total; });
            std::string s{};
            for (size_t i = 0; i < (std::min<size_t>)(list.size(), 8); ++i) {
                s += fmt::format("{}{} {:.2f}", i ? ", " : "", list[i].name, per_frame(list[i].acc.total));
            }
            return s.empty() ? std::string{"none"} : s;
        };
        spdlog::info("[PragmataVR] Perf mods, ms per frame: stage callbacks: {}; GUI callbacks: {}", mod_line(w_mod_entry), mod_line(w_mod_gui));

        // 5. GUI and present.
        spdlog::info("[PragmataVR] Perf GUI per frame: {:.0f} element draws, engine draw {:.2f} ms. VR present (render thread): avg {:.2f} ms, "
                     "worst {:.2f} ms, present interval avg {:.2f} ms",
            (double)w_gui.count / frames, per_frame(w_gui.total), avg(w_present), ms(w_present.worst), avg(w_present_interval));
    }

private:
    static void add_mods(std::vector<ModAcc>& dst, const ModSample* samples, size_t n) {
        if (dst.size() < n) {
            dst.resize(n);
        }
        for (size_t i = 0; i < n; ++i) {
            if (dst[i].name.empty() && samples[i].name != nullptr) {
                dst[i].name = samples[i].name;
            }
            dst[i].acc.add(samples[i].ns);
        }
    }

    std::mutex m_mtx{};
    std::unordered_map<const char*, Stage> m_stages[2]{};
    std::vector<ModAcc> m_mod_entry{};
    std::vector<ModAcc> m_mod_gui{};
    std::vector<float> m_frame_ms{};
    int64_t m_last_frame_start{0};
    int64_t m_last_present{0};
    int64_t m_window_start{0};
    Acc m_gui_engine{};
    Acc m_xr_wait{};
    Acc m_display_period{};
    Acc m_rerun_wait{};
    Acc m_rerun_chain{};
    Acc m_present{};
    Acc m_present_interval{};
};

} // namespace pragmatavr::perf
