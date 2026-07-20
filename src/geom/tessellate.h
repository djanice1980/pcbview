#pragma once

// BoardModel -> triangles.
//
// Coordinate convention: world X = KiCad X, world Y = -KiCad Y, world Z = height
// above the board bottom. The Y flip happens exactly once, when converting into
// Clipper coordinates, so everything downstream is a conventional Y-up / Z-up
// right-handed space. Flipping later would invert winding and every normal.

#include <cstdint>
#include <string>
#include <vector>

#include "geom/layer_art.h"
#include "model/board.h"

namespace pcbview::geom {

struct Vertex {
    float position[3];
    float normal[3];
};

struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    size_t triangleCount() const { return indices.size() / 3; }
};

enum class Material { Substrate, Copper, Soldermask, Silkscreen, Component };

// One drawable piece. Kept separate per layer/material because the renderer
// looks material up by instance ID -- see RT-readiness rule 3.
struct Part {
    Material material = Material::Copper;
    // Named, not indexed. A BoardModel layer index is meaningless on the Gerber
    // path -- there is no BoardModel -- so LayerArt and everything downstream
    // identify layers by name.
    std::string name;
    Mesh mesh;

    // Soldermask only: how many pad openings were punched. Cross-checks against
    // the flash count in KiCad's own F.Mask/B.Mask gerber.
    int maskOpenings = -1;

    // Component (Material::Component) only. Board layers ignore these.
    //   color:     RGBA from the source model (glTF baseColorFactor). Copper,
    //              mask and silkscreen derive their colour from the material
    //              enum; a rendered IC/cap/connector carries its own.
    //   mountSide: +1 mounts on the top copper, -1 on the bottom. The exploded
    //              view moves the part with that outer ring instead of giving it
    //              its own stage, and a thickness override shifts top parts to
    //              sit on the new surface.
    float color[4] = {0.8f, 0.8f, 0.8f, 1.0f};
    int mountSide = 1;

    // "vias" parts only: true for a blind/buried barrel that spans part of the
    // stack. Full-stack barrels stay pinned during the exploded peel; a
    // partial barrel instead travels with the layers it spans, so the
    // renderers rank it by its centre Z rather than parking it at the barrel
    // plane.
    bool partialBarrel = false;

    // Per-TRIANGLE net index (into BoardMesh::nets), -1 for none. Empty on a
    // part with no net information at all. Per triangle rather than per part
    // because a copper layer is one part carrying many nets -- splitting the
    // layer into a part per net would multiply the draw calls by the net
    // count for no visual gain.
    std::vector<int32_t> triNet;
};

struct Bounds {
    double min[3] = {0, 0, 0};
    double max[3] = {0, 0, 0};
};

// A point the measurement tool can snap to exactly: pad centres, drill/bore
// centres, board-outline vertices. Fab-exact coordinates from the importers,
// so a measurement between two snapped points is the true design dimension,
// not click precision. `net` indexes BoardMesh::nets (-1 when the point has
// no net -- Gerber, bare drills, outline vertices).
struct SnapPoint {
    float pos[3] = {0, 0, 0};
    int net = -1;
};

struct BoardMesh {
    std::vector<Part> parts;
    Bounds bounds;

    // Measurement snap targets (pad/drill centres, outline vertices) and the
    // Z of the board's top surface (masks included, components excluded) --
    // the plane free measurement points are projected onto.
    std::vector<SnapPoint> snapPoints;
    double boardTopZ = 0.0;
    // Net table for the measure tool (from KiCad, or from Gerber X2 %TO.N%
    // attributes when the package carries them), plus
    // every track segment with its net -- the graph netPathLength() walks.
    // Surface finish on exposed copper, carried through so the renderer can
    // shade pads as the plant will actually plate them. Empty = unknown.
    std::string copperFinish;

    std::vector<LayerArt::NetInfo> nets;
    std::vector<LayerArt::NetSeg> netSegments;
    // Board-outline bounding box (mm, components excluded) for the
    // width/height dimension callouts. Valid once an outline was assembled.
    bool outlineValid = false;
    double outlineMin[2] = {0, 0};
    double outlineMax[2] = {0, 0};

    size_t totalTriangles() const;
    size_t totalVertices() const;
};

// Note there are no thickness options here. Copper, mask and dielectric heights
// are fabrication facts and live on BoardModel; duplicating them as render
// options is how the board ended up 0.07mm too thick once already.
struct TessellateOptions {
    // Segments used to approximate a full circle.
    int circleSegments = 32;
    // Skip inner copper layers, which are invisible inside the substrate.
    bool innerLayers = true;
    // Emit soldermask parts.
    bool soldermask = true;
    // Emit silkscreen parts. Graphics only for now -- text needs a stroke font.
    bool silkscreen = true;
};

// BoardModel -> LayerArt. Resolves KiCad's semantics (tracks, pads, vias, zones,
// graphics, text) into filled polygons per layer. This is the only part of the
// pipeline that knows what a "track" is.
LayerArt buildLayerArt(const BoardModel& board, const TessellateOptions& opts = {});

// Shortest routed path along `net`'s track segments between two points
// (XY, mm), or a negative value when no track path connects them (e.g. the
// net is joined through a zone pour, which carries no segments). Endpoints
// snap onto the segment graph within a small radius, so pad/via centres --
// what the measure tool hands in -- resolve even when a track lands slightly
// off the exact centre.
double netPathLength(const BoardMesh& mesh, int net, double ax, double ay,
                     double bx, double by);

// LayerArt -> triangles. Format-agnostic: clips copper to the profile, derives
// the mask film from its openings (which is what makes via tenting free),
// subtracts drills, extrudes. Knows nothing about KiCad or Gerber.
BoardMesh assemble(const LayerArt& art, const TessellateOptions& opts = {});

// Re-derive every layer's Z for a new finished thickness, in place.
//
// Models what a fab actually varies: copper foil weight and mask film stay
// fixed, and the dielectric flexes to make up the difference. So a 0.8mm order
// of a 1.6mm design keeps 1oz copper and thins the core. The polygons are
// untouched -- only Z moves -- so the caller re-runs assemble() to get a mesh.
void applyThickness(LayerArt& art, double finishedThicknessMm);

// Convenience: buildLayerArt() then assemble().
BoardMesh tessellate(const BoardModel& board, const TessellateOptions& opts = {});

}  // namespace pcbview::geom
