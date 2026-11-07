// Frame Window Lab — playback cursor and trial construction.
#pragma once

#include "InputSequence.hpp"

#include <algorithm>

namespace fwl {

// Walks a sequence in tick order and fires every event whose tick has been
// reached. If the game ever skips a tick, events are still fired (late) and
// counted so the caller can mark the run as untrusted.
class PlaybackCursor {
public:
    void reset(InputSequence const* seq) {
        m_seq = seq;
        m_index = 0;
        m_late = 0;
    }

    template <class Fire>
    std::size_t fireUpTo(Tick tick, Fire&& fire) {
        if (!m_seq) return 0;
        auto const& ev = m_seq->events();
        std::size_t fired = 0;
        while (m_index < ev.size() && ev[m_index].tick <= tick) {
            if (ev[m_index].tick < tick) ++m_late;
            fire(ev[m_index]);
            ++m_index;
            ++fired;
        }
        return fired;
    }

    // Skip events before `tick` without firing (used when playback starts
    // from a later tick). Returns how many were skipped.
    std::size_t seek(Tick tick) {
        if (!m_seq) return 0;
        auto const& ev = m_seq->events();
        std::size_t before = m_index;
        while (m_index < ev.size() && ev[m_index].tick < tick) ++m_index;
        return m_index - before;
    }

    bool finished() const { return !m_seq || m_index >= m_seq->events().size(); }
    std::size_t lateCount() const { return m_late; }
    std::size_t position() const { return m_index; }

private:
    InputSequence const* m_seq = nullptr;
    std::size_t m_index = 0;
    std::size_t m_late = 0;
};

struct TrialPlan {
    InputSequence sequence; // modified copy
    Tick firstChangedTick;  // earliest tick at which the trial may differ from baseline
    Tick inputTick;         // tick at which the edited input now fires
};

// Builds the sequence for testing input `id` shifted by `offset` ticks.
inline Expected<TrialPlan> makeTrial(InputSequence const& base, std::uint64_t id, Tick offset, ShiftMode mode) {
    auto ev = base.find(id);
    if (!ev) return Expected<TrialPlan>::fail("input no longer exists");
    TrialPlan plan{base, std::min(ev->tick, ev->tick + offset), ev->tick + offset};
    if (auto st = plan.sequence.shift(id, offset, mode); !st) return Expected<TrialPlan>::fail(st.error);
    return Expected<TrialPlan>::success(std::move(plan));
}

// Sound events the UI can play.
enum class SoundEvent { Step, Select, MoveInput, CircleCreate, TestPass, TestFail, WindowDone };

// Ascending "count" sound: the n-th passing tick of a sweep (0-based) uses
// ding n+1, capped at the highest available ding.
inline int countDingIndex(int passIndex, int available = 6) {
    if (available <= 0) return 0;
    return std::clamp(passIndex + 1, 1, available);
}

} // namespace fwl
