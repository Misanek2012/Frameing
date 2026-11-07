#include "Session.hpp"
#include "Settings.hpp"
#include "Sounds.hpp"

#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>

#include <cctype>
#include <cstring>
#include <limits>

using namespace geode::prelude;

namespace fwl::gd {

namespace {
constexpr Tick kFar = std::numeric_limits<Tick>::max() / 4;

std::string sgn(Tick t) { return (t > 0 ? "+" : "") + std::to_string(t); }

std::filesystem::path projectPath(Project const& p) {
    std::string name;
    for (char c : p.levelName) name += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    if (name.empty()) name = "level";
    return Mod::get()->getSaveDir() / "projects" / fmt::format("{}-{}.fwl", p.levelId, name);
}
} // namespace

Session& Session::get() {
    static Session s;
    return s;
}

void Session::setStatus(std::string s, bool error) {
    if (error) log::warn("{}", s);
    else log::info("{}", s);
    status = std::move(s);
    statusIsError = error;
}

bool Session::isLayer(GJBaseGameLayer const* l) const {
    return m_layer && l && static_cast<GJBaseGameLayer const*>(m_layer) == l;
}

Tick Session::tick() const {
    return m_layer ? static_cast<Tick>(m_layer->m_gameState.m_currentProgress) : 0;
}

double Session::stepSize() const {
    double warp = m_layer ? m_layer->m_gameState.m_timeWarp : 1.0;
    if (!(warp > 0.0)) warp = 1.0;
    return std::min(1.0, warp) / kTicksPerSecond;
}

// ---------------------------------------------------------------- lifecycle

void Session::attach(PlayLayer* pl) {
    m_layer = pl;
    m_run = {};
    m_job.reset();
    m_cursor.reset(nullptr);
    mode = Mode::Idle;
    paused = false;
    m_pendingSteps = 0;
    m_accum = 0;
    selected.reset();
    selectedCircle.reset();
    circleTool = false;
    progress = {};
    m_recordTrace.clear();
    m_recordedSeq.reset();
    m_baselineVerifiedFor.reset();
    history.clear();
    overlayVisible = cfg::flag("show-overlay");
    hitboxes = cfg::flag("hitboxes");
    diag = {};
    diag.phase = static_cast<InjectPhase>(Mod::get()->getSavedValue<int>("inject-phase", 0));
    diag.phaseCalibrated = Mod::get()->getSavedValue<bool>("inject-phase-calibrated", false);

    project = {};
    project.levelId = pl->m_level ? pl->m_level->m_levelID.value() : 0;
    project.levelName = pl->m_level ? std::string(pl->m_level->m_levelName) : "";
    if (std::filesystem::exists(projectPath(project))) load();
    else setStatus("F3 records an attempt. F1 hides this overlay.");
}

void Session::detach(PlayLayer* pl) {
    if (m_layer != pl) return;
    if (!project.sequence.empty() || !project.circles.circles().empty()) save();
    m_job.reset();
    m_run = {};
    m_cursor.reset(nullptr);
    mode = Mode::Idle;
    m_layer = nullptr;
}

void Session::requestReset() {
    Loader::get()->queueInMainThread([] {
        auto& s = Session::get();
        if (s.m_layer) s.m_layer->resetLevelFromStart();
    });
}

void Session::startRun(Run run, bool reset) {
    m_run = std::move(run);
    m_cursor.reset(&m_run.sequence);
    m_accum = 0;
    if (reset) requestReset();
}

void Session::onReset() {
    m_cursor.reset(&m_run.sequence);
    m_run.trace.clear();
    m_run.obs = {};
    m_run.finished = false;
    m_run.watchPos.reset();
    diag.steps = 0;
    diag.tickJumps = 0;
    diag.lateInputs = 0;
    diag.lockedThisRun = controlsTicks();
    m_prevStepTick = -1;
    m_accum = 0;
    breakLoop = true;

    Tick t0 = tick();
    if (mode == Mode::Recording && m_run.kind == RunKind::Record) {
        if (t0 != 0) {
            stopAll();
            setStatus("Recording needs a reset to tick 0. Disable practice checkpoints or restart from the start.", true);
            return;
        }
        if (m_recordArmed) {
            Tick from = m_run.injectUntil;
            history.apply(project.sequence, [from](InputSequence& s) {
                s.truncateFrom(from);
                return Status::success();
            });
            m_run.sequence = project.sequence; // prefix to replay (empty for a fresh recording)
            m_cursor.reset(&m_run.sequence);
            m_recordArmed = false;
            project.startTick = 0;
            project.measurements.clear(); // old measurements refer to a different attempt
        } else {
            // Manual restart while recording: start the attempt again from the same point.
            project.sequence.truncateFrom(m_run.injectUntil);
        }
        m_recordTrace.clear();
        selected.reset();
        return;
    }
    if ((mode == Mode::Testing || mode == Mode::Playback) && t0 != 0) {
        if (mode == Mode::Testing) {
            failJob("Level did not reset to tick 0 (practice checkpoint?). Tests always start from the level start.");
            return;
        }
        m_cursor.seek(t0);
        setStatus(fmt::format("Playback resumed from tick {} (checkpoint): earlier inputs skipped, not tick-exact.", t0),
                  true);
    }
}

bool Session::controlsTicks() const {
    if (!m_layer) return false;
    if (paused || m_pendingSteps > 0 || m_run.pauseAt) return true;
    switch (mode) {
        case Mode::Playback:
        case Mode::Testing: return true;
        case Mode::Recording: return cfg::flag("lock-ticks-recording");
        case Mode::Idle: return false;
    }
    return false;
}

int Session::ticksForFrame(float dt) {
    breakLoop = false;
    if (mode == Mode::Testing && (m_run.finished || m_run.kind == RunKind::None)) return 0;
    if (paused) {
        int n = m_pendingSteps;
        m_pendingSteps = 0;
        return n;
    }
    if (m_run.fast || m_run.pauseAt) return static_cast<int>(cfg::integer("test-ticks-per-frame"));
    double step = stepSize();
    m_accum += std::min<double>(dt, 0.25);
    int n = static_cast<int>(m_accum / step);
    m_accum -= n * step;
    if (n > 64) {
        n = 64;
        m_accum = 0;
    }
    return n;
}

// ---------------------------------------------------------------- per step

void Session::onBeforeStep() {
    if (!m_layer) return;
    ++diag.steps;
    Tick t = tick();
    if (m_prevStepTick >= 0 && t != m_prevStepTick + 1) ++diag.tickJumps;
    m_prevStepTick = t;
    if (diag.phase == InjectPhase::BeforeStep) fireDue();
}

void Session::onAfterStep() {
    if (!m_layer) return;
    if (diag.phase == InjectPhase::AfterStep) fireDue();
    recordStep();
    Tick t = tick();
    runEndTick = t;

    if (m_run.pauseAt && t >= *m_run.pauseAt) {
        m_run.pauseAt.reset();
        m_run.fast = false;
        paused = true;
        breakLoop = true;
        setStatus(fmt::format("At tick {}. '.' steps forward, ',' steps back.", t));
    }

    bool testRun = m_run.kind == RunKind::Calibrate || m_run.kind == RunKind::Baseline || m_run.kind == RunKind::Trial;
    if (testRun && !m_run.finished) {
        if (!m_run.goalComplete && t >= m_run.goalTick) {
            m_run.obs.lastTick = t;
            finishRun();
        } else if (t >= m_run.safetyLimit) {
            m_run.obs.lastTick = t;
            m_run.obs.aborted = true;
            finishRun();
        }
    }
}

void Session::fireDue() {
    bool inject = false;
    switch (m_run.kind) {
        case RunKind::Play:
        case RunKind::Calibrate:
        case RunKind::Baseline:
        case RunKind::Trial: inject = true; break;
        case RunKind::Record: inject = tick() < m_run.injectUntil; break;
        case RunKind::None: break;
    }
    if (!inject || !m_layer) return;
    m_cursor.fireUpTo(tick(), [&](InputEvent const& e) {
        if (e.id == m_run.watchId && m_layer->m_player1)
            m_run.watchPos = std::pair{m_layer->m_player1->getPositionX(), m_layer->m_player1->getPositionY()};
        injecting = true;
        m_layer->handleButton(e.down, static_cast<int>(e.button), !e.player2);
        injecting = false;
    });
    diag.lateInputs = m_cursor.lateCount();
}

std::uint64_t Session::hashState() const {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&](auto v) { h = fnv1a(&v, sizeof v, h); };
    for (PlayerObject* p : {m_layer->m_player1, m_layer->m_player2}) {
        if (!p) continue;
        mix(p->getPositionX());
        mix(p->getPositionY());
        mix(p->getRotation());
        mix(p->m_yVelocity);
        mix(p->m_vehicleSize);
        mix(p->m_playerSpeed);
        std::uint32_t flags = (p->m_isShip << 0) | (p->m_isBird << 1) | (p->m_isBall << 2) | (p->m_isDart << 3) |
                              (p->m_isRobot << 4) | (p->m_isSpider << 5) | (p->m_isSwing << 6) |
                              (p->m_isUpsideDown << 7) | (p->m_isOnGround << 8) | (p->m_isDead << 9) |
                              (p->m_isSideways << 10) | (p->m_isDashing << 11);
        mix(flags);
    }
    return h;
}

void Session::recordStep() {
    if (m_run.kind == RunKind::None) return;
    auto h = hashState();
    m_run.trace.push(tick(), h);
    if (m_run.kind == RunKind::Record) m_recordTrace.push(tick(), h);
}

bool Session::passLiveButton(bool down, int button, bool isPlayer1) {
    if (injecting || !m_layer) return true;
    if (mode == Mode::Playback || mode == Mode::Testing) return false;
    if (m_run.pauseAt) return false; // fast-forwarding a rewind
    if (mode == Mode::Recording && m_run.kind == RunKind::Record) {
        if (tick() < m_run.injectUntil) return false;
        if (button < 1 || button > 3) return true;
        auto id = project.sequence.add(tick(), static_cast<Button>(button), down, !isPlayer1);
        if (!project.sequence.validate()) {
            // GD delivered e.g. two presses in a row; the second has no effect on the
            // press/release state, so it is not stored. Calibration will flag any mismatch.
            project.sequence.remove(id);
        }
    }
    return true;
}

bool Session::onDeath(PlayerObject*, GameObject* object) {
    if (!m_layer || object == m_layer->m_anticheatSpike) return false;
    Tick t = tick();
    switch (m_run.kind) {
        case RunKind::Record:
            if (t < m_run.injectUntil) {
                stopAll();
                setStatus(fmt::format("Rewind replay died at tick {} before reaching tick {}: replay is not reproducing "
                                      "the recording.",
                                      t, m_run.injectUntil),
                          true);
                return false;
            }
            runEndTick = t;
            mode = Mode::Idle;
            m_run.kind = RunKind::None;
            m_recordedSeq = project.sequence;
            setStatus(fmt::format("Recorded {} inputs; died at tick {}. Select an input on the timeline.",
                                  project.sequence.size(), t));
            return false;
        case RunKind::Play:
            setStatus(fmt::format("Replay died at tick {}.", t), true);
            return false;
        case RunKind::Calibrate:
        case RunKind::Baseline:
        case RunKind::Trial:
            if (!m_run.finished) {
                m_run.obs.died = true;
                m_run.obs.deathTick = t;
                m_run.obs.lastTick = t;
                finishRun();
            }
            return true; // no death effect / respawn during measurement
        case RunKind::None: return false;
    }
    return false;
}

bool Session::onComplete() {
    if (!m_layer) return false;
    Tick t = tick();
    switch (m_run.kind) {
        case RunKind::Record:
            mode = Mode::Idle;
            m_run.kind = RunKind::None;
            m_recordedSeq = project.sequence;
            setStatus(fmt::format("Recorded {} inputs; level completed at tick {}.", project.sequence.size(), t));
            return false;
        case RunKind::Calibrate:
        case RunKind::Baseline:
        case RunKind::Trial:
            if (!m_run.finished) {
                m_run.obs.completed = true;
                m_run.obs.lastTick = t;
                finishRun();
            }
            return true;
        default: return false;
    }
}

void Session::finishRun() {
    m_run.finished = true;
    breakLoop = true;
    Loader::get()->queueInMainThread([] { Session::get().advanceJob(); });
}

// ---------------------------------------------------------------- actions

void Session::stopAll(char const* why) {
    if (m_job) {
        cancelTest(why ? why : "stopped");
        return;
    }
    if (mode == Mode::Recording) m_recordedSeq = project.sequence;
    mode = Mode::Idle;
    m_run = {};
    m_cursor.reset(nullptr);
    paused = false;
    m_pendingSteps = 0;
    if (why) setStatus(why);
}

void Session::toggleRecording() {
    if (!m_layer) return;
    if (mode == Mode::Recording) {
        stopAll();
        setStatus(fmt::format("Recording stopped: {} inputs.", project.sequence.size()));
        return;
    }
    if (mode == Mode::Testing) return setStatus("Finish or cancel the test first.", true);
    Run r;
    r.kind = RunKind::Record;
    r.injectUntil = 0;
    mode = Mode::Recording;
    m_recordArmed = true;
    paused = false;
    startRun(std::move(r), true);
    setStatus("Recording from the start. Play the attempt; dying or F3 stops it.");
}

void Session::togglePlayback() {
    if (!m_layer) return;
    if (mode == Mode::Playback) return stopAll("Playback stopped.");
    if (mode == Mode::Testing || mode == Mode::Recording) return setStatus("Stop the current mode first.", true);
    if (project.sequence.empty()) return setStatus("Nothing to replay: record an attempt first (F3).", true);
    auto r = makeReplayRun(project.sequence, RunKind::Play, 0, Goal{Goal::Kind::SurviveUntilTick, kFar});
    r.fast = false;
    mode = Mode::Playback;
    paused = false;
    startRun(std::move(r), true);
    setStatus("Replaying the edited inputs.");
}

void Session::togglePause() {
    if (!m_layer || mode == Mode::Testing) return;
    paused = !paused;
    m_pendingSteps = 0;
    m_accum = 0;
    setStatus(paused ? fmt::format("Paused at tick {}.", tick()) : "Resumed.");
}

void Session::stepForward(int n) {
    if (!m_layer || mode == Mode::Testing) return;
    if (!paused) {
        paused = true;
        setStatus(fmt::format("Paused at tick {}.", tick()));
        return;
    }
    m_pendingSteps += n;
    sounds::play(SoundEvent::Step);
}

void Session::stepBack(int n) {
    if (!m_layer || mode == Mode::Testing) return;
    Tick target = std::max<Tick>(0, tick() - n);
    if (mode == Mode::Recording) {
        // Rewind and re-record: replay inputs before `target`, then hand control back.
        Run r;
        r.kind = RunKind::Record;
        r.injectUntil = target;
        r.pauseAt = target;
        r.fast = true;
        m_recordArmed = true;
        paused = false;
        startRun(std::move(r), true);
        setStatus(fmt::format("Rewinding to tick {}; inputs from there will be re-recorded.", target));
        sounds::play(SoundEvent::Step);
        return;
    }
    if (project.sequence.empty()) return setStatus("Nothing to rewind: record an attempt first.", true);
    auto r = makeReplayRun(project.sequence, RunKind::Play, 0, Goal{Goal::Kind::SurviveUntilTick, kFar});
    r.pauseAt = target;
    r.fast = true;
    mode = Mode::Playback;
    paused = false;
    startRun(std::move(r), true);
    sounds::play(SoundEvent::Step);
}

void Session::select(std::optional<std::uint64_t> id, bool withSound) {
    if (id && !project.sequence.find(*id)) id.reset();
    if (id != selected && id && withSound) sounds::play(SoundEvent::Select);
    selected = id;
}

void Session::selectNeighbour(int dir) {
    if (project.sequence.empty()) return;
    if (!selected) {
        auto near = project.sequence.nearest(tick());
        return select(near);
    }
    if (auto n = project.sequence.neighbour(*selected, dir)) select(n);
}

void Session::shiftSelected(Tick delta) {
    if (!selected) return setStatus("Select an input first (click a marker or Up/Down).", true);
    if (mode == Mode::Testing || mode == Mode::Recording) return setStatus("Can't edit inputs while recording/testing.", true);
    auto id = *selected;
    auto st = history.apply(project.sequence, [&](InputSequence& s) { return s.shift(id, delta, shiftMode); });
    if (!st) return setStatus(st.error, true);
    sounds::play(SoundEvent::MoveInput);
    auto ev = project.sequence.find(id);
    std::string rel;
    if (auto ms = selectedMeasurement(); ms && !measurementStale(id)) rel = " (" + sgn(ev->tick - ms->anchor) + ")";
    setStatus(fmt::format("#{} {} -> tick {}{}{}", id, ev->down ? "press" : "release", ev->tick, rel,
                          shiftMode == ShiftMode::Ripple ? " [ripple]" : ""));
}

void Session::beginDrag() {
    if (mode == Mode::Testing || mode == Mode::Recording) return;
    m_dragOrigin = project.sequence;
}

void Session::dragTo(Tick t) {
    if (!m_dragOrigin || !selected) return;
    auto ev = project.sequence.find(*selected);
    if (!ev || ev->tick == t) return;
    if (project.sequence.setTick(*selected, t, shiftMode)) sounds::play(SoundEvent::MoveInput);
}

void Session::endDrag() {
    if (!m_dragOrigin) return;
    auto after = project.sequence;
    project.sequence = std::move(*m_dragOrigin);
    m_dragOrigin.reset();
    history.apply(project.sequence, [&](InputSequence& s) {
        s = after;
        return Status::success();
    });
    if (selected)
        if (auto ev = project.sequence.find(*selected))
            setStatus(fmt::format("#{} {} -> tick {}", *selected, ev->down ? "press" : "release", ev->tick));
}

void Session::deleteSelected() {
    if (selectedCircle) {
        project.circles.remove(*selectedCircle);
        selectedCircle.reset();
        return setStatus("Circle deleted.");
    }
    if (!selected || mode == Mode::Testing || mode == Mode::Recording) return;
    auto id = *selected;
    auto st = history.apply(project.sequence, [&](InputSequence& s) { return s.removeWithPartner(id); });
    if (!st) return setStatus(st.error, true);
    selected.reset();
    setStatus("Input (and its matching press/release) deleted. Ctrl+Z to undo.");
}

void Session::toggleShiftMode() {
    shiftMode = shiftMode == ShiftMode::Single ? ShiftMode::Ripple : ShiftMode::Single;
    setStatus(shiftMode == ShiftMode::Ripple ? "Ripple: moving an input also moves every later input."
                                             : "Single: moving an input leaves the others in place.");
}

void Session::undo() {
    if (mode == Mode::Testing || mode == Mode::Recording) return;
    setStatus(history.undo(project.sequence) ? "Undone." : "Nothing to undo.");
    select(selected, false);
}

void Session::redo() {
    if (mode == Mode::Testing || mode == Mode::Recording) return;
    setStatus(history.redo(project.sequence) ? "Redone." : "Nothing to redo.");
    select(selected, false);
}

void Session::save() {
    auto path = projectPath(project);
    auto dir = path.parent_path();
    if (auto r = utils::file::createDirectoryAll(dir); r.isErr())
        return setStatus("Save failed: " + r.unwrapErr(), true);
    if (std::filesystem::exists(path)) {
        std::error_code ec;
        std::filesystem::copy_file(path, path.string() + ".bak", std::filesystem::copy_options::overwrite_existing, ec);
    }
    if (auto r = utils::file::writeStringSafe(path, serializeProject(project)); r.isErr())
        return setStatus("Save failed: " + r.unwrapErr(), true);
    setStatus("Saved " + utils::string::pathToString(path.filename()));
}

void Session::load() {
    auto path = projectPath(project);
    auto text = utils::file::readString(path);
    if (text.isErr()) return setStatus("No saved project for this level.", true);
    auto parsed = parseProject(text.unwrap());
    if (!parsed) return setStatus("Load failed: " + parsed.error, true);
    if (mode != Mode::Idle) stopAll();
    project = std::move(parsed.value);
    history.clear();
    selected.reset();
    selectedCircle.reset();
    m_recordTrace.clear();
    m_recordedSeq.reset();
    m_baselineVerifiedFor.reset();
    setStatus(fmt::format("Loaded {} inputs, {} circles.", project.sequence.size(), project.circles.circles().size()));
}

void Session::setGoalTick(Tick t) {
    project.goal = {Goal::Kind::SurviveUntilTick, std::max<Tick>(t, 0)};
    project.hasGoal = true;
    setStatus(fmt::format("Goal: survive until tick {}.", project.goal.tick));
}

void Session::setGoalComplete() {
    project.goal = {Goal::Kind::CompleteLevel, 0};
    project.hasGoal = true;
    setStatus("Goal: complete the level.");
}

Goal Session::effectiveGoal() const {
    if (project.hasGoal) return project.goal;
    Tick base = project.sequence.lastTick();
    if (selected)
        if (auto ev = project.sequence.find(*selected)) base = ev->tick;
    return {Goal::Kind::SurviveUntilTick, base + cfg::integer("goal-default-ticks")};
}

MeasurementSet const* Session::selectedMeasurement() const {
    if (!selected) return nullptr;
    auto it = project.measurements.find(*selected);
    return it == project.measurements.end() ? nullptr : &it->second;
}

std::uint64_t Session::measurementSetContext(std::uint64_t id) const {
    auto it = project.measurements.find(id);
    if (it == project.measurements.end()) return 0;
    return measurementContext(project.sequence, id, it->second.mode, it->second.goal);
}

bool Session::measurementStale(std::uint64_t id) const {
    auto it = project.measurements.find(id);
    if (it == project.measurements.end()) return false;
    return measurementSetContext(id) != it->second.context;
}

void Session::generateCircles() {
    auto ms = selectedMeasurement();
    if (!ms) return setStatus("Measure the selected input first (F5 / F6).", true);
    CircleStyle style;
    style.radius = static_cast<float>(cfg::real("circle-gen-radius"));
    style.pass = cfg::toCore(cfg::rgba("color-pass"));
    style.pass.a = 255;
    style.fail = cfg::toCore(cfg::rgba("color-fail"));
    style.fail.a = 255;
    auto ids = project.circles.generateFromWindow(*selected, ms->report(), ms->offsetPositions(), style);
    sounds::play(SoundEvent::CircleCreate);
    setStatus(fmt::format("{} circles at the player's position for each tested timing.", ids.size()));
}

// ---------------------------------------------------------------- test engine

Session::Run Session::makeReplayRun(InputSequence const& seq, RunKind kind, std::uint64_t watch,
                                    Goal const& goal) const {
    Run r;
    r.kind = kind;
    r.sequence = seq;
    r.fast = true;
    r.watchId = watch;
    if (goal.kind == Goal::Kind::CompleteLevel) {
        r.goalComplete = true;
        r.goalTick = kFar;
        r.safetyLimit = std::max(seq.lastTick(), runEndTick) + 60 * kTicksPerSecond;
    } else {
        r.goalTick = goal.tick;
        r.safetyLimit = goal.tick + kTicksPerSecond;
    }
    return r;
}

void Session::testSelected() {
    if (!m_layer || !selected) return setStatus("Select an input first.", true);
    if (mode != Mode::Idle && mode != Mode::Playback) return setStatus("Stop recording/testing first.", true);
    if (project.startTick != 0) return setStatus("Recording did not start at tick 0; re-record from the start.", true);
    auto id = *selected;
    auto ev = project.sequence.find(id);
    Job job;
    job.inputId = id;
    job.mode = shiftMode;
    job.anchor = ev->tick;
    auto it = project.measurements.find(id);
    if (it != project.measurements.end() && !measurementStale(id) && it->second.mode == shiftMode) {
        job.anchor = it->second.anchor;
        job.goal = it->second.goal;
    } else {
        job.goal = project.hasGoal ? project.goal
                                   : Goal{Goal::Kind::SurviveUntilTick, ev->tick + cfg::integer("goal-default-ticks")};
    }
    job.base = project.sequence;
    if (!job.base.setTick(id, job.anchor, shiftMode)) {
        job.anchor = ev->tick;
        job.base = project.sequence;
    }
    job.manualOffset = ev->tick - job.anchor;
    startJob(std::move(job));
}

void Session::sweepSelected() {
    if (!m_layer || !selected) return setStatus("Select an input first.", true);
    if (mode != Mode::Idle && mode != Mode::Playback) return setStatus("Stop recording/testing first.", true);
    if (project.startTick != 0) return setStatus("Recording did not start at tick 0; re-record from the start.", true);
    auto id = *selected;
    auto ev = project.sequence.find(id);
    Job job;
    job.inputId = id;
    job.mode = shiftMode;
    job.anchor = ev->tick;
    job.goal = project.hasGoal ? project.goal
                               : Goal{Goal::Kind::SurviveUntilTick, ev->tick + cfg::integer("goal-default-ticks")};
    job.base = project.sequence;
    auto base = std::make_shared<InputSequence>(job.base);
    auto mode_ = shiftMode;
    SweepConfig sc{cfg::integer("sweep-radius"), cfg::integer("sweep-max-radius"), true};
    job.planner.emplace(sc, [base, id, mode_](Tick off) {
        auto copy = *base;
        return copy.shift(id, off, mode_).ok;
    });
    startJob(std::move(job));
}

void Session::startJob(Job job) {
    if (mode == Mode::Playback) stopAll();
    mode = Mode::Testing;
    paused = false;
    m_pendingSteps = 0;
    progress = {true, "starting", 0, job.inputId};
    bool canCalibrate = !diag.phaseCalibrated && m_recordedSeq && m_recordTrace.size() > 0;
    job.stage = canCalibrate ? 0 : 1;
    job.tryPhase = diag.phase;
    m_job = std::move(job);
    m_run = {};
    Loader::get()->queueInMainThread([] { Session::get().advanceJob(); });
}

void Session::cancelTest(char const* why) {
    if (!m_job) return;
    m_job.reset();
    m_run = {};
    mode = Mode::Idle;
    progress.active = false;
    setStatus(fmt::format("Test {}.", why));
    requestReset();
}

void Session::failJob(std::string why) {
    m_job.reset();
    m_run = {};
    mode = Mode::Idle;
    progress.active = false;
    setStatus(std::move(why), true);
    sounds::play(SoundEvent::TestFail);
    requestReset();
}

void Session::advanceJob() {
    if (!m_job || !m_layer) return;
    auto& j = *m_job;

    // ---- consume the run that just finished
    if (m_run.finished) {
        auto kind = m_run.kind;
        m_run.finished = false;
        m_run.kind = RunKind::None;
        if (kind == RunKind::Calibrate) {
            Tick end = m_recordTrace.entries().back().first + 1;
            auto div = firstDivergence(m_recordTrace, m_run.trace, end);
            if (!div) {
                diag.phase = j.tryPhase;
                diag.phaseCalibrated = true;
                Mod::get()->setSavedValue("inject-phase", static_cast<int>(diag.phase));
                Mod::get()->setSavedValue("inject-phase-calibrated", true);
                j.stage = 1;
            } else if (!j.triedOther) {
                j.triedOther = true;
                j.tryPhase = j.tryPhase == InjectPhase::BeforeStep ? InjectPhase::AfterStep : InjectPhase::BeforeStep;
                diag.phase = j.tryPhase;
            } else {
                return failJob(fmt::format(
                    "Replay does not reproduce the recorded attempt (differs at tick {} with both injection phases). "
                    "Re-record with 'Lock 1 physics tick per update while recording' enabled.",
                    *div));
            }
        } else if (kind == RunKind::Baseline) {
            if (j.stage == 1) {
                j.baseline = m_run.trace;
                j.baselineObs = m_run.obs;
                j.baselineObs.desynced = diag.lateInputs > 0;
                j.baselinePos = m_run.watchPos;
                std::uint64_t key = fnv1a(&j.anchor, sizeof j.anchor,
                                          measurementContext(j.base, j.inputId, j.mode, j.goal));
                j.stage = (m_baselineVerifiedFor && *m_baselineVerifiedFor == key) ? 3 : 2;
            } else {
                auto div = firstDivergence(j.baseline, m_run.trace, kFar);
                if (div)
                    return failJob(fmt::format("Replay is not deterministic: two identical replays differ at tick {}. "
                                               "No measurement can be trusted on this level/setup.",
                                               *div));
                m_baselineVerifiedFor =
                    fnv1a(&j.anchor, sizeof j.anchor, measurementContext(j.base, j.inputId, j.mode, j.goal));
                j.stage = 3;
            }
        } else if (kind == RunKind::Trial) {
            auto obs = m_run.obs;
            if (diag.lateInputs > 0) obs.desynced = true;
            if (firstDivergence(j.baseline, m_run.trace, m_run.firstChanged)) obs.desynced = true;
            auto out = classify(obs, j.goal);
            j.results[m_run.offset] = out;
            if (m_run.watchPos) j.positions[m_run.offset] = *m_run.watchPos;
            if (j.planner) j.planner->report(m_run.offset, out);
            ++progress.done;
            if (out == Outcome::Pass) {
                if (j.planner) sounds::count(j.passCount);
                else sounds::play(SoundEvent::TestPass);
                ++j.passCount;
            } else if (!j.planner) {
                sounds::play(SoundEvent::TestFail);
            }
        }
    }

    // ---- start the next run
    switch (j.stage) {
        case 0: {
            progress.phase = "calibrating replay";
            Tick end = m_recordTrace.entries().back().first;
            auto r = makeReplayRun(*m_recordedSeq, RunKind::Calibrate, 0, Goal{Goal::Kind::SurviveUntilTick, end});
            startRun(std::move(r), true);
            return;
        }
        case 1:
        case 2: {
            progress.phase = j.stage == 1 ? "baseline" : "determinism check";
            auto r = makeReplayRun(j.base, RunKind::Baseline, j.inputId, j.goal);
            startRun(std::move(r), true);
            return;
        }
        default: break;
    }

    auto baselineOutcome = classify(j.baselineObs, j.goal);
    while (true) {
        std::optional<Tick> off;
        if (j.planner) {
            off = j.planner->next();
        } else if (!j.manualDone) {
            off = j.manualOffset;
            j.manualDone = true;
        }
        if (!off) return finishJob();
        if (*off == 0) { // identical to the baseline run
            j.results[0] = baselineOutcome;
            if (j.baselinePos) j.positions[0] = *j.baselinePos;
            if (j.planner) j.planner->report(0, baselineOutcome);
            if (baselineOutcome == Outcome::Pass) {
                if (j.planner) sounds::count(j.passCount);
                else sounds::play(SoundEvent::TestPass);
                ++j.passCount;
            } else if (!j.planner) {
                sounds::play(SoundEvent::TestFail);
            }
            continue;
        }
        auto plan = makeTrial(j.base, j.inputId, *off, j.mode);
        if (!plan) {
            j.results[*off] = Outcome::Blocked;
            if (j.planner) j.planner->report(*off, Outcome::Blocked);
            continue;
        }
        auto r = makeReplayRun(plan.value.sequence, RunKind::Trial, j.inputId, j.goal);
        r.offset = *off;
        r.firstChanged = plan.value.firstChangedTick;
        progress.phase = "testing " + sgn(*off);
        j.currentOffset = *off;
        startRun(std::move(r), true);
        return;
    }
}

void Session::finishJob() {
    auto j = std::move(*m_job);
    m_job.reset();
    m_run = {};
    mode = Mode::Idle;
    progress.active = false;

    auto ctx = measurementContext(project.sequence, j.inputId, j.mode, j.goal);
    auto& ms = project.measurements[j.inputId];
    bool merge = ms.context == ctx && ms.anchor == j.anchor && ms.mode == j.mode && !ms.byTick.empty();
    if (!merge) ms = MeasurementSet{};
    ms.anchor = j.anchor;
    ms.mode = j.mode;
    ms.goal = j.goal;
    ms.context = ctx;
    for (auto const& [off, out] : j.results) ms.byTick[j.anchor + off] = out;
    for (auto const& [off, pos] : j.positions) ms.positions[j.anchor + off] = pos;

    auto report = ms.report();
    if (j.planner) {
        sounds::play(SoundEvent::WindowDone);
        setStatus("Window: " + report.summary());
        if (cfg::flag("auto-circles")) {
            selected = j.inputId;
            generateCircles();
            setStatus("Window: " + report.summary());
        }
    } else if (!j.results.empty()) {
        auto last = j.results.begin();
        setStatus(fmt::format("Offset {} -> {}. {}", sgn(last->first), outcomeName(last->second), report.summary()),
                  last->second != Outcome::Pass);
    }
    requestReset();
}

} // namespace fwl::gd
