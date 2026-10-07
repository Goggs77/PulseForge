// Profiling accumulators and the timed-scope implementations. At level 0 this
// translation unit is empty: no code, no strings, nothing to link against.
#include "core/Profile.h"

#if PF_PROFILE_LEVEL > 0

#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <vector>

#include "raylib.h"

namespace pf::profile {

namespace {

struct Accumulator {
    double total = 0.0;
    int count = 0;
    void add(double seconds) {
        total += seconds;
        ++count;
    }
    void reset() {
        total = 0.0;
        count = 0;
    }
};

Accumulator gPhases[static_cast<int>(Phase::Count)];
Accumulator gPanels[static_cast<int>(Panel::Count)];
std::unordered_map<int, double> gNodeMs;
std::unordered_map<int, std::string> gNodeNames;
// Frames since the last report, and since the app started (the block ranking
// covers the whole run while the phase averages restart every report).
int gFrames = 0;
long long gFramesTotal = 0;

const char *phaseNames[] = {"layout", "transport", "render", "live", "ui",
                            "swap",   "frame"};
const char *panelNames[] = {"palette", "canvas", "timeline", "preview", "inspector",
                            "topbar",  "status", "chrome",   "dialogs"};

}  // namespace

void add(Phase phase, double seconds) {
    const int index = static_cast<int>(phase);
    if (index >= 0 && index < static_cast<int>(Phase::Count)) gPhases[index].add(seconds);
}

void add(Panel panel, double seconds) {
    const int index = static_cast<int>(panel);
    if (index >= 0 && index < static_cast<int>(Panel::Count)) gPanels[index].add(seconds);
}

void addNode(int nodeId, const std::string &name, double milliseconds) {
    gNodeMs[nodeId] += milliseconds;
    if (!name.empty()) gNodeNames[nodeId] = name;
}

void frame() {
    ++gFrames;
    ++gFramesTotal;
}

void report(double windowSeconds, bool final, const std::string &context) {
    const double frames = std::max(1, gFrames);
    std::printf("[prof] %5.1fs  frames %6d  fps %6.1f | ", windowSeconds, gFrames,
                frames / std::max(0.001, windowSeconds));
    for (int i = 0; i < static_cast<int>(Phase::Count); ++i) {
        // Milliseconds per frame, not per sample: a phase may be probed more
        // than once per frame (the live audio work sits on both sides of the
        // pipeline render).
        std::printf("%s %.2f%s", phaseNames[i], gPhases[i].total * 1000.0 / frames,
                    i + 1 < static_cast<int>(Phase::Count) ? " + " : "");
    }
    if (!context.empty()) std::printf(" | %s", context.c_str());
    std::printf("\n");
    if (PF_PROFILE_LEVEL >= 2) {
        std::printf("[prof]   ui breakdown:");
        for (int i = 0; i < static_cast<int>(Panel::Count); ++i) {
            std::printf("  %s %.2f", panelNames[i], gPanels[i].total * 1000.0 / frames);
        }
        std::printf("\n");
    }
    if (final && PF_PROFILE_LEVEL >= 3) {
        const double total = static_cast<double>(std::max<long long>(1, gFramesTotal));
        std::vector<std::pair<double, int>> ranked;
        ranked.reserve(gNodeMs.size());
        for (const auto &entry : gNodeMs) ranked.emplace_back(entry.second, entry.first);
        std::sort(ranked.begin(), ranked.end(), std::greater<>());
        std::printf("[prof]   top blocks by evaluation time (ms/frame):\n");
        for (size_t i = 0; i < ranked.size() && i < 12; ++i) {
            const auto name = gNodeNames.find(ranked[i].second);
            std::printf("[prof]     %8.3f  %s\n", ranked[i].first / total,
                        name == gNodeNames.end() ? "?" : name->second.c_str());
        }
    }
    gFrames = 0;
    for (Accumulator &phase : gPhases) phase.reset();
    for (Accumulator &panel : gPanels) panel.reset();
}

PhaseScope::PhaseScope(Phase phase) : phase_(phase), start_(GetTime()) {}

PhaseScope::~PhaseScope() { add(phase_, GetTime() - start_); }

PanelScope::PanelScope(Panel panel) : panel_(panel), start_(GetTime()) {}

PanelScope::~PanelScope() { add(panel_, GetTime() - start_); }

}  // namespace pf::profile

#endif  // PF_PROFILE_LEVEL > 0
