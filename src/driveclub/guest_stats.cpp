// SPDX-FileCopyrightText: Copyright 2026 Driveclub-PC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
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
#include "driveclub/guest_stats.h"
#include "driveclub/stats_trigger.h"

namespace DriveclubPC::GuestStats {

bool g_active = false;

namespace {

constexpr u32 kMaxFunctions = 4096;
constexpr u32 kFaultRead = kMaxFunctions; // pseudo ids for the fault handler
constexpr u32 kFaultWrite = kMaxFunctions + 1;
constexpr u32 kSlots = kMaxFunctions + 2;
enum Group : u32 { Main, Tbb, Other, NumGroups };
constexpr const char* kGroupNames[] = {"Game:Main", "TBB workers", "other game threads"};

struct Counter {
    std::atomic<u64> calls{};
    std::atomic<u64> ticks{};
};
std::array<std::array<Counter, kSlots>, NumGroups> g_counters;
std::atomic<u64> g_main_hle_ticks{}, g_main_fault_ticks{};

std::mutex g_names_mutex;
std::vector<std::string> g_names;

thread_local u64 t_child_ticks = 0;
thread_local int t_group = -1;

Group CurrentGroup() {
    if (t_group < 0) {
        const std::string name = Common::GetCurrentThreadName();
        if (name.empty()) {
            return Other; // not cached: the guest may name the thread later
        }
        t_group = name == "Game:Main" ? Main : name.starts_with("TBB Worker") ? Tbb : Other;
    }
    return static_cast<Group>(t_group);
}

void Book(u32 slot, u64 ticks) {
    const Group group = CurrentGroup();
    g_counters[group][slot].calls.fetch_add(1, std::memory_order_relaxed);
    g_counters[group][slot].ticks.fetch_add(ticks, std::memory_order_relaxed);
    if (group == Main) {
        (slot >= kMaxFunctions ? g_main_fault_ticks : g_main_hle_ticks)
            .fetch_add(ticks, std::memory_order_relaxed);
    }
}

// Window report state (frame writer thread, under the profiler's lock).
std::chrono::steady_clock::time_point g_install_time;
double g_window_from = -1, g_window_to = -1;
int g_window_state = 0;
std::chrono::steady_clock::time_point g_window_start_time;
u64 g_window_frames = 0;
struct Snap {
    std::array<std::array<u64, kSlots>, NumGroups> calls{}, ticks{};
};
std::unique_ptr<Snap> g_window_start;
double g_ticks_per_ms = 0;
u64 g_frame_hle = 0, g_frame_fault = 0;

std::unique_ptr<Snap> TakeSnap() {
    auto s = std::make_unique<Snap>();
    for (u32 g = 0; g < NumGroups; ++g) {
        for (u32 i = 0; i < kSlots; ++i) {
            s->calls[g][i] = g_counters[g][i].calls.load(std::memory_order_relaxed);
            s->ticks[g][i] = g_counters[g][i].ticks.load(std::memory_order_relaxed);
        }
    }
    return s;
}

std::string ShortName(u32 slot) {
    if (slot == kFaultRead) {
        return "[guest read fault handling]";
    }
    if (slot == kFaultWrite) {
        return "[guest write fault handling]";
    }
    std::scoped_lock lk{g_names_mutex};
    if (slot >= g_names.size()) {
        return "?";
    }
    // "... HostCallWrapperImpl<&Libraries::Kernel::sceKernelUsleep>::wrap(...)"
    const std::string& sig = g_names[slot];
    const size_t open = sig.find("HostCallWrapperImpl<");
    const size_t close = sig.find(">::wrap");
    if (open == std::string::npos || close == std::string::npos) {
        return sig;
    }
    std::string name = sig.substr(open + 20, close - open - 20);
    // Nested wrappers ("Kernel::OrbisWrapperImpl<&Libraries::Kernel::f>") name the inner one.
    if (const size_t inner = name.rfind("<&"); inner != std::string::npos) {
        name.erase(0, inner + 2);
    }
    if (!name.empty() && name[0] == '&') {
        name.erase(0, 1);
    }
    while (!name.empty() && name.back() == '>') {
        name.pop_back();
    }
    if (name.starts_with("Libraries::")) {
        name.erase(0, 11);
    }
    return name;
}

void WriteWindowReport() {
    const auto end = TakeSnap();
    const double wall_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - g_window_start_time)
            .count();
    const double frames = std::max<double>(1.0, static_cast<double>(g_window_frames));
    const auto path = Common::FS::GetUserPath(Common::FS::PathType::LogDir) / "guest_stats.csv";
    std::FILE* f = std::fopen(path.string().c_str(), "w");
    if (f) {
        std::fprintf(f, "# window %.1f-%.1f s after install, %.2f s wall, %llu guest frames\n",
                     g_window_from, g_window_to, wall_s,
                     static_cast<unsigned long long>(g_window_frames));
        std::fprintf(f, "group,function,calls_per_s,ms_per_s,us_per_call,ms_per_frame\n");
    }
    for (u32 g = 0; g < NumGroups; ++g) {
        struct Row {
            u32 slot;
            u64 calls, ticks;
        };
        std::vector<Row> rows;
        u64 total_ticks = 0, fault_ticks = 0;
        for (u32 i = 0; i < kSlots; ++i) {
            const u64 calls = end->calls[g][i] - g_window_start->calls[g][i];
            const u64 ticks = end->ticks[g][i] - g_window_start->ticks[g][i];
            if (calls == 0) {
                continue;
            }
            total_ticks += ticks;
            if (i >= kMaxFunctions) {
                fault_ticks += ticks;
            }
            rows.push_back({i, calls, ticks});
        }
        std::ranges::sort(rows, [](const Row& a, const Row& b) { return a.ticks > b.ticks; });
        const double total_ms_s = total_ticks / g_ticks_per_ms / wall_s;
        const double fault_ms_s = fault_ticks / g_ticks_per_ms / wall_s;
        LOG_INFO(Debug,
                 "Guest stats {:.1f}-{:.1f} s, {}: HLE {:.1f} ms/s, fault handling {:.1f} ms/s "
                 "(per guest frame {:.2f} ms){}",
                 g_window_from, g_window_to, kGroupNames[g], total_ms_s - fault_ms_s, fault_ms_s,
                 total_ticks / g_ticks_per_ms / frames,
                 g == Main
                     ? fmt::format("; the rest of its {:.0f} ms/s is game code and unmeasured",
                                   1000.0 - total_ms_s)
                     : std::string{});
        for (size_t r = 0; r < rows.size(); ++r) {
            const Row& row = rows[r];
            const std::string name = ShortName(row.slot);
            const double ms = row.ticks / g_ticks_per_ms;
            if (f) {
                std::fprintf(f, "%s,\"%s\",%.1f,%.3f,%.2f,%.4f\n", kGroupNames[g], name.c_str(),
                             row.calls / wall_s, ms / wall_s, ms * 1000.0 / row.calls, ms / frames);
            }
            if (r < (g == Main ? 20u : 8u)) {
                LOG_INFO(Debug, "  {:<60} {:9.0f}/s {:8.2f} ms/s {:8.2f} us/call", name,
                         row.calls / wall_s, ms / wall_s, ms * 1000.0 / row.calls);
            }
        }
    }
    if (f) {
        std::fclose(f);
    }
    LOG_INFO(Debug, "Guest stats written to {}", path.string());
}

} // namespace

u32 Register(const char* signature) {
    std::scoped_lock lk{g_names_mutex};
    if (g_names.size() >= kMaxFunctions) {
        return kMaxFunctions - 1; // shared overflow slot
    }
    g_names.emplace_back(signature);
    return static_cast<u32>(g_names.size() - 1);
}

void BookCall(u32 id, u64 ticks) {
    Book(id, ticks);
}

void BookFault(bool write, u64 ticks) {
    Book(write ? kFaultWrite : kFaultRead, ticks);
}

// Each call books its exclusive time (its own time minus nested HLE calls), so a long-lived
// wrapped call such as a thread entry never hides the calls made inside it.
Scope::Scope(u32 id) : id_{id}, start_{__rdtsc()}, parent_child_ticks_{t_child_ticks} {
    t_child_ticks = 0;
}

Scope::~Scope() {
    const u64 total = __rdtsc() - start_;
    BookCall(id_, total - std::min(total, t_child_ticks));
    t_child_ticks = parent_child_ticks_ + total;
}

void Install() {
    const char* env = std::getenv("DCPC_GUEST_STATS");
    g_active = env && std::atoi(env) != 0;
    if (!g_active) {
        return;
    }
    g_ticks_per_ms = static_cast<double>(Common::EstimateRDTSCFrequency()) / 1e3;
    g_install_time = std::chrono::steady_clock::now();
    if (const char* window = std::getenv("DCPC_GUEST_STATS_WINDOW")) {
        const std::string w = window;
        const size_t colon = w.find(':');
        if (colon != std::string::npos) {
            g_window_from = std::atof(w.substr(0, colon).c_str());
            g_window_to = std::atof(w.substr(colon + 1).c_str());
        }
    }
    LOG_INFO(Debug, "Guest thread statistics on{}",
             g_window_from >= 0
                 ? fmt::format(", report window {:.1f}-{:.1f} s", g_window_from, g_window_to)
                 : std::string{});
}

void TakeFrame(double& main_hle_ms, double& main_fault_ms) {
    main_hle_ms = main_fault_ms = 0;
    if (!g_active) {
        return;
    }
    const u64 hle = g_main_hle_ticks.load(std::memory_order_relaxed);
    const u64 fault = g_main_fault_ticks.load(std::memory_order_relaxed);
    main_hle_ms = (hle - g_frame_hle) / g_ticks_per_ms;
    main_fault_ms = (fault - g_frame_fault) / g_ticks_per_ms;
    g_frame_hle = hle;
    g_frame_fault = fault;

    if (g_window_state == 2 || (g_window_from < 0 && !StatsTrigger::Configured())) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const double now_s = std::chrono::duration<double>(now - g_install_time).count();
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
        g_window_start = TakeSnap();
        g_window_start_time = now;
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

} // namespace DriveclubPC::GuestStats
