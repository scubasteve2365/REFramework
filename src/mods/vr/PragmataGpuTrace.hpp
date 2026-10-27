#pragma once

// PragmataVR Build 18: GPU trace, a diagnostic for removing the flat HUD elements from the eye images (option A).
//
// The engine draws its UI twice per frame: into the eye image and into its UI buffer (the HUD-only image the HUD panel
// is built from). To send health, stamina, weapon wheel and tutorial only to the UI buffer, we first need to know how
// those two writes happen: one draw writing both targets at once (multiple render targets), two separate draws, or a
// copy. This trace answers that at the D3D12 level, and dumps the engine's own render target setup for comparison.
//
// While a snapshot is taken, every OMSetRenderTargets for the next two eye frames is logged with the resources it binds
// (known ones named: UIBUF = the engine UI buffer, BACKBUF = the swapchain image) and the number of draws that follow
// on that command list; copies into or out of the UI buffer are logged too. RTV descriptors are mapped back to their
// resources by watching ID3D12Device::CreateRenderTargetView from the time the hooks are installed.
//
// Included from vr/D3D12Component.cpp inside its PRAGMATA anonymous namespace.

#include <d3d12.h>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <optional>
#include <chrono>

#include <utility/FunctionHook.hpp>

namespace pragmata_trace {
struct RtvInfo {
    ID3D12Resource* res{nullptr};
    uint32_t w{0};
    uint32_t h{0};
    uint32_t format{0};
};

struct Event {
    uint32_t tid{0};
    void* list{nullptr};
    char kind{'R'}; // R = render targets, C = copy
    uint32_t n{0};
    uint64_t handle[4]{};
    RtvInfo info[4]{};
    bool known[4]{};
    uint32_t draws{0};
    uint32_t frame{0};
};

inline std::unique_ptr<FunctionHook> g_create_rtv_hook{};
inline std::unique_ptr<FunctionHook> g_om_set_hook{};
inline std::unique_ptr<FunctionHook> g_draw_hook{};
inline std::unique_ptr<FunctionHook> g_draw_indexed_hook{};
inline std::unique_ptr<FunctionHook> g_copy_region_hook{};
inline std::unique_ptr<FunctionHook> g_copy_resource_hook{};
inline std::unique_ptr<FunctionHook> g_barrier_hook{};

inline std::shared_mutex g_rtv_mtx{};
inline std::unordered_map<uint64_t, RtvInfo> g_rtvs{};
inline UINT g_rtv_increment{0};

inline std::atomic<bool> g_active{false};
inline std::atomic<uint32_t> g_frame{0};
inline std::mutex g_events_mtx{};
inline std::vector<Event> g_events{};
inline std::unordered_map<void*, size_t> g_last_event{}; // command list -> index of its last render target event
inline std::atomic<ID3D12Resource*> g_ui_buffer{nullptr};
inline std::atomic<uint64_t> g_creates_total{0};
inline std::atomic<uint64_t> g_ui_rtv_creates{0};

inline constexpr size_t MAX_EVENTS = 4000;

// Build 20 UI buffer hand-off state (see barrier()).
inline std::atomic<ID3D12Resource*> g_capture{nullptr};   // HUD panel source, kept in D3D12_RESOURCE_STATE_COMMON
inline std::atomic<bool> g_intercept{false};               // HUD panel mode on: capture at the hand-off
inline std::atomic<bool> g_erase{false};                   // Glued HUD REMOVED: clear the element areas after the copy
inline std::atomic<uint64_t> g_ui_rtv{0};                  // our own RTV of the UI buffer (for the clear)
inline std::atomic<uint64_t> g_handoffs{0};                // hand-offs seen (all time)
inline std::atomic<uint64_t> g_handoffs_window{0};         // hand-offs in the current 10 s report window
inline std::mutex g_rects_mtx{};
inline std::vector<D3D12_RECT> g_rects{};                  // element areas in UI buffer pixels

// Build 26: the engine hands out a different UI buffer texture from frame to frame (a pool). The overlay layer hook
// (VR::on_pre_overlay_layer_draw) reports each one as it is handed out (note_ui_buffer); the last few are remembered
// and any of them counts as the UI buffer at a hand-off. Each gets its own RTV (slot) for the erase.
inline constexpr int UI_RECENT = 8;
inline std::mutex g_ui_mtx{};
inline std::array<ID3D12Resource*, UI_RECENT> g_ui_recent{};
inline std::array<uint64_t, UI_RECENT> g_ui_recent_rtv{};  // our RTV for each remembered buffer (0 = not made yet)
inline int g_ui_recent_next{0};
inline std::atomic<uint64_t> g_ui_notes_window{0};         // overlay hook reports in the 10 s window
inline std::atomic<uint64_t> g_ui_new_window{0};           // reports of a buffer not seen among the recent ones
inline std::atomic<uint64_t> g_handoff_current_window{0};  // hand-offs of the buffer reported last
inline std::atomic<uint64_t> g_handoff_older_window{0};    // hand-offs of an older remembered buffer
inline std::atomic<uint64_t> g_handoff_skipped_window{0};
inline std::atomic<uint64_t> g_captures_window{0};         // Build 28: hand-offs copied to the panel (first pass only)  // hand-offs not captured (size differs from the capture)

// Build 19: the Glued HUD removal. When a render target set binds the UI buffer together with other targets (the
// engine writing a UI draw into the eye image and the UI buffer at once), the other targets are swapped for throwaway
// textures of the same size and format, so that UI reaches only the UI buffer.
inline std::atomic<bool> g_redirect{false};
inline std::atomic<uint64_t> g_redirected_sets{0};
inline std::atomic<uint64_t> g_ui_alone_sets{0};
inline std::atomic<uint64_t> g_ui_unknown_sets{0};
inline std::atomic<uint64_t> g_ui_mrt_sets{0};
inline ID3D12Device* g_device{nullptr};

struct Scratch {
    Microsoft::WRL::ComPtr<ID3D12Resource> tex{};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
};
inline std::mutex g_scratch_mtx{};
inline Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> g_scratch_heap{};
inline uint32_t g_scratch_used{0};
inline std::unordered_map<uint64_t, Scratch> g_scratch{}; // key: width, height, format
inline constexpr uint32_t SCRATCH_MAX = 32;

inline void STDMETHODCALLTYPE create_rtv(ID3D12Device* self, ID3D12Resource* res, const D3D12_RENDER_TARGET_VIEW_DESC* desc,
    D3D12_CPU_DESCRIPTOR_HANDLE handle);

inline std::optional<D3D12_CPU_DESCRIPTOR_HANDLE> scratch_rtv(const RtvInfo& info) {
    if (g_device == nullptr || info.w == 0 || info.h == 0 || info.format == 0) {
        return std::nullopt;
    }
    const uint64_t key = ((uint64_t)info.w << 40) ^ ((uint64_t)info.h << 16) ^ (uint64_t)info.format;
    std::scoped_lock _{g_scratch_mtx};
    auto it = g_scratch.find(key);
    if (it != g_scratch.end()) {
        return it->second.rtv;
    }
    if (g_scratch_heap == nullptr) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = SCRATCH_MAX;
        if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_scratch_heap)))) {
            return std::nullopt;
        }
    }
    if (g_scratch_used >= SCRATCH_MAX) {
        return std::nullopt;
    }
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = info.w;
    d.Height = info.h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = (DXGI_FORMAT)info.format;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    Scratch sc{};
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&sc.tex)))) {
        spdlog::error("[PragmataVR] Glued HUD: could not create a {} x {} format {} throwaway target", info.w, info.h, info.format);
        g_scratch[key] = Scratch{}; // do not retry every draw
        return std::nullopt;
    }
    sc.rtv = g_scratch_heap->GetCPUDescriptorHandleForHeapStart();
    sc.rtv.ptr += (SIZE_T)g_scratch_used * g_rtv_increment;
    ++g_scratch_used;
    // Created through the original function so the trace map does not record it.
    if (g_create_rtv_hook != nullptr) {
        g_create_rtv_hook->get_original<decltype(create_rtv)>()(g_device, sc.tex.Get(), nullptr, sc.rtv);
    } else {
        g_device->CreateRenderTargetView(sc.tex.Get(), nullptr, sc.rtv);
    }
    spdlog::info("[PragmataVR] Glued HUD: throwaway target {} x {} format {} ready", info.w, info.h, info.format);
    g_scratch[key] = sc;
    return sc.rtv;
}

inline void report_redirects() {
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(10)) {
        return;
    }
    last = now;
    const auto r = g_redirected_sets.exchange(0);
    const auto a = g_ui_alone_sets.exchange(0);
    const auto u = g_ui_unknown_sets.exchange(0);
    const auto m = g_ui_mrt_sets.exchange(0);
    const auto ho = g_handoffs_window.exchange(0);
    const auto notes = g_ui_notes_window.exchange(0);
    const auto fresh = g_ui_new_window.exchange(0);
    const auto cur = g_handoff_current_window.exchange(0);
    const auto older = g_handoff_older_window.exchange(0);
    const auto skipped = g_handoff_skipped_window.exchange(0);
    const auto captures = g_captures_window.exchange(0);
    static int reports = 0;
    if (reports < 60) {
        ++reports;
        spdlog::info("[PragmataVR] HUD hand-off: last 10 s, {} UI buffer hand-offs ({}; {} of the buffer reported last, {} of an older one, "
            "{} not the in-game buffer, {} copied to the panel), erase {}; overlay hook reported a UI buffer {} times ({} new); UI-buffer target sets: {} with other "
            "targets, {} alone, {} with unknown targets", ho, g_intercept.load() ? "captured" : "not captured, HUD mode 3D",
            cur, older, skipped, captures, g_erase.load() ? "on" : "off", notes, fresh, m, a, u);
    }
}

inline void STDMETHODCALLTYPE create_rtv(ID3D12Device* self, ID3D12Resource* res, const D3D12_RENDER_TARGET_VIEW_DESC* desc,
    D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    auto original = g_create_rtv_hook->get_original<decltype(create_rtv)>();
    original(self, res, desc, handle);

    RtvInfo info{};
    info.res = res;
    if (res != nullptr) {
        const auto d = res->GetDesc();
        info.w = (uint32_t)d.Width;
        info.h = d.Height;
        info.format = desc != nullptr ? (uint32_t)desc->Format : (uint32_t)d.Format;
        if (res == g_ui_buffer.load()) {
            g_ui_rtv_creates++;
        }
    }
    g_creates_total++;
    std::unique_lock _{g_rtv_mtx};
    g_rtvs[handle.ptr] = info;
}

inline void STDMETHODCALLTYPE om_set(ID3D12GraphicsCommandList* self, UINT num, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single,
    const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    auto original = g_om_set_hook->get_original<decltype(om_set)>();

    // Build 19: Glued HUD removal (see g_redirect).
    if (rtvs != nullptr && num >= 1 && num <= 8 && g_ui_buffer.load() != nullptr) {
        D3D12_CPU_DESCRIPTOR_HANDLE handles[8]{};
        RtvInfo infos[8]{};
        bool has_ui = false;
        bool all_known = true;
        {
            std::shared_lock _{g_rtv_mtx};
            for (UINT i = 0; i < num; ++i) {
                handles[i].ptr = single ? rtvs[0].ptr + (SIZE_T)i * g_rtv_increment : rtvs[i].ptr;
                auto it = g_rtvs.find(handles[i].ptr);
                if (it != g_rtvs.end()) {
                    infos[i] = it->second;
                    has_ui = has_ui || infos[i].res == g_ui_buffer.load();
                } else {
                    all_known = false;
                }
            }
        }
        if (has_ui && num == 1) {
            g_ui_alone_sets++;
        } else if (has_ui && !all_known) {
            g_ui_unknown_sets++;
        } else if (has_ui && g_redirect.load()) {
            bool ok = true;
            for (UINT i = 0; i < num && ok; ++i) {
                if (infos[i].res != g_ui_buffer.load()) {
                    auto sc = scratch_rtv(infos[i]);
                    if (sc) {
                        handles[i] = *sc;
                    } else {
                        ok = false;
                    }
                }
            }
            if (ok) {
                g_redirected_sets++;
                if (g_active.load()) {
                    Event e{};
                    e.tid = GetCurrentThreadId();
                    e.list = self;
                    e.kind = 'X'; // redirected
                    e.n = num;
                    e.frame = g_frame.load();
                    std::scoped_lock _{g_events_mtx};
                    if (g_events.size() < MAX_EVENTS) {
                        g_events.push_back(e);
                    }
                }
                original(self, num, handles, FALSE, dsv);
                return;
            }
        }
        if (has_ui && num > 1 && all_known) {
            g_ui_mrt_sets++;
        }
    }

    if (g_active.load() && rtvs != nullptr) {
        Event e{};
        e.tid = GetCurrentThreadId();
        e.list = self;
        e.kind = 'R';
        e.n = num;
        e.frame = g_frame.load();
        {
            std::shared_lock _{g_rtv_mtx};
            for (UINT i = 0; i < num && i < 4; ++i) {
                const uint64_t h = single ? rtvs[0].ptr + (uint64_t)i * g_rtv_increment : rtvs[i].ptr;
                e.handle[i] = h;
                auto it = g_rtvs.find(h);
                if (it != g_rtvs.end()) {
                    e.info[i] = it->second;
                    e.known[i] = true;
                }
            }
        }
        std::scoped_lock _{g_events_mtx};
        if (g_events.size() < MAX_EVENTS) {
            g_events.push_back(e);
            g_last_event[self] = g_events.size() - 1;
        }
    }

    original(self, num, rtvs, single, dsv);
}

inline void count_draw(ID3D12GraphicsCommandList* self) {
    if (!g_active.load()) {
        return;
    }
    std::scoped_lock _{g_events_mtx};
    auto it = g_last_event.find(self);
    if (it != g_last_event.end() && it->second < g_events.size()) {
        g_events[it->second].draws++;
    }
}

inline void STDMETHODCALLTYPE draw(ID3D12GraphicsCommandList* self, UINT a, UINT b, UINT c, UINT d) {
    count_draw(self);
    g_draw_hook->get_original<decltype(draw)>()(self, a, b, c, d);
}

inline void STDMETHODCALLTYPE draw_indexed(ID3D12GraphicsCommandList* self, UINT a, UINT b, UINT c, INT d, UINT e) {
    count_draw(self);
    g_draw_indexed_hook->get_original<decltype(draw_indexed)>()(self, a, b, c, d, e);
}

inline void log_copy(ID3D12GraphicsCommandList* self, ID3D12Resource* dst, ID3D12Resource* src) {
    if (!g_active.load()) {
        return;
    }
    const auto ui = g_ui_buffer.load();
    if (ui == nullptr || (dst != ui && src != ui)) {
        return;
    }
    Event e{};
    e.tid = GetCurrentThreadId();
    e.list = self;
    e.kind = 'C';
    e.n = 2;
    e.frame = g_frame.load();
    for (int i = 0; i < 2; ++i) {
        auto r = i == 0 ? dst : src;
        e.info[i].res = r;
        e.known[i] = r != nullptr;
        if (r != nullptr) {
            const auto d = r->GetDesc();
            e.info[i].w = (uint32_t)d.Width;
            e.info[i].h = d.Height;
            e.info[i].format = (uint32_t)d.Format;
        }
    }
    std::scoped_lock _{g_events_mtx};
    if (g_events.size() < MAX_EVENTS) {
        g_events.push_back(e);
    }
}

inline void STDMETHODCALLTYPE copy_region(ID3D12GraphicsCommandList* self, const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z,
    const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box) {
    log_copy(self, dst != nullptr ? dst->pResource : nullptr, src != nullptr ? src->pResource : nullptr);
    g_copy_region_hook->get_original<decltype(copy_region)>()(self, dst, x, y, z, src, box);
}

inline void STDMETHODCALLTYPE copy_resource(ID3D12GraphicsCommandList* self, ID3D12Resource* dst, ID3D12Resource* src) {
    log_copy(self, dst, src);
    g_copy_resource_hook->get_original<decltype(copy_resource)>()(self, dst, src);
}

// Build 20: the UI buffer hand-off. The engine draws its UI into the UI buffer on its own, then turns the buffer from a
// render target into a shader input and blends it onto the eye image (the "glued" HUD). At that state change, while
// the buffer still holds the whole UI, it is copied to g_capture (the HUD panel's source) and the HUD elements' areas
// are cleared from it (g_erase), so they reach the HUD panel but not the eye images. Recorded on the engine's own
// command list, so it runs at exactly that point in the frame.

// Build 27: a ring of RTV slots. A fresh RTV is written for the UI buffer at every hand-off (Build 26 cached one per
// address, and when the engine freed a texture and reused its address the cached RTV pointed at the freed texture:
// crash in the driver at startup). ClearRenderTargetView reads the descriptor when it is recorded, so a slot can be
// reused once the ring comes round.
inline constexpr int RTV_RING = 16;
inline Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> g_ui_rtv_heap{};
inline UINT g_ui_rtv_step{0};
inline std::atomic<uint32_t> g_ui_rtv_next{0};

// The in-game UI buffer size and format: the largest render-target 2D texture the overlay hook has reported (menus use
// a smaller desktop-size one). Read from the texture in the hook, while it is certainly alive.
inline std::atomic<uint32_t> g_game_ui_w{0};
inline std::atomic<uint32_t> g_game_ui_h{0};
inline std::atomic<int> g_game_ui_fmt{0};

// Build 28: the engine passes the UI buffer through RENDER_TARGET -> other more than once per frame, and only the pass
// right after the overlay hook hands the buffer out still holds the HUD (a later pass sees it wiped). The capture is
// taken only on that first pass: note_ui_buffer arms the buffer, the first hand-off of it disarms it.
inline std::atomic<ID3D12Resource*> g_ui_armed{nullptr};

// Called from the overlay layer hook with the UI buffer the engine is handing out right now.
inline void note_ui_buffer(ID3D12Resource* r) {
    if (r == nullptr) {
        return;
    }
    g_ui_notes_window++;
    g_ui_buffer = r;
    g_ui_armed = r;
    const auto d = r->GetDesc();
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0 &&
        (uint64_t)d.Width * d.Height > (uint64_t)g_game_ui_w.load() * g_game_ui_h.load()) {
        g_game_ui_fmt = (int)d.Format;
        g_game_ui_h = d.Height;
        g_game_ui_w = (uint32_t)d.Width;
    }
    std::scoped_lock _{g_ui_mtx};
    for (auto x : g_ui_recent) {
        if (x == r) {
            return;
        }
    }
    g_ui_new_window++;
    g_ui_recent[g_ui_recent_next] = r;
    g_ui_recent_next = (g_ui_recent_next + 1) % UI_RECENT;
}

inline bool is_recent_ui_buffer(ID3D12Resource* r) {
    std::scoped_lock _{g_ui_mtx};
    for (auto x : g_ui_recent) {
        if (x == r) {
            return true;
        }
    }
    return false;
}

// A fresh RTV for r (typed with its own format), or 0.
inline uint64_t fresh_rtv(ID3D12Resource* r, DXGI_FORMAT format) {
    if (g_device == nullptr) {
        return 0;
    }
    {
        std::scoped_lock _{g_ui_mtx};
        if (g_ui_rtv_heap == nullptr) {
            D3D12_DESCRIPTOR_HEAP_DESC hd{};
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            hd.NumDescriptors = RTV_RING;
            if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_ui_rtv_heap)))) {
                g_ui_rtv_heap.Reset();
                return 0;
            }
            g_ui_rtv_step = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        }
    }
    auto h = g_ui_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)(g_ui_rtv_next.fetch_add(1) % RTV_RING) * g_ui_rtv_step;
    D3D12_RENDER_TARGET_VIEW_DESC vd{};
    vd.Format = format;
    vd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    if (g_create_rtv_hook != nullptr) {
        g_create_rtv_hook->get_original<decltype(create_rtv)>()(g_device, r, &vd, h);
    } else {
        g_device->CreateRenderTargetView(r, &vd, h);
    }
    return (uint64_t)h.ptr;
}

inline void STDMETHODCALLTYPE barrier(ID3D12GraphicsCommandList* self, UINT n, const D3D12_RESOURCE_BARRIER* b) {
    auto original = g_barrier_hook->get_original<decltype(barrier)>();

    if (g_intercept.load() && b != nullptr) {
        bool handoff = false;
        ID3D12Resource* ui = nullptr;
        uint64_t ui_rtv = 0;
        for (UINT i = 0; i < n && !handoff; ++i) {
            const auto& t = b[i];
            if (t.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && t.Transition.pResource != nullptr &&
                t.Transition.StateBefore == D3D12_RESOURCE_STATE_RENDER_TARGET && t.Transition.StateAfter != D3D12_RESOURCE_STATE_RENDER_TARGET &&
                is_recent_ui_buffer(t.Transition.pResource)) {
                handoff = true;
                ui = t.Transition.pResource;
            }
        }

        if (handoff) {
            g_handoffs++;
            g_handoffs_window++;
            if (ui == g_ui_buffer.load()) {
                g_handoff_current_window++;
            } else {
                g_handoff_older_window++;
            }
            // Build 27: only the in-game UI buffer (2D render target with the capture's size and format) is captured or
            // cleared. The texture is the one being transitioned, so it is alive here.
            auto cap = g_capture.load();
            const auto du = ui->GetDesc();
            bool game_ui = cap != nullptr && du.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                           (du.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0 && du.MipLevels == 1 && du.DepthOrArraySize == 1;
            if (game_ui) {
                const auto dc = cap->GetDesc();
                game_ui = dc.Width == du.Width && dc.Height == du.Height && dc.Format == du.Format;
            }
            if (!game_ui) {
                cap = nullptr;
                g_handoff_skipped_window++;
            } else {
                ui_rtv = fresh_rtv(ui, du.Format);
                ID3D12Resource* expected = ui;
                if (!g_ui_armed.compare_exchange_strong(expected, nullptr)) {
                    cap = nullptr; // not the first pass after the hand-out: the buffer may already be wiped
                } else {
                    g_captures_window++;
                }
            }
            if (cap != nullptr) {
                D3D12_RESOURCE_BARRIER pre[2]{};
                pre[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                pre[0].Transition.pResource = ui;
                pre[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                pre[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                pre[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                pre[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                pre[1].Transition.pResource = cap;
                pre[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                pre[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                pre[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                original(self, 2, pre);

                if (g_copy_resource_hook != nullptr) {
                    g_copy_resource_hook->get_original<decltype(copy_resource)>()(self, cap, ui);
                } else {
                    self->CopyResource(cap, ui);
                }

                D3D12_RESOURCE_BARRIER post[2] = {pre[0], pre[1]};
                std::swap(post[0].Transition.StateBefore, post[0].Transition.StateAfter);
                std::swap(post[1].Transition.StateBefore, post[1].Transition.StateAfter);
                original(self, 2, post);
            }

            const auto rtv = ui_rtv;
            if (g_erase.load() && game_ui && rtv != 0) {
                std::vector<D3D12_RECT> rects{};
                {
                    std::scoped_lock _{g_rects_mtx};
                    rects = g_rects;
                }
                if (!rects.empty()) {
                    const float clear[4]{0.0f, 0.0f, 0.0f, 0.0f};
                    D3D12_CPU_DESCRIPTOR_HANDLE h{};
                    h.ptr = (SIZE_T)rtv;
                    self->ClearRenderTargetView(h, clear, (UINT)rects.size(), rects.data());
                }
            }
        }
    }

    original(self, n, b);
}

// Our own RTV of the UI buffer for the clear; recreated when the buffer changes. Called from the present thread.
inline void update_ui_rtv(ID3D12Resource* ui) {
    static ID3D12Resource* last = nullptr;
    static Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap{};
    if (ui == last || g_device == nullptr) {
        return;
    }
    last = ui;
    g_ui_rtv = 0;
    if (ui == nullptr) {
        return;
    }
    if (heap == nullptr) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1;
        if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))) {
            return;
        }
    }
    auto h = heap->GetCPUDescriptorHandleForHeapStart();
    if (g_create_rtv_hook != nullptr) {
        g_create_rtv_hook->get_original<decltype(create_rtv)>()(g_device, ui, nullptr, h);
    } else {
        g_device->CreateRenderTargetView(ui, nullptr, h);
    }
    g_ui_rtv = (uint64_t)h.ptr;

    // Element areas in the flat (Screen mode) layout, measured from Build 17-18 snapshots of the UI buffer, as
    // fractions of its width and height: tutorial box, health, stamina, weapon wheel.
    const auto d = ui->GetDesc();
    const float w = (float)d.Width;
    const float hh = (float)d.Height;
    static constexpr float areas[4][4] = {
        {0.00f, 0.27f, 0.30f, 0.46f}, // tutorial box
        {0.00f, 0.68f, 0.17f, 0.79f}, // health
        {0.36f, 0.71f, 0.64f, 0.81f}, // stamina
        {0.74f, 0.60f, 1.00f, 0.86f}, // weapon wheel
    };
    std::scoped_lock _{g_rects_mtx};
    g_rects.clear();
    for (const auto& a : areas) {
        g_rects.push_back(D3D12_RECT{(LONG)(a[0] * w), (LONG)(a[1] * hh), (LONG)(a[2] * w), (LONG)(a[3] * hh)});
    }
    spdlog::info("[PragmataVR] UI buffer hand-off: own RTV ready for {} x {}, {} element areas", d.Width, d.Height, g_rects.size());
}

// Hooks the D3D12 functions through the vtables of the game's device and a throwaway command list. All devices and
// command lists of the runtime share these functions, so the game's own calls go through the hooks.
inline bool install(ID3D12Device* device) {
    static bool tried = false;
    static bool ok = false;
    if (tried) {
        return ok;
    }
    tried = true;

    if (device == nullptr) {
        return false;
    }

    g_rtv_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g_device = device;

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc{};
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list{};
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)))) {
        spdlog::error("[PragmataVR] GPU trace: could not create a command list to find the hooks");
        return false;
    }
    list->Close();

    auto dev_vt = *(uintptr_t**)device;
    auto list_vt = *(uintptr_t**)list.Get();

    const auto make = [](std::unique_ptr<FunctionHook>& h, uintptr_t target, uintptr_t dest, const char* what) {
        h = std::make_unique<FunctionHook>(target, dest);
        if (!h->create()) {
            spdlog::error("[PragmataVR] GPU trace: could not hook {}", what);
            h.reset();
            return false;
        }
        return true;
    };

    ok = make(g_create_rtv_hook, dev_vt[20], (uintptr_t)&create_rtv, "CreateRenderTargetView");
    ok = make(g_om_set_hook, list_vt[46], (uintptr_t)&om_set, "OMSetRenderTargets") && ok;
    ok = make(g_draw_hook, list_vt[12], (uintptr_t)&draw, "DrawInstanced") && ok;
    ok = make(g_draw_indexed_hook, list_vt[13], (uintptr_t)&draw_indexed, "DrawIndexedInstanced") && ok;
    ok = make(g_copy_region_hook, list_vt[16], (uintptr_t)&copy_region, "CopyTextureRegion") && ok;
    ok = make(g_copy_resource_hook, list_vt[17], (uintptr_t)&copy_resource, "CopyResource") && ok;
    ok = make(g_barrier_hook, list_vt[26], (uintptr_t)&barrier, "ResourceBarrier") && ok;

    spdlog::info("[PragmataVR] GPU trace hooks {} (RTV descriptor size {})", ok ? "installed" : "partly failed", g_rtv_increment);
    return ok;
}

inline std::string describe(const RtvInfo& i, bool known, uint64_t handle, ID3D12Resource* backbuffer) {
    if (!known) {
        return fmt::format("?{:x}", handle);
    }
    if (i.res == g_ui_buffer.load()) {
        return fmt::format("UIBUF({}x{} f{})", i.w, i.h, i.format);
    }
    if (i.res == backbuffer) {
        return fmt::format("BACKBUF({}x{} f{})", i.w, i.h, i.format);
    }
    return fmt::format("{:x}({}x{} f{})", (uintptr_t)i.res, i.w, i.h, i.format);
}

// Writes the collected events to the log. Consecutive identical render target sets on one command list are merged.
inline void dump(ID3D12Resource* backbuffer) {
    std::vector<Event> events{};
    {
        std::scoped_lock _{g_events_mtx};
        events.swap(g_events);
        g_last_event.clear();
    }

    size_t ui_sets = 0;
    size_t ui_draws = 0;
    size_t ui_mrt = 0;
    std::string lines{};
    size_t written = 0;

    for (size_t k = 0; k < events.size(); ++k) {
        const auto& e = events[k];
        bool touches_ui = false;
        for (uint32_t i = 0; i < e.n && i < 4; ++i) {
            if (e.known[i] && e.info[i].res == g_ui_buffer.load()) {
                touches_ui = true;
            }
        }
        if (e.kind == 'R' && touches_ui) {
            ++ui_sets;
            ui_draws += e.draws;
            if (e.n > 1) {
                ++ui_mrt;
            }
        }

        // Write UI-related events, and render target sets around them, in full; skip the rest past a limit.
        bool near_ui = touches_ui;
        for (size_t j = (k > 3 ? k - 3 : 0); j < (std::min)(events.size(), k + 4) && !near_ui; ++j) {
            const auto& o = events[j];
            for (uint32_t i = 0; i < o.n && i < 4; ++i) {
                if (o.known[i] && o.info[i].res == g_ui_buffer.load()) {
                    near_ui = true;
                }
            }
        }
        if (!near_ui && written > 400) {
            continue;
        }

        std::string targets{};
        for (uint32_t i = 0; i < e.n && i < 4; ++i) {
            targets += (i > 0 ? ", " : "") + describe(e.info[i], e.known[i], e.handle[i], backbuffer);
        }
        if (e.kind == 'X') {
            lines += fmt::format("\n  #{} f{} t{} list {:x} REDIRECTED a UI-buffer set of {} targets", k, e.frame, e.tid, (uintptr_t)e.list, e.n);
        } else if (e.kind == 'C') {
            lines += fmt::format("\n  #{} f{} t{} list {:x} COPY dst {} <- src {}", k, e.frame, e.tid, (uintptr_t)e.list,
                describe(e.info[0], e.known[0], 0, backbuffer), describe(e.info[1], e.known[1], 0, backbuffer));
        } else {
            lines += fmt::format("\n  #{} f{} t{} list {:x} RT[{}] {} draws {}{}", k, e.frame, e.tid, (uintptr_t)e.list, e.n, targets,
                e.draws, touches_ui ? "  <== UI BUFFER" : "");
        }
        ++written;
        if (lines.size() > 120000) {
            lines += "\n  (log cut)";
            break;
        }
    }

    spdlog::info("[PragmataVR] GPU trace: {} events, {} render target sets bind the UI buffer ({} with more than one target, {} draws); "
        "RTVs seen created {} (UI buffer RTVs {}); UI buffer {:x}, backbuffer {:x}:{}",
        events.size(), ui_sets, ui_mrt, ui_draws, g_creates_total.load(), g_ui_rtv_creates.load(),
        (uintptr_t)g_ui_buffer.load(), (uintptr_t)backbuffer, lines);
}
} // namespace pragmata_trace
