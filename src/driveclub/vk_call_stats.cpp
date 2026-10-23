// SPDX-FileCopyrightText: Copyright 2026 Driveclub-PC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>
#include <vector>

#include <fmt/format.h>
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/rdtsc.h"
#include "common/thread.h"
#include "driveclub/vk_call_stats.h"
#include "driveclub/stats_trigger.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace DriveclubPC::VkCallStats {

namespace {

using Dispatch = std::remove_cvref_t<decltype(VULKAN_HPP_DEFAULT_DISPATCHER)>;

// Non-command calls measured besides every vkCmd*.
#define DCPC_VK_STATS_EXTRA(X)                                                                     \
    X(vkQueueSubmit)                                                                               \
    X(vkQueueSubmit2)                                                                              \
    X(vkQueueBindSparse)                                                                           \
    X(vkQueuePresentKHR)                                                                           \
    X(vkBeginCommandBuffer)                                                                        \
    X(vkEndCommandBuffer)                                                                          \
    X(vkUpdateDescriptorSets)                                                                      \
    X(vkUpdateDescriptorSetWithTemplate)                                                           \
    X(vkAllocateDescriptorSets)                                                                    \
    X(vkResetDescriptorPool)                                                                       \
    X(vkWaitSemaphores)                                                                            \
    X(vkGetSemaphoreCounterValue)                                                                  \
    X(vkWaitForFences)                                                                             \
    X(vkResetFences)                                                                               \
    X(vkGetFenceStatus)                                                                            \
    X(vkCreateBuffer)                                                                              \
    X(vkCreateImage)                                                                               \
    X(vkCreateImageView)                                                                           \
    X(vkCreateSampler)                                                                             \
    X(vkAllocateMemory)                                                                            \
    X(vkFlushMappedMemoryRanges)                                                                   \
    X(vkInvalidateMappedMemoryRanges)

enum Id : u32 {
#define X(name) Id_##name,
#include "driveclub/vkcmd_list.inc"
    DCPC_VK_STATS_EXTRA(X)
#undef X
        Id_Count,
};

constexpr const char* kNames[] = {
#define X(name) #name,
#include "driveclub/vkcmd_list.inc"
    DCPC_VK_STATS_EXTRA(X)
#undef X
};

Dispatch g_real{};
bool g_enabled = false;

// Command processor thread: single writer, plain load+store. Other threads: locked adds.
struct Counter {
    std::atomic<u64> calls{};
    std::atomic<u64> ticks{};
};
std::array<Counter, Id_Count> g_cp;
std::array<Counter, Id_Count> g_other;
std::atomic<u64> g_cp_calls_total{}, g_cp_ticks_total{}, g_other_ticks_total{};

thread_local int t_is_cp = -1;

bool IsCommandProcessor() {
    if (t_is_cp < 0) {
        t_is_cp = Common::GetCurrentThreadName() == "shadPS4:GpuCommandProcessor" ? 1 : 0;
    }
    return t_is_cp == 1;
}

void Plain(std::atomic<u64>& c, u64 v) {
    c.store(c.load(std::memory_order_relaxed) + v, std::memory_order_relaxed);
}

struct Timer {
    u32 id;
    u64 start = __rdtsc();
    ~Timer() {
        const u64 ticks = __rdtsc() - start;
        if (IsCommandProcessor()) {
            Plain(g_cp[id].calls, 1);
            Plain(g_cp[id].ticks, ticks);
            Plain(g_cp_calls_total, 1);
            Plain(g_cp_ticks_total, ticks);
        } else {
            g_other[id].calls.fetch_add(1, std::memory_order_relaxed);
            g_other[id].ticks.fetch_add(ticks, std::memory_order_relaxed);
            g_other_ticks_total.fetch_add(ticks, std::memory_order_relaxed);
        }
    }
};

template <auto Member, u32 Index>
struct Thunk;

template <typename R, typename... A, R (VKAPI_PTR* Dispatch::*Member)(A...), u32 Index>
struct Thunk<Member, Index> {
    static R VKAPI_CALL Call(A... args) {
        Timer timer{Index};
        return (g_real.*Member)(args...);
    }
};

template <auto Member, u32 Index>
void InstallOne(Dispatch& d) {
    if (d.*Member != nullptr) {
        d.*Member = &Thunk<Member, Index>::Call;
    }
}

// DCPC_VK_STATS_WINDOW report state (frame writer thread, under the profiler's lock).
struct Snapshot {
    std::array<u64, Id_Count> cp_calls{}, cp_ticks{}, other_calls{}, other_ticks{};
};
std::chrono::steady_clock::time_point g_install_time;
double g_window_from = -1, g_window_to = -1;
int g_window_state = 0; // 0 before, 1 inside, 2 done
u64 g_window_frames = 0;
Snapshot g_window_start;
double g_ticks_per_ms = 0;
u64 g_frame_cp_calls = 0, g_frame_cp_ticks = 0, g_frame_other_ticks = 0;

Snapshot Take() {
    Snapshot s;
    for (u32 i = 0; i < Id_Count; ++i) {
        s.cp_calls[i] = g_cp[i].calls.load(std::memory_order_relaxed);
        s.cp_ticks[i] = g_cp[i].ticks.load(std::memory_order_relaxed);
        s.other_calls[i] = g_other[i].calls.load(std::memory_order_relaxed);
        s.other_ticks[i] = g_other[i].ticks.load(std::memory_order_relaxed);
    }
    return s;
}

void WriteWindowReport() {
    const Snapshot end = Take();
    const double frames = std::max<double>(1.0, static_cast<double>(g_window_frames));
    struct Row {
        u32 id;
        u64 cp_calls, cp_ticks, other_calls, other_ticks;
    };
    std::vector<Row> rows;
    u64 draw_calls = 0, total_cp_ticks = 0, total_cp_calls = 0;
    for (u32 i = 0; i < Id_Count; ++i) {
        Row r{i, end.cp_calls[i] - g_window_start.cp_calls[i],
              end.cp_ticks[i] - g_window_start.cp_ticks[i],
              end.other_calls[i] - g_window_start.other_calls[i],
              end.other_ticks[i] - g_window_start.other_ticks[i]};
        if (r.cp_calls == 0 && r.other_calls == 0) {
            continue;
        }
        total_cp_ticks += r.cp_ticks;
        total_cp_calls += r.cp_calls;
        const std::string name = kNames[i];
        if (name.starts_with("vkCmdDraw")) {
            draw_calls += r.cp_calls + r.other_calls;
        }
        rows.push_back(r);
    }
    std::ranges::sort(rows, [](const Row& a, const Row& b) {
        return a.cp_ticks + a.other_ticks > b.cp_ticks + b.other_ticks;
    });
    const double draws = std::max<double>(1.0, static_cast<double>(draw_calls));
    const auto path = Common::FS::GetUserPath(Common::FS::PathType::LogDir) / "vk_call_stats.csv";
    if (std::FILE* f = std::fopen(path.string().c_str(), "w")) {
        std::fprintf(f,
                     "# window %.1f-%.1f s after device creation, %llu guest frames, %.0f "
                     "host draw calls per frame\n",
                     g_window_from, g_window_to, static_cast<unsigned long long>(g_window_frames),
                     draws / frames);
        std::fprintf(f, "function,cp_calls_per_frame,cp_ms_per_frame,cp_ns_per_call,"
                        "cp_calls_per_draw,other_calls_per_frame,other_ms_per_frame\n");
        for (const Row& r : rows) {
            std::fprintf(f, "%s,%.2f,%.4f,%.1f,%.3f,%.2f,%.4f\n", kNames[r.id], r.cp_calls / frames,
                         r.cp_ticks / g_ticks_per_ms / frames,
                         r.cp_calls ? r.cp_ticks / g_ticks_per_ms * 1e6 / r.cp_calls : 0.0,
                         r.cp_calls / draws, r.other_calls / frames,
                         r.other_ticks / g_ticks_per_ms / frames);
        }
        std::fclose(f);
    }
    LOG_INFO(Debug,
             "Vulkan call stats {:.1f}-{:.1f} s: {} guest frames, {:.0f} draws/frame, command "
             "processor {:.0f} calls/frame, {:.2f} ms/frame in the driver; written to {}",
             g_window_from, g_window_to, g_window_frames, draws / frames, total_cp_calls / frames,
             total_cp_ticks / g_ticks_per_ms / frames, path.string());
    for (size_t i = 0; i < std::min<size_t>(rows.size(), 20); ++i) {
        const Row& r = rows[i];
        LOG_INFO(Debug,
                 "  {:<36} CP {:7.1f}/frame {:6.3f} ms/frame {:6.0f} ns/call; other "
                 "{:7.1f}/frame {:6.3f} ms/frame",
                 kNames[r.id], r.cp_calls / frames, r.cp_ticks / g_ticks_per_ms / frames,
                 r.cp_calls ? r.cp_ticks / g_ticks_per_ms * 1e6 / r.cp_calls : 0.0,
                 r.other_calls / frames, r.other_ticks / g_ticks_per_ms / frames);
    }
}

} // namespace

void Install() {
    const char* env = std::getenv("DCPC_VK_STATS");
    g_enabled = env && std::atoi(env) != 0;
    if (!g_enabled) {
        return;
    }
    g_ticks_per_ms = static_cast<double>(Common::EstimateRDTSCFrequency()) / 1e3;
    g_install_time = std::chrono::steady_clock::now();
    if (const char* window = std::getenv("DCPC_VK_STATS_WINDOW")) {
        const std::string w = window;
        const size_t colon = w.find(':');
        if (colon != std::string::npos) {
            g_window_from = std::atof(w.substr(0, colon).c_str());
            g_window_to = std::atof(w.substr(colon + 1).c_str());
        }
    }
    auto& d = VULKAN_HPP_DEFAULT_DISPATCHER;
    g_real = d;
#define X(name) InstallOne<&Dispatch::name, Id_##name>(d);
#include "driveclub/vkcmd_list.inc"
    DCPC_VK_STATS_EXTRA(X)
#undef X
    LOG_INFO(Debug, "Vulkan call statistics on ({} functions){}", static_cast<u32>(Id_Count),
             g_window_from >= 0
                 ? fmt::format(", report window {:.1f}-{:.1f} s", g_window_from, g_window_to)
                 : std::string{});
}

bool Enabled() {
    return g_enabled;
}

void TakeFrame(u64& cp_calls, double& cp_ms, double& other_ms) {
    cp_calls = 0;
    cp_ms = other_ms = 0;
    if (!g_enabled) {
        return;
    }
    const u64 calls = g_cp_calls_total.load(std::memory_order_relaxed);
    const u64 cp_ticks = g_cp_ticks_total.load(std::memory_order_relaxed);
    const u64 other_ticks = g_other_ticks_total.load(std::memory_order_relaxed);
    cp_calls = calls - g_frame_cp_calls;
    cp_ms = (cp_ticks - g_frame_cp_ticks) / g_ticks_per_ms;
    other_ms = (other_ticks - g_frame_other_ticks) / g_ticks_per_ms;
    g_frame_cp_calls = calls;
    g_frame_cp_ticks = cp_ticks;
    g_frame_other_ticks = other_ticks;

    if (g_window_state == 2 || (g_window_from < 0 && !StatsTrigger::Configured())) {
        return;
    }
    const double now_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - g_install_time).count();
    bool begin = now_s >= g_window_from;
    bool end = now_s >= g_window_to;
    if (StatsTrigger::Configured()) {
        const auto trigger = StatsTrigger::Poll();
        begin = trigger != StatsTrigger::State::Waiting;
        end = trigger == StatsTrigger::State::Stopped;
    }
    if (g_window_state == 0 && begin) {
        g_window_state = end ? 2 : 1;
        g_window_from = now_s;
        g_window_start = Take();
        g_window_frames = 0;
    } else if (g_window_state == 1) {
        if (end) {
            g_window_to = now_s;
            WriteWindowReport();
            g_window_state = 2;
        } else {
            ++g_window_frames;
        }
    }
}

} // namespace DriveclubPC::VkCallStats
