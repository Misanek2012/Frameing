// Frame Window Lab — core types (no Geometry Dash / Geode dependency).
#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace fwl {

// One simulation step. GD 2.2 runs physics at 240 steps per second
// (scaled by time-warp triggers). Rendering FPS is unrelated to this.
using Tick = std::int64_t;
inline constexpr int kTicksPerSecond = 240;

// Values match GD's PlayerButton enum (Jump = 1, Left = 2, Right = 3),
// verified against geode-sdk/bindings include/Geode/Enums.hpp.
enum class Button : std::uint8_t { Jump = 1, Left = 2, Right = 3 };

struct InputEvent {
    std::uint64_t id = 0;
    Tick tick = 0;
    Button button = Button::Jump;
    bool down = true;     // true = press, false = release
    bool player2 = false; // two-player mode second player

    bool operator==(InputEvent const&) const = default;
};

// Minimal status type so the core stays dependency-free.
struct Status {
    bool ok = true;
    std::string error;

    static Status success() { return {}; }
    static Status fail(std::string msg) { return {false, std::move(msg)}; }
    explicit operator bool() const { return ok; }
};

template <class T>
struct Expected {
    bool ok = false;
    T value{};
    std::string error;

    static Expected success(T v) { return {true, std::move(v), {}}; }
    static Expected fail(std::string msg) { return {false, T{}, std::move(msg)}; }
    explicit operator bool() const { return ok; }
};

inline char const* buttonName(Button b) {
    switch (b) {
        case Button::Jump: return "jump";
        case Button::Left: return "left";
        case Button::Right: return "right";
    }
    return "?";
}

inline double ticksToMs(Tick t) { return static_cast<double>(t) * 1000.0 / kTicksPerSecond; }

} // namespace fwl
