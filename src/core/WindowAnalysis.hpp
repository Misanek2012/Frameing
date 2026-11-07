// Frame Window Lab — frame-window measurement logic.
//
// A "trial" replays the recorded sequence with one input (optionally plus
// everything after it) moved by `offset` ticks, then classifies the result.
// The analysis below turns a set of trial outcomes into windows.
#pragma once

#include "Types.hpp"

#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace fwl {

enum class Outcome : std::uint8_t {
    Pass,      // reached the goal alive
    Fail,      // died before the goal
    Untrusted, // replay diverged from the baseline before the edited input,
               // or the trial ended without a decisive result
    Blocked    // offset impossible without crossing a neighbouring input
};

char const* outcomeName(Outcome o);
std::optional<Outcome> outcomeFromName(std::string const& s);

struct Goal {
    enum class Kind : std::uint8_t { SurviveUntilTick, CompleteLevel };
    Kind kind = Kind::SurviveUntilTick;
    Tick tick = 0; // used by SurviveUntilTick
};

struct TrialObservation {
    bool died = false;
    Tick deathTick = 0;
    bool completed = false;
    Tick lastTick = 0;          // last simulated tick of the trial
    bool desynced = false;      // state diverged from baseline before the edit
    bool aborted = false;       // stopped by the user / safety limit
};

Outcome classify(TrialObservation const& obs, Goal const& goal);

struct Interval {
    Tick lo = 0, hi = 0; // inclusive, in offsets relative to the original tick
    Tick size() const { return hi - lo + 1; }
    bool contains(Tick t) const { return t >= lo && t <= hi; }
    bool operator==(Interval const&) const = default;
};

// Why a window edge is where it is.
enum class Edge : std::uint8_t {
    Fail,      // the neighbouring offset was tested and failed: exact edge
    Blocked,   // neighbouring offset would cross another input: hard limit
    Untested,  // neighbouring offset not tested: window may extend further
    Untrusted  // neighbouring offset gave an untrusted result
};
char const* edgeName(Edge e);

struct WindowSegment {
    Interval range;
    Edge lowEdge = Edge::Untested;
    Edge highEdge = Edge::Untested;
    bool exact() const {
        auto ok = [](Edge e) { return e == Edge::Fail || e == Edge::Blocked; };
        return ok(lowEdge) && ok(highEdge);
    }
};

struct WindowReport {
    Tick originalTick = 0;                 // absolute tick of the unshifted input
    std::map<Tick, Outcome> results;       // offset -> outcome
    std::vector<WindowSegment> segments;   // passing runs, ascending
    std::optional<std::size_t> originalSegment; // segment containing offset 0
    Tick totalPass = 0;
    std::optional<Tick> earliest, latest;  // offsets of first/last passing tick
    std::vector<Tick> untrusted;
    bool contiguous() const { return segments.size() == 1; }
    // True only if every segment edge is known and no trial was untrusted.
    bool exact() const;
    std::string summary() const;
};

WindowReport analyzeWindow(std::map<Tick, Outcome> const& results, Tick originalTick);

struct SweepConfig {
    Tick initialRadius = 12;  // test offsets [-r, +r] first
    Tick maxRadius = 240;     // never look further than this
    bool autoExpand = true;   // grow the range while an edge still passes
};

// Chooses which offsets to test. Tests every offset in range (needed to
// find non-contiguous windows), expanding outward while an edge passes.
class SweepPlanner {
public:
    // `feasible(offset)` returns false when the shifted input would cross a
    // neighbouring press/release on its channel.
    SweepPlanner(SweepConfig cfg, std::function<bool(Tick)> feasible);

    std::optional<Tick> next();
    void report(Tick offset, Outcome o);
    std::map<Tick, Outcome> const& results() const { return m_results; }
    bool done() const { return m_done; }
    std::pair<Tick, Tick> range() const { return {m_lo, m_hi}; }

private:
    void enqueue(Tick from, Tick to);
    bool expand();

    SweepConfig m_cfg;
    std::function<bool(Tick)> m_feasible;
    std::vector<Tick> m_queue; // processed front to back
    std::size_t m_cursor = 0;
    std::map<Tick, Outcome> m_results;
    Tick m_lo = 0, m_hi = 0;
    bool m_done = false;
};

// ---- determinism ---------------------------------------------------------

std::uint64_t fnv1a(void const* data, std::size_t size, std::uint64_t seed = 1469598103934665603ull);

// Per-tick hash of player state captured during a replay.
class StateTrace {
public:
    void clear() { m_entries.clear(); }
    void push(Tick tick, std::uint64_t hash);
    std::size_t size() const { return m_entries.size(); }
    std::optional<std::uint64_t> at(Tick tick) const;
    std::vector<std::pair<Tick, std::uint64_t>> const& entries() const { return m_entries; }

private:
    std::vector<std::pair<Tick, std::uint64_t>> m_entries; // ascending ticks
};

// First tick < upTo at which the traces differ (missing entries count as a
// difference). nullopt = identical on [start, upTo).
std::optional<Tick> firstDivergence(StateTrace const& a, StateTrace const& b, Tick upTo);

} // namespace fwl
