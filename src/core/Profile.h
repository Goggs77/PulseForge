// Compile-time profiling instrumentation.
//
// PF_PROFILE_LEVEL (CMake: -DPF_PROFILE_LEVEL=n, default 0) selects how much is
// compiled in, so a release build carries none of it and every probe - macro,
// timer and report - costs nothing and leaves no code or string behind:
//
//   0  nothing at all (the default; what a release build uses)
//   1  frame phases: layout, transport, pipeline render, live audio, UI draw,
//      swap. Reported every few seconds while a session runs and once at the
//      end, together with whatever context the caller passes (renderer stats).
//   2  + the cost of each editor panel (palette, canvas, timeline, preview,
//      inspector, top bar, status bar, chrome, dialogs).
//   3  + a ranking of the blocks by evaluation time, including the compiled
//      audio-rate region and each block sampled inside it.
//
// A build with any level also honours two environment variables so a session can
// run unattended: PF_PROFILE_SECONDS=<seconds> stops the app after that long and
// prints the summary, PF_PROFILE_AUTOPLAY=1 starts playback immediately.
#pragma once

#ifndef PF_PROFILE_LEVEL
#define PF_PROFILE_LEVEL 0
#endif

#include <string>

namespace pf::profile {

inline constexpr int kLevel = PF_PROFILE_LEVEL;

// Frame phases (level 1).
enum class Phase : int {
    Layout = 0,
    Transport,
    Render,
    LiveAudio,
    UiDraw,
    Swap,
    Frame,
    Count,
};

// Editor panels inside the UI draw (level 2).
enum class Panel : int {
    Palette = 0,
    Canvas,
    Timeline,
    Preview,
    Inspector,
    TopBar,
    StatusBar,
    Chrome,
    Dialogs,
    Count,
};

#if PF_PROFILE_LEVEL > 0

// Records one sample. `seconds` is the wall time the phase or panel took.
void add(Phase phase, double seconds);
void add(Panel panel, double seconds);
// Level 3: one block's evaluation time for this frame (the number the block's
// footer shows), remembered under its title for the ranking.
void addNode(int nodeId, const std::string &name, double milliseconds);
// Counts one frame for the averages.
void frame();
// Prints the averages collected since the previous call. `windowSeconds` is the
// wall time they cover, `context` is a caller-formatted one-liner (renderer
// stats, fps); a final report also prints the block ranking.
void report(double windowSeconds, bool final, const std::string &context);

#else

inline void add(Phase, double) {}
inline void add(Panel, double) {}
inline void addNode(int, const std::string &, double) {}
inline void frame() {}
inline void report(double, bool, const std::string &) {}

#endif

#if PF_PROFILE_LEVEL > 0

// Scope timers: they take one sample when the scope ends.
class PhaseScope {
public:
    explicit PhaseScope(Phase phase);
    ~PhaseScope();
    PhaseScope(const PhaseScope &) = delete;
    PhaseScope &operator=(const PhaseScope &) = delete;

private:
    Phase phase_;
    double start_ = 0.0;
};

class PanelScope {
public:
    explicit PanelScope(Panel panel);
    ~PanelScope();
    PanelScope(const PanelScope &) = delete;
    PanelScope &operator=(const PanelScope &) = delete;

private:
    Panel panel_;
    double start_ = 0.0;
};

#else

// Empty shells, so a probe written by hand still compiles; the macros below
// expand to nothing at all.
class PhaseScope {
public:
    explicit PhaseScope(Phase) {}
};
class PanelScope {
public:
    explicit PanelScope(Panel) {}
};

#endif

}  // namespace pf::profile

// Probe macros: one per scope, so the names have to be unique in a block. At
// level 0 they expand to `(void)0` and disappear from the build entirely.
#if PF_PROFILE_LEVEL > 0
#define PF_PROFILE_ENABLED 1
#define PF_PROFILE_PHASE(name, phase) \
    ::pf::profile::PhaseScope pfPhase_##name(::pf::profile::Phase::phase)
#define PF_PROFILE_FRAME() ::pf::profile::frame()
#else
#define PF_PROFILE_ENABLED 0
#define PF_PROFILE_PHASE(name, phase) ((void)0)
#define PF_PROFILE_FRAME() ((void)0)
#endif

// The deeper probes only exist at the level that reports them, so a level 1
// build does not pay for panel timers it never prints.
#if PF_PROFILE_LEVEL >= 2
#define PF_PROFILE_PANEL(name, panel) \
    ::pf::profile::PanelScope pfPanel_##name(::pf::profile::Panel::panel)
#else
#define PF_PROFILE_PANEL(name, panel) ((void)0)
#endif

#if PF_PROFILE_LEVEL >= 3
#define PF_PROFILE_NODE(node) \
    ::pf::profile::addNode((node).id, (node).displayTitle(), (node).lastEvalMs)
#else
#define PF_PROFILE_NODE(node) ((void)0)
#endif
