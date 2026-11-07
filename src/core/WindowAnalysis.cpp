#include "WindowAnalysis.hpp"

#include <algorithm>
#include <cstring>
#include <locale>
#include <sstream>

namespace fwl {

char const* outcomeName(Outcome o) {
    switch (o) {
        case Outcome::Pass: return "pass";
        case Outcome::Fail: return "fail";
        case Outcome::Untrusted: return "untrusted";
        case Outcome::Blocked: return "blocked";
    }
    return "?";
}

std::optional<Outcome> outcomeFromName(std::string const& s) {
    if (s == "pass") return Outcome::Pass;
    if (s == "fail") return Outcome::Fail;
    if (s == "untrusted") return Outcome::Untrusted;
    if (s == "blocked") return Outcome::Blocked;
    return std::nullopt;
}

char const* edgeName(Edge e) {
    switch (e) {
        case Edge::Fail: return "fail";
        case Edge::Blocked: return "blocked";
        case Edge::Untested: return "untested";
        case Edge::Untrusted: return "untrusted";
    }
    return "?";
}

Outcome classify(TrialObservation const& obs, Goal const& goal) {
    if (obs.desynced) return Outcome::Untrusted;
    if (goal.kind == Goal::Kind::CompleteLevel) {
        if (obs.died) return Outcome::Fail;
        if (obs.completed) return Outcome::Pass;
        return Outcome::Untrusted;
    }
    if (obs.died && obs.deathTick <= goal.tick) return Outcome::Fail;
    if (obs.lastTick >= goal.tick || obs.completed) return Outcome::Pass;
    return Outcome::Untrusted; // ended (aborted) before a decision was possible
}

bool WindowReport::exact() const {
    if (!untrusted.empty()) return false;
    for (auto const& s : segments)
        if (!s.exact()) return false;
    return true;
}

std::string WindowReport::summary() const {
    std::ostringstream os;
    os.imbue(std::locale::classic()); // never "12 345" / "1,5", whatever the host locale
    auto sgn = [](Tick t) { return (t > 0 ? "+" : "") + std::to_string(t); };
    if (segments.empty()) {
        os << "no passing timing found in " << results.size() << " tested offsets";
    } else {
        os << totalPass << " passing tick" << (totalPass == 1 ? "" : "s");
        if (!contiguous()) os << " in " << segments.size() << " separate windows";
        os << ":";
        for (auto const& s : segments) {
            os << " [" << sgn(s.range.lo) << ".." << sgn(s.range.hi) << "]=" << s.range.size();
            if (!s.exact()) os << "?";
        }
    }
    if (!untrusted.empty()) os << " | " << untrusted.size() << " untrusted";
    os << (exact() ? " | exact" : " | NOT exact");
    return os.str();
}

WindowReport analyzeWindow(std::map<Tick, Outcome> const& results, Tick originalTick) {
    WindowReport r;
    r.originalTick = originalTick;
    r.results = results;

    auto edgeAt = [&](Tick t) {
        auto it = results.find(t);
        if (it == results.end()) return Edge::Untested;
        switch (it->second) {
            case Outcome::Fail: return Edge::Fail;
            case Outcome::Blocked: return Edge::Blocked;
            case Outcome::Untrusted: return Edge::Untrusted;
            case Outcome::Pass: return Edge::Untested; // unreachable for a true edge
        }
        return Edge::Untested;
    };

    std::optional<WindowSegment> cur;
    auto flush = [&] {
        if (!cur) return;
        cur->highEdge = edgeAt(cur->range.hi + 1);
        r.segments.push_back(*cur);
        cur.reset();
    };

    for (auto const& [off, out] : results) {
        if (out == Outcome::Untrusted) r.untrusted.push_back(off);
        if (out == Outcome::Pass) {
            ++r.totalPass;
            if (cur && off == cur->range.hi + 1) {
                cur->range.hi = off;
            } else {
                flush();
                cur = WindowSegment{{off, off}, edgeAt(off - 1), Edge::Untested};
            }
        } else {
            flush();
        }
    }
    flush();

    if (!r.segments.empty()) {
        r.earliest = r.segments.front().range.lo;
        r.latest = r.segments.back().range.hi;
    }
    for (std::size_t i = 0; i < r.segments.size(); ++i)
        if (r.segments[i].range.contains(0)) r.originalSegment = i;
    return r;
}

SweepPlanner::SweepPlanner(SweepConfig cfg, std::function<bool(Tick)> feasible)
    : m_cfg(cfg), m_feasible(std::move(feasible)) {
    if (m_cfg.initialRadius < 0) m_cfg.initialRadius = 0;
    if (m_cfg.maxRadius < m_cfg.initialRadius) m_cfg.maxRadius = m_cfg.initialRadius;
    m_lo = -m_cfg.initialRadius;
    m_hi = m_cfg.initialRadius;
    enqueue(m_lo, m_hi);
}

void SweepPlanner::enqueue(Tick from, Tick to) {
    for (Tick t = from; t <= to; ++t) m_queue.push_back(t);
}

std::optional<Tick> SweepPlanner::next() {
    while (!m_done) {
        while (m_cursor < m_queue.size()) {
            Tick t = m_queue[m_cursor++];
            if (m_results.count(t)) continue;
            if (!m_feasible(t)) {
                m_results[t] = Outcome::Blocked;
                continue;
            }
            return t;
        }
        if (!expand()) m_done = true;
    }
    return std::nullopt;
}

void SweepPlanner::report(Tick offset, Outcome o) { m_results[offset] = o; }

bool SweepPlanner::expand() {
    if (!m_cfg.autoExpand) return false;
    Tick step = std::max<Tick>(m_cfg.initialRadius, 1);
    bool grew = false;
    auto passes = [&](Tick t) {
        auto it = m_results.find(t);
        return it != m_results.end() && it->second == Outcome::Pass;
    };
    if (passes(m_lo) && m_lo > -m_cfg.maxRadius) {
        Tick newLo = std::max(-m_cfg.maxRadius, m_lo - step);
        enqueue(newLo, m_lo - 1);
        m_lo = newLo;
        grew = true;
    }
    if (passes(m_hi) && m_hi < m_cfg.maxRadius) {
        Tick newHi = std::min(m_cfg.maxRadius, m_hi + step);
        enqueue(m_hi + 1, newHi);
        m_hi = newHi;
        grew = true;
    }
    return grew;
}

std::uint64_t fnv1a(void const* data, std::size_t size, std::uint64_t h) {
    auto p = static_cast<unsigned char const*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

void StateTrace::push(Tick tick, std::uint64_t hash) {
    // A tick can be processed more than once if the game re-runs a step;
    // keep the latest value for that tick.
    if (!m_entries.empty() && m_entries.back().first == tick) {
        m_entries.back().second = hash;
        return;
    }
    m_entries.emplace_back(tick, hash);
}

std::optional<std::uint64_t> StateTrace::at(Tick tick) const {
    auto it = std::lower_bound(m_entries.begin(), m_entries.end(), tick,
                               [](auto const& e, Tick t) { return e.first < t; });
    if (it == m_entries.end() || it->first != tick) return std::nullopt;
    return it->second;
}

std::optional<Tick> firstDivergence(StateTrace const& a, StateTrace const& b, Tick upTo) {
    auto const& ea = a.entries();
    auto const& eb = b.entries();
    std::size_t i = 0, j = 0;
    while (true) {
        bool ha = i < ea.size() && ea[i].first < upTo;
        bool hb = j < eb.size() && eb[j].first < upTo;
        if (!ha && !hb) return std::nullopt;
        if (ha && hb) {
            if (ea[i].first != eb[j].first) return std::min(ea[i].first, eb[j].first);
            if (ea[i].second != eb[j].second) return ea[i].first;
            ++i, ++j;
            continue;
        }
        return ha ? ea[i].first : eb[j].first; // one trace has a tick the other lacks
    }
}

} // namespace fwl
