#include "Overlay.hpp"

#include "../Session.hpp"
#include "../Settings.hpp"
#include "../Sounds.hpp"

#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/CCMouseDispatcher.hpp>

using namespace geode::prelude;

namespace fwl::gd {

namespace {
constexpr int kTouchPriority = -600; // ahead of GD's UILayer (click = jump) and menus
constexpr float kMargin = 6.f;
constexpr float kButtonRow = 15.f;

ccColor4F col(ccColor4B c, float a = 1.f) { return cfg::toF(c, a); }
ccColor4F rgba4(float r, float g, float b, float a) { return {r, g, b, a}; }
std::string sgn(Tick t) { return (t > 0 ? "+" : "") + std::to_string(t); }

char const* modeName(Mode m) {
    switch (m) {
        case Mode::Idle: return "IDLE";
        case Mode::Recording: return "REC";
        case Mode::Playback: return "REPLAY";
        case Mode::Testing: return "TEST";
    }
    return "?";
}

constexpr RGBA kPalette[] = {
    {80, 255, 120, 255}, {255, 70, 80, 255}, {255, 210, 40, 255},
    {60, 220, 255, 255}, {255, 90, 230, 255}, {255, 255, 255, 255},
};
} // namespace

Overlay* Overlay::s_instance = nullptr;

// ============================================================ CircleLayer

CircleLayer* CircleLayer::create() {
    auto ret = new CircleLayer();
    if (ret->init()) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

bool CircleLayer::init() {
    if (!CCNode::init()) return false;
    m_draw = CCDrawNode::create();
    m_draw->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
    addChild(m_draw);
    return true;
}

void CircleLayer::redraw() {
    auto& s = Session::get();
    m_draw->clear();
    std::size_t li = 0;
    for (auto const& c : s.project.circles.circles()) {
        if (!c.visible) continue;
        bool sel = s.selectedCircle && *s.selectedCircle == c.id;
        bool linkedToSel = s.selected && c.linkedInput && *c.linkedInput == *s.selected;
        auto line = cfg::toF(c.color);
        auto fill = cfg::toF(c.color, c.fillAlpha);
        if (c.glow) { // soft halo: two wider, faint rings
            m_draw->drawCircle({c.x, c.y}, c.radius + c.thickness * 1.5f, rgba4(0, 0, 0, 0), c.thickness * 2.0f,
                               cfg::toF(c.color, 0.18f), 48);
            m_draw->drawCircle({c.x, c.y}, c.radius + c.thickness * 0.8f, rgba4(0, 0, 0, 0), c.thickness * 1.2f,
                               cfg::toF(c.color, 0.35f), 48);
        }
        m_draw->drawCircle({c.x, c.y}, c.radius, fill, c.thickness, line, 48);
        if (sel || linkedToSel) {
            m_draw->drawCircle({c.x, c.y}, c.radius + c.thickness + 2.f, rgba4(0, 0, 0, 0), 0.6f,
                               sel ? rgba4(1, 1, 1, 0.95f) : rgba4(1, 1, 1, 0.45f), 48);
        }
        if (!c.label.empty()) {
            if (li >= m_labels.size()) {
                auto l = CCLabelBMFont::create("", "chatFont.fnt");
                l->setScale(0.32f);
                addChild(l, 2);
                m_labels.push_back(l);
            }
            auto l = m_labels[li++];
            l->setString(c.label.c_str());
            l->setPosition({c.x, c.y + c.radius + 5.f});
            l->setColor({c.color.r, c.color.g, c.color.b});
            l->setVisible(true);
        }
    }
    for (; li < m_labels.size(); ++li) m_labels[li]->setVisible(false);
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

bool Overlay::init(PlayLayer* pl) {
    if (!CCLayer::init()) return false;
    m_pl = pl;
    setID("frame-window-lab-overlay"_spr);

    m_draw = CCDrawNode::create();
    m_draw->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
    addChild(m_draw);

    m_info = CCLabelBMFont::create("", "chatFont.fnt");
    m_info->setAnchorPoint({0, 0});
    m_info->setScale(0.42f);
    addChild(m_info, 2);

    m_status = CCLabelBMFont::create("", "chatFont.fnt");
    m_status->setAnchorPoint({0, 0});
    m_status->setScale(0.42f);
    addChild(m_status, 2);

    m_tooltip = CCLabelBMFont::create("", "chatFont.fnt");
    m_tooltip->setAnchorPoint({0.5f, 0});
    m_tooltip->setScale(0.38f);
    addChild(m_tooltip, 3);

    m_menu = CCMenu::create();
    m_menu->setPosition({0, 0});
    addChild(m_menu, 2);
    struct B { char const* key; char const* text; SEL_MenuHandler fn; };
    B defs[] = {
        {"rec", "REC", menu_selector(Overlay::onRec)},       {"play", "PLAY", menu_selector(Overlay::onPlay)},
        {"pause", "PAUSE", menu_selector(Overlay::onPause)}, {"back", "<", menu_selector(Overlay::onStepBack)},
        {"step", ">", menu_selector(Overlay::onStep)},       {"test", "TEST", menu_selector(Overlay::onTest)},
        {"sweep", "SWEEP", menu_selector(Overlay::onSweep)}, {"stop", "STOP", menu_selector(Overlay::onStop)},
        {"mode", "1", menu_selector(Overlay::onMode)},       {"circle", "O", menu_selector(Overlay::onCircleTool)},
        {"undo", "UNDO", menu_selector(Overlay::onUndo)},    {"redo", "REDO", menu_selector(Overlay::onRedo)},
        {"save", "SAVE", menu_selector(Overlay::onSave)},    {"load", "LOAD", menu_selector(Overlay::onLoad)},
        {"hb", "HB", menu_selector(Overlay::onHitboxes)},
    };
    for (auto const& d : defs) {
        auto spr = ButtonSprite::create(d.text, "bigFont.fnt", "GJ_button_04.png", 0.6f);
        spr->setScale(0.42f);
        auto item = CCMenuItemSpriteExtra::create(spr, this, d.fn);
        item->setID(d.key);
        m_menu->addChild(item);
        m_buttons[d.key] = item;
    }

    m_circles = CircleLayer::create();
    if (pl->m_objectLayer) pl->m_objectLayer->addChild(m_circles, 9999);

    m_view.pxPerTick = 6;
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

CCRect Overlay::stripRect() const {
    auto win = CCDirector::get()->getWinSize();
    float h = static_cast<float>(cfg::real("timeline-height"));
    return {kMargin, kMargin, win.width - 2 * kMargin, h};
}

void Overlay::layout() {
    auto r = stripRect();
    m_view.width = r.size.width;
    float x = r.getMaxX();
    float y = r.getMaxY() + kButtonRow * 0.5f + 2.f;
    // right-aligned compact button row above the strip
    auto children = m_menu->getChildren();
    for (int i = static_cast<int>(children->count()) - 1; i >= 0; --i) {
        auto item = static_cast<CCNode*>(children->objectAtIndex(i));
        float w = item->getScaledContentSize().width;
        item->setPosition({x - w * 0.5f, y});
        x -= w + 2.f;
    }
    m_info->setPosition({r.getMinX(), r.getMaxY() + 2.f});
    m_status->setPosition({r.getMinX(), r.getMaxY() + kButtonRow + 4.f});
}

bool Overlay::overStrip(CCPoint p) const { return stripRect().containsPoint(p); }

bool Overlay::overButtons(CCPoint p) {
    for (auto const& [k, item] : m_buttons) {
        auto box = item->boundingBox();
        if (box.containsPoint(m_menu->convertToNodeSpace(convertToWorldSpace(p)))) return true;
    }
    return false;
}

CCPoint Overlay::toWorld(CCTouch* t) const {
    return m_pl->m_objectLayer->convertToNodeSpace(t->getLocation());
}

std::optional<std::uint64_t> Overlay::markerAt(float localX) const {
    auto& s = Session::get();
    std::optional<std::uint64_t> best;
    float bestD = 5.f;
    for (auto const& e : s.project.sequence.events()) {
        float d = std::abs(m_view.tickToX(static_cast<double>(e.tick)) - localX);
        bool prefer = s.selected && *s.selected == e.id; // keep grabbing the selected one when overlapping
        if (d < bestD || (prefer && d <= bestD)) {
            bestD = d;
            best = e.id;
        }
    }
    return best;
}

CCLabelBMFont* Overlay::tickLabel(std::size_t i) {
    while (m_tickLabels.size() <= i) {
        auto l = CCLabelBMFont::create("", "chatFont.fnt");
        l->setScale(0.3f);
        l->setAnchorPoint({0.5f, 1.f});
        addChild(l, 1);
        m_tickLabels.push_back(l);
    }
    return m_tickLabels[i];
}

void Overlay::update(float) {
    auto& s = Session::get();
    setVisible(s.overlayVisible);
    m_circles->setVisible(s.overlayVisible);
    if (!s.overlayVisible) return;
    if (s.followPlayhead && m_drag == Drag::None) m_view.ensureVisible(s.tick());
    redraw();
    m_circles->redraw();
    updateLabels();
    updateButtons();
}

void Overlay::redraw() {
    auto& s = Session::get();
    auto r = stripRect();
    m_view.width = r.size.width;
    m_draw->clear();

    float x0 = r.getMinX(), y0 = r.getMinY(), w = r.size.width, h = r.size.height;
    float labelH = h * 0.2f, resultH = h * 0.16f;
    float markTop = y0 + h - labelH - resultH;
    auto X = [&](double t) { return x0 + m_view.tickToX(t); };
    auto inside = [&](float x) { return x >= x0 - 0.5f && x <= x0 + w + 0.5f; };

    // background
    m_draw->drawRect({x0, y0}, {x0 + w, y0 + h}, rgba4(0, 0, 0, static_cast<float>(cfg::real("timeline-opacity"))), 0.5f,
                     rgba4(1, 1, 1, 0.25f));

    // grid
    Tick first = static_cast<Tick>(std::floor(m_view.startTick));
    Tick last = static_cast<Tick>(std::ceil(m_view.startTick + m_view.visibleTicks()));
    Tick g = m_view.gridStep(5.f), L = m_view.labelStep(48.f);
    std::size_t li = 0;
    for (Tick t = (first / g) * g; t <= last; t += g) {
        if (t < 0) continue;
        float x = X(static_cast<double>(t));
        if (!inside(x)) continue;
        bool major = t % L == 0;
        m_draw->drawSegment({x, y0}, {x, markTop}, major ? 0.35f : 0.2f, rgba4(1, 1, 1, major ? 0.45f : 0.18f));
        if (major) {
            auto lab = tickLabel(li++);
            lab->setString(std::to_string(t).c_str());
            lab->setPosition({x, y0 + h - 1.f});
            lab->setVisible(true);
        }
    }
    for (; li < m_tickLabels.size(); ++li) m_tickLabels[li]->setVisible(false);

    // measured results band for the selected input
    if (auto ms = s.selectedMeasurement()) {
        bool stale = s.measurementStale(*s.selected);
        auto pass = col(cfg::rgba("color-pass"), stale ? 0.35f : 1.f);
        auto fail = col(cfg::rgba("color-fail"), stale ? 0.35f : 1.f);
        auto grey = rgba4(0.6f, 0.6f, 0.6f, stale ? 0.2f : 0.5f);
        float ry0 = markTop, ry1 = markTop + resultH;
        for (auto const& [t, out] : ms->byTick) {
            float a = X(static_cast<double>(t) - 0.5), b = X(static_cast<double>(t) + 0.5);
            if (b < x0 || a > x0 + w) continue;
            a = std::max(a, x0);
            b = std::min(b, x0 + w);
            auto c = out == Outcome::Pass ? pass : out == Outcome::Fail ? fail : grey;
            if (out == Outcome::Blocked) c = rgba4(0.2f, 0.2f, 0.2f, 0.5f);
            m_draw->drawRect({a, ry0}, {b, ry1}, c, 0, c);
        }
        // highlight each passing window segment
        auto rep = ms->report();
        for (auto const& seg : rep.segments) {
            float a = X(static_cast<double>(ms->anchor + seg.range.lo) - 0.5);
            float b = X(static_cast<double>(ms->anchor + seg.range.hi) + 0.5);
            if (b < x0 || a > x0 + w) continue;
            m_draw->drawRect({std::max(a, x0), y0}, {std::min(b, x0 + w), ry1}, col(cfg::rgba("color-pass"), 0.08f),
                             0.6f, col(cfg::rgba("color-pass"), seg.exact() ? 0.9f : 0.4f));
        }
        float ax = X(static_cast<double>(ms->anchor));
        if (inside(ax)) m_draw->drawSegment({ax, ry0}, {ax, ry1}, 0.6f, rgba4(1, 0.85f, 0.2f, 1));
    }

    // goal marker
    auto goal = s.effectiveGoal();
    if (goal.kind == Goal::Kind::SurviveUntilTick) {
        float gx = X(static_cast<double>(goal.tick));
        if (inside(gx))
            m_draw->drawSegment({gx, y0}, {gx, markTop}, s.project.hasGoal ? 0.7f : 0.4f,
                                col(cfg::rgba("color-goal"), s.project.hasGoal ? 1.f : 0.5f));
    }

    // input markers: presses tall, releases short, so they never hide each other
    auto press = col(cfg::rgba("color-press")), release = col(cfg::rgba("color-release"));
    for (auto const& e : s.project.sequence.events()) {
        float x = X(static_cast<double>(e.tick));
        if (!inside(x)) continue;
        bool sel = s.selected && *s.selected == e.id;
        float top = y0 + (markTop - y0) * (e.down ? 0.85f : 0.55f);
        float bottom = y0 + (e.player2 ? (markTop - y0) * 0.1f : 0.f) + 1.f;
        auto c = e.down ? press : release;
        if (sel) m_draw->drawSegment({x, bottom}, {x, top}, 2.2f, rgba4(1, 1, 1, 0.9f));
        m_draw->drawSegment({x, bottom}, {x, top}, sel ? 1.4f : 0.8f, c);
        if (sel) { // drag handle
            m_draw->drawRect({x - 2.5f, top - 2.5f}, {x + 2.5f, top + 2.5f}, c, 0.5f, rgba4(1, 1, 1, 1));
        }
        if (e.button != Button::Jump) // left/right (platformer) marked with a small arrow
            m_draw->drawSegment({x, top}, {x + (e.button == Button::Left ? -3.f : 3.f), top - 2.f}, 0.4f, c);
    }

    // playhead
    float px = X(static_cast<double>(s.tick()));
    if (inside(px)) {
        auto ph = col(cfg::rgba("color-playhead"));
        m_draw->drawSegment({px, y0}, {px, y0 + h}, 0.5f, ph);
        CCPoint tri[3] = {{px - 3.f, y0 + h}, {px + 3.f, y0 + h}, {px, y0 + h - 4.f}};
        m_draw->drawPolygon(tri, 3, ph, 0, ph);
    }

    // focus outline
    if (m_focus) m_draw->drawRect({x0, y0}, {x0 + w, y0 + h}, rgba4(0, 0, 0, 0), 0.6f, rgba4(1, 1, 1, 0.6f));
}

void Overlay::updateLabels() {
    auto& s = Session::get();
    std::string info = fmt::format("{} tick {} ({:.3f}s)", modeName(s.mode), s.tick(), ticksToMs(s.tick()) / 1000.0);
    if (s.paused) info += " | PAUSED";
    if (s.progress.active) info += fmt::format(" | {} ({} trials)", s.progress.phase, s.progress.done);
    if (s.selected) {
        if (auto e = s.project.sequence.find(*s.selected)) {
            info += fmt::format(" | #{} {} {}{} @{}", e->id, e->down ? "press" : "release", buttonName(e->button),
                                e->player2 ? " P2" : "", e->tick);
            if (auto ms = s.selectedMeasurement()) {
                info += " (" + sgn(e->tick - ms->anchor) + ")";
                auto rep = ms->report();
                info += " | " + (s.measurementStale(*s.selected) ? std::string("STALE: ") : std::string()) + rep.summary();
            }
        }
    }
    info += fmt::format(" | {}", s.shiftMode == ShiftMode::Ripple ? "ripple" : "single");
    auto g = s.effectiveGoal();
    info += g.kind == Goal::Kind::CompleteLevel ? " | goal: complete" : fmt::format(" | goal: tick {}", g.tick);
    if (!s.project.hasGoal) info += "(auto)";
    if (s.diag.tickJumps || s.diag.lateInputs)
        info += fmt::format(" | WARN jumps {} late {}", s.diag.tickJumps, s.diag.lateInputs);
    if (!s.diag.lockedThisRun && s.mode == Mode::Recording) info += " | unlocked";
    m_info->setString(info.c_str());
    m_info->limitLabelWidth(stripRect().size.width, 0.42f, 0.2f);

    m_status->setString(s.status.c_str());
    m_status->setColor(s.statusIsError ? ccColor3B{255, 110, 110} : ccColor3B{255, 255, 255});
    m_status->limitLabelWidth(stripRect().size.width * 0.55f, 0.42f, 0.2f);

    // tooltip with exact tick/time/marker/result under the mouse
    auto mouse = convertToNodeSpace(geode::cocos::getMousePos());
    auto r = stripRect();
    if (r.containsPoint(mouse)) {
        float lx = mouse.x - r.getMinX();
        Tick t = m_view.xToTick(lx);
        std::string tip = fmt::format("tick {} | {:.2f} ms", t, ticksToMs(t));
        if (auto id = markerAt(lx))
            if (auto e = s.project.sequence.find(*id))
                tip = fmt::format("#{} {} {} @ tick {} ({:.2f} ms)", e->id, e->down ? "press" : "release",
                                  buttonName(e->button), e->tick, ticksToMs(e->tick));
        if (auto ms = s.selectedMeasurement())
            if (auto it = ms->byTick.find(t); it != ms->byTick.end())
                tip += fmt::format(" | {} {}", sgn(t - ms->anchor), outcomeName(it->second));
        m_tooltip->setString(tip.c_str());
        m_tooltip->setPosition({std::clamp(mouse.x, r.getMinX() + 60.f, r.getMaxX() - 60.f), r.getMaxY() + kButtonRow + 14.f});
        m_tooltip->setVisible(true);
    } else {
        m_tooltip->setVisible(false);
    }
}

void Overlay::updateButtons() {
    auto& s = Session::get();
    auto tint = [&](char const* k, bool on, ccColor3B c = {120, 255, 140}) {
        auto spr = static_cast<ButtonSprite*>(m_buttons[k]->getNormalImage());
        spr->setColor(on ? c : ccColor3B{255, 255, 255});
    };
    tint("rec", s.mode == Mode::Recording, {255, 90, 90});
    tint("play", s.mode == Mode::Playback);
    tint("pause", s.paused, {255, 220, 90});
    tint("test", s.progress.active && !s.project.sequence.empty());
    tint("circle", s.circleTool, {255, 200, 60});
    tint("hb", s.hitboxes);
    tint("mode", s.shiftMode == ShiftMode::Ripple, {120, 200, 255});
}

// ------------------------------------------------------------ touches

bool Overlay::ccTouchBegan(CCTouch* touch, CCEvent*) {
    auto& s = Session::get();
    if (!s.overlayVisible || !isVisible()) return false;
    auto p = convertToNodeSpace(touch->getLocation());
    if (overButtons(p)) return false; // let the CCMenu take it

    if (overStrip(p)) {
        m_focus = true;
        float lx = p.x - stripRect().getMinX();
        m_dragLastX = lx;
        auto kb = CCKeyboardDispatcher::get();
        if (kb->getAltKeyPressed()) {
            s.setGoalTick(m_view.xToTick(lx));
            return true;
        }
        if (auto id = markerAt(lx)) {
            s.select(id);
            s.beginDrag();
            m_drag = Drag::Marker;
        } else {
            m_drag = Drag::Pan;
            s.followPlayhead = false;
        }
        return true;
    }

    m_focus = false;
    if (s.circleTool && m_pl->m_objectLayer) {
        auto w = toWorld(touch);
        m_dragStartWorld = w;
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
    float lx = p.x - stripRect().getMinX();
    switch (m_drag) {
        case Drag::Marker:
            s.dragTo(m_view.xToTick(std::clamp(lx, 0.f, m_view.width))); // snapped to whole ticks
            break;
        case Drag::Pan:
            m_view.scrollPx(m_dragLastX - lx);
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

void Overlay::ccTouchEnded(CCTouch*, CCEvent*) {
    if (m_drag == Drag::Marker) Session::get().endDrag();
    m_drag = Drag::None;
}

void Overlay::ccTouchCancelled(CCTouch* t, CCEvent* e) { ccTouchEnded(t, e); }

// ------------------------------------------------------------ wheel & keys

bool Overlay::onScroll(float y, float) {
    auto& s = Session::get();
    if (!s.overlayVisible) return false;
    auto mouse = convertToNodeSpace(geode::cocos::getMousePos());
    auto kb = CCKeyboardDispatcher::get();
    if (overStrip(mouse)) {
        float lx = mouse.x - stripRect().getMinX();
        if (kb->getShiftKeyPressed()) {
            m_view.scrollPx(y > 0 ? 40.f : -40.f);
            s.followPlayhead = false;
        } else {
            m_view.zoomAround(lx, y > 0 ? 1 / 1.25 : 1.25);
        }
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
    if (key == KEY_F1 && !repeat) {
        s.overlayVisible = !s.overlayVisible;
        return true;
    }
    if (!s.overlayVisible) return false;

    switch (key) {
        case KEY_F2: if (!repeat) s.hitboxes = !s.hitboxes; return true;
        case KEY_F3: if (!repeat) s.toggleRecording(); return true;
        case KEY_F4: if (!repeat) s.togglePlayback(); return true;
        case KEY_F5: if (!repeat) s.testSelected(); return true;
        case KEY_F6: if (!repeat) s.sweepSelected(); return true;
        case KEY_F7: if (!repeat) s.circleTool = !s.circleTool; return true;
        case KEY_F8: if (!repeat) s.generateCircles(); return true;
        case KEY_F9: if (!repeat) s.togglePause(); return true;
        case KEY_F10: if (!repeat) s.stopAll("Stopped."); return true;
        case KEY_OEMPeriod: s.stepForward(shift ? 10 : 1); return true;
        case KEY_OEMComma: s.stepBack(shift ? 10 : 1); return true;
        default: break;
    }
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
            int k = static_cast<int>(key);
            if (k >= KEY_One && k <= KEY_Six) {
                c->color = kPalette[k - KEY_One];
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
                        s.setStatus(fmt::format("Circle linked to input #{}.", *s.selected));
                    }
                    return true;
                case KEY_Delete:
                case KEY_Backspace: s.deleteSelected(); return true;
                default: break;
            }
        }
    }

    // timeline editing keys need focus (click the timeline) and no live recording
    if (!m_focus || s.mode == Mode::Recording) return false;
    switch (key) {
        case KEY_Left:
        case KEY_Right: {
            Tick d = key == KEY_Left ? -1 : 1;
            auto prev = s.shiftMode;
            if (ctrl) s.shiftMode = ShiftMode::Ripple;
            s.shiftSelected(d);
            s.shiftMode = prev;
            if (s.selected)
                if (auto e = s.project.sequence.find(*s.selected)) m_view.ensureVisible(e->tick);
            return true;
        }
        case KEY_Up: s.selectNeighbour(-1); return true;
        case KEY_Down: s.selectNeighbour(1); return true;
        case KEY_M: if (!repeat) s.toggleShiftMode(); return true;
        case KEY_F: if (!repeat) s.followPlayhead = !s.followPlayhead; return true;
        case KEY_G:
            if (repeat) return true;
            if (shift) s.setGoalComplete();
            else s.setGoalTick(s.tick());
            return true;
        case KEY_Delete:
        case KEY_Backspace: s.deleteSelected(); return true;
        case KEY_PageUp: m_view.zoomAround(m_view.tickToX(static_cast<double>(s.tick())), 1.25); return true;
        case KEY_PageDown: m_view.zoomAround(m_view.tickToX(static_cast<double>(s.tick())), 1 / 1.25); return true;
        case KEY_Escape: m_focus = false; return false; // let GD open its pause menu too
        default: break;
    }
    int raw = static_cast<int>(key);
    if (key == KEY_OEMPlus || raw == 0xBB) { m_view.zoomAround(m_view.width / 2, 1.25); return true; }
    if (key == KEY_OEMMinus) { m_view.zoomAround(m_view.width / 2, 1 / 1.25); return true; }
    (void)alt;
    return false;
}

// ------------------------------------------------------------ buttons

void Overlay::onRec(CCObject*) { Session::get().toggleRecording(); }
void Overlay::onPlay(CCObject*) { Session::get().togglePlayback(); }
void Overlay::onPause(CCObject*) { Session::get().togglePause(); }
void Overlay::onStepBack(CCObject*) { Session::get().stepBack(1); }
void Overlay::onStep(CCObject*) { Session::get().stepForward(1); }
void Overlay::onTest(CCObject*) { Session::get().testSelected(); }
void Overlay::onSweep(CCObject*) { Session::get().sweepSelected(); }
void Overlay::onStop(CCObject*) { Session::get().stopAll("Stopped."); }
void Overlay::onMode(CCObject*) { Session::get().toggleShiftMode(); }
void Overlay::onCircleTool(CCObject*) {
    auto& s = Session::get();
    s.circleTool = !s.circleTool;
    s.setStatus(s.circleTool ? "Circle tool: click to place, drag to size, Shift+drag to resize, 1-6 colour."
                             : "Circle tool off.");
}
void Overlay::onUndo(CCObject*) { Session::get().undo(); }
void Overlay::onRedo(CCObject*) { Session::get().redo(); }
void Overlay::onSave(CCObject*) { Session::get().save(); }
void Overlay::onLoad(CCObject*) { Session::get().load(); }
void Overlay::onHitboxes(CCObject*) {
    auto& s = Session::get();
    s.hitboxes = !s.hitboxes;
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
