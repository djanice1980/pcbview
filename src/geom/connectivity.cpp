#include "geom/connectivity.h"

#include <algorithm>
#include <cmath>
#include <map>
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
    // Never overwrite a real netlist -- EXCEPT a 356 test-point table, which
    // is names without geometry: binding those names to the copper is exactly
    // what this function exists to finish.
    if (!art.nets.empty() && !art.netsFromTestPoints) return {};

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

    // 356 test points, snapshotted BEFORE the net table is replaced: each is
    // a (name, position) pair, and a point inside an island names its whole
    // group. Disagreements are findings, not errors: one name across several
    // groups is an OPEN, several names in one group is a SHORT.
    struct NamedPoint {
        std::string name;
        Point64 at;
        size_t pointIdx;  // back-reference into art.netPoints
    };
    std::vector<NamedPoint> testPoints;
    if (art.netsFromTestPoints) {
        for (size_t pi = 0; pi < art.netPoints.size(); ++pi) {
            const LayerArt::NetPoint& np = art.netPoints[pi];
            if (np.net < 0 || np.net >= static_cast<int>(art.nets.size()))
                continue;
            testPoints.push_back(
                {art.nets[np.net].name,
                 Point64{static_cast<int64_t>(std::llround(np.pos[0] * kScale)),
                         static_cast<int64_t>(std::llround(np.pos[1] * kScale))},
                 pi});
        }
    }

    art.nets.clear();
    for (ArtLayer* al : copper) al->netArt.clear();
    // (layer, island) -> published net index, for the assignments below.
    std::vector<std::vector<int>> islandNet(perLayer.size());
    for (size_t li = 0; li < perLayer.size(); ++li)
        islandNet[li].assign(perLayer[li].size(), -1);
    for (size_t g = 0; g < groups.size(); ++g)
        for (const auto& [li, ii] : groups[g].members)
            islandNet[li][ii] = static_cast<int>(g);

    // Vote: which names' test points land inside which group's copper. A
    // through-hole point is present on every layer, so testing all layers is
    // correct for it and harmless for SMD (only its own layer contains it).
    std::vector<std::map<std::string, int>> votes(groups.size());
    std::vector<int> pointGroup(testPoints.size(), -1);
    std::map<std::string, std::vector<int>> nameGroups;
    for (size_t ti = 0; ti < testPoints.size(); ++ti) {
        for (size_t li = 0; li < perLayer.size() && pointGroup[ti] < 0; ++li) {
            const int hit = islandAt(perLayer[li], testPoints[ti].at);
            if (hit < 0) continue;
            pointGroup[ti] = islandNet[li][hit];
        }
        if (pointGroup[ti] >= 0) {
            ++votes[pointGroup[ti]][testPoints[ti].name];
            auto& gs = nameGroups[testPoints[ti].name];
            if (std::find(gs.begin(), gs.end(), pointGroup[ti]) == gs.end())
                gs.push_back(pointGroup[ti]);
        }
    }

    // Name the groups. Majority name wins; a second distinct name in the same
    // group is a short finding. A name spanning groups gets #2, #3 suffixes on
    // the smaller pieces -- an open finding.
    std::vector<std::string> groupName(groups.size());
    for (size_t g = 0; g < groups.size(); ++g) {
        std::string best;
        int bestVotes = 0;
        for (const auto& [name, n] : votes[g]) {
            if (n > bestVotes) {
                bestVotes = n;
                best = name;
            }
        }
        groupName[g] = best;  // empty = unnamed, resolved to "~k" below
        if (votes[g].size() > 1) {
            std::string all;
            for (const auto& [name, n] : votes[g])
                all += (all.empty() ? "" : ", ") + name;
            art.warnings.push_back("copper group joins nets " + all +
                                   " -- short?");
        }
    }
    for (auto& [name, gs] : nameGroups) {
        if (gs.size() <= 1) continue;
        art.warnings.push_back(
            "net '" + name + "' spans " + std::to_string(gs.size()) +
            " unconnected copper groups -- open?");
        int k = 1;
        for (const int g : gs) {
            if (groupName[g] != name) continue;  // a short vote lost here
            if (k > 1)
                groupName[g] = name + "#" + std::to_string(k);
            ++k;
        }
    }

    for (size_t g = 0; g < groups.size(); ++g) {
        LayerArt::NetInfo info;
        info.name = groupName[g].empty() ? "~" + std::to_string(g + 1)
                                         : groupName[g];
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

    // Point the 356 test points at their groups' published nets, so pad
    // snapping and click naming carry the REAL names. A point that landed on
    // no copper keeps no net (and says so).
    if (art.netsFromTestPoints) {
        int unplaced = 0;
        for (size_t ti = 0; ti < testPoints.size(); ++ti) {
            art.netPoints[testPoints[ti].pointIdx].net = pointGroup[ti];
            if (pointGroup[ti] < 0) ++unplaced;
        }
        if (unplaced > 0)
            art.warnings.push_back(
                std::to_string(unplaced) +
                " netlist test point(s) sit on no copper");
    }

    // Give the pseudo-nets their SEGMENT GRAPH: every untagged stroke the
    // parser kept (per layer) belongs to whichever island contains its
    // midpoint, and therefore to that island's group. This is what makes
    // click-to-identify and along-the-copper measurement work on derived
    // nets exactly as they do on a real netlist -- without it, pseudo-nets
    // could be highlighted but never walked. Also the panel's routed length.
    art.netSegments.clear();
    for (size_t li = 0; li < copper.size(); ++li) {
        for (const ArtLayer::LooseSeg& sg : copper[li]->looseSegments) {
            const Point64 mid{
                static_cast<int64_t>(std::llround((sg.ax + sg.bx) * 0.5 * kScale)),
                static_cast<int64_t>(std::llround((sg.ay + sg.by) * 0.5 * kScale))};
            const int hit = islandAt(perLayer[li], mid);
            if (hit < 0) continue;  // a stroke wholly erased by clear polarity
            const int net = islandNet[li][hit];
            if (net < 0) continue;
            art.netSegments.push_back({sg.ax, sg.ay, sg.bx, sg.by, net});
            art.nets[net].routedMm +=
                std::hypot(sg.bx - sg.ax, sg.by - sg.ay);
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
