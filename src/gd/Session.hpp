// Frame Window Lab — session state shared by hooks and UI.
//
// Tick model (see README "Tick model"):
//  * The authoritative tick is GJGameState::m_currentProgress, read when an
//    input is recorded / injected.
//  * While the mod controls time ("locked" mode) every GJBaseGameLayer::update
//    call runs exactly ONE physics step: getModifiedDelta is forced to a
//    single step and update() is called N times per rendered frame. This makes
//    replays independent of rendering FPS.
#pragma once

#include "../core/Playback.hpp"
#include "../core/Project.hpp"

#include <Geode/Geode.hpp>

#include <optional>
#include <string>

namespace fwl::gd {

enum class Mode { Idle, Recording, Playback, Testing };

// Whether recorded inputs are re-fired at the start or end of the physics
// step that carries their tick. Chosen automatically by calibration.
enum class InjectPhase : int { BeforeStep = 0, AfterStep = 1 };

class Session {
public:
    static Session& get();

    // ---------- hook entry points --------------------------------------
    void attach(PlayLayer* pl);
    void detach(PlayLayer* pl);
    PlayLayer* layer() const { return m_layer; }
    bool isLayer(GJBaseGameLayer const* l) const; // true if `l` is the attached PlayLayer

    void onReset();                       // after PlayLayer::resetLevel
    void onBeforeStep();                  // start of processCommands
    void onAfterStep();                   // end of processCommands
    bool passLiveButton(bool down, int button, bool isPlayer1); // false = swallow
    bool onDeath(PlayerObject* player, GameObject* object);   // true = suppress original
    bool onComplete();                                        // true = suppress original

    bool controlsTicks() const;          // should update() be driven by us?
    int ticksForFrame(float dt);         // how many single-tick updates to run now
    double stepSize() const;             // dt of one physics step
    bool inLockedUpdate = false;
    bool injecting = false;
    bool breakLoop = false;              // set when the single-tick update loop must stop early

    // ---------- user actions -------------------------------------------
    void toggleRecording();
    void togglePlayback();
    void stopAll(char const* why = nullptr);
    void togglePause();
    void stepForward(int n = 1);
    void stepBack(int n = 1);

    void select(std::optional<std::uint64_t> id, bool withSound = true);
    void selectNeighbour(int dir);
    void shiftSelected(Tick delta);
    void beginDrag();
    void dragTo(Tick tick);
    void endDrag();
    void deleteSelected();
    void toggleShiftMode();
    void undo();
    void redo();
    void save();
    void load();

    void setGoalTick(Tick t);
    void setGoalComplete();
    Goal effectiveGoal() const;

    void testSelected();   // manual mode: one trial at the input's current position
    void sweepSelected();  // automatic mode
    void cancelTest(char const* why = "cancelled");
    void generateCircles();

    // ---------- state read by the UI -------------------------------------
    Project project;
    EditHistory history;
    std::optional<std::uint64_t> selected;
    ShiftMode shiftMode = ShiftMode::Single;
    Mode mode = Mode::Idle;
    bool paused = false;
    Tick tick() const;
    Tick runEndTick = 0; // last tick reached by the most recent run
    std::optional<std::uint64_t> selectedCircle;
    bool circleTool = false;
    bool followPlayhead = true;
    bool overlayVisible = true;
    bool hitboxes = true;

    std::string status;
    bool statusIsError = false;
    void setStatus(std::string s, bool error = false);

    struct TestProgress {
        bool active = false;
        std::string phase;   // "calibrating", "baseline", "testing +3", ...
        int done = 0;
        std::uint64_t inputId = 0;
    } progress;

    struct Diagnostics {
        std::int64_t steps = 0;          // processCommands calls since reset
        std::int64_t tickJumps = 0;      // steps where the tick advanced by != 1
        std::size_t lateInputs = 0;      // inputs injected after their tick
        InjectPhase phase = InjectPhase::BeforeStep;
        bool phaseCalibrated = false;
        bool lockedThisRun = false;
    } diag;

    // Measurement for the selected input, if any (nullptr otherwise).
    MeasurementSet const* selectedMeasurement() const;
    bool measurementStale(std::uint64_t id) const;
    std::uint64_t measurementSetContext(std::uint64_t id) const;
    bool recordingTraceAvailable() const { return m_recordTrace.size() > 0; }

private:
    Session() = default;

    // ----- runs ---------------------------------------------------------
    enum class RunKind { None, Record, Play, Calibrate, Baseline, Trial };
    struct Run {
        RunKind kind = RunKind::None;
        InputSequence sequence;          // what is injected
        Tick injectUntil = 0;            // Record: inject below this tick, record live input from it
        std::optional<Tick> pauseAt;     // rewind target
        bool fast = false;               // run at test speed
        TrialObservation obs;
        StateTrace trace;
        bool finished = false;
        std::uint64_t watchId = 0;       // input whose firing position is captured
        std::optional<std::pair<float, float>> watchPos;
        Tick goalTick = 0;
        bool goalComplete = false;
        Tick safetyLimit = 0;
        Tick offset = 0;
        Tick firstChanged = 0;
    } m_run;
    PlaybackCursor m_cursor;

    void startRun(Run run, bool reset);
    void finishRun();
    void fireDue();
    void recordStep();
    std::uint64_t hashState() const;
    void requestReset();
    Tick lastTickOrZero() const { return project.sequence.lastTick(); }

    // ----- test engine --------------------------------------------------
    struct Job {
        std::uint64_t inputId = 0;
        Tick anchor = 0;
        ShiftMode mode = ShiftMode::Single;
        Goal goal;
        InputSequence base;                 // input placed at anchor
        std::optional<SweepPlanner> planner;// automatic mode
        std::optional<Tick> manualOffset;   // manual mode
        bool manualDone = false;
        int stage = 0;                      // 0 calibrate, 1 baseline A, 2 baseline B, 3 trials
        InjectPhase tryPhase = InjectPhase::BeforeStep;
        bool triedOther = false;
        StateTrace baseline;
        TrialObservation baselineObs;
        std::optional<std::pair<float, float>> baselinePos;
        std::map<Tick, Outcome> results;     // offset -> outcome
        std::map<Tick, std::pair<float, float>> positions;
        int passCount = 0;
        Tick currentOffset = 0;
    };
    std::optional<Job> m_job;
    void startJob(Job job);
    void advanceJob();
    void runTrial(Tick offset);
    void finishJob();
    void failJob(std::string why);
    Run makeReplayRun(InputSequence const& seq, RunKind kind, std::uint64_t watch, Goal const& goal) const;

    // ----- recording ------------------------------------------------------
    StateTrace m_recordTrace;  // trace of the recorded attempt (for calibration)
    std::optional<InputSequence> m_recordedSeq; // the inputs that produced m_recordTrace
    bool m_recordArmed = false;
    std::optional<InputSequence> m_dragOrigin;
    std::optional<std::uint64_t> m_baselineVerifiedFor;

    PlayLayer* m_layer = nullptr;
    double m_accum = 0;
    int m_pendingSteps = 0;
    Tick m_prevStepTick = -1;
};

} // namespace fwl::gd
