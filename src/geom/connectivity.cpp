#include "geom/connectivity.h"

#include <algorithm>
#include <numeric>
#include <string>
#include <vector>

namespace pcbview::geom {
namespace {

using namespace Clipper2Lib;

// Flat union-find over (layer, island) pairs, indexed by a global id.
struct DisjointSet {
    std::vector<int> parent;
    explicit DisjointSet(size_t n) : parent(n) {
        std::iota(parent.begin(), parent.end(), 0);
    }
    int find(int x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    }
    void join(int a, int b) {
        a = find(a);
        b = find(b);
        if (a != b) parent[b] = a;
    }
};

// One connected piece of copper on one layer.
struct Island {
    Paths64 paths;   // outer contour first, then its holes
    Rect64 bounds;
    int global = -1;  // index into the DisjointSet
};

// Split a layer's copper into connected islands.
//
// The union is taken through a PolyTree so nesting is explicit: each TOP-LEVEL
// outer contour is one island, and its immediate children are that island's
// holes. A flat Paths64 could not distinguish "a second island" from "a hole
// in the first", which would merge a pour with whatever sits in its cutout.
std::vector<Island> islandsOf(const Paths64& copper) {
    PolyTree64 tree;
    Paths64 open;
    Clipper64 c;
    c.AddSubject(copper);
    c.Execute(ClipType::Union, FillRule::NonZero, tree, open);

    std::vector<Island> out;
    out.reserve(tree.Count());
    for (const auto& top : tree) {
        Island is;
        is.paths.push_back(top->Polygon());
        // Holes only. A grandchild is an island sitting INSIDE a hole (a pad
        // in a pour cutout) -- a separate piece of copper, so it is picked up
        // as its own island below rather than being merged in here.
        for (const auto& child : *top) is.paths.push_back(child->Polygon());
        is.bounds = GetBounds(Paths64{is.paths.front()});
        out.push_back(std::move(is));

        for (const auto& child : *top)
            for (const auto& grand : *child) {
                Island inner;
                inner.paths.push_back(grand->Polygon());
                for (const auto& gh : *grand) inner.paths.push_back(gh->Polygon());
                inner.bounds = GetBounds(Paths64{inner.paths.front()});
                out.push_back(std::move(inner));
            }
    }
    return out;
}

// Centroid of a contour's bounding box -- the point a barrel is tested at.
// Barrels are annular rings, so their bbox centre is the hole centre.
Point64 centreOf(const Path64& p) {
    const Rect64 r = GetBounds(Paths64{p});
    return Point64{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
}

// Which island on this layer contains `pt`, or -1. The bbox test first is not
// an optimisation detail -- a board can carry thousands of islands and tens of
// thousands of barrel/island pairs, and PointInPolygon on every one is the
// difference between instant and unusable.
int islandAt(const std::vector<Island>& islands, Point64 pt) {
    for (size_t i = 0; i < islands.size(); ++i) {
        const Rect64& b = islands[i].bounds;
        if (pt.x < b.left || pt.x > b.right || pt.y < b.top || pt.y > b.bottom)
            continue;
        if (PointInPolygon(pt, islands[i].paths.front()) == PointInPolygonResult::IsOutside)
            continue;
        // Inside the outer contour: reject if it fell in one of its holes.
        bool inHole = false;
        for (size_t h = 1; h < islands[i].paths.size(); ++h)
            if (PointInPolygon(pt, islands[i].paths[h]) ==
                PointInPolygonResult::IsInside) {
                inHole = true;
                break;
            }
        if (!inHole) return static_cast<int>(i);
    }
    return -1;
}

double areaOf(const Paths64& p) { return std::abs(Area(p)); }

}  // namespace

PseudoNetStats extractPseudoNets(LayerArt& art, const ProgressFn& progress) {
    // Nothing below mutates `art` until the publish step at the end, so
    // bailing out here leaves the board untouched.
    const auto report = [&](const std::string& stage, int pct) {
        return progress ? progress(stage, pct) : true;
    };
    // Never overwrite a real netlist.
    if (!art.nets.empty()) return {};

    std::vector<ArtLayer*> copper;
    for (ArtLayer& al : art.layers)
        if (al.kind == LayerKind::Copper) copper.push_back(&al);
    if (copper.empty()) return {};

    std::vector<std::vector<Island>> perLayer(copper.size());
    size_t total = 0;
    for (size_t i = 0; i < copper.size(); ++i) {
        if (!report("Finding copper islands on " + copper[i]->name,
                    static_cast<int>(i * 30 / copper.size())))
            return {};
        perLayer[i] = islandsOf(copper[i]->art);
        total += perLayer[i].size();
    }
    if (total == 0) return {};

    DisjointSet ds(total);
    int next = 0;
    for (auto& layer : perLayer)
        for (Island& is : layer) is.global = next++;

    // Plated through-holes span the WHOLE stack, so one barrel joins every
    // layer it lands on. Unplated holes are deliberately absent from `barrels`
    // (they conduct nothing), which is exactly the behaviour wanted here.
    // The dominant cost: every barrel is tested against every layer's islands.
    size_t done = 0;
    for (const Path64& barrel : art.barrels) {
        if ((done++ & 0x3F) == 0 &&
            !report("Following " + std::to_string(art.barrels.size()) +
                        " plated holes through the stack",
                    30 + static_cast<int>(done * 60 / std::max<size_t>(art.barrels.size(), 1))))
            return {};
        const Point64 c = centreOf(barrel);
        int first = -1;
        for (size_t i = 0; i < copper.size(); ++i) {
            const int hit = islandAt(perLayer[i], c);
            if (hit < 0) continue;
            if (first < 0) first = perLayer[i][hit].global;
            else ds.join(first, perLayer[i][hit].global);
        }
    }

    // Blind/buried vias conduct only between their two end layers, so they
    // must join ONLY the layers inside their span -- joining the whole stack
    // would invent connections the board does not have.
    for (const LayerArt::PartialBore& bore : art.partialBores) {
        int fromIdx = -1, toIdx = -1;
        for (size_t i = 0; i < copper.size(); ++i) {
            if (copper[i]->name == bore.fromLayer) fromIdx = static_cast<int>(i);
            if (copper[i]->name == bore.toLayer) toIdx = static_cast<int>(i);
        }
        if (fromIdx < 0 || toIdx < 0) continue;
        if (fromIdx > toIdx) std::swap(fromIdx, toIdx);
        const Point64 c = centreOf(bore.path);
        int first = -1;
        for (int i = fromIdx; i <= toIdx; ++i) {
            const int hit = islandAt(perLayer[i], c);
            if (hit < 0) continue;
            if (first < 0) first = perLayer[i][hit].global;
            else ds.join(first, perLayer[i][hit].global);
        }
    }

    // Gather groups, then order by copper area so the big pours -- almost
    // always ground and power -- land at the top of the panel.
    struct Group {
        int root = -1;
        double area = 0.0;
        std::vector<std::pair<size_t, size_t>> members;  // (layer, island)
    };
    if (!report("Grouping connected copper", 92)) return {};
    std::vector<Group> groups;
    std::vector<int> rootToGroup(total, -1);
    for (size_t li = 0; li < perLayer.size(); ++li)
        for (size_t ii = 0; ii < perLayer[li].size(); ++ii) {
            const int root = ds.find(perLayer[li][ii].global);
            int& g = rootToGroup[root];
            if (g < 0) {
                g = static_cast<int>(groups.size());
                groups.push_back({root, 0.0, {}});
            }
            groups[g].area += areaOf(perLayer[li][ii].paths);
            groups[g].members.emplace_back(li, ii);
        }
    std::sort(groups.begin(), groups.end(),
              [](const Group& a, const Group& b) { return a.area > b.area; });

    // Publish. The "~" prefix is the signal that these are derived; the UI
    // says so too, but a name that travels with the data cannot be lost.
    report("Publishing nets", 98);
    art.nets.clear();
    for (ArtLayer* al : copper) al->netArt.clear();
    for (size_t g = 0; g < groups.size(); ++g) {
        LayerArt::NetInfo info;
        info.name = "~" + std::to_string(g + 1);
        // Clipper works in integer units; kScale converts back, squared for
        // an area.
        info.copperMm2 = groups[g].area / (kScale * kScale);
        art.nets.push_back(std::move(info));
        for (const auto& [li, ii] : groups[g].members) {
            ArtLayer::NetRegion nr;
            nr.net = static_cast<int>(g);
            nr.paths = perLayer[li][ii].paths;
            copper[li]->netArt.push_back(std::move(nr));
        }
    }
    art.netsArePseudo = true;
    PseudoNetStats stats;
    stats.groups = static_cast<int>(groups.size());
    for (const Group& g : groups)
        if (g.members.size() > 1) ++stats.connecting;
    return stats;
}

}  // namespace pcbview::geom
