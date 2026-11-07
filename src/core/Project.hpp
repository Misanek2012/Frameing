// Frame Window Lab — circles, timeline view math, project persistence.
#pragma once

#include "InputSequence.hpp"
#include "WindowAnalysis.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fwl {

struct RGBA {
    std::uint8_t r = 255, g = 255, b = 255, a = 255;
    bool operator==(RGBA const&) const = default;
};

// A marker circle drawn in level (world) coordinates.
struct Circle {
    std::uint64_t id = 0;
    float x = 0, y = 0;      // world position (GD object-layer units)
    float radius = 12;
    RGBA color{80, 255, 120, 255};
    float thickness = 2;
    float fillAlpha = 0.15f; // 0..1, multiplied with color alpha for the fill
    bool glow = true;        // soft halo outline
    bool visible = true;
    std::string label;
    // Set for circles generated from measured data. Removing/regenerating a
    // measurement replaces exactly these circles.
    std::optional<std::uint64_t> linkedInput;
    std::optional<Tick> linkedOffset;
    std::optional<Outcome> linkedOutcome;
    bool operator==(Circle const&) const = default;
};

struct CircleStyle {
    float radius = 6;
    RGBA pass{60, 255, 110, 255};
    RGBA fail{255, 60, 70, 255};
    RGBA untrusted{170, 170, 170, 255};
    float thickness = 2;
    float fillAlpha = 0.35f;
};

class CircleSet {
public:
    std::vector<Circle> const& circles() const { return m_circles; }
    std::uint64_t add(Circle c); // assigns id
    Status insertWithId(Circle c);
    bool remove(std::uint64_t id);
    Circle* find(std::uint64_t id);
    Circle const* find(std::uint64_t id) const;
    // Topmost visible circle whose disc (plus `tolerance`) contains the point.
    std::optional<std::uint64_t> hitTest(float x, float y, float tolerance = 4) const;
    void clear() { m_circles.clear(); }
    std::uint64_t nextId() const { return m_nextId; }
    void setNextId(std::uint64_t v) { m_nextId = v; }

    // Replaces circles linked to `inputId` with one circle per tested offset
    // for which the player position at the moment the input fired is known.
    // Positions come from the actual trial replays.
    std::vector<std::uint64_t> generateFromWindow(std::uint64_t inputId, WindowReport const& report,
                                                  std::map<Tick, std::pair<float, float>> const& positions,
                                                  CircleStyle const& style);
    std::size_t removeLinked(std::uint64_t inputId);

private:
    std::vector<Circle> m_circles;
    std::uint64_t m_nextId = 1;
};

// Pure maths for the zoomable/scrollable timeline strip.
class TimelineView {
public:
    double startTick = 0;     // tick at the left edge (may be fractional while panning)
    double pxPerTick = 6;     // zoom
    float width = 400;        // strip width in points

    static constexpr double kMinPxPerTick = 0.05;
    static constexpr double kMaxPxPerTick = 64;

    float tickToX(double tick) const { return static_cast<float>((tick - startTick) * pxPerTick); }
    double xToTickExact(float x) const { return startTick + x / pxPerTick; }
    Tick xToTick(float x) const; // snapped to the nearest tick
    double visibleTicks() const { return width / pxPerTick; }
    void zoomAround(float anchorX, double factor);
    void scrollPx(float dx);
    // Keeps `tick` inside the strip with `marginPx` on each side.
    void ensureVisible(Tick tick, float marginPx = 40);
    // Tick spacing for minor grid lines so that lines are >= minPx apart.
    Tick gridStep(float minPx = 5) const;
    // Spacing for labelled major lines.
    Tick labelStep(float minPx = 60) const;
};

// Results of measuring one input. Results are keyed by the ABSOLUTE tick the
// input fired at; offsets are relative to `anchor` (its tick when measuring
// began). `context` fingerprints everything else that affects the result, so
// edits to other inputs or the goal mark the set as stale.
struct MeasurementSet {
    Tick anchor = 0;
    ShiftMode mode = ShiftMode::Single;
    Goal goal{};
    std::uint64_t context = 0;
    std::map<Tick, Outcome> byTick;                       // absolute tick -> outcome
    std::map<Tick, std::pair<float, float>> positions;    // absolute tick -> player pos when it fired

    std::map<Tick, Outcome> offsets() const;
    std::map<Tick, std::pair<float, float>> offsetPositions() const;
    WindowReport report() const { return analyzeWindow(offsets(), anchor); }
};

// Fingerprint of the sequence as seen by input `id`: all other inputs
// (relative to `id` for those it drags along in ripple mode) plus the goal.
// Independent of where `id` itself currently sits.
std::uint64_t measurementContext(InputSequence const& seq, std::uint64_t id, ShiftMode mode, Goal const& goal);

struct Project {
    int levelId = 0;
    std::string levelName;
    Tick startTick = 0; // tick at which recording began (0 = level/start-pos start)
    InputSequence sequence;
    CircleSet circles;
    Goal goal{Goal::Kind::SurviveUntilTick, 0};
    bool hasGoal = false;
    std::map<std::uint64_t, MeasurementSet> measurements; // per input id
};

std::string serializeProject(Project const& p);
Expected<Project> parseProject(std::string_view text);

std::string escapeField(std::string_view s);
std::string unescapeField(std::string_view s);

} // namespace fwl
