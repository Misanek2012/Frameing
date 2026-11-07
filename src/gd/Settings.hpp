// Thin typed accessors for mod.json settings.
#pragma once

#include <Geode/Geode.hpp>
#include "../core/Project.hpp"

namespace fwl::gd::cfg {

inline bool flag(std::string_view key) { return geode::Mod::get()->getSettingValue<bool>(key); }
inline std::int64_t integer(std::string_view key) { return geode::Mod::get()->getSettingValue<std::int64_t>(key); }
inline double real(std::string_view key) { return geode::Mod::get()->getSettingValue<double>(key); }
inline cocos2d::ccColor4B rgba(std::string_view key) { return geode::Mod::get()->getSettingValue<cocos2d::ccColor4B>(key); }

inline cocos2d::ccColor4F toF(cocos2d::ccColor4B c, float alphaMul = 1.f) {
    return {c.r / 255.f, c.g / 255.f, c.b / 255.f, (c.a / 255.f) * alphaMul};
}
inline cocos2d::ccColor4F toF(fwl::RGBA c, float alphaMul = 1.f) {
    return {c.r / 255.f, c.g / 255.f, c.b / 255.f, (c.a / 255.f) * alphaMul};
}
inline fwl::RGBA toCore(cocos2d::ccColor4B c) { return {c.r, c.g, c.b, c.a}; }

} // namespace fwl::gd::cfg
