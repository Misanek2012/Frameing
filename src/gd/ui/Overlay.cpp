#include "Overlay.hpp"

#include "../Session.hpp"
#include "../Settings.hpp"
#include "../Sounds.hpp"

#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/CCMouseDispatcher.hpp>

using namespace geode::prelude;

namespace fwl::gd {

namespace {
constexpr int kTouchPriority = -600; // ahead of GD's UILayer (click = jump) and menus
constexpr float kSide = 6.f;          // panel side margin
constexpr float kBottom = 4.f;        // panel bottom margin

ccColor4F col(ccColor4B c, float a = 1.f) { return cfg::toF(c, a); }
ccColor4F rgba4(float r, float g, float b, float a) { return {r, g, b, a}; }
ccColor4F rgb255(int r, int g, int b, float a = 1.f) { return {r / 255.f, g / 255.f, b / 255.f, a}; }
std::string sgn(Tick t) { return (t > 0 ? "+" : "") + std::to_string(t); }

// NaN's look: white minor ticks every 10 frames, yellow labelled ticks every 60.
constexpr Tick kMinor[] = {1, 5, 10, 30, 60, 120, 300, 600, 1200, 3000, 6000, 12000, 30000};
constexpr Tick kMajor[] = {5, 10, 30, 60, 120, 300, 600, 1200, 3000, 6000, 12000, 30000, 60000};

Tick pick(Tick const* steps, std::size_t n, double pxPerTick, float minPx, Tick multipleOf = 1) {
    for (std::size_t i = 0; i < n; ++i)
        if (steps[i] * pxPerTick >= minPx && steps[i] % multipleOf == 0) return steps[i];
    return steps[n - 1];
}

constexpr RGBA kPalette[] = {
    {80, 255, 120, 255}, {255, 70, 80, 255}, {255, 210, 40, 255},
    {60, 220, 255, 255}, {255, 90, 230, 255}, {255, 255, 255, 255},
};

char const* buttonWord(Button b) {
    switch (b) {
        case Button::Jump: return "Jump";
        case Button::Left: return "Left";
        case Button::Right: return "Right";
    }
    return "?";
}

// Frame-window size shown on an indicator: the passing run that contains the
// recorded timing (0 if the recorded timing itself fails).
struct WindowCount {
    Tick count = 0;
    bool exact = true;
    bool measured = false;
};
WindowCount windowCount(MeasurementSet const& ms) {
    auto rep = ms.report();
    WindowCount w;
    w.measured = !rep.results.empty();
    if (rep.originalSegment) {
        auto const& seg = rep.segments[*rep.originalSegment];
        w.count = seg.range.size();
        w.exact = seg.exact();
    }
    return w;
}
} // namespace

Overlay* Overlay::s_instance = nullptr;

// ============================================================ LevelLayer

LevelLayer* LevelLayer::create() {
    auto ret = new LevelLayer();
    if (ret->init()) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

bool LevelLayer::init() {
    if (!CCNode::init()) return false;
    m_path = CCDrawNode::create();
    m_path->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
    addChild(m_path, 0);
    m_draw = CCDrawNode::create();
    m_draw->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
    addChild(m_draw, 1);
    return true;
}

CCLabelBMFont* LevelLayer::label(std::size_t i) {
    while (m_labels.size() <= i) {
        auto l = CCLabelBMFont::create("", "bigFont.fnt");
        l->setScale(0.42f);
        l->setAnchorPoint({1.f, 0.5f});
        addChild(l, 3);
        m_labels.push_back(l);
    }
    m_labels[i]->setVisible(true);
    return m_labels[i];
}

void LevelLayer::redraw() {
    drawPath();
    drawIndicators();
}

void LevelLayer::drawPath() {
    auto& s = Session::get();
    bool show = cfg::flag("show-path");
    if (!show) {
        if (m_pathDrawn) m_path->clear();
        m_pathDrawn = 0;
        m_pathVersion = -1;
        return;
    }
    if (m_pathVersion != s.pathVersion || m_pathDrawn > s.path.size()) {
        m_path->clear();
        m_pathDrawn = 0;
        m_pathVersion = s.pathVersion;
    }
    auto c = col(cfg::rgba("color-path"));
    auto const& p = s.path;
    for (std::size_t i = std::max<std::size_t>(m_pathDrawn, 1); i < p.size(); ++i)
        m_path->drawSegment({p[i - 1].first, p[i - 1].second}, {p[i].first, p[i].second}, 0.45f, c);
    m_pathDrawn = p.size();
}

void LevelLayer::drawIndicators() {
    auto& s = Session::get();
    m_draw->clear();
    std::size_t li = 0;
    auto none = rgba4(0, 0, 0, 0);

    // small hollow circles where each input fires, on the path
    if (cfg::flag("show-path")) {
        auto pc = col(cfg::rgba("color-path"));
        for (auto const& e : s.project.sequence.events()) {
            auto it = s.inputPos.find(e.id);
            if (it == s.inputPos.end()) continue;
            bool sel = s.selected && *s.selected == e.id;
            m_draw->drawCircle({it->second.first, it->second.second}, sel ? 3.2f : 2.4f, none, sel ? 0.7f : 0.45f,
                               sel ? rgba4(1, 1, 1, 1) : pc, 16);
        }
    }

    // circle indicators: a white/black target at the input with its window size
    if (cfg::flag("show-indicators")) {
        for (auto const& [id, ms] : s.project.measurements) {
            auto ev = s.project.sequence.find(id);
            if (!ev) continue;
            std::optional<Pos> pos;
            if (auto it = ms.positions.find(ms.anchor); it != ms.positions.end()) pos = it->second;
            else if (auto it2 = s.inputPos.find(id); it2 != s.inputPos.end()) pos = it2->second;
            if (!pos) continue;
            auto w = windowCount(ms);
            if (!w.measured) continue;
            bool stale = s.measurementStale(id);
            bool sel = s.selected && *s.selected == id;
            float a = stale ? 0.45f : 1.f;
            CCPoint c{pos->first, pos->second};
            float r = static_cast<float>(cfg::real("indicator-radius"));
            m_draw->drawCircle(c, r, none, r * 0.2f, rgba4(1, 1, 1, a), 40);
            m_draw->drawCircle(c, r * 0.62f, none, r * 0.14f, rgba4(0, 0, 0, a * 0.9f), 32);
            m_draw->drawCircle(c, r * 0.18f, rgba4(1, 1, 1, a), 0, none, 12);
            if (w.count == 0) m_draw->drawCircle(c, r * 1.25f, none, 0.6f, rgba4(1, 0.25f, 0.25f, a), 40);
            if (sel) m_draw->drawCircle(c, r * 1.35f, none, 0.6f, rgba4(1, 0.85f, 0.2f, 1), 40);
            auto l = label(li++);
            l->setString(fmt::format("{}{}", w.count, w.exact ? "" : "+").c_str());
            l->setPosition({c.x - r * 1.25f, c.y});
            l->setOpacity(static_cast<GLubyte>(255 * a));
            l->setColor(w.count == 0 ? ccColor3B{255, 90, 90} : ccColor3B{255, 255, 255});
        }
    }
    for (std::size_t i = li; i < m_labels.size(); ++i) m_labels[i]->setVisible(false);

    // the user's own circles
    for (auto const& c : s.project.circles.circles()) {
        if (!c.visible) continue;
        bool sel = s.selectedCircle && *s.selectedCircle == c.id;
        auto line = cfg::toF(c.color);
        auto fill = cfg::toF(c.color, c.fillAlpha);
        if (c.glow) {
            m_draw->drawCircle({c.x, c.y}, c.radius + c.thickness * 1.5f, none, c.thickness * 2.0f,
                               cfg::toF(c.color, 0.18f), 48);
        }
        m_draw->drawCircle({c.x, c.y}, c.radius, fill, c.thickness, line, 48);
        if (sel)
            m_draw->drawCircle({c.x, c.y}, c.radius + c.thickness + 2.f, none, 0.6f, rgba4(1, 1, 1, 0.95f), 48);
    }
}

// ============================================================ Overlay

void Overlay::install(PlayLayer* pl) {
    auto o = new Overlay();
    if (o->init(pl)) {
        o->autorelease();
        pl->addChild(o, 1000);
    } else {
        delete o;
    }
}

void Overlay::addButton(char const* key, char const* text, char const* bg, ccColor3B textColor, SEL_MenuHandler fn,
                        bool right) {
    auto spr = ButtonSprite::create(text, 22, true, "bigFont.fnt", bg, 26.f, 0.55f);
    if (spr->m_label) spr->m_label->setColor(textColor);
    spr->setScale(0.5f);
    auto item = CCMenuItemSpriteExtra::create(spr, this, fn);
    item->setID(key);
    m_menu->addChild(item);
    (right ? m_right : m_left).emplace_back(key, item);
}

bool Overlay::init(PlayLayer* pl) {
    if (!CCLayer::init()) return false;
    m_pl = pl;
    setID("frame-window-lab-overlay"_spr);

    m_draw = CCDrawNode::create();
    m_draw->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
    addChild(m_draw);

    auto mk = [&](float scale, CCPoint anchor, int z = 2) {
        auto l = CCLabelBMFont::create("", "chatFont.fnt");
        l->setScale(scale);
        l->setAnchorPoint(anchor);
        addChild(l, z);
        return l;
    };
    m_hud = mk(0.5f, {0, 1});
    m_status = mk(0.38f, {0, 1});
    m_window = mk(0.38f, {1, 1});
    m_tooltip = mk(0.4f, {0.5f, 0}, 5);
    m_resetLabel = mk(0.28f, {0, 0.5f});
    m_goalLabel = mk(0.28f, {0, 0.5f});

    m_menu = CCMenu::create();
    m_menu->setPosition({0, 0});
    addChild(m_menu, 3);
    ccColor3B white{255, 255, 255}, yellow{255, 230, 60}, orange{255, 170, 40};
    addButton("ci", "CI", "GJ_button_02.png", white, menu_selector(Overlay::onCI), false);
    addButton("rec", "R", "GJ_button_01.png", orange, menu_selector(Overlay::onRec), false);
    addButton("hb", "H", "GJ_button_01.png", yellow, menu_selector(Overlay::onHitboxes), false);
    addButton("circle", "O", "GJ_button_01.png", yellow, menu_selector(Overlay::onCircleTool), false);
    addButton("play", "P", "GJ_button_02.png", white, menu_selector(Overlay::onPlay), true);
    addButton("back", "<", "GJ_button_01.png", yellow, menu_selector(Overlay::onStepBack), true);
    addButton("step", ">", "GJ_button_01.png", yellow, menu_selector(Overlay::onStep), true);
    addButton("add", "+", "GJ_button_01.png", yellow, menu_selector(Overlay::onAdd), true);
    addButton("test", "T", "GJ_button_01.png", yellow, menu_selector(Overlay::onTest), true);
    addButton("analyze", "A", "GJ_button_01.png", yellow, menu_selector(Overlay::onAnalyze), true);

    m_level = LevelLayer::create();
    if (pl->m_objectLayer) pl->m_objectLayer->addChild(m_level, 9999);

    m_view.pxPerTick = cfg::real("timeline-zoom");
    layout();
    setTouchEnabled(true);
    scheduleUpdate();
    return true;
}

void Overlay::onEnter() {
    CCLayer::onEnter();
    s_instance = this;
}

void Overlay::onExit() {
    if (s_instance == this) s_instance = nullptr;
    Session::get().detach(m_pl); // no-op if onQuit already detached
    CCLayer::onExit();
}

void Overlay::registerWithTouchDispatcher() {
    CCTouchDispatcher::get()->addTargetedDelegate(this, kTouchPriority, true);
}

CCRect Overlay::panelRect() const {
    auto win = CCDirector::get()->getWinSize();
    float h = static_cast<float>(cfg::real("timeline-height"));
    return {kSide, kBottom, win.width - 2 * kSide, h};
}

float Overlay::baselineY() const {
    auto r = panelRect();
    return r.getMinY() + r.size.height * 0.3f;
}

void Overlay::layout() {
    auto r = panelRect();
    m_view.width = r.size.width;
    float y = r.getMaxY() + 8.f;
    float x = r.getMinX() + 2.f;
    for (auto& [k, item] : m_left) {
        float w = item->getScaledContentSize().width;
        item->setPosition({x + w * 0.5f, y});
        x += w + 3.f;
    }
    x = r.getMaxX() - 2.f;
    for (auto it = m_right.rbegin(); it != m_right.rend(); ++it) {
        float w = it->second->getScaledContentSize().width;
        it->second->setPosition({x - w * 0.5f, y});
        x -= w + 3.f;
    }
    auto win = CCDirector::get()->getWinSize();
    m_hud->setPosition({4.f, win.height - 14.f});
    m_status->setPosition({4.f, win.height - 25.f});
    m_window->setPosition({win.width - 4.f, r.getMaxY() + 30.f});
}

bool Overlay::overPanel(CCPoint p) const { return panelRect().containsPoint(p); }

bool Overlay::overButtons(CCPoint p) {
    auto mp = m_menu->convertToNodeSpace(convertToWorldSpace(p));
    for (auto* list : {&m_left, &m_right})
        for (auto& [k, item] : *list)
            if (item->isVisible() && item->boundingBox().containsPoint(mp)) return true;
    return false;
}

CCPoint Overlay::toWorld(CCTouch* t) const {
    return m_pl->m_objectLayer->convertToNodeSpace(t->getLocation());
}

std::optional<std::uint64_t> Overlay::markerAt(CCPoint p) const {
    auto& s = Session::get();
    auto r = panelRect();
    float lx = p.x - r.getMinX();
    float tol = std::max(4.f, static_cast<float>(m_view.pxPerTick) * 0.6f);
    std::optional<std::uint64_t> best;
    float bestD = tol;
    for (auto const& e : s.project.sequence.events()) {
        float d = std::abs(m_view.tickToX(static_cast<double>(e.tick)) - lx);
        bool prefer = s.selected && *s.selected == e.id; // keep grabbing the selected one when overlapping
        if (d < bestD || (prefer && d <= bestD)) {
            bestD = d;
            best = e.id;
        }
    }
    return best;
}

CCLabelBMFont* Overlay::pooled(std::vector<CCLabelBMFont*>& pool, std::size_t i, float scale, CCPoint anchor) {
    while (pool.size() <= i) {
        auto l = CCLabelBMFont::create("", "chatFont.fnt");
        l->setScale(scale);
        l->setAnchorPoint(anchor);
        addChild(l, 1);
        pool.push_back(l);
    }
    pool[i]->setVisible(true);
    return pool[i];
}

void Overlay::fitAll() {
    auto& s = Session::get();
    Tick last = std::max<Tick>({s.project.sequence.lastTick(), s.runEndTick, 240});
    m_view.pxPerTick = std::clamp(m_view.width / static_cast<double>(last + 120), 0.08, 2.0);
    m_view.startTick = -40 / m_view.pxPerTick * 0.5;
    m_userView = true;
}

bool Overlay::editKeysActive() const {
    auto& s = Session::get();
    return (m_focus || s.paused) && s.mode != Mode::Recording && s.mode != Mode::Testing;
}

void Overlay::update(float) {
    auto& s = Session::get();
    setVisible(s.overlayVisible);
    m_level->setVisible(s.overlayVisible);
    if (!s.overlayVisible) return;
    layout();
    if (!m_fitted && !s.project.sequence.empty()) {
        fitAll();
        m_userView = false;
        m_fitted = true;
    }
    Tick t = s.tick();
    bool running = !s.paused && (s.mode == Mode::Recording || s.mode == Mode::Playback || s.mode == Mode::Idle);
    if (s.mode == Mode::Testing) {
        if (s.selected)
            if (auto e = s.project.sequence.find(*s.selected)) m_view.ensureVisible(e->tick, m_view.width * 0.3f);
    } else if (!m_userView || (running && s.mode != Mode::Idle)) {
        if (t != m_lastTick) m_view.ensureVisible(t, m_view.width * 0.2f);
        if (running && s.mode != Mode::Idle) m_userView = false;
    }
    m_lastTick = t;
    redraw();
    m_level->redraw();
    updateLabels();
    updateButtons();
}

void Overlay::redraw() {
    auto& s = Session::get();
    auto r = panelRect();
    m_view.width = r.size.width;
    m_draw->clear();

    float x0 = r.getMinX(), y0 = r.getMinY(), w = r.size.width, h = r.size.height;
    float base = baselineY();
    float top = y0 + h;
    auto X = [&](double t) { return x0 + m_view.tickToX(t); };
    auto inside = [&](float x) { return x >= x0 - 0.5f && x <= x0 + w + 0.5f; };

    // translucent panel
    m_draw->drawRect({x0, y0}, {x0 + w, top}, rgba4(0, 0, 0, static_cast<float>(cfg::real("timeline-opacity"))), 0,
                     rgba4(0, 0, 0, 0));

    // measured window of the selected input (behind the grid)
    if (auto ms = s.selectedMeasurement()) {
        bool stale = s.measurementStale(*s.selected);
        float a = stale ? 0.35f : 1.f;
        auto pass = col(cfg::rgba("color-pass"), a);
        auto fail = col(cfg::rgba("color-fail"), a);
        float by0 = top - h * 0.1f, by1 = top - 1.f;
        for (auto const& [t, out] : ms->byTick) {
            float a0 = X(static_cast<double>(t) - 0.5), a1 = X(static_cast<double>(t) + 0.5);
            if (a1 < x0 || a0 > x0 + w) continue;
            a0 = std::max(a0, x0);
            a1 = std::min(a1, x0 + w);
            auto c = out == Outcome::Pass ? pass : out == Outcome::Fail ? fail : rgba4(0.6f, 0.6f, 0.6f, 0.5f * a);
            if (out == Outcome::Blocked) c = rgba4(0.25f, 0.25f, 0.25f, 0.6f * a);
            m_draw->drawRect({a0, by0}, {std::max(a1, a0 + 0.6f), by1}, c, 0, c);
        }
        auto rep = ms->report();
        for (auto const& seg : rep.segments) {
            float a0 = X(static_cast<double>(ms->anchor + seg.range.lo) - 0.5);
            float a1 = X(static_cast<double>(ms->anchor + seg.range.hi) + 0.5);
            if (a1 < x0 || a0 > x0 + w) continue;
            m_draw->drawRect({std::max(a0, x0), base}, {std::min(a1, x0 + w), by0}, col(cfg::rgba("color-pass"), 0.13f * a),
                             0, rgba4(0, 0, 0, 0));
        }
    }

    // grid: white minor ticks, yellow labelled major ticks
    Tick minor = pick(kMinor, std::size(kMinor), m_view.pxPerTick, 4.5f);
    Tick major = pick(kMajor, std::size(kMajor), m_view.pxPerTick, 34.f, minor);
    Tick first = static_cast<Tick>(std::floor(m_view.startTick));
    Tick last = static_cast<Tick>(std::ceil(m_view.startTick + m_view.visibleTicks()));
    float minorTop = base + (top - base) * 0.45f, majorTop = base + (top - base) * 0.66f;
    std::size_t li = 0;
    for (Tick t = std::max<Tick>(0, (first / minor) * minor); t <= last; t += minor) {
        float x = X(static_cast<double>(t));
        if (!inside(x)) continue;
        bool isMajor = t % major == 0;
        if (isMajor) {
            m_draw->drawSegment({x, base}, {x, majorTop}, 0.45f, rgb255(255, 215, 40));
            auto lab = pooled(m_tickLabels, li++, 0.33f, {0.5f, 1.f});
            lab->setString(std::to_string(t).c_str());
            lab->setPosition({x, base - 1.5f});
            lab->setColor({200, 200, 200});
        } else {
            m_draw->drawSegment({x, base}, {x, minorTop}, 0.32f, rgba4(1, 1, 1, 0.9f));
        }
    }
    for (; li < m_tickLabels.size(); ++li) m_tickLabels[li]->setVisible(false);
    m_draw->drawSegment({x0, base}, {x0 + w, base}, 0.45f, rgba4(1, 1, 1, 1));

    // reset marker (blue) and goal marker
    if (s.resetTick >= 0) {
        float x = X(static_cast<double>(s.resetTick));
        if (inside(x)) {
            m_draw->drawRect({x - 1.6f, base - 3.f}, {x + 1.6f, top - 2.f}, rgb255(40, 110, 255, 0.95f), 0,
                             rgba4(0, 0, 0, 0));
            m_resetLabel->setString(fmt::format("Reset @ {}", s.resetTick).c_str());
            m_resetLabel->setPosition({x + 3.f, majorTop + 2.f});
            m_resetLabel->setVisible(true);
        } else {
            m_resetLabel->setVisible(false);
        }
    } else {
        m_resetLabel->setVisible(false);
    }
    auto goal = s.effectiveGoal();
    m_goalLabel->setVisible(false);
    if (goal.kind == Goal::Kind::SurviveUntilTick && (s.selected || s.project.hasGoal)) {
        float gx = X(static_cast<double>(goal.tick));
        if (inside(gx)) {
            auto gc = col(cfg::rgba("color-goal"), s.project.hasGoal ? 1.f : 0.55f);
            for (float y = base; y < top - 2.f; y += 3.f) m_draw->drawSegment({gx, y}, {gx, std::min(y + 1.6f, top - 2.f)}, 0.4f, gc);
            m_goalLabel->setString(s.project.hasGoal ? "goal" : "goal (auto)");
            m_goalLabel->setPosition({gx + 2.f, top - 5.f});
            m_goalLabel->setVisible(true);
        }
    }

    // inputs: green press / red release boxes on the baseline, numbered below
    auto press = col(cfg::rgba("color-press"), 0.85f), release = col(cfg::rgba("color-release"), 0.85f);
    float bw = std::clamp(static_cast<float>(m_view.pxPerTick) * 0.8f, 2.4f, 5.f);
    float bh = (top - base) * 0.27f;
    std::size_t ii = 0;
    float lastLabelX = -1e9f;
    auto const& evs = s.project.sequence.events();
    for (std::size_t i = 0; i < evs.size(); ++i) {
        auto const& e = evs[i];
        float x = X(static_cast<double>(e.tick));
        if (!inside(x)) continue;
        bool sel = s.selected && *s.selected == e.id;
        auto c = e.down ? press : release;
        float yb = base + (e.player2 ? bh + 1.f : 0.f);
        m_draw->drawRect({x - bw * 0.5f, yb}, {x + bw * 0.5f, yb + bh}, c, sel ? 0.6f : 0.f,
                         sel ? rgb255(255, 230, 60) : c);
        if (e.button != Button::Jump) // platformer left/right: small arrow
            m_draw->drawSegment({x, yb + bh + 1.f}, {x + (e.button == Button::Left ? -2.5f : 2.5f), yb + bh + 2.5f}, 0.35f, c);
        if (x - lastLabelX > 9.f || sel) {
            auto lab = pooled(m_indexLabels, ii++, 0.28f, {0.5f, 1.f});
            lab->setString(std::to_string(i + 1).c_str());
            lab->setPosition({x, base - 8.5f});
            lab->setColor(sel ? ccColor3B{255, 230, 60} : ccColor3B{170, 170, 170});
            lastLabelX = x;
        }
        // measured window size above the box
        if (auto ms = s.measurementFor(e.id); ms && cfg::flag("show-indicators")) {
            auto wc = windowCount(*ms);
            if (wc.measured && m_view.pxPerTick > 0.25) {
                auto lab = pooled(m_indexLabels, ii++, 0.3f, {0.5f, 0.f});
                lab->setString(fmt::format("{}{}", wc.count, wc.exact ? "" : "+").c_str());
                lab->setPosition({x, yb + bh + 0.5f});
                lab->setColor(s.measurementStale(e.id) ? ccColor3B{140, 140, 140}
                              : wc.count == 0              ? ccColor3B{255, 100, 100}
                                                           : ccColor3B{255, 255, 255});
            }
        }
    }
    for (; ii < m_indexLabels.size(); ++ii) m_indexLabels[ii]->setVisible(false);

    // current frame (cyan)
    float px = X(static_cast<double>(s.tick()));
    if (inside(px))
        m_draw->drawRect({px - 0.8f, base - 3.f}, {px + 0.8f, top - 1.f}, col(cfg::rgba("color-current-frame")), 0,
                         rgba4(0, 0, 0, 0));
    // scrub ghost
    if (m_drag == Drag::Scrub) {
        float gx = std::clamp(m_dragLastX + x0, x0, x0 + w);
        m_draw->drawRect({gx - 0.6f, base - 3.f}, {gx + 0.6f, top - 1.f}, col(cfg::rgba("color-current-frame"), 0.5f), 0,
                         rgba4(0, 0, 0, 0));
    }
    if (m_focus) m_draw->drawRect({x0, y0}, {x0 + w, top}, rgba4(0, 0, 0, 0), 0.4f, rgba4(1, 1, 1, 0.35f));
}

void Overlay::updateLabels() {
    auto& s = Session::get();
    Tick t = s.tick();
    std::string hud = fmt::format("Frame: {}, Input: {}", t, s.inputsBefore(t));
    switch (s.mode) {
        case Mode::Recording: hud += "  REC"; break;
        case Mode::Playback: hud += s.paused ? "  PAUSED" : "  REPLAY"; break;
        case Mode::Testing:
            hud += fmt::format("  ANALYZING {}{}", s.progress.phase,
                               s.progress.queueLeft > 0 ? fmt::format(" ({} left)", s.progress.queueLeft) : "");
            break;
        case Mode::Idle:
            if (s.paused) hud += "  PAUSED";
            break;
    }
    if (s.shiftMode == ShiftMode::Ripple) hud += "  [ripple]";
    if (s.circleTool) hud += "  [circle tool]";
    m_hud->setString(hud.c_str());
    m_hud->setColor(s.mode == Mode::Recording ? ccColor3B{255, 120, 120} : ccColor3B{255, 255, 255});

    m_status->setString(s.status.c_str());
    m_status->setColor(s.statusIsError ? ccColor3B{255, 120, 120} : ccColor3B{210, 210, 210});
    m_status->limitLabelWidth(CCDirector::get()->getWinSize().width * 0.62f, 0.38f, 0.15f);

    // window summary for the selected input
    std::string win;
    if (auto ms = s.selectedMeasurement()) {
        auto rep = ms->report();
        win = (s.measurementStale(*s.selected) ? "STALE  " : "") + rep.summary();
    }
    m_window->setString(win.c_str());
    m_window->limitLabelWidth(CCDirector::get()->getWinSize().width * 0.5f, 0.38f, 0.15f);

    // tooltip
    auto mouse = convertToNodeSpace(geode::cocos::getMousePos());
    auto r = panelRect();
    if (r.containsPoint(mouse) && m_drag != Drag::Pan) {
        float lx = mouse.x - r.getMinX();
        Tick mt = m_view.xToTick(lx);
        std::string tip = fmt::format("frame {}", std::max<Tick>(0, mt));
        if (auto id = markerAt(mouse))
            if (auto e = s.project.sequence.find(*id)) {
                tip = fmt::format("{}: {} {} P{} @ {}", *s.project.sequence.indexOf(*id) + 1, e->down ? "+" : "-",
                                  buttonWord(e->button), e->player2 ? 2 : 1, e->tick);
                if (auto ms = s.measurementFor(*id)) {
                    auto wc = windowCount(*ms);
                    if (wc.measured) tip += fmt::format("  |  window {}{}", wc.count, wc.exact ? "" : "+");
                }
            }
        if (auto ms = s.selectedMeasurement())
            if (auto it = ms->byTick.find(mt); it != ms->byTick.end())
                tip += fmt::format("  |  {} {}", sgn(mt - ms->anchor), outcomeName(it->second));
        m_tooltip->setString(tip.c_str());
        m_tooltip->setPosition({std::clamp(mouse.x, r.getMinX() + 50.f, r.getMaxX() - 50.f), r.getMaxY() + 17.f});
        m_tooltip->setVisible(true);
    } else {
        m_tooltip->setVisible(false);
    }
}

void Overlay::updateButtons() {
    auto& s = Session::get();
    auto tint = [&](std::vector<std::pair<std::string, CCMenuItemSpriteExtra*>>& list, char const* k, bool on) {
        for (auto& [key, item] : list)
            if (key == k) static_cast<ButtonSprite*>(item->getNormalImage())->setColor(on ? ccColor3B{255, 255, 255} : ccColor3B{150, 150, 150});
    };
    // lit = active
    tint(m_left, "rec", s.mode == Mode::Recording);
    tint(m_left, "hb", s.hitboxes);
    tint(m_left, "circle", s.circleTool);
    tint(m_left, "ci", true);
    tint(m_right, "play", s.mode == Mode::Playback && !s.paused);
    tint(m_right, "analyze", s.mode == Mode::Testing || true);
    tint(m_right, "back", true);
    tint(m_right, "step", true);
    tint(m_right, "add", true);
    tint(m_right, "test", true);
}

// ------------------------------------------------------------ touches

bool Overlay::ccTouchBegan(CCTouch* touch, CCEvent*) {
    auto& s = Session::get();
    if (!s.overlayVisible || !isVisible()) return false;
    auto p = convertToNodeSpace(touch->getLocation());
    if (overButtons(p)) return false; // let the CCMenu take it

    if (overPanel(p)) {
        m_focus = true;
        float lx = p.x - panelRect().getMinX();
        m_dragStartX = m_dragLastX = lx;
        m_dragMoved = false;
        auto kb = CCKeyboardDispatcher::get();
        if (kb->getAltKeyPressed()) {
            s.setGoalTick(m_view.xToTick(lx));
            m_drag = Drag::None;
            return true;
        }
        float playX = m_view.tickToX(static_cast<double>(s.tick()));
        if (auto id = markerAt(p)) {
            s.select(id);
            s.beginDrag();
            m_drag = Drag::Marker;
        } else if (std::abs(playX - lx) < 4.f && s.mode != Mode::Testing) {
            m_drag = Drag::Scrub;
        } else {
            m_drag = Drag::Pan;
        }
        return true;
    }

    m_focus = false;
    if (s.circleTool && m_pl->m_objectLayer) {
        auto w = toWorld(touch);
        if (auto hit = s.project.circles.hitTest(w.x, w.y)) {
            s.selectedCircle = hit;
            auto c = s.project.circles.find(*hit);
            m_grabOffset = CCPoint(c->x - w.x, c->y - w.y);
            m_drag = CCKeyboardDispatcher::get()->getShiftKeyPressed() ? Drag::CircleResize : Drag::CircleMove;
        } else {
            Circle c;
            c.x = w.x;
            c.y = w.y;
            c.radius = static_cast<float>(cfg::real("circle-radius"));
            c.thickness = static_cast<float>(cfg::real("circle-thickness"));
            c.fillAlpha = static_cast<float>(cfg::real("circle-fill"));
            c.color = cfg::toCore(cfg::rgba("circle-color"));
            s.selectedCircle = s.project.circles.add(c);
            sounds::play(SoundEvent::CircleCreate);
            m_drag = Drag::CircleResize; // drag out to size; a click keeps the default radius
        }
        return true;
    }
    return false; // pass through to the game (jump)
}

void Overlay::ccTouchMoved(CCTouch* touch, CCEvent*) {
    auto& s = Session::get();
    auto p = convertToNodeSpace(touch->getLocation());
    float lx = p.x - panelRect().getMinX();
    if (std::abs(lx - m_dragStartX) > 3.f) m_dragMoved = true;
    switch (m_drag) {
        case Drag::Marker:
            s.dragTo(m_view.xToTick(std::clamp(lx, 0.f, m_view.width))); // snapped to whole frames
            break;
        case Drag::Scrub: m_dragLastX = lx; break;
        case Drag::Pan:
            if (m_dragMoved) {
                m_view.scrollPx(m_dragLastX - lx);
                m_userView = true;
            }
            m_dragLastX = lx;
            break;
        case Drag::CircleMove:
            if (s.selectedCircle)
                if (auto c = s.project.circles.find(*s.selectedCircle)) {
                    auto w = toWorld(touch);
                    c->x = w.x + m_grabOffset.x;
                    c->y = w.y + m_grabOffset.y;
                }
            break;
        case Drag::CircleResize:
            if (s.selectedCircle)
                if (auto c = s.project.circles.find(*s.selectedCircle)) {
                    auto w = toWorld(touch);
                    float d = std::hypot(w.x - c->x, w.y - c->y);
                    if (d > 3.f) c->radius = std::max(2.f, d);
                }
            break;
        case Drag::None: break;
    }
}

void Overlay::ccTouchEnded(CCTouch* touch, CCEvent*) {
    auto& s = Session::get();
    auto p = convertToNodeSpace(touch->getLocation());
    float lx = p.x - panelRect().getMinX();
    switch (m_drag) {
        case Drag::Marker: s.endDrag(); break;
        case Drag::Scrub:
            if (m_dragMoved) s.seekTo(m_view.xToTick(std::clamp(lx, 0.f, m_view.width)));
            break;
        case Drag::Pan:
            // a plain click on the timeline jumps there
            if (!m_dragMoved && s.mode != Mode::Testing) {
                s.select(std::nullopt);
                s.seekTo(m_view.xToTick(lx));
            }
            break;
        default: break;
    }
    m_drag = Drag::None;
}

void Overlay::ccTouchCancelled(CCTouch* t, CCEvent* e) { ccTouchEnded(t, e); }

// ------------------------------------------------------------ wheel & keys

bool Overlay::onScroll(float y, float) {
    auto& s = Session::get();
    if (!s.overlayVisible || !isVisible()) return false;
    auto mouse = convertToNodeSpace(geode::cocos::getMousePos());
    auto kb = CCKeyboardDispatcher::get();
    if (overPanel(mouse)) {
        float lx = mouse.x - panelRect().getMinX();
        if (kb->getShiftKeyPressed()) m_view.scrollPx(y > 0 ? 40.f : -40.f);
        else m_view.zoomAround(lx, y > 0 ? 1 / 1.2 : 1.2);
        m_userView = true;
        return true;
    }
    if (s.circleTool && s.selectedCircle) {
        if (auto c = s.project.circles.find(*s.selectedCircle)) {
            c->radius = std::max(2.f, c->radius * (y > 0 ? 0.9f : 1.1f));
            return true;
        }
    }
    return false;
}

bool Overlay::onKey(enumKeyCodes key, bool repeat, bool shift, bool ctrl, bool alt) {
    auto& s = Session::get();
    if (m_pl->m_isPaused) return false; // GD pause menu is open
    int raw = static_cast<int>(key);
    if (key == KEY_F1 && !repeat) {
        s.overlayVisible = !s.overlayVisible;
        return true;
    }
    if (!s.overlayVisible) return false;

    // Esc freezes the game with the timeline usable (GD's pause menu would cover
    // it). Shift+Esc or GD's on-screen pause button still open GD's menu.
    if (key == KEY_Escape && cfg::flag("esc-pauses")) {
        if (shift || s.mode == Mode::Testing) {
            m_focus = false;
            return false;
        }
        if (!repeat) {
            s.togglePause();
            if (s.paused) s.setStatus("Paused. Esc resumes, '.' / ',' step, Shift+Esc opens the GD pause menu.");
        }
        return true;
    }

    switch (key) {
        case KEY_F2: if (!repeat) s.hitboxes = !s.hitboxes; return true;
        case KEY_F3: if (!repeat) { s.toggleRecording(); m_userView = false; } return true;
        case KEY_F4: if (!repeat) { s.togglePlayback(); m_userView = false; } return true;
        case KEY_F5: if (!repeat) s.testSelected(); return true;
        case KEY_F6: if (!repeat) shift ? s.analyzeAll() : s.sweepSelected(); return true;
        case KEY_F7: if (!repeat) onCircleTool(nullptr); return true;
        case KEY_F8: if (!repeat) s.refreshIndicators(); return true;
        case KEY_F9: if (!repeat) s.togglePause(); return true;
        case KEY_F10: if (!repeat) s.stopAll("Stopped."); return true;
        case KEY_OEMPeriod: s.stepForward(shift ? 10 : 1); m_userView = false; return true;
        case KEY_OEMComma: s.stepBack(shift ? 10 : 1); m_userView = false; return true;
        default: break;
    }
    // [ and ] select the previous / next input any time
    if (raw == 0xDB || key == KEY_LeftBracket) { s.selectNeighbour(-1); return true; }
    if (raw == 0xDD || key == KEY_RightBracket) { s.selectNeighbour(1); return true; }

    if (ctrl && !repeat) {
        switch (key) {
            case KEY_Z: shift ? s.redo() : s.undo(); return true;
            case KEY_Y: s.redo(); return true;
            case KEY_S: s.save(); return true;
            case KEY_O: s.load(); return true;
            default: break;
        }
    }

    // circle editing (circle tool on, a circle selected)
    if (s.circleTool && s.selectedCircle && !ctrl) {
        auto c = s.project.circles.find(*s.selectedCircle);
        if (c) {
            if (raw >= KEY_One && raw <= KEY_Six) {
                c->color = kPalette[raw - KEY_One];
                return true;
            }
            switch (key) {
                case KEY_Q: c->thickness = std::max(0.5f, c->thickness - 0.5f); return true;
                case KEY_E: c->thickness = std::min(12.f, c->thickness + 0.5f); return true;
                case KEY_Z: c->fillAlpha = std::max(0.f, c->fillAlpha - 0.05f); return true;
                case KEY_X: c->fillAlpha = std::min(1.f, c->fillAlpha + 0.05f); return true;
                case KEY_H:
                    if (shift) {
                        for (auto const& cc : s.project.circles.circles())
                            if (auto m = s.project.circles.find(cc.id)) m->visible = true;
                    } else {
                        c->visible = false;
                        s.selectedCircle.reset();
                    }
                    return true;
                case KEY_L:
                    if (s.selected) {
                        c->linkedInput = s.selected;
                        s.setStatus("Circle linked to the selected input.");
                    }
                    return true;
                case KEY_Delete:
                case KEY_Backspace: s.deleteSelected(); return true;
                default: break;
            }
        }
    }

    // editing keys: while paused (F9 / '.') or after clicking the timeline,
    // so they never steal arrow-key / space jumps during normal play
    if (!editKeysActive()) return false;
    switch (key) {
        case KEY_Left:
        case KEY_Right: {
            Tick d = (key == KEY_Left ? -1 : 1) * (shift ? 10 : 1);
            auto prev = s.shiftMode;
            if (ctrl) s.shiftMode = ShiftMode::Ripple;
            s.shiftSelected(d);
            s.shiftMode = prev;
            if (s.selected)
                if (auto e = s.project.sequence.find(*s.selected)) m_view.ensureVisible(e->tick, 30.f);
            return true;
        }
        case KEY_Up: s.selectNeighbour(-1); return true;
        case KEY_Down: s.selectNeighbour(1); return true;
        case KEY_M: if (!repeat) s.toggleShiftMode(); return true;
        case KEY_F: if (!repeat) fitAll(); return true;
        case KEY_Home: if (!repeat) s.seekTo(0); return true;
        case KEY_End: if (!repeat) s.seekTo(s.project.sequence.lastTick()); return true;
        case KEY_Insert:
        case KEY_I: if (!repeat) s.addClickAtPlayhead(); return true;
        case KEY_G:
            if (repeat) return true;
            if (ctrl) s.clearGoal();
            else if (shift) s.setGoalComplete();
            else s.setGoalTick(s.tick());
            return true;
        case KEY_Delete:
        case KEY_Backspace: s.deleteSelected(); return true;
        case KEY_PageUp: m_view.zoomAround(m_view.tickToX(static_cast<double>(s.tick())), 1.25); m_userView = true; return true;
        case KEY_PageDown: m_view.zoomAround(m_view.tickToX(static_cast<double>(s.tick())), 1 / 1.25); m_userView = true; return true;
        case KEY_Escape: m_focus = false; return false; // let GD open its pause menu too
        default: break;
    }
    if (raw == 0xBB || key == KEY_OEMPlus) { m_view.zoomAround(m_view.width / 2, 1.25); m_userView = true; return true; }
    if (key == KEY_OEMMinus) { m_view.zoomAround(m_view.width / 2, 1 / 1.25); m_userView = true; return true; }
    (void)alt;
    return false;
}

// ------------------------------------------------------------ buttons

void Overlay::onCI(CCObject*) { Session::get().refreshIndicators(); }
void Overlay::onRec(CCObject*) {
    Session::get().toggleRecording();
    m_userView = false;
}
void Overlay::onHitboxes(CCObject*) {
    auto& s = Session::get();
    s.hitboxes = !s.hitboxes;
}
void Overlay::onCircleTool(CCObject*) {
    auto& s = Session::get();
    s.circleTool = !s.circleTool;
    s.setStatus(s.circleTool ? "Circle tool: click to place, drag to size, Shift+drag resizes, 1-6 colour, Del removes."
                             : "Circle tool off.");
}
void Overlay::onPlay(CCObject*) {
    Session::get().togglePlayback();
    m_userView = false;
}
void Overlay::onStepBack(CCObject*) {
    Session::get().stepBack(1);
    m_userView = false;
}
void Overlay::onStep(CCObject*) {
    Session::get().stepForward(1);
    m_userView = false;
}
void Overlay::onAdd(CCObject*) { Session::get().addClickAtPlayhead(); }
void Overlay::onTest(CCObject*) { Session::get().testSelected(); }
void Overlay::onAnalyze(CCObject*) {
    auto& s = Session::get();
    if (s.mode == Mode::Testing) return s.stopAll("stopped");
    if (CCKeyboardDispatcher::get()->getShiftKeyPressed()) s.sweepSelected();
    else s.analyzeAll();
}

} // namespace fwl::gd

// ============================================================ input hooks

class $modify(FWLKeyboard, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double timestamp) {
        if (down)
            if (auto o = fwl::gd::Overlay::get())
                if (o->onKey(key, repeat, getShiftKeyPressed(), getControlKeyPressed(), getAltKeyPressed())) return true;
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
    }
};

class $modify(FWLMouse, CCMouseDispatcher) {
    bool dispatchScrollMSG(float y, float x) {
        if (auto o = fwl::gd::Overlay::get())
            if (o->onScroll(y, x)) return true;
        return CCMouseDispatcher::dispatchScrollMSG(y, x);
    }
};
