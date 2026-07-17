#include "model/validate.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <unordered_map>

namespace pcbview {
namespace {

constexpr double kJointTolerance = 0.01;  // mm, track-to-track / track-to-via
constexpr size_t kMaxExamples = 6;

struct Anchor {
    Vec2 at;
    double radius = 0.0;  // how far from `at` still counts as touching
    const void* owner = nullptr;
};

double dist(Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

}  // namespace

GeometryReport validateNetGeometry(const BoardModel& board) {
    GeometryReport report;

    // Bucket every connectable feature by net.
    std::unordered_map<std::string, std::vector<Anchor>> byNet;

    for (const Pad& pad : board.pads) {
        if (pad.net.empty()) continue;
        // A track may terminate anywhere on the pad, not just dead centre.
        const double reach = std::max(pad.size.x, pad.size.y) * 0.5 + 0.05;
        byNet[pad.net].push_back(Anchor{pad.at, reach, &pad});
    }
    for (const Via& via : board.vias) {
        if (via.net.empty()) continue;
        byNet[via.net].push_back(Anchor{via.at, via.size * 0.5 + 0.05, &via});
    }
    for (const Track& track : board.tracks) {
        if (track.net.empty()) continue;
        byNet[track.net].push_back(Anchor{track.start, kJointTolerance, &track});
        byNet[track.net].push_back(Anchor{track.end, kJointTolerance, &track});
    }

    for (const Track& track : board.tracks) {
        if (track.net.empty()) continue;
        const auto it = byNet.find(track.net);
        if (it == byNet.end()) continue;

        for (const Vec2 endpoint : {track.start, track.end}) {
            ++report.trackEndpoints;

            bool touched = false;
            for (const Anchor& anchor : it->second) {
                if (anchor.owner == &track) continue;  // ignore self
                if (dist(endpoint, anchor.at) <= anchor.radius) {
                    touched = true;
                    break;
                }
            }
            if (touched) continue;

            ++report.orphanEndpoints;
            if (report.examples.size() < kMaxExamples) {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "net %-10s endpoint (%.3f, %.3f) on %s touches nothing",
                              track.net.c_str(), endpoint.x, endpoint.y,
                              board.layers[track.layer].name.c_str());
                report.examples.emplace_back(buf);
            }
        }
    }
    return report;
}

PadReport validatePadConnectivity(const BoardModel& board) {
    PadReport report;

    // Only tracks and vias count as "reaching" a pad. Deliberately excluding
    // other pads, so two coincident pads cannot vouch for each other.
    std::unordered_map<std::string, std::vector<Vec2>> byNet;
    for (const Track& track : board.tracks) {
        if (track.net.empty()) continue;
        byNet[track.net].push_back(track.start);
        byNet[track.net].push_back(track.end);
    }
    for (const Via& via : board.vias) {
        if (via.net.empty()) continue;
        byNet[via.net].push_back(via.at);
    }

    for (const Pad& pad : board.pads) {
        if (pad.net.empty()) continue;

        const bool bottom = pad.component >= 0 &&
                            board.components[pad.component].bottom;
        if (bottom) ++report.bottomPads; else ++report.topPads;

        const auto it = byNet.find(pad.net);
        if (it == byNet.end()) continue;

        const double reach = std::max(pad.size.x, pad.size.y) * 0.5 + 0.05;
        const bool touched =
            std::any_of(it->second.begin(), it->second.end(),
                        [&](Vec2 p) { return dist(pad.at, p) <= reach; });

        if (!touched) continue;
        if (bottom) ++report.bottomTouched; else ++report.topTouched;
    }
    return report;
}

namespace {

struct Obb {
    Vec2 center;
    Vec2 axis[2];  // unit, already rotated
    double half[2];
};

Obb padObb(const Pad& pad) {
    // Matches the tessellator's convention: KiCad's angle is clockwise in raw
    // file coordinates.
    const double rad = -pad.rotation * std::numbers::pi / 180.0;
    const double c = std::cos(rad), s = std::sin(rad);

    Obb obb;
    obb.center = pad.at;
    obb.axis[0] = Vec2{c, s};
    obb.axis[1] = Vec2{-s, c};
    obb.half[0] = pad.size.x * 0.5;
    obb.half[1] = pad.size.y * 0.5;
    return obb;
}

double project(const Obb& obb, Vec2 axis) {
    return std::abs(obb.axis[0].x * axis.x + obb.axis[0].y * axis.y) * obb.half[0] +
           std::abs(obb.axis[1].x * axis.x + obb.axis[1].y * axis.y) * obb.half[1];
}

// Separating axis theorem. A small tolerance keeps pads that merely abut from
// reading as shorts.
bool obbOverlap(const Obb& a, const Obb& b, double tolerance) {
    const Vec2 delta{b.center.x - a.center.x, b.center.y - a.center.y};
    const Vec2 axes[4] = {a.axis[0], a.axis[1], b.axis[0], b.axis[1]};

    for (const Vec2& axis : axes) {
        const double gap = std::abs(delta.x * axis.x + delta.y * axis.y);
        if (gap + tolerance >= project(a, axis) + project(b, axis)) {
            return false;  // separating axis found
        }
    }
    return true;
}

bool sharesLayer(const Pad& a, const Pad& b) {
    for (int la : a.layers) {
        if (std::find(b.layers.begin(), b.layers.end(), la) != b.layers.end()) {
            return true;
        }
    }
    return false;
}

}  // namespace

OverlapReport validatePadOverlaps(const BoardModel& board) {
    OverlapReport report;
    constexpr double kTolerance = 0.001;  // mm

    // Cache OBBs and a cheap reject radius.
    std::vector<Obb> obbs;
    std::vector<double> radius;
    obbs.reserve(board.pads.size());
    radius.reserve(board.pads.size());
    for (const Pad& pad : board.pads) {
        obbs.push_back(padObb(pad));
        radius.push_back(std::hypot(pad.size.x, pad.size.y) * 0.5);
    }

    for (size_t i = 0; i < board.pads.size(); ++i) {
        for (size_t j = i + 1; j < board.pads.size(); ++j) {
            const Pad& a = board.pads[i];
            const Pad& b = board.pads[j];

            // Same net may legitimately overlap (a pad stitched to its own via).
            if (a.net.empty() || b.net.empty() || a.net == b.net) continue;
            if (!sharesLayer(a, b)) continue;

            if (dist(a.at, b.at) > radius[i] + radius[j]) continue;  // cheap reject
            ++report.pairsChecked;

            if (!obbOverlap(obbs[i], obbs[j], kTolerance)) continue;

            ++report.shorts;
            if (report.examples.size() < kMaxExamples) {
                char buf[200];
                std::snprintf(buf, sizeof(buf),
                              "pad %s(%s) overlaps pad %s(%s) at (%.3f, %.3f)",
                              a.number.c_str(), a.net.c_str(), b.number.c_str(),
                              b.net.c_str(), a.at.x, a.at.y);
                report.examples.emplace_back(buf);
            }
        }
    }
    return report;
}

}  // namespace pcbview
