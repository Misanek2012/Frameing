// Hitboxes: drives GD's own debug-draw renderer (GJBaseGameLayer::updateDebugDraw),
// which draws the game's real collision shapes (oriented boxes, circles, slopes).
// Outside practice mode PlayLayer::shouldDebugDraw() is false, so we call it ourselves.
//
// Styling: while updateDebugDraw runs we intercept CCDrawNode::drawPolygon/drawCircle
// and classify each shape:
//   * player  — geometry matches a player's object rect (exact geometric test)
//   * hazard / solid / special — by the colour GD itself chose (heuristic: dominant channel)
//   * other   — anything unclassified (never silently dropped unless toggled off)
#include "Session.hpp"
#include "Settings.hpp"

#include <Geode/modify/CCDrawNode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

using namespace geode::prelude;
using fwl::gd::Session;
namespace cfg = fwl::gd::cfg;

namespace {

enum class Cat { Player, Hazard, Solid, Special, Other, Count };

struct Style {
    bool enabled[static_cast<int>(Cat::Count)]{};
    ccColor4F color[static_cast<int>(Cat::Count)]{};
    float thickness = 0.5f;
    float fill = 0.12f;
};

bool g_inDebugDraw = false;
Style g_style;
CCRect g_playerRects[2];
int g_playerCount = 0;

void loadStyle() {
    static constexpr char const* keys[] = {"player", "hazard", "solid", "special", "other"};
    for (int i = 0; i < static_cast<int>(Cat::Count); ++i) {
        g_style.enabled[i] = cfg::flag(std::string("hb-") + keys[i]);
        g_style.color[i] = cfg::toF(cfg::rgba(std::string("hb-color-") + keys[i]));
    }
    g_style.thickness = static_cast<float>(cfg::real("hb-thickness"));
    g_style.fill = static_cast<float>(cfg::real("hb-fill"));
}

bool matchesPlayer(CCRect const& box) {
    for (int i = 0; i < g_playerCount; ++i) {
        auto const& r = g_playerRects[i];
        float dcx = (box.getMidX() - r.getMidX());
        float dcy = (box.getMidY() - r.getMidY());
        // Player outer box, or the smaller inner box centred on the player.
        if (std::abs(dcx) < 0.75f && std::abs(dcy) < 0.75f && box.size.width <= r.size.width + 1.5f &&
            box.size.height <= r.size.height + 1.5f)
            return true;
    }
    return false;
}

Cat classifyColor(ccColor4F const& fill, ccColor4F const& border) {
    auto const& c = border.a > 0.01f ? border : fill;
    float hi = std::max({c.r, c.g, c.b});
    if (hi < 0.2f) return Cat::Other;
    bool r = c.r >= hi - 0.15f, g = c.g >= hi - 0.15f, b = c.b >= hi - 0.15f;
    if (r && !g && !b) return Cat::Hazard;
    if (b && !r && !g) return Cat::Solid;
    if (g && !r) return Cat::Special;
    if (r && g && !b) return Cat::Special; // yellow/orange interaction shapes
    return Cat::Other;
}

CCRect boundsOf(CCPoint const* v, unsigned n) {
    float minX = v[0].x, maxX = v[0].x, minY = v[0].y, maxY = v[0].y;
    for (unsigned i = 1; i < n; ++i) {
        minX = std::min(minX, v[i].x);
        maxX = std::max(maxX, v[i].x);
        minY = std::min(minY, v[i].y);
        maxY = std::max(maxY, v[i].y);
    }
    return {minX, minY, maxX - minX, maxY - minY};
}

} // namespace

class $modify(FWLDebugDrawLayer, GJBaseGameLayer) {
    void updateDebugDraw() {
        auto& s = Session::get();
        bool mine = s.isLayer(this) && s.hitboxes;
        if (mine) {
            loadStyle();
            g_playerCount = 0;
            for (PlayerObject* p : {m_player1, m_player2})
                if (p && g_playerCount < 2) g_playerRects[g_playerCount++] = p->getObjectRect();
        }
        g_inDebugDraw = mine;
        GJBaseGameLayer::updateDebugDraw();
        g_inDebugDraw = false;
    }
};

class $modify(FWLDrawNode, CCDrawNode) {
    bool drawPolygon(CCPoint* verts, unsigned int count, ccColor4F const& fill, float borderWidth,
                     ccColor4F const& border, BorderAlignment alignment) {
        if (!g_inDebugDraw || count == 0) return CCDrawNode::drawPolygon(verts, count, fill, borderWidth, border, alignment);
        auto cat = matchesPlayer(boundsOf(verts, count)) ? Cat::Player : classifyColor(fill, border);
        auto i = static_cast<int>(cat);
        if (!g_style.enabled[i]) return true;
        auto line = g_style.color[i];
        auto inner = line;
        inner.a *= g_style.fill;
        return CCDrawNode::drawPolygon(verts, count, inner, g_style.thickness, line, alignment);
    }

    bool drawCircle(CCPoint const& center, float radius, ccColor4F const& fill, float borderWidth,
                    ccColor4F const& border, unsigned int segments) {
        if (!g_inDebugDraw) return CCDrawNode::drawCircle(center, radius, fill, borderWidth, border, segments);
        CCRect box{center.x - radius, center.y - radius, radius * 2, radius * 2};
        auto cat = matchesPlayer(box) ? Cat::Player : classifyColor(fill, border);
        auto i = static_cast<int>(cat);
        if (!g_style.enabled[i]) return true;
        auto line = g_style.color[i];
        auto inner = line;
        inner.a *= g_style.fill;
        return CCDrawNode::drawCircle(center, radius, inner, g_style.thickness, line, segments);
    }
};

class $modify(FWLHitboxPlayLayer, PlayLayer) {
    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        auto& s = Session::get();
        if (!s.isLayer(this) || !m_debugDrawNode) return;
        if (s.hitboxes) {
            if (!shouldDebugDraw()) { // the game will not draw them itself
                bool was = m_isDebugDrawEnabled;
                m_isDebugDrawEnabled = true;
                m_debugDrawNode->setVisible(true);
                updateDebugDraw();
                m_isDebugDrawEnabled = was;
            }
        } else if (!shouldDebugDraw()) {
            m_debugDrawNode->clear();
        }
    }
};
