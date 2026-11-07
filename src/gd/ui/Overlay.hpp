// Frame Window Lab — on-screen overlay in the style of NaN's frame window counter:
// bottom timeline, lettered tool buttons, Frame/Input counter, player path and
// circle indicators drawn over the level.
#pragma once

#include "../../core/Project.hpp"

#include <Geode/Geode.hpp>

namespace fwl::gd {

// Everything drawn in level coordinates (child of the object layer, so it
// follows the camera exactly like the level): player path, input dots,
// circle indicators and the user's own circles.
class LevelLayer : public cocos2d::CCNode {
public:
    static LevelLayer* create();
    void redraw();

private:
    bool init() override;
    void drawPath();
    void drawIndicators();
    cocos2d::CCLabelBMFont* label(std::size_t i);

    cocos2d::CCDrawNode* m_path = nullptr;  // rebuilt only when the path changes
    cocos2d::CCDrawNode* m_draw = nullptr;  // indicators + circles, every frame
    std::vector<cocos2d::CCLabelBMFont*> m_labels;
    int m_pathVersion = -1;
    std::size_t m_pathDrawn = 0;
};

class Overlay : public cocos2d::CCLayer {
public:
    static void install(PlayLayer* pl);
    static Overlay* get() { return s_instance; }

    bool onKey(cocos2d::enumKeyCodes key, bool repeat, bool shift, bool ctrl, bool alt);
    bool onScroll(float y, float x);

private:
    bool init(PlayLayer* pl);
    void onEnter() override;
    void onExit() override;
    void update(float dt) override;
    void registerWithTouchDispatcher() override;
    bool ccTouchBegan(cocos2d::CCTouch* touch, cocos2d::CCEvent* ev) override;
    void ccTouchMoved(cocos2d::CCTouch* touch, cocos2d::CCEvent* ev) override;
    void ccTouchEnded(cocos2d::CCTouch* touch, cocos2d::CCEvent* ev) override;
    void ccTouchCancelled(cocos2d::CCTouch* touch, cocos2d::CCEvent* ev) override;

    void layout();
    void redraw();
    void updateLabels();
    void updateButtons();
    cocos2d::CCRect panelRect() const; // whole timeline panel
    float baselineY() const;
    bool overPanel(cocos2d::CCPoint p) const;
    bool overButtons(cocos2d::CCPoint p);
    std::optional<std::uint64_t> markerAt(cocos2d::CCPoint p) const;
    cocos2d::CCPoint toWorld(cocos2d::CCTouch* t) const;
    cocos2d::CCLabelBMFont* pooled(std::vector<cocos2d::CCLabelBMFont*>& pool, std::size_t i, float scale,
                                   cocos2d::CCPoint anchor);
    bool editKeysActive() const;
    void fitAll();
    void syncCursor(bool restore = false);
    bool m_cursorForced = false;

    void addButton(char const* key, char const* text, char const* bg, cocos2d::ccColor3B textColor,
                   cocos2d::SEL_MenuHandler fn, bool right);
    void onCI(cocos2d::CCObject*);
    void onRec(cocos2d::CCObject*);
    void onHitboxes(cocos2d::CCObject*);
    void onCircleTool(cocos2d::CCObject*);
    void onPlay(cocos2d::CCObject*);
    void onStepBack(cocos2d::CCObject*);
    void onStep(cocos2d::CCObject*);
    void onAdd(cocos2d::CCObject*);
    void onTest(cocos2d::CCObject*);
    void onAnalyze(cocos2d::CCObject*);
    void onSlow(cocos2d::CCObject*);

    static Overlay* s_instance;
    PlayLayer* m_pl = nullptr;
    TimelineView m_view;
    cocos2d::CCDrawNode* m_draw = nullptr;
    cocos2d::CCLabelBMFont* m_hud = nullptr;
    cocos2d::CCLabelBMFont* m_status = nullptr;
    cocos2d::CCLabelBMFont* m_window = nullptr;
    cocos2d::CCLabelBMFont* m_tooltip = nullptr;
    cocos2d::CCLabelBMFont* m_resetLabel = nullptr;
    cocos2d::CCLabelBMFont* m_goalLabel = nullptr;
    std::vector<cocos2d::CCLabelBMFont*> m_tickLabels;
    std::vector<cocos2d::CCLabelBMFont*> m_indexLabels;
    cocos2d::CCMenu* m_menu = nullptr;
    std::vector<std::pair<std::string, CCMenuItemSpriteExtra*>> m_left, m_right;
    LevelLayer* m_level = nullptr;

    enum class Drag { None, Marker, Scrub, Pan, CircleMove, CircleResize };
    Drag m_drag = Drag::None;
    float m_dragStartX = 0;
    float m_dragLastX = 0;
    bool m_dragMoved = false;
    cocos2d::CCPoint m_grabOffset;
    bool m_focus = false; // timeline has keyboard focus
    bool m_userView = false; // the user scrolled/zoomed: don't auto-follow until play
    Tick m_lastTick = -1;
    bool m_fitted = false;
};

} // namespace fwl::gd
