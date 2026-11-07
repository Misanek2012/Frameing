// Frame Window Lab — recorded input sequence, editing and undo/redo.
#pragma once

#include "Types.hpp"

#include <functional>
#include <optional>
#include <vector>

namespace fwl {

enum class ShiftMode {
    Single, // move only the chosen input
    Ripple  // move the chosen input and every input after it
};

// Ordered list of presses/releases keyed by simulation tick.
//
// Invariants (checked by validate()):
//  * every tick >= 0
//  * events are sorted by tick; ties keep their relative order (stable)
//  * on each channel (button, player) presses and releases alternate.
//    The first event of a channel may be a release (recording may start
//    while a button is already held).
class InputSequence {
public:
    std::vector<InputEvent> const& events() const { return m_events; }
    bool empty() const { return m_events.empty(); }
    std::size_t size() const { return m_events.size(); }

    // Append a recorded event. Recording only ever appends at the current
    // tick, so ordering is preserved; the event is still inserted in sorted
    // position for safety. Returns the new id.
    std::uint64_t add(Tick tick, Button button, bool down, bool player2);

    // Insert while keeping an explicit id (used by the loader).
    Status insertWithId(InputEvent ev);

    Status remove(std::uint64_t id);
    // Removes the input and its partner on the same channel (press -> the
    // following release, release -> the preceding press) so the remaining
    // sequence stays valid. Falls back to removing only `id` if that is valid.
    Status removeWithPartner(std::uint64_t id);
    // Drops every input at or after `tick` (used when re-recording from a rewind).
    void truncateFrom(Tick tick);
    Status shift(std::uint64_t id, Tick delta, ShiftMode mode);
    Status setTick(std::uint64_t id, Tick tick, ShiftMode mode);

    InputEvent const* find(std::uint64_t id) const;
    std::optional<std::size_t> indexOf(std::uint64_t id) const;
    // Previous/next event (dir = -1 / +1) in timeline order.
    std::optional<std::uint64_t> neighbour(std::uint64_t id, int dir) const;
    // Id of the event closest to `tick` (ties prefer the earlier event).
    std::optional<std::uint64_t> nearest(Tick tick) const;

    Tick firstTick() const { return m_events.empty() ? 0 : m_events.front().tick; }
    Tick lastTick() const { return m_events.empty() ? 0 : m_events.back().tick; }

    Status validate() const;
    void clear() { m_events.clear(); }

    std::uint64_t nextId() const { return m_nextId; }
    void setNextId(std::uint64_t id) { m_nextId = id; }

    // Ticks a single-input shift may legally reach without crossing the
    // neighbouring events on the same channel: [lo, hi] inclusive.
    // (Used by the sweep planner to mark offsets as "blocked".)
    std::pair<Tick, Tick> legalRange(std::uint64_t id) const;

private:
    Status applyTicks(std::vector<InputEvent> candidate);

    std::vector<InputEvent> m_events;
    std::uint64_t m_nextId = 1;
};

// Snapshot-based undo/redo. Sequences are small (hundreds to a few
// thousand events) so whole-snapshot history is simple and exact.
class EditHistory {
public:
    explicit EditHistory(std::size_t limit = 256) : m_limit(limit) {}

    // Runs `edit` on `seq`; if it succeeds the previous state is pushed to
    // the undo stack and the redo stack is cleared. Failed edits leave both
    // the sequence and history untouched.
    Status apply(InputSequence& seq, std::function<Status(InputSequence&)> const& edit);

    bool undo(InputSequence& seq);
    bool redo(InputSequence& seq);
    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }
    void clear() { m_undo.clear(); m_redo.clear(); }

private:
    std::size_t m_limit;
    std::vector<InputSequence> m_undo;
    std::vector<InputSequence> m_redo;
};

} // namespace fwl
