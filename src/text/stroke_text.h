#pragma once

// Newstroke text -> polylines.
//
// Turns a string into stroke centrelines in board space. The caller thickens
// them into polygons (they are drawn with a round-capped pen of `thickness`,
// exactly like a track).
//
// Coordinates come out in KiCad's sense -- Y increasing downward -- so the
// tessellator's single Y flip handles them like everything else.

#include <string>
#include <vector>

#include "model/board.h"

namespace pcbview::text {

struct TextStyle {
    Vec2 size{1.0, 1.0};      // em width/height in mm
    double thickness = 0.15;  // pen width in mm
    double rotation = 0.0;    // degrees, KiCad sense
    bool mirror = false;      // bottom-side text reads correctly from below
    bool italic = false;
};

// One pen-down run. Thicken with a round pen to get the printed stroke.
using Polyline = std::vector<Vec2>;

// Lay `utf8` out centred on `origin` (KiCad centres its reference/value text on
// the item's position). Characters outside Basic Latin are skipped.
std::vector<Polyline> layout(const std::string& utf8, Vec2 origin,
                             const TextStyle& style);

// Advance width of a string in mm, before rotation.
double measure(const std::string& utf8, const TextStyle& style);

}  // namespace pcbview::text
