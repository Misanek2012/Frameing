// Frame Window Lab — on-screen overlay: timeline, controls, circles.
#pragma once

#include "../../core/Project.hpp"

#include <Geode/Geode.hpp>

namespace fwl::gd {

// Draws project circles in level coordinates (child of the object layer, so it
// follows the camera exactly like the level does).
class CircleLayer : public cocos2d::CCNode {
public:
    static CircleLayer* create();
    void redraw();

private:
    bool init() override;
    cocos2d::CCDrawNode* m_draw = nullptr;
    std::vector<cocos2d::CCLabelBMFont*> m_labels;
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
    cocos2d::CCRect stripRect() const;
    bool overStrip(cocos2d::CCPoint p) const;
    bool overButtons(cocos2d::CCPoint p);
    std::optional<std::uint64_t> markerAt(float localX) const;
    cocos2d::CCPoint toWorld(cocos2d::CCTouch* t) const;
    cocos2d::CCLabelBMFont* tickLabel(std::size_t i);

    // button callbacks
    void onRec(cocos2d::CCObject*);
    void onPlay(cocos2d::CCObject*);
    void onPause(cocos2d::CCObject*);
    void onStepBack(cocos2d::CCObject*);
    void onStep(cocos2d::CCObject*);
    void onTest(cocos2d::CCObject*);
    void onSweep(cocos2d::CCObject*);
    void onStop(cocos2d::CCObject*);
    void onCircleTool(cocos2d::CCObject*);
    void onMode(cocos2d::CCObject*);
    void onUndo(cocos2d::CCObject*);
    void onRedo(cocos2d::CCObject*);
    void onSave(cocos2d::CCObject*);
    void onLoad(cocos2d::CCObject*);
    void onHitboxes(cocos2d::CCObject*);

    static Overlay* s_instance;
    PlayLayer* m_pl = nullptr;
    TimelineView m_view;
    cocos2d::CCDrawNode* m_draw = nullptr;
    cocos2d::CCLabelBMFont* m_info = nullptr;
    cocos2d::CCLabelBMFont* m_status = nullptr;
    cocos2d::CCLabelBMFont* m_tooltip = nullptr;
    std::vector<cocos2d::CCLabelBMFont*> m_tickLabels;
    cocos2d::CCMenu* m_menu = nullptr;
    std::map<std::string, CCMenuItemSpriteExtra*> m_buttons;
    CircleLayer* m_circles = nullptr;

    enum class Drag { None, Marker, Pan, CircleMove, CircleResize };
    Drag m_drag = Drag::None;
    float m_dragLastX = 0;
    cocos2d::CCPoint m_grabOffset;
    cocos2d::CCPoint m_dragStartWorld;
    bool m_focus = false; // timeline has keyboard focus
};

} // namespace fwl::gd
