#include "InputSequence.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <string>

namespace fwl {

namespace {
int channelKey(InputEvent const& e) {
    return static_cast<int>(e.button) * 2 + (e.player2 ? 1 : 0);
}
std::string describe(InputEvent const& e) {
    return std::string(e.down ? "press" : "release") + " #" + std::to_string(e.id) + " (" +
           buttonName(e.button) + (e.player2 ? ", P2" : ", P1") + ") at tick " + std::to_string(e.tick);
}
void stableSortByTick(std::vector<InputEvent>& v) {
    std::stable_sort(v.begin(), v.end(), [](InputEvent const& a, InputEvent const& b) { return a.tick < b.tick; });
}
} // namespace

std::uint64_t InputSequence::add(Tick tick, Button button, bool down, bool player2) {
    InputEvent ev{m_nextId++, tick, button, down, player2};
    // upper_bound keeps insertion order for equal ticks.
    auto it = std::upper_bound(m_events.begin(), m_events.end(), tick,
                               [](Tick t, InputEvent const& e) { return t < e.tick; });
    m_events.insert(it, ev);
    return ev.id;
}

Status InputSequence::insertWithId(InputEvent ev) {
    if (find(ev.id)) return Status::fail("duplicate input id " + std::to_string(ev.id));
    if (ev.tick < 0) return Status::fail("negative tick in " + describe(ev));
    auto it = std::upper_bound(m_events.begin(), m_events.end(), ev.tick,
                               [](Tick t, InputEvent const& e) { return t < e.tick; });
    m_events.insert(it, ev);
    m_nextId = std::max(m_nextId, ev.id + 1);
    return Status::success();
}

Status InputSequence::remove(std::uint64_t id) {
    auto idx = indexOf(id);
    if (!idx) return Status::fail("no input with id " + std::to_string(id));
    auto candidate = m_events;
    candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(*idx));
    return applyTicks(std::move(candidate));
}

Status InputSequence::removeWithPartner(std::uint64_t id) {
    auto idx = indexOf(id);
    if (!idx) return Status::fail("no input with id " + std::to_string(id));
    if (remove(id)) return Status::success();
    auto const& ev = m_events[*idx];
    std::optional<std::size_t> partner;
    if (ev.down) {
        for (std::size_t i = *idx + 1; i < m_events.size() && !partner; ++i)
            if (channelKey(m_events[i]) == channelKey(ev)) partner = i;
    } else {
        for (std::size_t i = *idx; i-- > 0 && !partner;)
            if (channelKey(m_events[i]) == channelKey(ev)) partner = i;
    }
    if (!partner) return Status::fail("cannot remove " + describe(ev) + " without breaking press/release order");
    auto candidate = m_events;
    auto a = std::max(*idx, *partner), b = std::min(*idx, *partner);
    candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(a));
    candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(b));
    return applyTicks(std::move(candidate));
}

void InputSequence::truncateFrom(Tick tick) {
    m_events.erase(std::remove_if(m_events.begin(), m_events.end(), [&](InputEvent const& e) { return e.tick >= tick; }),
                   m_events.end());
}

Status InputSequence::shift(std::uint64_t id, Tick delta, ShiftMode mode) {
    auto idx = indexOf(id);
    if (!idx) return Status::fail("no input with id " + std::to_string(id));
    if (delta == 0) return Status::success();
    auto candidate = m_events;
    if (mode == ShiftMode::Single) {
        candidate[*idx].tick += delta;
    } else {
        for (std::size_t i = *idx; i < candidate.size(); ++i) candidate[i].tick += delta;
    }
    return applyTicks(std::move(candidate));
}

Status InputSequence::setTick(std::uint64_t id, Tick tick, ShiftMode mode) {
    auto ev = find(id);
    if (!ev) return Status::fail("no input with id " + std::to_string(id));
    return shift(id, tick - ev->tick, mode);
}

Status InputSequence::applyTicks(std::vector<InputEvent> candidate) {
    stableSortByTick(candidate);
    InputSequence probe;
    probe.m_events = std::move(candidate);
    if (auto st = probe.validate(); !st) return st;
    m_events = std::move(probe.m_events);
    return Status::success();
}

InputEvent const* InputSequence::find(std::uint64_t id) const {
    for (auto const& e : m_events)
        if (e.id == id) return &e;
    return nullptr;
}

std::optional<std::size_t> InputSequence::indexOf(std::uint64_t id) const {
    for (std::size_t i = 0; i < m_events.size(); ++i)
        if (m_events[i].id == id) return i;
    return std::nullopt;
}

std::optional<std::uint64_t> InputSequence::neighbour(std::uint64_t id, int dir) const {
    auto idx = indexOf(id);
    if (!idx) return std::nullopt;
    auto j = static_cast<std::ptrdiff_t>(*idx) + (dir < 0 ? -1 : 1);
    if (j < 0 || j >= static_cast<std::ptrdiff_t>(m_events.size())) return std::nullopt;
    return m_events[static_cast<std::size_t>(j)].id;
}

std::optional<std::uint64_t> InputSequence::nearest(Tick tick) const {
    std::optional<std::uint64_t> best;
    Tick bestDist = std::numeric_limits<Tick>::max();
    for (auto const& e : m_events) {
        Tick d = e.tick > tick ? e.tick - tick : tick - e.tick;
        if (d < bestDist) { bestDist = d; best = e.id; }
    }
    return best;
}

Status InputSequence::validate() const {
    std::map<int, InputEvent const*> last;
    Tick prevTick = std::numeric_limits<Tick>::min();
    for (auto const& e : m_events) {
        if (e.tick < 0) return Status::fail("input would move before tick 0: " + describe(e));
        if (e.tick < prevTick) return Status::fail("internal ordering error at " + describe(e));
        prevTick = e.tick;
        auto key = channelKey(e);
        auto it = last.find(key);
        if (it != last.end() && it->second->down == e.down) {
            return Status::fail("presses and releases would no longer alternate: " + describe(*it->second) +
                                " is followed by " + describe(e));
        }
        last[key] = &e;
    }
    return Status::success();
}

std::pair<Tick, Tick> InputSequence::legalRange(std::uint64_t id) const {
    auto idx = indexOf(id);
    if (!idx) return {0, -1};
    auto const& ev = m_events[*idx];
    Tick lo = 0;
    Tick hi = std::numeric_limits<Tick>::max();
    for (std::size_t i = *idx; i-- > 0;) {
        if (channelKey(m_events[i]) == channelKey(ev)) { lo = m_events[i].tick; break; }
    }
    for (std::size_t i = *idx + 1; i < m_events.size(); ++i) {
        if (channelKey(m_events[i]) == channelKey(ev)) { hi = m_events[i].tick; break; }
    }
    return {lo, hi};
}

Status EditHistory::apply(InputSequence& seq, std::function<Status(InputSequence&)> const& edit) {
    InputSequence before = seq;
    auto st = edit(seq);
    if (!st) {
        seq = std::move(before);
        return st;
    }
    if (seq.events() == before.events()) return st; // no-op edits do not create history
    m_undo.push_back(std::move(before));
    if (m_undo.size() > m_limit) m_undo.erase(m_undo.begin());
    m_redo.clear();
    return st;
}

bool EditHistory::undo(InputSequence& seq) {
    if (m_undo.empty()) return false;
    m_redo.push_back(seq);
    seq = std::move(m_undo.back());
    m_undo.pop_back();
    return true;
}

bool EditHistory::redo(InputSequence& seq) {
    if (m_redo.empty()) return false;
    m_undo.push_back(seq);
    seq = std::move(m_redo.back());
    m_redo.pop_back();
    return true;
}

} // namespace fwl
