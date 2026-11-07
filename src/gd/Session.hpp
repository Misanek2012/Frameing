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

#include <chrono>
#include <optional>
#include <string>

namespace fwl::gd {

enum class Mode { Idle, Recording, Playback, Testing };

// Whether recorded inputs are re-fired at the start or end of the physics
// step that carries their tick. Chosen automatically by calibration.
enum class InjectPhase : int { BeforeStep = 0, AfterStep = 1 };

using Pos = std::pair<float, float>;

class Session {
public:
    static Session& get();

    // ---------- hook entry points --------------------------------------
    void attach(PlayLayer* pl);
    void detach(PlayLayer* pl);
    PlayLayer* layer() const { return m_layer; }
    bool isLayer(GJBaseGameLayer const* l) const; // true if `l` is the attached PlayLayer

    void onReset();                       // after PlayLayer::resetLevel
    void onBeforeStep(float dt, bool halfTick, bool lastTick); // start of processCommands
    void onUpdateCall(float dt);          // every GJBaseGameLayer::update (diagnostics)
    void onAfterStep();                   // end of processCommands
    bool passLiveButton(bool down, int button, bool isPlayer1); // false = swallow
    bool onDeath(PlayerObject* player, GameObject* object);   // true = suppress original
    bool onComplete();                                        // true = suppress original

    bool controlsTicks() const;          // should update() be driven by us?
    int ticksForFrame(float dt);         // how many single-tick updates to run now
    bool fastRun() const { return m_run.fast || m_run.pauseAt.has_value(); }
    std::chrono::steady_clock::time_point frameDeadline() const;
    double stepSize() const;             // dt of one physics step
    bool inLockedUpdate = false;
    bool injecting = false;
    bool breakLoop = false;              // set when the single-tick update loop must stop early

    // ---------- user actions -------------------------------------------
    void toggleRecording();
    void togglePlayback();               // P: play / pause the replay
    void playFromReset();                // P: replay from the Reset point at showcase speed
    void toggleShowcase();               // S: slow motion for showcases
    void markEarliest();                 // E: selected input's current timing is the earliest that works
    void markLatest();                   // L: ... the latest that works
    void clearWindow();
    bool showcase = false;
    double playSpeed() const;
    void stopAll(char const* why = nullptr);
    void togglePause();
    void stepForward(int n = 1);
    void stepBack(int n = 1);
    void seekTo(Tick t);                 // replay to tick t and pause there

    void select(std::optional<std::uint64_t> id, bool withSound = true);
    void selectNeighbour(int dir);
    bool ensureSelection();              // auto-selects the input nearest the playhead
    void shiftSelected(Tick delta);
    void beginDrag();
    void dragTo(Tick tick);
    void endDrag();
    void deleteSelected();
    void addClickAtPlayhead();
    void toggleShiftMode();
    void undo();
    void redo();
    void save(bool quiet = false);
    void load();

    void setGoalTick(Tick t);
    void setGoalComplete();
    void clearGoal();
    Goal effectiveGoal() const;
    Goal goalFor(std::uint64_t id) const;

    void testSelected();   // manual mode: one trial at the input's current position
    void sweepSelected();  // automatic mode, selected input
    void analyzeAll();     // automatic mode, every press in order
    void cancelTest(char const* why = "cancelled");
    void refreshIndicators(bool notify = true);

    // ---------- state read by the UI -------------------------------------
    Project project;
    EditHistory history;
    std::optional<std::uint64_t> selected;
    ShiftMode shiftMode = ShiftMode::Single;
    Mode mode = Mode::Idle;
    bool paused = false;
    Tick tick() const;
    Tick runEndTick = 0;  // last tick reached by the most recent run
    std::optional<Tick> deathTick; // where the recorded attempt died
    Tick resetTick = -1;  // last rewind / checkpoint point ("Reset @")
    std::optional<std::uint64_t> selectedCircle;
    bool circleTool = false;
    bool overlayVisible = true;
    bool hitboxes = true;
    int inputsBefore(Tick t) const; // NaN-style "Input:" counter

    // Player path of the last record / replay (one point per tick) and where
    // each input fired. Drawn in level space.
    std::vector<Pos> path;
    Tick pathStart = 0;
    std::map<std::uint64_t, Pos> inputPos;
    std::map<std::uint64_t, std::chrono::steady_clock::time_point> markerSpawn; // showcase pop animation
    bool showcasing() const { return mode == Mode::Playback && !paused; }
    int pathVersion = 0;
    int indicatorVersion = 0;

    std::string status;
    bool statusIsError = false;
    void setStatus(std::string s, bool error = false);
    void notify(std::string const& s, bool error = false);

    struct TestProgress {
        bool active = false;
        std::string phase;   // "calibrating", "baseline", "testing +3", ...
        int done = 0;
        std::uint64_t inputId = 0;
        int queueLeft = 0;
    } progress;

    struct Diagnostics {
        std::int64_t steps = 0;          // processCommands calls since reset
        std::int64_t tickJumps = 0;      // steps where the tick advanced by != 1
        std::size_t lateInputs = 0;      // inputs injected after their tick
        InjectPhase phase = InjectPhase::BeforeStep;
        bool phaseCalibrated = false;
        bool lockedThisRun = false;
    } diag;
    std::string conflictWarning;

    // Measurement for the selected input, if any (nullptr otherwise).
    MeasurementSet const* selectedMeasurement() const;
    MeasurementSet const* measurementFor(std::uint64_t id) const;
    bool measurementStale(std::uint64_t id) const;
    std::uint64_t measurementSetContext(std::uint64_t id) const;

private:
    Session() = default;

    // ----- runs ---------------------------------------------------------
    enum class RunKind { None, Record, Play, Calibrate, Baseline, Trial };
    struct Run {
        RunKind kind = RunKind::None;
        InputSequence sequence;          // what is injected
        Tick injectUntil = 0;            // Record: inject below this tick, record live input from it
        std::optional<Tick> pauseAt;     // seek target
        bool fast = false;               // run at test speed
        bool resumeAfterSeek = false;    // pauseAt: keep playing at normal speed instead of pausing
        TrialObservation obs;
        StateTrace trace;
        bool finished = false;
        std::uint64_t watchId = 0;       // input whose firing position is captured
        std::optional<Pos> watchPos;
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
    std::optional<Pos> playerPos(bool p2 = false) const;
    void resimAfterEdit();
    bool editable(bool quiet = false);
    void setMusicPaused(bool p);

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
        std::optional<Pos> baselinePos;
        std::map<Tick, Outcome> results;     // offset -> outcome
        std::map<Tick, Pos> positions;
        int passCount = 0;
        Tick currentOffset = 0;
    };
    std::optional<Job> m_job;
    std::vector<std::uint64_t> m_queue; // analyze-all
    bool m_batch = false;
    std::uint64_t m_pathSeqHash = 0;
    bool startSweep(std::uint64_t id, bool quiet);
    void startJob(Job job);
    void advanceJob();
    void finishJob();
    void failJob(std::string why);
    Run makeReplayRun(InputSequence const& seq, RunKind kind, std::uint64_t watch, Goal const& goal) const;
    bool canTest();

    // ----- recording ------------------------------------------------------
    StateTrace m_recordTrace;  // trace of the recorded attempt (for calibration)
    std::optional<InputSequence> m_recordedSeq; // the inputs that produced m_recordTrace
    bool m_recordArmed = false;
    bool m_held[2][4]{};       // live press state per player/button while recording
    std::optional<InputSequence> m_dragOrigin;
    std::optional<std::uint64_t> m_baselineVerifiedFor;

    PlayLayer* m_layer = nullptr;
    struct TimingProbe {
        std::chrono::steady_clock::time_point start{};
        std::int64_t updates = 0, commands = 0, halfTicks = 0, lastTicks = 0;
        Tick progress0 = 0;
        double levelTime0 = 0, stepDtSum = 0;
    } m_probe;
    double m_accum = 0;
    int m_pendingSteps = 0;
    Tick m_prevStepTick = -1;
    bool m_musicPaused = false;
    std::chrono::steady_clock::time_point m_frameStart;
};

} // namespace fwl::gd
