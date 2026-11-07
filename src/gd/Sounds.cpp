#include "Sounds.hpp"
#include "Settings.hpp"

using namespace geode::prelude;

namespace fwl::gd::sounds {

namespace {
constexpr int kDings = 6;

char const* settingFor(SoundEvent ev) {
    switch (ev) {
        case SoundEvent::Step: return "sound-step";
        case SoundEvent::Select: return "sound-select";
        case SoundEvent::MoveInput: return "sound-move";
        case SoundEvent::CircleCreate: return "sound-circle";
        case SoundEvent::TestPass: return "sound-pass";
        case SoundEvent::TestFail: return "sound-fail";
        case SoundEvent::WindowDone: return "sound-done";
    }
    return nullptr;
}
} // namespace

void ding(int index) {
    if (index < 1 || index > kDings) return;
    if (!cfg::flag("sound-enabled")) return;
    auto volume = static_cast<float>(cfg::real("sound-volume"));
    if (volume <= 0.f) return;
    auto path = Mod::get()->getResourcesDir() / fmt::format("ding-{}.mp3", index);
    if (!std::filesystem::exists(path)) {
        log::warn("missing sound {}", path.string());
        return;
    }
    // playEffect(path, speed, unknown, volume) — signature from bindings 2.2081.
    FMODAudioEngine::get()->playEffect(geode::utils::string::pathToString(path), 1.f, 0.f, volume);
}

void play(SoundEvent ev) {
    auto key = settingFor(ev);
    if (!key) return;
    ding(static_cast<int>(cfg::integer(key)));
}

void count(int passIndex) {
    if (!cfg::flag("sound-count")) return;
    ding(countDingIndex(passIndex, kDings));
}

} // namespace fwl::gd::sounds
