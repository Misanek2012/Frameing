#include "Project.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <locale>
#include <sstream>

namespace fwl {

// ---- circles ---------------------------------------------------------------

std::uint64_t CircleSet::add(Circle c) {
    c.id = m_nextId++;
    m_circles.push_back(std::move(c));
    return m_circles.back().id;
}

Status CircleSet::insertWithId(Circle c) {
    if (find(c.id)) return Status::fail("duplicate circle id " + std::to_string(c.id));
    m_nextId = std::max(m_nextId, c.id + 1);
    m_circles.push_back(std::move(c));
    return Status::success();
}

bool CircleSet::remove(std::uint64_t id) {
    auto it = std::find_if(m_circles.begin(), m_circles.end(), [&](Circle const& c) { return c.id == id; });
    if (it == m_circles.end()) return false;
    m_circles.erase(it);
    return true;
}

Circle* CircleSet::find(std::uint64_t id) {
    for (auto& c : m_circles)
        if (c.id == id) return &c;
    return nullptr;
}

Circle const* CircleSet::find(std::uint64_t id) const {
    for (auto const& c : m_circles)
        if (c.id == id) return &c;
    return nullptr;
}

std::optional<std::uint64_t> CircleSet::hitTest(float x, float y, float tolerance) const {
    for (auto it = m_circles.rbegin(); it != m_circles.rend(); ++it) {
        if (!it->visible) continue;
        float dx = x - it->x, dy = y - it->y;
        float reach = it->radius + it->thickness * 0.5f + tolerance;
        if (dx * dx + dy * dy <= reach * reach) return it->id;
    }
    return std::nullopt;
}

std::size_t CircleSet::removeLinked(std::uint64_t inputId) {
    auto before = m_circles.size();
    m_circles.erase(std::remove_if(m_circles.begin(), m_circles.end(),
                                   [&](Circle const& c) { return c.linkedInput && *c.linkedInput == inputId; }),
                    m_circles.end());
    return before - m_circles.size();
}

std::vector<std::uint64_t> CircleSet::generateFromWindow(std::uint64_t inputId, WindowReport const& report,
                                                         std::map<Tick, std::pair<float, float>> const& positions,
                                                         CircleStyle const& style) {
    removeLinked(inputId);
    std::vector<std::uint64_t> ids;
    for (auto const& [off, out] : report.results) {
        if (out == Outcome::Blocked) continue; // never simulated: no position exists
        auto pos = positions.find(off);
        if (pos == positions.end()) continue;
        Circle c;
        c.x = pos->second.first;
        c.y = pos->second.second;
        c.radius = style.radius;
        c.thickness = style.thickness;
        c.fillAlpha = style.fillAlpha;
        c.color = out == Outcome::Pass ? style.pass : out == Outcome::Fail ? style.fail : style.untrusted;
        c.label = (off > 0 ? "+" : "") + std::to_string(off);
        c.linkedInput = inputId;
        c.linkedOffset = off;
        c.linkedOutcome = out;
        ids.push_back(add(std::move(c)));
    }
    return ids;
}

// ---- measurements ------------------------------------------------------------

std::map<Tick, Outcome> MeasurementSet::offsets() const {
    std::map<Tick, Outcome> m;
    for (auto const& [t, o] : byTick) m[t - anchor] = o;
    return m;
}

std::map<Tick, std::pair<float, float>> MeasurementSet::offsetPositions() const {
    std::map<Tick, std::pair<float, float>> m;
    for (auto const& [t, p] : positions) m[t - anchor] = p;
    return m;
}

std::uint64_t measurementContext(InputSequence const& seq, std::uint64_t id, ShiftMode mode, Goal const& goal) {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&](auto v) { h = fnv1a(&v, sizeof v, h); };
    auto idx = seq.indexOf(id);
    mix(static_cast<int>(goal.kind));
    mix(goal.kind == Goal::Kind::SurviveUntilTick ? goal.tick : Tick{0});
    mix(static_cast<int>(mode));
    if (!idx) return h;
    Tick self = seq.events()[*idx].tick;
    auto const& ev = seq.events();
    for (std::size_t i = 0; i < ev.size(); ++i) {
        if (i == *idx) continue;
        bool rel = mode == ShiftMode::Ripple && i > *idx;
        mix(ev[i].id);
        mix(rel ? ev[i].tick - self : ev[i].tick);
        mix(rel);
        mix(static_cast<int>(ev[i].button));
        mix(ev[i].down);
        mix(ev[i].player2);
    }
    return h;
}

// ---- timeline view -----------------------------------------------------------

Tick TimelineView::xToTick(float x) const { return static_cast<Tick>(std::llround(xToTickExact(x))); }

void TimelineView::zoomAround(float anchorX, double factor) {
    double anchorTick = xToTickExact(anchorX);
    pxPerTick = std::clamp(pxPerTick * factor, kMinPxPerTick, kMaxPxPerTick);
    startTick = anchorTick - anchorX / pxPerTick;
}

void TimelineView::scrollPx(float dx) { startTick += dx / pxPerTick; }

void TimelineView::ensureVisible(Tick tick, float marginPx) {
    if (width <= 2 * marginPx) {
        startTick = static_cast<double>(tick) - (width * 0.5) / pxPerTick;
        return;
    }
    float x = tickToX(static_cast<double>(tick));
    if (x < marginPx) startTick = static_cast<double>(tick) - marginPx / pxPerTick;
    else if (x > width - marginPx) startTick = static_cast<double>(tick) - (width - marginPx) / pxPerTick;
}

namespace {
constexpr std::array<Tick, 16> kSteps{1, 2, 5, 10, 20, 30, 60, 120, 240, 480, 960, 2400, 4800, 9600, 24000, 48000};
Tick pickStep(double pxPerTick, float minPx) {
    for (Tick s : kSteps)
        if (static_cast<double>(s) * pxPerTick >= minPx) return s;
    return kSteps.back();
}
} // namespace

Tick TimelineView::gridStep(float minPx) const { return pickStep(pxPerTick, minPx); }
Tick TimelineView::labelStep(float minPx) const { return pickStep(pxPerTick, minPx); }

// ---- serialization -----------------------------------------------------------

std::string escapeField(std::string_view s) {
    std::string out = "=";
    for (char ch : s) {
        switch (ch) {
            case '%': out += "%25"; break;
            case ' ': out += "%20"; break;
            case '\n': out += "%0A"; break;
            case '\r': out += "%0D"; break;
            case '\t': out += "%09"; break;
            default: out += ch;
        }
    }
    return out;
}

std::string unescapeField(std::string_view s) {
    if (!s.empty() && s.front() == '=') s.remove_prefix(1);
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int v = 0;
            auto r = std::from_chars(s.data() + i + 1, s.data() + i + 3, v, 16);
            if (r.ec == std::errc() && r.ptr == s.data() + i + 3) {
                out += static_cast<char>(v);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

namespace {
std::string fmtFloat(float f) {
    std::array<char, 64> buf{};
    auto r = std::to_chars(buf.data(), buf.data() + buf.size(), f); // shortest round-trip form
    return std::string(buf.data(), r.ptr);
}
std::string hexColor(RGBA c) {
    char b[16];
    std::snprintf(b, sizeof b, "%02x%02x%02x%02x", c.r, c.g, c.b, c.a);
    return b;
}
template <class T>
bool parseInt(std::string_view s, T& out) {
    auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}
bool parseFloat(std::string_view s, float& out) {
    if (s.empty()) return false;
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
#else
    // libc++ (macOS / Android NDK) has no floating-point from_chars. Parse with
    // the classic "C" locale so a decimal-comma system locale can't change the
    // result, and require the whole token to be consumed.
    std::istringstream in{std::string(s)};
    in.imbue(std::locale::classic());
    float v{};
    in >> std::noskipws >> v;
    if (in.fail() || in.peek() != std::char_traits<char>::eof()) return false;
    out = v;
    return true;
#endif
}
bool parseColor(std::string_view s, RGBA& c) {
    if (s.size() != 8) return false;
    std::uint32_t v = 0;
    auto r = std::from_chars(s.data(), s.data() + 8, v, 16);
    if (r.ec != std::errc() || r.ptr != s.data() + 8) return false;
    c = {std::uint8_t(v >> 24), std::uint8_t(v >> 16), std::uint8_t(v >> 8), std::uint8_t(v)};
    return true;
}
std::vector<std::string_view> tokens(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        std::size_t j = i;
        while (j < line.size() && line[j] != ' ' && line[j] != '\t') ++j;
        if (j > i) out.push_back(line.substr(i, j - i));
        i = j;
    }
    return out;
}
} // namespace

std::string serializeProject(Project const& p) {
    std::ostringstream os;
    os.imbue(std::locale::classic()); // never "12 345" / "1,5", whatever the host locale
    os << "FWL 1\n";
    os << "level " << p.levelId << ' ' << escapeField(p.levelName) << '\n';
    os << "start " << p.startTick << '\n';
    if (p.hasGoal) {
        if (p.goal.kind == Goal::Kind::CompleteLevel) os << "goal complete\n";
        else os << "goal survive " << p.goal.tick << '\n';
    }
    os << "next_input " << p.sequence.nextId() << '\n';
    os << "next_circle " << p.circles.nextId() << '\n';
    for (auto const& e : p.sequence.events()) {
        os << "input " << e.id << ' ' << e.tick << ' ' << buttonName(e.button) << ' '
           << (e.down ? "press" : "release") << ' ' << (e.player2 ? "p2" : "p1") << '\n';
    }
    for (auto const& c : p.circles.circles()) {
        os << "circle " << c.id << ' ' << fmtFloat(c.x) << ' ' << fmtFloat(c.y) << ' ' << fmtFloat(c.radius) << ' '
           << hexColor(c.color) << ' ' << fmtFloat(c.thickness) << ' ' << fmtFloat(c.fillAlpha) << ' '
           << (c.glow ? 1 : 0) << ' ' << (c.visible ? 1 : 0) << ' ' << escapeField(c.label) << ' '
           << (c.linkedInput ? std::to_string(*c.linkedInput) : "-") << ' '
           << (c.linkedOffset ? std::to_string(*c.linkedOffset) : "-") << ' '
           << (c.linkedOutcome ? outcomeName(*c.linkedOutcome) : "-") << '\n';
    }
    for (auto const& [id, w] : p.windows) {
        if (!w.earliest && !w.latest) continue;
        os << "window " << id << ' ' << (w.earliest ? std::to_string(*w.earliest) : "-") << ' '
           << (w.latest ? std::to_string(*w.latest) : "-") << '\n';
    }
    for (auto const& [id, m] : p.measurements) {
        os << "mset " << id << ' ' << m.anchor << ' ' << (m.mode == ShiftMode::Ripple ? "ripple" : "single") << ' '
           << (m.goal.kind == Goal::Kind::CompleteLevel ? "complete" : "survive") << ' ' << m.goal.tick << ' '
           << m.context << '\n';
        for (auto const& [tick, out] : m.byTick) {
            os << "measure " << id << ' ' << tick << ' ' << outcomeName(out);
            if (auto it = m.positions.find(tick); it != m.positions.end())
                os << ' ' << fmtFloat(it->second.first) << ' ' << fmtFloat(it->second.second);
            os << '\n';
        }
    }
    return os.str();
}

Expected<Project> parseProject(std::string_view text) {
    using R = Expected<Project>;
    Project p;
    std::uint64_t nextInput = 1, nextCircle = 1;
    int lineNo = 0;
    bool sawHeader = false;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        auto nl = text.find('\n', pos);
        auto line = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        auto t = tokens(line);
        if (t.empty() || t[0].front() == '#') continue;
        auto err = [&](std::string const& m) { return R::fail("line " + std::to_string(lineNo) + ": " + m); };

        if (!sawHeader) {
            if (t.size() != 2 || t[0] != "FWL" || t[1] != "1") return err("not a Frame Window Lab v1 file");
            sawHeader = true;
            continue;
        }
        if (t[0] == "level") {
            if (t.size() != 3 || !parseInt(t[1], p.levelId)) return err("bad level line");
            p.levelName = unescapeField(t[2]);
        } else if (t[0] == "start") {
            if (t.size() != 2 || !parseInt(t[1], p.startTick)) return err("bad start line");
        } else if (t[0] == "goal") {
            if (t.size() == 2 && t[1] == "complete") {
                p.goal = {Goal::Kind::CompleteLevel, 0};
            } else if (t.size() == 3 && t[1] == "survive" && parseInt(t[2], p.goal.tick)) {
                p.goal.kind = Goal::Kind::SurviveUntilTick;
            } else {
                return err("bad goal line");
            }
            p.hasGoal = true;
        } else if (t[0] == "next_input") {
            if (t.size() != 2 || !parseInt(t[1], nextInput)) return err("bad next_input");
        } else if (t[0] == "next_circle") {
            if (t.size() != 2 || !parseInt(t[1], nextCircle)) return err("bad next_circle");
        } else if (t[0] == "input") {
            if (t.size() != 6) return err("input needs 5 fields");
            InputEvent e;
            if (!parseInt(t[1], e.id) || !parseInt(t[2], e.tick)) return err("bad input id/tick");
            if (t[3] == "jump") e.button = Button::Jump;
            else if (t[3] == "left") e.button = Button::Left;
            else if (t[3] == "right") e.button = Button::Right;
            else return err("unknown button '" + std::string(t[3]) + "'");
            if (t[4] == "press") e.down = true;
            else if (t[4] == "release") e.down = false;
            else return err("expected press/release");
            if (t[5] == "p1") e.player2 = false;
            else if (t[5] == "p2") e.player2 = true;
            else return err("expected p1/p2");
            if (auto st = p.sequence.insertWithId(e); !st) return err(st.error);
        } else if (t[0] == "circle") {
            if (t.size() != 14) return err("circle needs 13 fields");
            Circle c;
            if (!parseInt(t[1], c.id) || !parseFloat(t[2], c.x) || !parseFloat(t[3], c.y) ||
                !parseFloat(t[4], c.radius) || !parseColor(t[5], c.color) || !parseFloat(t[6], c.thickness) ||
                !parseFloat(t[7], c.fillAlpha))
                return err("bad circle numeric field");
            c.glow = t[8] == "1";
            c.visible = t[9] == "1";
            c.label = unescapeField(t[10]);
            if (t[11] != "-") {
                std::uint64_t v;
                if (!parseInt(t[11], v)) return err("bad linked input");
                c.linkedInput = v;
            }
            if (t[12] != "-") {
                Tick v;
                if (!parseInt(t[12], v)) return err("bad linked offset");
                c.linkedOffset = v;
            }
            if (t[13] != "-") {
                auto o = outcomeFromName(std::string(t[13]));
                if (!o) return err("bad linked outcome");
                c.linkedOutcome = o;
            }
            if (auto st = p.circles.insertWithId(c); !st) return err(st.error);
        } else if (t[0] == "window") {
            std::uint64_t id;
            if (t.size() != 4 || !parseInt(t[1], id)) return err("bad window line");
            Project::MarkedWindow w;
            for (int k = 0; k < 2; ++k) {
                if (t[2 + k] == "-") continue;
                Tick v;
                if (!parseInt(t[2 + k], v)) return err("bad window tick");
                (k == 0 ? w.earliest : w.latest) = v;
            }
            p.windows[id] = w;
        } else if (t[0] == "mset") {
            std::uint64_t id;
            MeasurementSet m;
            if (t.size() != 7 || !parseInt(t[1], id) || !parseInt(t[2], m.anchor) || !parseInt(t[5], m.goal.tick) ||
                !parseInt(t[6], m.context))
                return err("bad mset line");
            if (t[3] == "ripple") m.mode = ShiftMode::Ripple;
            else if (t[3] == "single") m.mode = ShiftMode::Single;
            else return err("bad shift mode");
            if (t[4] == "complete") m.goal.kind = Goal::Kind::CompleteLevel;
            else if (t[4] == "survive") m.goal.kind = Goal::Kind::SurviveUntilTick;
            else return err("bad goal kind");
            p.measurements[id] = std::move(m);
        } else if (t[0] == "measure") {
            std::uint64_t id;
            Tick tick;
            if ((t.size() != 4 && t.size() != 6) || !parseInt(t[1], id) || !parseInt(t[2], tick))
                return err("bad measure line");
            auto it = p.measurements.find(id);
            if (it == p.measurements.end()) return err("measure before its mset");
            auto o = outcomeFromName(std::string(t[3]));
            if (!o) return err("bad outcome");
            it->second.byTick[tick] = *o;
            if (t.size() == 6) {
                float x, y;
                if (!parseFloat(t[4], x) || !parseFloat(t[5], y)) return err("bad measure position");
                it->second.positions[tick] = {x, y};
            }
        } else {
            return err("unknown record '" + std::string(t[0]) + "'");
        }
    }
    if (!sawHeader) return R::fail("empty file");
    if (auto st = p.sequence.validate(); !st) return R::fail("inputs invalid: " + st.error);
    p.sequence.setNextId(std::max(nextInput, p.sequence.nextId()));
    p.circles.setNextId(std::max(nextCircle, p.circles.nextId()));
    return R::success(std::move(p));
}

} // namespace fwl
