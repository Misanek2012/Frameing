// Game hooks: tick control, input recording/injection, deaths, resets.
// Every hooked function and member below was checked against
// geode-sdk/bindings bindings/2.2081/GeometryDash.bro (Windows addresses present).
#include "Session.hpp"
#include "ui/Overlay.hpp"

#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

using namespace geode::prelude;
using fwl::gd::Session;

class $modify(FWLBaseGameLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        // Run before other mods so recording sees every real press and so
        // injected presses are not filtered by input-altering mods.
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", Priority::VeryEarly);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto& s = Session::get();
        if (s.isLayer(this) && !s.passLiveButton(down, button, isPlayer1)) return;
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    // Called once per physics step.
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto& s = Session::get();
        bool mine = s.isLayer(this);
        if (mine) s.onBeforeStep();
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        if (mine) s.onAfterStep();
    }

    // Locked mode: run N single-tick updates per rendered frame.
    void update(float dt) {
        auto& s = Session::get();
        if (!s.isLayer(this) || !s.controlsTicks()) return GJBaseGameLayer::update(dt);
        int n = s.ticksForFrame(dt);
        s.inLockedUpdate = true;
        for (int i = 0; i < n; ++i) {
            GJBaseGameLayer::update(static_cast<float>(s.stepSize()));
            if (s.breakLoop || !s.isLayer(this)) break;
        }
        s.inLockedUpdate = false;
    }

    // In locked mode exactly one physics step happens per update() call.
    double getModifiedDelta(float dt) {
        auto& s = Session::get();
        if (!s.inLockedUpdate || !s.isLayer(this) || m_resumeTimer > 0) return GJBaseGameLayer::getModifiedDelta(dt);
        m_extraDelta = 0.0;
        return s.stepSize();
    }
};

class $modify(FWLPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        // See deaths/completions before noclip-style mods can swallow them.
        (void)self.setHookPriority("PlayLayer::destroyPlayer", Priority::VeryEarly);
        (void)self.setHookPriority("PlayLayer::levelComplete", Priority::VeryEarly);
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        Session::get().attach(this);
        fwl::gd::Overlay::install(this);
        return true;
    }

    void onQuit() {
        Session::get().detach(this);
        PlayLayer::onQuit();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        auto& s = Session::get();
        if (s.isLayer(this)) s.onReset();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        auto& s = Session::get();
        if (s.isLayer(this) && s.onDeath(player, object)) return;
        PlayLayer::destroyPlayer(player, object);
    }

    void levelComplete() {
        auto& s = Session::get();
        if (s.isLayer(this) && s.onComplete()) return;
        PlayLayer::levelComplete();
    }
};
