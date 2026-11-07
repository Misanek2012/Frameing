#include "Session.hpp"
#include "Settings.hpp"
#include "Sounds.hpp"

#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/ui/Notification.hpp>

#include <algorithm>
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

std::uint64_t sequenceHash(InputSequence const& seq) {
    std::uint64_t h = 1469598103934665603ull;
    for (auto const& e : seq.events()) {
        h = fnv1a(&e.tick, sizeof e.tick, h);
        std::uint8_t b[3] = {static_cast<std::uint8_t>(e.button), e.down, e.player2};
        h = fnv1a(b, sizeof b, h);
    }
    return h;
}

StateTrace truncatedTrace(StateTrace const& t, Tick before) {
    StateTrace out;
    for (auto const& [tick, h] : t.entries())
        if (tick < before) out.push(tick, h);
    return out;
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

void Session::notify(std::string const& s, bool error) {
    setStatus(s, error);
    Notification::create(s, error ? NotificationIcon::Error : NotificationIcon::Success, 1.6f)->show();
}

bool Session::isLayer(GJBaseGameLayer const* l) const {
    return m_layer && l && static_cast<GJBaseGameLayer const*>(m_layer) == l;
}

Tick Session::tick() const {
    return m_layer ? static_cast<Tick>(m_layer->m_gameState.m_currentProgress) : 0;
}

int Session::inputsBefore(Tick t) const {
    auto const& ev = project.sequence.events();
    auto it = std::upper_bound(ev.begin(), ev.end(), t, [](Tick v, InputEvent const& e) { return v < e.tick; });
    return static_cast<int>(it - ev.begin());
}

double Session::stepSize() const {
    double warp = m_layer ? m_layer->m_gameState.m_timeWarp : 1.0;
    if (!(warp > 0.0)) warp = 1.0;
    return std::min(1.0, warp) / kTicksPerSecond;
}

std::optional<Pos> Session::playerPos(bool p2) const {
    if (!m_layer) return std::nullopt;
    auto p = p2 ? m_layer->m_player2 : m_layer->m_player1;
    if (!p) return std::nullopt;
    return Pos{p->getPositionX(), p->getPositionY()};
}

void Session::setMusicPaused(bool p) {
    if (p == m_musicPaused) return;
    m_musicPaused = p;
    auto fmod = FMODAudioEngine::get();
    if (!fmod) return;
    if (p) fmod->pauseAllMusic(true);
    else fmod->resumeAllMusic();
}

// ---------------------------------------------------------------- lifecycle

void Session::attach(PlayLayer* pl) {
    m_layer = pl;
    m_run = {};
    m_job.reset();
    m_queue.clear();
    m_cursor.reset(nullptr);
    mode = Mode::Idle;
    paused = false;
    m_pendingSteps = 0;
    m_accum = 0;
    m_musicPaused = false;
    selected.reset();
    selectedCircle.reset();
    circleTool = false;
    progress = {};
    m_recordTrace.clear();
    m_recordedSeq.reset();
    m_baselineVerifiedFor.reset();
    std::memset(m_held, 0, sizeof m_held);
    history.clear();
    path.clear();
    inputPos.clear();
    ++pathVersion;
    ++indicatorVersion;
    deathTick.reset();
    resetTick = -1;
    overlayVisible = cfg::flag("show-overlay");
    hitboxes = cfg::flag("hitboxes");
    diag = {};
    diag.phase = static_cast<InjectPhase>(Mod::get()->getSavedValue<int>("inject-phase", 0));
    diag.phaseCalibrated = Mod::get()->getSavedValue<bool>("inject-phase-calibrated", false);

    conflictWarning.clear();
    if (auto cbf = Loader::get()->getLoadedMod("syzzi.click_between_frames");
        cbf && !cbf->getSettingValue<bool>("soft-toggle"))
        conflictWarning = "Click Between Frames is enabled: it moves clicks between physics ticks, so recordings "
                          "may not replay exactly. Disable it while measuring.";

    project = {};
    project.levelId = pl->m_level ? pl->m_level->m_levelID.value() : 0;
    project.levelName = pl->m_level ? std::string(pl->m_level->m_levelName) : "";
    if (std::filesystem::exists(projectPath(project))) load();
    else setStatus("R / F3 records an attempt. F1 hides the timeline.");
    if (!conflictWarning.empty()) setStatus(conflictWarning, true);
}

void Session::detach(PlayLayer* pl) {
    if (m_layer != pl) return;
    if (!project.sequence.empty() || !project.circles.circles().empty()) save(true);
    setMusicPaused(false);
    m_job.reset();
    m_queue.clear();
    m_run = {};
    m_cursor.reset(nullptr);
    mode = Mode::Idle;
    paused = false;
    m_layer = nullptr;
}

void Session::requestReset() {
    Loader::get()->queueInMainThread([] {
        auto& s = Session::get();
        if (!s.m_layer) return;
        if (s.m_layer->m_isPracticeMode) s.m_layer->removeAllCheckpoints();
        s.m_layer->resetLevelFromStart();
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
    bool tracksPath = m_run.kind == RunKind::Play;

    if (mode == Mode::Recording && m_run.kind == RunKind::Record) {
        if (t0 != 0) {
            // Practice-mode checkpoint: keep everything before it, re-record from here.
            history.apply(project.sequence, [t0](InputSequence& s) {
                s.truncateFrom(t0);
                return Status::success();
            });
            for (int p = 0; p < 2; ++p)
                for (int b = 1; b <= 3; ++b) {
                    // A press still held at the checkpoint is released there.
                    InputEvent const* last = nullptr;
                    for (auto const& e : project.sequence.events())
                        if (static_cast<int>(e.button) == b && e.player2 == (p == 1)) last = &e;
                    if (last && last->down) project.sequence.add(t0, static_cast<Button>(b), false, p == 1);
                }
            std::memset(m_held, 0, sizeof m_held);
            m_recordTrace = truncatedTrace(m_recordTrace, t0);
            if (static_cast<Tick>(path.size()) > t0 - pathStart && t0 >= pathStart) path.resize(t0 - pathStart);
            ++pathVersion;
            resetTick = t0;
            m_run.injectUntil = std::min(m_run.injectUntil, t0);
            setStatus(fmt::format("Checkpoint at frame {}: inputs after it were discarded, keep playing.", t0));
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
            for (auto it = project.windows.begin(); it != project.windows.end();)
                it = project.sequence.find(it->first) ? std::next(it) : project.windows.erase(it);
            ++indicatorVersion;
            deathTick.reset();
        } else {
            // Manual restart while recording: start the attempt again from the same point.
            project.sequence.truncateFrom(m_run.injectUntil);
        }
        std::memset(m_held, 0, sizeof m_held);
        // A prefix that is replayed counts as already pressed/released.
        for (auto const& e : project.sequence.events())
            m_held[e.player2 ? 1 : 0][static_cast<int>(e.button) & 3] = e.down;
        m_recordTrace.clear();
        path.clear();
        pathStart = 0;
        m_pathSeqHash = 0;
        ++pathVersion;
        return;
    }
    if ((mode == Mode::Testing || mode == Mode::Playback) && t0 != 0) {
        if (mode == Mode::Testing) {
            failJob("The level did not restart at frame 0 (practice checkpoint?). Tests always start from the beginning.");
            return;
        }
        m_cursor.seek(t0);
        setStatus(fmt::format("Replay resumed from frame {} (checkpoint): earlier inputs skipped, not frame-exact.", t0),
                  true);
    }
    if (tracksPath) {
        // Same inputs as the drawn path: keep it (points are overwritten identically),
        // so the full trajectory stays visible ahead of the playhead.
        auto h = sequenceHash(m_run.sequence);
        if (h != m_pathSeqHash || pathStart != t0) {
            path.clear();
            pathStart = t0;
            m_pathSeqHash = h;
            ++pathVersion;
        }
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

std::chrono::steady_clock::time_point Session::frameDeadline() const {
    auto ms = std::clamp<std::int64_t>(cfg::integer("replay-budget-ms"), 2, 100);
    return m_frameStart + std::chrono::milliseconds(ms);
}

int Session::ticksForFrame(float dt) {
    breakLoop = false;
    m_frameStart = std::chrono::steady_clock::now();
    bool testing = mode == Mode::Testing;
    setMusicPaused(paused || fastRun() || testing || (mode == Mode::Playback && playSpeed() != 1.0));
    if (testing && (m_run.finished || m_run.kind == RunKind::None)) return 0;
    if (paused) {
        int n = m_pendingSteps;
        m_pendingSteps = 0;
        return n;
    }
    if (fastRun()) return 1'000'000; // bounded by frameDeadline()
    double step = stepSize();
    m_accum += std::min<double>(dt, 0.25) * (mode == Mode::Playback ? playSpeed() : 1.0);
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

    if (m_run.kind == RunKind::Record || m_run.kind == RunKind::Play) {
        if (auto p = playerPos()) {
            Tick idx = t - pathStart;
            if (idx >= 0 && idx < static_cast<Tick>(path.size())) {
                if (path[idx] != *p) { // diverged from the drawn path
                    path.resize(idx);
                    path.push_back(*p);
                    ++pathVersion;
                }
            } else if (idx == static_cast<Tick>(path.size())) {
                path.push_back(*p);
            }
        }
    }

    if (m_run.pauseAt && t >= *m_run.pauseAt) {
        m_run.pauseAt.reset();
        m_run.fast = false;
        breakLoop = true;
        m_accum = 0;
        if (m_run.resumeAfterSeek) {
            m_run.resumeAfterSeek = false;
        } else {
            paused = true;
            setStatus(fmt::format("Frame {}. '.' / '>' steps forward, ',' / '<' steps back.", t));
        }
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
    bool keepPos = m_run.kind == RunKind::Play || m_run.kind == RunKind::Record;
    m_cursor.fireUpTo(tick(), [&](InputEvent const& e) {
        auto pos = playerPos(e.player2);
        if (e.id == m_run.watchId && pos) m_run.watchPos = pos;
        if (keepPos && pos) inputPos[e.id] = *pos;
        // NaN's showcase: a click with a marked window pops its circle and plays
        // the ding for its window size (1 frame = ding 1 ... 6+ frames = ding 6).
        if (m_run.kind == RunKind::Play && !fastRun()) {
            if (auto w = project.windows.find(e.id); w != project.windows.end() && w->second.complete()) {
                markerSpawn[e.id] = std::chrono::steady_clock::now();
                if (cfg::flag("window-dings")) sounds::ding(static_cast<int>(std::clamp<Tick>(w->second.frames(), 1, 6)));
            }
        }
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
        bool& held = m_held[isPlayer1 ? 0 : 1][button];
        // A release without a press (e.g. the click that started the attempt) or
        // a second press while held changes nothing in the game: don't record it.
        if (held == down) return true;
        held = down;
        auto id = project.sequence.add(tick(), static_cast<Button>(button), down, !isPlayer1);
        if (!project.sequence.validate()) project.sequence.remove(id);
        else if (auto p = playerPos(!isPlayer1)) inputPos[id] = *p;
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
                setStatus(fmt::format("The rewind replay died at frame {} before reaching frame {}: the replay does "
                                      "not reproduce the recording.",
                                      t, m_run.injectUntil),
                          true);
                return false;
            }
            if (m_layer->m_isPracticeMode) {
                setStatus(fmt::format("Died at frame {}. Respawning at the checkpoint; keep recording.", t));
                return false;
            }
            runEndTick = t;
            deathTick = t;
            mode = Mode::Idle;
            m_run.kind = RunKind::None;
            m_recordedSeq = project.sequence;
            if (!selected) ensureSelection();
            save(true);
            setStatus(fmt::format("Recorded {} inputs, died at frame {}. A analyzes every input.",
                                  project.sequence.size(), t));
            return false;
        case RunKind::Play:
            mode = Mode::Idle;
            m_run = {};
            m_cursor.reset(nullptr);
            setStatus(fmt::format("The replay died at frame {}.", t), true);
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
            deathTick.reset();
            save(true);
            setStatus(fmt::format("Recorded {} inputs; level completed at frame {}.", project.sequence.size(), t));
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
        save(true);
        setStatus(fmt::format("Recording stopped: {} inputs.", project.sequence.size()));
        return;
    }
    if (mode == Mode::Testing) return setStatus("Finish or stop the analysis first (F10).", true);
    Run r;
    r.kind = RunKind::Record;
    r.injectUntil = 0;
    mode = Mode::Recording;
    m_recordArmed = true;
    paused = false;
    resetTick = -1;
    startRun(std::move(r), true);
    setStatus("Recording from the start. Play; dying or R stops it.");
}

void Session::togglePlayback() {
    if (!m_layer) return;
    if (mode == Mode::Testing) return setStatus("Analysis running (F10 stops it).", true);
    if (mode == Mode::Playback) return togglePause();
    if (mode == Mode::Recording) stopAll();
    auto r = makeReplayRun(project.sequence, RunKind::Play, 0, Goal{Goal::Kind::SurviveUntilTick, kFar});
    r.fast = false;
    mode = Mode::Playback;
    paused = false;
    resetTick = 0;
    startRun(std::move(r), true);
    setStatus(project.sequence.empty() ? "Replaying (no inputs recorded yet)." : "Replaying the inputs.");
}

double Session::playSpeed() const {
    return showcase ? std::clamp(cfg::real("showcase-speed"), 0.01, 1.0) : 1.0;
}

void Session::toggleShowcase() {
    showcase = !showcase;
    setStatus(showcase ? fmt::format("Showcase speed {}x: replays run in slow motion; windows ding frame by frame.",
                                     playSpeed())
                       : "Normal speed.");
}

void Session::playFromReset() {
    if (!m_layer) return;
    if (mode == Mode::Testing) return setStatus("Analysis running (F10 stops it).", true);
    if (mode == Mode::Playback && !paused) return togglePause();
    if (mode == Mode::Recording) stopAll();
    Tick from = std::max<Tick>(0, resetTick);
    auto r = makeReplayRun(project.sequence, RunKind::Play, 0, Goal{Goal::Kind::SurviveUntilTick, kFar});
    r.fast = false;
    if (from > 0) {
        r.pauseAt = from;
        r.fast = true;
        r.resumeAfterSeek = true;
    }
    mode = Mode::Playback;
    paused = false;
    startRun(std::move(r), true);
    setStatus(from > 0 ? fmt::format("Replaying from Reset @ {}.", from) : "Replaying from the start.");
}

void Session::markEarliest() {
    if (!ensureSelection()) return;
    auto ev = project.sequence.find(*selected);
    auto& w = project.windows[*selected];
    w.earliest = ev->tick;
    if (w.latest && *w.latest < ev->tick) w.latest.reset();
    ++indicatorVersion;
    sounds::ding(1);
    auto idx = *project.sequence.indexOf(*selected) + 1;
    setStatus(w.complete() ? fmt::format("Input {}: {}-frame window ({} .. {}).", idx, w.frames(), *w.earliest, *w.latest)
                           : fmt::format("Input {}: earliest working frame {}. Now find the latest and press L.", idx,
                                         ev->tick));
}

void Session::markLatest() {
    if (!ensureSelection()) return;
    auto ev = project.sequence.find(*selected);
    auto& w = project.windows[*selected];
    w.latest = ev->tick;
    if (w.earliest && *w.earliest > ev->tick) w.earliest.reset();
    ++indicatorVersion;
    sounds::ding(static_cast<int>(std::clamp<Tick>(w.frames(), 1, 6)));
    auto idx = *project.sequence.indexOf(*selected) + 1;
    setStatus(w.complete() ? fmt::format("Input {}: {}-frame window ({} .. {}).", idx, w.frames(), *w.earliest, *w.latest)
                           : fmt::format("Input {}: latest working frame {}. Now find the earliest and press E.", idx,
                                         ev->tick));
}

void Session::clearWindow() {
    if (!selected) return;
    project.windows.erase(*selected);
    ++indicatorVersion;
    setStatus("Window cleared for the selected input.");
}

void Session::togglePause() {
    if (!m_layer || mode == Mode::Testing) return;
    paused = !paused;
    m_pendingSteps = 0;
    m_accum = 0;
    setStatus(paused ? fmt::format("Paused at frame {}.", tick()) : "Resumed.");
}

void Session::stepForward(int n) {
    if (!m_layer || mode == Mode::Testing) return;
    if (!paused) {
        paused = true;
        setStatus(fmt::format("Paused at frame {}.", tick()));
        return;
    }
    m_pendingSteps += n;
    sounds::play(SoundEvent::Step);
}

void Session::stepBack(int n) {
    if (!m_layer || mode == Mode::Testing) return;
    seekTo(std::max<Tick>(0, tick() - n));
    sounds::play(SoundEvent::Step);
}

void Session::seekTo(Tick target) {
    if (!m_layer || mode == Mode::Testing) return;
    target = std::max<Tick>(0, target);
    if (mode == Mode::Recording) {
        if (target >= tick()) return setStatus("While recording you can only go back.", true);
        // Rewind and re-record: replay inputs before `target`, then hand control back.
        Run r;
        r.kind = RunKind::Record;
        r.injectUntil = target;
        r.pauseAt = target;
        r.fast = true;
        m_recordArmed = true;
        paused = false;
        resetTick = target;
        startRun(std::move(r), true);
        setStatus(fmt::format("Rewinding to frame {}; inputs from there will be re-recorded.", target));
        return;
    }
    if (mode == Mode::Playback && target > tick()) { // fast-forward the running replay
        m_run.pauseAt = target;
        paused = false;
        return;
    }
    if (mode == Mode::Playback && target == tick() && paused) return;
    auto r = makeReplayRun(project.sequence, RunKind::Play, 0, Goal{Goal::Kind::SurviveUntilTick, kFar});
    r.pauseAt = target;
    r.fast = true;
    mode = Mode::Playback;
    paused = false;
    resetTick = target;
    startRun(std::move(r), true);
}

void Session::resimAfterEdit() {
    if (!cfg::flag("resim-on-edit")) return;
    if (mode == Mode::Playback && paused) {
        Tick t = tick();
        Loader::get()->queueInMainThread([t] {
            auto& s = Session::get();
            if (s.mode == Mode::Playback && s.paused) s.seekTo(t);
        });
    }
}

bool Session::editable(bool quiet) {
    if (mode == Mode::Testing || mode == Mode::Recording) {
        if (!quiet) setStatus("Inputs can't be edited while recording or analyzing.", true);
        return false;
    }
    return true;
}

void Session::select(std::optional<std::uint64_t> id, bool withSound) {
    if (id && !project.sequence.find(*id)) id.reset();
    if (id != selected && id && withSound) sounds::play(SoundEvent::Select);
    selected = id;
    if (id)
        if (auto idx = project.sequence.indexOf(*id))
            if (auto e = project.sequence.find(*id))
                setStatus(fmt::format("{}: {} {} P{} @ {}", *idx + 1, e->down ? "+" : "-",
                                      e->button == Button::Jump ? "Jump" : e->button == Button::Left ? "Left" : "Right",
                                      e->player2 ? 2 : 1, e->tick));
}

void Session::selectNeighbour(int dir) {
    if (project.sequence.empty()) return;
    if (!selected) {
        ensureSelection();
        return;
    }
    if (auto n = project.sequence.neighbour(*selected, dir)) select(n);
}

bool Session::ensureSelection() {
    if (selected && project.sequence.find(*selected)) return true;
    if (project.sequence.empty()) {
        setStatus("No inputs yet: record an attempt first (R / F3).", true);
        return false;
    }
    // The last press at or before the playhead, else the nearest input.
    Tick t = tick();
    std::optional<std::uint64_t> pick;
    for (auto const& e : project.sequence.events()) {
        if (e.tick > t) break;
        if (e.down) pick = e.id;
    }
    if (!pick) pick = project.sequence.nearest(t);
    select(pick);
    return selected.has_value();
}

void Session::shiftSelected(Tick delta) {
    if (!ensureSelection() || !editable()) return;
    auto id = *selected;
    auto st = history.apply(project.sequence, [&](InputSequence& s) { return s.shift(id, delta, shiftMode); });
    if (!st) return setStatus(st.error, true);
    sounds::play(SoundEvent::MoveInput);
    auto ev = project.sequence.find(id);
    std::string rel;
    if (auto ms = selectedMeasurement(); ms && !measurementStale(id)) rel = " (" + sgn(ev->tick - ms->anchor) + ")";
    setStatus(fmt::format("Input {} -> frame {}{}{}", *project.sequence.indexOf(id) + 1, ev->tick, rel,
                          shiftMode == ShiftMode::Ripple ? " [ripple]" : ""));
    resimAfterEdit();
}

void Session::beginDrag() {
    if (!editable(true)) return;
    m_dragOrigin = project.sequence;
}

void Session::dragTo(Tick t) {
    if (!m_dragOrigin || !selected) return;
    auto ev = project.sequence.find(*selected);
    if (!ev || ev->tick == t) return;
    // Clamp to the legal range so a fast drag still ends next to its neighbour.
    auto [lo, hi] = project.sequence.legalRange(*selected);
    if (shiftMode == ShiftMode::Single) t = std::clamp(t, lo, hi);
    if (project.sequence.setTick(*selected, t, shiftMode)) sounds::play(SoundEvent::MoveInput);
}

void Session::endDrag() {
    if (!m_dragOrigin) return;
    auto after = project.sequence;
    project.sequence = std::move(*m_dragOrigin);
    m_dragOrigin.reset();
    if (after.events() == project.sequence.events()) return;
    history.apply(project.sequence, [&](InputSequence& s) {
        s = after;
        return Status::success();
    });
    if (selected)
        if (auto ev = project.sequence.find(*selected))
            setStatus(fmt::format("Input {} -> frame {}", *project.sequence.indexOf(*selected) + 1, ev->tick));
    resimAfterEdit();
}

void Session::deleteSelected() {
    if (selectedCircle) {
        project.circles.remove(*selectedCircle);
        selectedCircle.reset();
        return setStatus("Circle deleted.");
    }
    if (!selected || !editable()) return;
    auto id = *selected;
    auto st = history.apply(project.sequence, [&](InputSequence& s) { return s.removeWithPartner(id); });
    if (!st) return setStatus(st.error, true);
    for (auto it = project.windows.begin(); it != project.windows.end();)
        it = project.sequence.find(it->first) ? std::next(it) : project.windows.erase(it);
    ++indicatorVersion;
    selected.reset();
    setStatus("Click deleted (press and its release). Ctrl+Z undoes it.");
    resimAfterEdit();
}

void Session::addClickAtPlayhead() {
    if (!m_layer || !editable()) return;
    Tick t = tick();
    std::uint64_t pressId = 0;
    auto st = history.apply(project.sequence, [&](InputSequence& s) {
        bool held = false;
        std::optional<Tick> next;
        for (auto const& e : s.events()) {
            if (e.button != Button::Jump || e.player2) continue;
            if (e.tick <= t) held = e.down;
            else if (!next) next = e.tick;
        }
        if (held) {
            pressId = s.add(t, Button::Jump, false, false);
            return s.validate();
        }
        if (next && *next <= t + 1) return Status::fail("No room for a click here (next input is on the next frame).");
        pressId = s.add(t, Button::Jump, true, false);
        s.add(t + 1, Button::Jump, false, false);
        return s.validate();
    });
    if (!st) return setStatus(st.error, true);
    select(pressId);
    setStatus(fmt::format("Added a click at frame {}.", t));
    resimAfterEdit();
}

void Session::toggleShiftMode() {
    shiftMode = shiftMode == ShiftMode::Single ? ShiftMode::Ripple : ShiftMode::Single;
    setStatus(shiftMode == ShiftMode::Ripple ? "Ripple: moving an input also moves every later input."
                                             : "Single: moving an input leaves the others in place.");
}

void Session::undo() {
    if (!editable()) return;
    setStatus(history.undo(project.sequence) ? "Undone." : "Nothing to undo.");
    select(selected, false);
    resimAfterEdit();
}

void Session::redo() {
    if (!editable()) return;
    setStatus(history.redo(project.sequence) ? "Redone." : "Nothing to redo.");
    select(selected, false);
    resimAfterEdit();
}

void Session::save(bool quiet) {
    auto path_ = projectPath(project);
    auto dir = path_.parent_path();
    if (auto r = utils::file::createDirectoryAll(dir); r.isErr())
        return setStatus("Save failed: " + r.unwrapErr(), true);
    if (std::filesystem::exists(path_)) {
        std::error_code ec;
        std::filesystem::copy_file(path_, path_.string() + ".bak", std::filesystem::copy_options::overwrite_existing,
                                   ec);
    }
    if (auto r = utils::file::writeStringSafe(path_, serializeProject(project)); r.isErr())
        return setStatus("Save failed: " + r.unwrapErr(), true);
    if (quiet) log::info("Saved {}", utils::string::pathToString(path_.filename()));
    else notify("Saved " + utils::string::pathToString(path_.filename()));
}

void Session::load() {
    auto path_ = projectPath(project);
    auto text = utils::file::readString(path_);
    if (text.isErr()) return setStatus("No saved project for this level.", true);
    auto parsed = parseProject(text.unwrap());
    if (!parsed) return setStatus("Load failed: " + parsed.error, true);
    if (mode != Mode::Idle) stopAll();
    project = std::move(parsed.value);
    // Drop a leading release without a press (recorded by older versions).
    std::vector<std::uint64_t> orphans;
    std::map<int, bool> held;
    for (auto const& e : project.sequence.events()) {
        int key = static_cast<int>(e.button) + (e.player2 ? 8 : 0);
        if (!e.down && !held[key]) orphans.push_back(e.id);
        else held[key] = e.down;
    }
    for (auto id : orphans) project.sequence.remove(id);
    history.clear();
    selected.reset();
    selectedCircle.reset();
    m_recordTrace.clear();
    m_recordedSeq.reset();
    m_baselineVerifiedFor.reset();
    inputPos.clear();
    ++indicatorVersion;
    setStatus(fmt::format("Loaded {} inputs. P replays them, A analyzes every input.", project.sequence.size()));
}

void Session::setGoalTick(Tick t) {
    project.goal = {Goal::Kind::SurviveUntilTick, std::max<Tick>(t, 0)};
    project.hasGoal = true;
    setStatus(fmt::format("Goal: survive until frame {}.", project.goal.tick));
}

void Session::setGoalComplete() {
    project.goal = {Goal::Kind::CompleteLevel, 0};
    project.hasGoal = true;
    setStatus("Goal: complete the level.");
}

void Session::clearGoal() {
    project.hasGoal = false;
    setStatus(fmt::format("Goal: survive {} frames after the tested input.", cfg::integer("goal-default-ticks")));
}

Goal Session::goalFor(std::uint64_t id) const {
    auto ev = project.sequence.find(id);
    Tick at = ev ? ev->tick : project.sequence.lastTick();
    if (project.hasGoal && (project.goal.kind == Goal::Kind::CompleteLevel || project.goal.tick > at + 1))
        return project.goal;
    Tick g = at + cfg::integer("goal-default-ticks");
    // The recorded attempt died at deathTick: a goal past it could never pass.
    if (deathTick && *deathTick > at) g = std::min(g, *deathTick - 1);
    return {Goal::Kind::SurviveUntilTick, std::max(g, at + 2)};
}

Goal Session::effectiveGoal() const {
    if (selected) return goalFor(*selected);
    if (project.hasGoal) return project.goal;
    return {Goal::Kind::SurviveUntilTick, project.sequence.lastTick() + cfg::integer("goal-default-ticks")};
}

MeasurementSet const* Session::measurementFor(std::uint64_t id) const {
    auto it = project.measurements.find(id);
    return it == project.measurements.end() ? nullptr : &it->second;
}

MeasurementSet const* Session::selectedMeasurement() const {
    return selected ? measurementFor(*selected) : nullptr;
}

std::uint64_t Session::measurementSetContext(std::uint64_t id) const {
    auto it = project.measurements.find(id);
    if (it == project.measurements.end()) return 0;
    return measurementContext(project.sequence, id, it->second.mode, it->second.goal);
}

bool Session::measurementStale(std::uint64_t id) const {
    auto it = project.measurements.find(id);
    if (it == project.measurements.end()) return false;
    if (auto ev = project.sequence.find(id); !ev) return true;
    return measurementSetContext(id) != it->second.context;
}

void Session::refreshIndicators(bool notify_) {
    ++indicatorVersion;
    if (notify_) {
        if (project.measurements.empty()) notify("No windows measured yet: A analyzes every input.", true);
        else notify("Circle indicators refreshed.");
    }
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

bool Session::canTest() {
    if (!m_layer) return false;
    if (mode == Mode::Testing) {
        setStatus("Already analyzing (F10 stops it).", true);
        return false;
    }
    if (mode == Mode::Recording) {
        setStatus("Stop recording first (R / F3).", true);
        return false;
    }
    if (project.startTick != 0) {
        setStatus("This recording did not start at frame 0; record again from the start.", true);
        return false;
    }
    return ensureSelection();
}

void Session::testSelected() {
    if (!canTest()) return;
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
        job.goal = goalFor(id);
    }
    job.base = project.sequence;
    if (!job.base.setTick(id, job.anchor, shiftMode)) {
        job.anchor = ev->tick;
        job.base = project.sequence;
    }
    job.manualOffset = ev->tick - job.anchor;
    m_queue.clear();
    m_batch = false;
    startJob(std::move(job));
}

bool Session::startSweep(std::uint64_t id, bool quiet) {
    auto ev = project.sequence.find(id);
    if (!ev) return false;
    Job job;
    job.inputId = id;
    job.mode = shiftMode;
    job.anchor = ev->tick;
    job.goal = goalFor(id);
    job.base = project.sequence;
    auto base = std::make_shared<InputSequence>(job.base);
    auto mode_ = shiftMode;
    SweepConfig sc{cfg::integer("sweep-radius"), cfg::integer("sweep-max-radius"), true};
    job.planner.emplace(sc, [base, id, mode_](Tick off) {
        auto copy = *base;
        return copy.shift(id, off, mode_).ok;
    });
    (void)quiet;
    startJob(std::move(job));
    return true;
}

void Session::sweepSelected() {
    if (!canTest()) return;
    m_queue.clear();
    m_batch = false;
    startSweep(*selected, false);
}

void Session::analyzeAll() {
    if (!m_layer) return;
    if (project.sequence.empty()) return setStatus("No inputs yet: record an attempt first (R / F3).", true);
    if (!canTest()) return;
    m_queue.clear();
    bool releases = false; // presses only
    for (auto const& e : project.sequence.events())
        if (e.down || releases) m_queue.push_back(e.id);
    progress.queueLeft = static_cast<int>(m_queue.size());
    m_batch = true;
    auto first = m_queue.front();
    m_queue.erase(m_queue.begin());
    startSweep(first, true);
}

void Session::startJob(Job job) {
    if (mode == Mode::Playback) {
        m_run = {};
        m_cursor.reset(nullptr);
    }
    mode = Mode::Testing;
    paused = false;
    m_pendingSteps = 0;
    progress.active = true;
    progress.phase = "starting";
    progress.done = 0;
    progress.inputId = job.inputId;
    progress.queueLeft = static_cast<int>(m_queue.size());
    select(job.inputId, false);
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
    m_queue.clear();
    m_run = {};
    mode = Mode::Idle;
    progress.active = false;
    setStatus(fmt::format("Analysis {}.", why));
    requestReset();
}

void Session::failJob(std::string why) {
    m_job.reset();
    m_queue.clear();
    m_batch = false;
    m_run = {};
    mode = Mode::Idle;
    progress.active = false;
    notify(why, true);
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
                    "The replay does not reproduce your recorded attempt (it differs at frame {}). {}", *div,
                    conflictWarning.empty() ? "Record again with 'Lock 1 physics tick per update' on."
                                            : conflictWarning));
            }
        } else if (kind == RunKind::Baseline) {
            if (j.stage == 1) {
                j.baseline = m_run.trace;
                j.baselineObs = m_run.obs;
                j.baselineObs.desynced = diag.lateInputs > 0;
                j.baselinePos = m_run.watchPos;
                std::uint64_t key = sequenceHash(j.base);
                bool verified = m_baselineVerifiedFor && *m_baselineVerifiedFor == key;
                j.stage = verified ? 3 : 2;
            } else {
                auto div = firstDivergence(j.baseline, m_run.trace, kFar);
                if (div)
                    return failJob(fmt::format("The level does not replay identically twice (differs at frame {}). "
                                               "No window on it can be trusted.",
                                               *div));
                m_baselineVerifiedFor = sequenceHash(j.base);
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
            progress.phase = "checking the replay";
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
    if (j.baselinePos) inputPos[j.inputId] = *j.baselinePos;
    ++indicatorVersion;

    auto report = ms.report();
    auto idx = project.sequence.indexOf(j.inputId);
    std::string who = idx ? fmt::format("Input {}", *idx + 1) : std::string("Input");
    auto baselineOk = classify(j.baselineObs, j.goal) == Outcome::Pass;

    // Analyze-all: continue with the next input.
    while (!m_queue.empty()) {
        auto next = m_queue.front();
        m_queue.erase(m_queue.begin());
        if (project.sequence.find(next)) {
            setStatus(fmt::format("{}: {}", who, report.summary()));
            startSweep(next, true);
            return;
        }
    }

    mode = Mode::Idle;
    progress.active = false;
    if (j.planner) {
        sounds::play(SoundEvent::WindowDone);
        if (!baselineOk)
            notify(fmt::format("{}: the unchanged attempt itself fails before the goal (frame {}). Move the goal (G) "
                               "earlier or fix the inputs.",
                               who, j.goal.tick),
                   true);
        else if (m_batch)
            refreshIndicators(true);
        else
            notify(fmt::format("{}: {}", who, report.summary()));
        progress.queueLeft = 0;
        m_batch = false;
    } else if (!j.results.empty()) {
        auto last = j.results.begin();
        setStatus(fmt::format("Offset {} -> {}. {}", sgn(last->first), outcomeName(last->second), report.summary()),
                  last->second != Outcome::Pass);
    }
    save(true);
    requestReset();
}

} // namespace fwl::gd
