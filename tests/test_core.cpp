// Unit tests for the game-independent core of Frame Window Lab.
// Build: see tests/CMakeLists.txt (runs on any C++20 compiler, no GD needed).
#include "../src/core/InputSequence.hpp"
#include "../src/core/Playback.hpp"
#include "../src/core/Project.hpp"
#include "../src/core/WindowAnalysis.hpp"

#include <clocale>
#include <cmath>
#include <cstdio>
#include <locale>
#include <functional>
#include <string>
#include <vector>

using namespace fwl;

static int g_failed = 0, g_checks = 0;
static std::vector<std::pair<char const*, std::function<void()>>>& registry() {
    static std::vector<std::pair<char const*, std::function<void()>>> r;
    return r;
}
struct Reg {
    Reg(char const* n, std::function<void()> f) { registry().emplace_back(n, std::move(f)); }
};
#define TEST(name)                         \
    static void name();                    \
    static Reg reg_##name(#name, name);    \
    static void name()
#define CHECK(cond)                                                                   \
    do {                                                                              \
        ++g_checks;                                                                   \
        if (!(cond)) {                                                                \
            ++g_failed;                                                               \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
        }                                                                             \
    } while (0)

static InputSequence twoClicks() {
    // press@10 release@20 press@30 release@40 (jump, P1)
    InputSequence s;
    s.add(10, Button::Jump, true, false);
    s.add(20, Button::Jump, false, false);
    s.add(30, Button::Jump, true, false);
    s.add(40, Button::Jump, false, false);
    return s;
}

TEST(add_keeps_order_and_ids) {
    InputSequence s;
    auto a = s.add(50, Button::Jump, true, false);
    auto b = s.add(10, Button::Jump, false, false);
    CHECK(s.events().front().id == b);
    CHECK(s.events().back().id == a);
    CHECK(s.validate().ok);
}

TEST(single_shift_within_bounds) {
    auto s = twoClicks();
    auto id = s.events()[0].id;
    CHECK(s.shift(id, 5, ShiftMode::Single).ok);
    CHECK(s.find(id)->tick == 15);
    CHECK(s.events()[1].tick == 20); // others untouched
}

TEST(shift_cannot_cross_own_release) {
    auto s = twoClicks();
    auto before = s.events();
    auto id = s.events()[0].id;
    auto st = s.shift(id, 15, ShiftMode::Single); // press would land at 25, after its release
    CHECK(!st.ok);
    CHECK(st.error.find("alternate") != std::string::npos);
    CHECK(s.events() == before); // unchanged on failure
}

TEST(shift_to_same_tick_as_release_keeps_order) {
    auto s = twoClicks();
    auto id = s.events()[0].id;
    CHECK(s.setTick(id, 20, ShiftMode::Single).ok); // zero-length press is representable
    CHECK(s.events()[0].down && s.events()[1].tick == 20 && !s.events()[1].down);
}

TEST(shift_before_zero_fails) {
    auto s = twoClicks();
    CHECK(!s.shift(s.events()[0].id, -11, ShiftMode::Single).ok);
    CHECK(s.shift(s.events()[0].id, -10, ShiftMode::Single).ok);
}

TEST(ripple_moves_following_inputs) {
    auto s = twoClicks();
    auto id = s.events()[1].id; // release@20
    CHECK(s.shift(id, 7, ShiftMode::Ripple).ok);
    CHECK(s.events()[0].tick == 10);
    CHECK(s.events()[1].tick == 27);
    CHECK(s.events()[2].tick == 37);
    CHECK(s.events()[3].tick == 47);
}

TEST(other_channels_may_be_crossed) {
    InputSequence s;
    auto p = s.add(10, Button::Jump, true, false);
    s.add(12, Button::Left, true, false);
    s.add(30, Button::Jump, false, false);
    CHECK(s.shift(p, 5, ShiftMode::Single).ok); // crosses the Left press: fine
    CHECK(s.events()[0].button == Button::Left);
}

TEST(legal_range_matches_shift_rules) {
    auto s = twoClicks();
    auto id = s.events()[2].id; // press@30, between release@20 and release@40
    auto [lo, hi] = s.legalRange(id);
    CHECK(lo == 20 && hi == 40);
    CHECK(s.setTick(id, 20, ShiftMode::Single).ok);
    auto s2 = twoClicks();
    CHECK(!s2.setTick(id, 19, ShiftMode::Single).ok);
    CHECK(s2.setTick(id, 40, ShiftMode::Single).ok);
    auto s3 = twoClicks();
    CHECK(!s3.setTick(id, 41, ShiftMode::Single).ok);
}

TEST(neighbour_and_nearest) {
    auto s = twoClicks();
    auto ids = s.events();
    CHECK(*s.neighbour(ids[1].id, -1) == ids[0].id);
    CHECK(*s.neighbour(ids[1].id, 1) == ids[2].id);
    CHECK(!s.neighbour(ids[3].id, 1));
    CHECK(*s.nearest(26) == ids[2].id);
    CHECK(*s.nearest(25) == ids[1].id); // tie -> earlier
}

TEST(remove_with_partner_and_truncate) {
    auto s = twoClicks();
    auto release = s.events()[1].id;
    CHECK(s.removeWithPartner(release).ok); // removes press@10 + release@20
    CHECK(s.size() == 2 && s.events()[0].tick == 30);
    auto t = twoClicks();
    CHECK(t.removeWithPartner(t.events()[2].id).ok); // press@30 + release@40
    CHECK(t.size() == 2 && t.lastTick() == 20);
    auto u = twoClicks();
    u.truncateFrom(30);
    CHECK(u.size() == 2 && u.lastTick() == 20 && u.validate().ok);
    InputSequence lone;
    auto r = lone.add(5, Button::Jump, false, false); // first event may be a release
    CHECK(lone.removeWithPartner(r).ok && lone.empty());
}

TEST(undo_redo) {
    auto s = twoClicks();
    EditHistory h;
    auto id = s.events()[0].id;
    CHECK(h.apply(s, [&](InputSequence& q) { return q.shift(id, 1, ShiftMode::Single); }).ok);
    CHECK(h.apply(s, [&](InputSequence& q) { return q.shift(id, 1, ShiftMode::Single); }).ok);
    CHECK(s.find(id)->tick == 12);
    CHECK(h.undo(s) && s.find(id)->tick == 11);
    CHECK(h.undo(s) && s.find(id)->tick == 10);
    CHECK(!h.undo(s));
    CHECK(h.redo(s) && s.find(id)->tick == 11);
    // a new edit clears redo
    CHECK(h.apply(s, [&](InputSequence& q) { return q.shift(id, -1, ShiftMode::Single); }).ok);
    CHECK(!h.canRedo());
    // failing edits do not create history
    auto st = h.apply(s, [&](InputSequence& q) { return q.shift(id, 15, ShiftMode::Single); }); // would land between release@20 and press@30
    CHECK(!st.ok);
    CHECK(s.find(id)->tick == 10);
    CHECK(h.undo(s) && s.find(id)->tick == 11);
}

TEST(classify_rules) {
    Goal g{Goal::Kind::SurviveUntilTick, 100};
    CHECK(classify({false, 0, false, 100, false, false}, g) == Outcome::Pass);
    CHECK(classify({true, 100, false, 100, false, false}, g) == Outcome::Fail); // died on the goal tick
    CHECK(classify({true, 101, false, 101, false, false}, g) == Outcome::Pass); // died after goal
    CHECK(classify({false, 0, false, 50, false, true}, g) == Outcome::Untrusted); // aborted early
    CHECK(classify({false, 0, true, 60, false, false}, g) == Outcome::Pass);    // completed level
    CHECK(classify({false, 0, false, 200, true, false}, g) == Outcome::Untrusted);
    Goal c{Goal::Kind::CompleteLevel, 0};
    CHECK(classify({false, 0, true, 999, false, false}, c) == Outcome::Pass);
    CHECK(classify({true, 500, false, 500, false, false}, c) == Outcome::Fail);
    CHECK(classify({false, 0, false, 500, false, true}, c) == Outcome::Untrusted);
}

static std::map<Tick, Outcome> res(std::initializer_list<std::pair<Tick, char>> v) {
    std::map<Tick, Outcome> m;
    for (auto [t, c] : v)
        m[t] = c == 'P' ? Outcome::Pass : c == 'F' ? Outcome::Fail : c == 'U' ? Outcome::Untrusted : Outcome::Blocked;
    return m;
}

TEST(window_contiguous_exact) {
    auto r = analyzeWindow(res({{-4, 'F'}, {-3, 'P'}, {-2, 'P'}, {-1, 'P'}, {0, 'P'}, {1, 'P'}, {2, 'F'}}), 1000);
    CHECK(r.contiguous());
    CHECK(r.totalPass == 5);
    CHECK(*r.earliest == -3 && *r.latest == 1);
    CHECK(r.originalSegment && *r.originalSegment == 0);
    CHECK(r.exact());
}

TEST(window_non_contiguous_reported_separately) {
    auto r = analyzeWindow(res({{-4, 'F'}, {-3, 'P'}, {-2, 'P'}, {-1, 'F'}, {0, 'F'}, {1, 'P'}, {2, 'P'}, {3, 'P'}, {4, 'F'}}), 0);
    CHECK(r.segments.size() == 2);
    CHECK(!r.contiguous());
    CHECK((r.segments[0].range == Interval{-3, -2}));
    CHECK((r.segments[1].range == Interval{1, 3}));
    CHECK(r.totalPass == 5);
    CHECK(!r.originalSegment); // original timing fails
    CHECK(r.exact());
}

TEST(window_open_edge_not_exact) {
    auto r = analyzeWindow(res({{-2, 'P'}, {-1, 'P'}, {0, 'P'}, {1, 'F'}}), 0);
    CHECK(r.segments.size() == 1);
    CHECK(r.segments[0].lowEdge == Edge::Untested);
    CHECK(!r.exact());
    CHECK(r.summary().find("NOT exact") != std::string::npos);
}

TEST(window_untrusted_and_blocked_edges) {
    auto r = analyzeWindow(res({{-2, 'B'}, {-1, 'P'}, {0, 'P'}, {1, 'U'}, {2, 'P'}, {3, 'F'}}), 0);
    CHECK(r.segments.size() == 2);
    CHECK(r.segments[0].lowEdge == Edge::Blocked);
    CHECK(r.segments[0].highEdge == Edge::Untrusted);
    CHECK(r.segments[1].lowEdge == Edge::Untrusted);
    CHECK(r.untrusted.size() == 1);
    CHECK(!r.exact());
}

TEST(sweep_planner_expands_until_edges_fail) {
    // Ground truth: passes on [-7, 3]
    auto truth = [](Tick t) { return t >= -7 && t <= 3 ? Outcome::Pass : Outcome::Fail; };
    SweepPlanner p({2, 50, true}, [](Tick) { return true; });
    int trials = 0;
    while (auto off = p.next()) {
        p.report(*off, truth(*off));
        ++trials;
    }
    auto r = analyzeWindow(p.results(), 0);
    CHECK(r.contiguous());
    CHECK((r.segments[0].range == Interval{-7, 3}));
    CHECK(r.exact());
    CHECK(trials < 30);
}

TEST(sweep_planner_respects_max_and_blocked) {
    SweepPlanner p({3, 5, true}, [](Tick t) { return t >= -2; }); // can't go below -2
    while (auto off = p.next()) p.report(*off, Outcome::Pass);    // everything passes
    auto r = analyzeWindow(p.results(), 0);
    CHECK(r.segments.size() == 1);
    CHECK(r.segments[0].range.lo == -2 && r.segments[0].lowEdge == Edge::Blocked);
    CHECK(r.segments[0].range.hi == 5 && r.segments[0].highEdge == Edge::Untested); // hit max radius
    CHECK(!r.exact());
}

TEST(sweep_planner_finds_gap) {
    auto truth = [](Tick t) { return (t >= -3 && t <= -2) || (t >= 1 && t <= 2) ? Outcome::Pass : Outcome::Fail; };
    SweepPlanner p({4, 20, true}, [](Tick) { return true; });
    while (auto off = p.next()) p.report(*off, truth(*off));
    auto r = analyzeWindow(p.results(), 0);
    CHECK(r.segments.size() == 2);
    CHECK(r.exact());
}

TEST(state_trace_divergence) {
    StateTrace a, b;
    for (Tick t = 0; t < 10; ++t) { a.push(t, t * 7); b.push(t, t * 7); }
    CHECK(!firstDivergence(a, b, 10));
    StateTrace c = b;
    StateTrace d;
    for (Tick t = 0; t < 10; ++t) d.push(t, t == 6 ? 999 : t * 7);
    CHECK(*firstDivergence(a, d, 10) == 6);
    CHECK(!firstDivergence(a, d, 6)); // only compare before the edit
    StateTrace e;
    for (Tick t = 0; t < 4; ++t) e.push(t, t * 7); // shorter (e.g. died early)
    CHECK(*firstDivergence(a, e, 10) == 4);
    double x = 1.5;
    CHECK(fnv1a(&x, sizeof x) == fnv1a(&x, sizeof x));
    (void)c;
}

TEST(playback_cursor_fires_in_order_and_counts_late) {
    auto s = twoClicks();
    PlaybackCursor cur;
    cur.reset(&s);
    std::vector<Tick> fired;
    auto f = [&](InputEvent const& e) { fired.push_back(e.tick); };
    for (Tick t = 0; t <= 25; ++t) cur.fireUpTo(t, f);
    CHECK((fired == std::vector<Tick>{10, 20}));
    CHECK(cur.lateCount() == 0);
    cur.fireUpTo(45, f); // skipped ticks 26..44
    CHECK(fired.size() == 4);
    CHECK(cur.lateCount() == 2);
    CHECK(cur.finished());
}

TEST(make_trial) {
    auto s = twoClicks();
    auto id = s.events()[2].id; // press@30
    auto t = makeTrial(s, id, -4, ShiftMode::Single);
    CHECK(t.ok && t.value.firstChangedTick == 26 && t.value.inputTick == 26);
    auto t2 = makeTrial(s, id, 3, ShiftMode::Single);
    CHECK(t2.ok && t2.value.firstChangedTick == 30 && t2.value.inputTick == 33);
    CHECK(!makeTrial(s, id, 20, ShiftMode::Single).ok);
    CHECK(s.find(id)->tick == 30); // base untouched
}

TEST(circles_hit_test_and_generation) {
    CircleSet cs;
    Circle a; a.x = 0; a.y = 0; a.radius = 10;
    Circle b; b.x = 5; b.y = 0; b.radius = 10;
    auto ia = cs.add(a);
    auto ib = cs.add(b);
    CHECK(*cs.hitTest(4, 0) == ib); // topmost wins
    CHECK(*cs.hitTest(-12, 0, 4) == ia);
    CHECK(!cs.hitTest(100, 100));
    auto report = analyzeWindow(res({{-1, 'F'}, {0, 'P'}, {1, 'P'}, {2, 'U'}, {3, 'B'}}), 500);
    std::map<Tick, std::pair<float, float>> pos{{-1, {1, 1}}, {0, {2, 2}}, {1, {3, 3}}, {2, {4, 4}}};
    CircleStyle st;
    auto ids = cs.generateFromWindow(42, report, pos, st);
    CHECK(ids.size() == 4); // blocked offset has no position -> no circle
    CHECK(cs.find(ids[0])->color == st.fail);
    CHECK(cs.find(ids[1])->color == st.pass);
    CHECK(cs.find(ids[3])->color == st.untrusted);
    CHECK(cs.find(ids[2])->label == "+1");
    auto again = cs.generateFromWindow(42, report, pos, st); // regenerating replaces
    CHECK(cs.circles().size() == 2 + again.size());
    CHECK(cs.removeLinked(42) == 4);
    CHECK(cs.circles().size() == 2);
}

TEST(timeline_view_math) {
    TimelineView v;
    v.startTick = 100;
    v.pxPerTick = 8;
    v.width = 400;
    CHECK(v.tickToX(110) == 80.f);
    CHECK(v.xToTick(83) == 110);  // snaps to nearest
    CHECK(v.xToTick(85) == 111);
    double before = v.xToTickExact(200);
    v.zoomAround(200, 2.0);
    CHECK(std::abs(v.xToTickExact(200) - before) < 1e-9); // anchor fixed under cursor
    CHECK(v.pxPerTick == 16);
    v.zoomAround(0, 1000);
    CHECK(v.pxPerTick == TimelineView::kMaxPxPerTick);
    v.pxPerTick = 8;
    v.startTick = 0;
    v.ensureVisible(1000, 40);
    CHECK(std::abs(v.tickToX(1000) - (400 - 40)) < 1e-3);
    v.ensureVisible(0, 40);
    CHECK(std::abs(v.tickToX(0) - 40) < 1e-3);
    v.pxPerTick = 8;
    CHECK(v.gridStep(5) == 1);
    v.pxPerTick = 0.5;
    CHECK(v.gridStep(5) == 10);
    CHECK(v.labelStep(60) == 120);
}

TEST(escape_roundtrip) {
    for (std::string s : {"", "plain", "two words", "100% sure", "tab\there", "line\nbreak", "=lead"}) {
        CHECK(unescapeField(escapeField(s)) == s);
        CHECK(escapeField(s).find(' ') == std::string::npos);
    }
}

// Projects must save/load identically whatever locale the host process runs in.
// A decimal-comma locale (cs_CZ, de_DE) must neither change how floats are
// written/read nor add thousands separators to integers.
struct ScopedGlobalLocale {
    std::locale prevCpp;
    std::string prevC;
    bool active = false;
    explicit ScopedGlobalLocale(std::initializer_list<char const*> names) : prevCpp(std::locale()) {
        if (auto c = std::setlocale(LC_ALL, nullptr)) prevC = c;
        for (auto n : names) {
            try {
                std::locale::global(std::locale(n)); // also sets the C locale
                active = true;
                return;
            } catch (std::runtime_error const&) {}
        }
    }
    ~ScopedGlobalLocale() {
        std::locale::global(prevCpp);
        std::setlocale(LC_ALL, prevC.c_str());
    }
};

TEST(project_roundtrip_under_decimal_comma_locale) {
    Project p;
    p.levelId = 98765432;
    p.levelName = "Locale Test";
    p.startTick = 12345;
    p.sequence.add(12345, Button::Jump, true, false);
    p.sequence.add(23456, Button::Jump, false, false);
    p.goal = {Goal::Kind::SurviveUntilTick, 1234567};
    p.hasGoal = true;
    Circle c;
    c.x = 12345.625f; c.y = -0.1f; c.radius = 1.5f; c.thickness = 0.3f; c.fillAlpha = 1e-7f;
    p.circles.add(c);

    std::string reference = serializeProject(p); // written in the "C" locale

    ScopedGlobalLocale loc({"cs_CZ.UTF-8", "cs_CZ.utf8", "de_DE.UTF-8", "de_DE.utf8"});
    if (!loc.active) {
        std::printf("  (skipped: no decimal-comma locale installed)\n");
        return;
    }
    CHECK(std::use_facet<std::numpunct<char>>(std::locale()).decimal_point() == ',');
    CHECK(std::localeconv()->decimal_point[0] == ',');

    auto text = serializeProject(p);
    CHECK(text == reference);                          // no "12345,625" or "12 345"
    CHECK(text.find("12345.625") != std::string::npos);

    auto q = parseProject(text);
    CHECK(q.ok);
    if (!q.ok) { std::printf("  parse error: %s\n", q.error.c_str()); return; }
    CHECK(q.value.levelId == 98765432 && q.value.startTick == 12345);
    CHECK(q.value.goal.tick == 1234567);
    CHECK(q.value.sequence.events() == p.sequence.events());
    CHECK(q.value.circles.circles() == p.circles.circles()); // bit-exact floats

    // A comma-decimal value in a file is malformed, not silently "12345".
    auto bad = text;
    bad.replace(bad.find("12345.625"), 9, "12345,625");
    CHECK(!parseProject(bad).ok);
}

TEST(project_roundtrip) {
    Project p;
    p.levelId = 128;
    p.levelName = "Bloodbath Remix";
    p.startTick = 0;
    p.sequence = twoClicks();
    p.sequence.add(35, Button::Left, true, true);
    p.goal = {Goal::Kind::SurviveUntilTick, 777};
    p.hasGoal = true;
    Circle c;
    c.x = 123.25f; c.y = -4.5f; c.radius = 7.75f; c.color = {1, 2, 3, 4};
    c.thickness = 1.5f; c.fillAlpha = 0.3f; c.glow = false; c.visible = false;
    c.label = "orb 1"; c.linkedInput = 3; c.linkedOffset = -2; c.linkedOutcome = Outcome::Pass;
    p.circles.add(c);
    MeasurementSet m;
    m.anchor = 30;
    m.mode = ShiftMode::Ripple;
    m.goal = p.goal;
    m.context = measurementContext(p.sequence, 3, m.mode, m.goal);
    m.byTick = {{29, Outcome::Fail}, {30, Outcome::Pass}, {31, Outcome::Untrusted}};
    m.positions = {{29, {10.5f, 20.25f}}, {30, {11.f, 20.f}}};
    p.measurements[3] = m;
    auto text = serializeProject(p);
    auto q = parseProject(text);
    CHECK(q.ok);
    if (!q.ok) { std::printf("  parse error: %s\n", q.error.c_str()); return; }
    CHECK(q.value.levelId == 128 && q.value.levelName == "Bloodbath Remix");
    CHECK(q.value.sequence.events() == p.sequence.events());
    CHECK(q.value.sequence.nextId() == p.sequence.nextId());
    CHECK(q.value.circles.circles() == p.circles.circles());
    CHECK(q.value.hasGoal && q.value.goal.tick == 777);
    CHECK(q.value.measurements.size() == 1);
    auto const& qm = q.value.measurements.at(3);
    CHECK(qm.anchor == 30 && qm.mode == ShiftMode::Ripple && qm.context == m.context);
    CHECK(qm.byTick == m.byTick && qm.positions == m.positions);
    CHECK(serializeProject(q.value) == text); // stable
}

TEST(project_parse_errors_have_line_numbers) {
    auto r = parseProject("FWL 1\ninput 1 10 jump press p1\ninput 2 12 jump press p1\n");
    CHECK(!r.ok); // two presses in a row
    auto r2 = parseProject("FWL 1\nbogus 1\n");
    CHECK(!r2.ok && r2.error.find("line 2") != std::string::npos);
    auto r3 = parseProject("NOPE\n");
    CHECK(!r3.ok);
    auto r4 = parseProject("FWL 1\r\ninput 1 10 jump press p1\r\n"); // CRLF tolerated
    CHECK(r4.ok);
}

TEST(measurement_context_tracks_relevant_edits) {
    auto s = twoClicks();
    Goal g{Goal::Kind::SurviveUntilTick, 100};
    auto id = s.events()[2].id; // press@30
    auto c0 = measurementContext(s, id, ShiftMode::Single, g);
    auto moved = s;
    CHECK(moved.shift(id, 3, ShiftMode::Single).ok);
    CHECK(measurementContext(moved, id, ShiftMode::Single, g) == c0); // moving the measured input itself
    auto other = s;
    CHECK(other.shift(s.events()[0].id, 1, ShiftMode::Single).ok);
    CHECK(measurementContext(other, id, ShiftMode::Single, g) != c0); // editing another input -> stale
    CHECK(measurementContext(s, id, ShiftMode::Single, Goal{Goal::Kind::SurviveUntilTick, 101}) != c0);
    auto cr = measurementContext(s, id, ShiftMode::Ripple, g);
    auto rip = s;
    CHECK(rip.shift(id, 4, ShiftMode::Ripple).ok);
    CHECK(measurementContext(rip, id, ShiftMode::Ripple, g) == cr); // ripple keeps followers relative
    MeasurementSet ms;
    ms.anchor = 30;
    ms.byTick = {{28, Outcome::Fail}, {29, Outcome::Pass}, {30, Outcome::Pass}, {31, Outcome::Fail}};
    auto r = ms.report();
    CHECK(r.exact() && r.segments[0].range.lo == -1 && r.segments[0].range.hi == 0 && r.originalTick == 30);
}

TEST(count_ding_index) {
    CHECK(countDingIndex(0) == 1);
    CHECK(countDingIndex(4) == 5);
    CHECK(countDingIndex(9) == 6);
    CHECK(countDingIndex(-3) == 1);
}

int main() {
    for (auto& [name, fn] : registry()) {
        int before = g_failed;
        fn();
        std::printf("%s %s\n", g_failed == before ? "ok  " : "FAIL", name);
    }
    std::printf("\n%zu tests, %d checks, %d failed\n", registry().size(), g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
