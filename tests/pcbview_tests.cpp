// Geometry/importer regression suite. Runs headless with NO Vulkan, NO Qt --
// exactly what a CI runner can execute -- and exercises the pipeline where
// most regressions have actually happened: Gerber parsing (X2 nets, flashes,
// untagged strokes), package assembly, pseudo-net inference, the measurement
// path solver and snap points.
//
// The fixture is authored HERE as string literals with hand-computable ground
// truth, so every expected number below is exact by construction. Assertions
// are deliberately coordinate-flip-immune: they check lengths, counts and
// relationships, never absolute positions -- the importer owns its coordinate
// convention and these tests must not freeze it by accident.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "geom/connectivity.h"
#include "geom/layer_art.h"
#include "geom/tessellate.h"
#include "io/gerber/gerber_parser.h"
#include "io/gerber/gerber_project.h"

namespace fs = std::filesystem;
using namespace pcbview;

static int g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                    \
    } while (0)
#define CHECK_NEAR(a, b, eps)                                                \
    do {                                                                     \
        const double va = (a), vb = (b);                                     \
        if (std::abs(va - vb) > (eps)) {                                     \
            ++g_failures;                                                    \
            std::printf("FAIL %s:%d  %s=%f != %s=%f\n", __FILE__, __LINE__,  \
                        #a, va, #b, vb);                                     \
        }                                                                    \
    } while (0)

// ---- fixture ---------------------------------------------------------------
//
// A 9x5mm two-layer board.
//   F.Cu: net NETA = 4mm + 2mm strokes with a pad flash on each end;
//         net NETB = one 3mm stroke.
//   B.Cu: net NETA = one 2mm stroke starting under the (1,1) pad.
//   Drill: one plated 0.4mm hole at (1,1) joining the layers.
//   F.Cu also carries net NETC: a 1x2mm G36 plane REGION with two pad
//   flashes on it -- copper with no walkable centreline, the "joined through
//   the plane" case.
// Ground truth: NETA routed 8.0mm, NETB routed 3.0mm, NETC routed 0 with
// hasPlane set; 4 segments total; 4 pad flashes. Netless variant: 3 connected
// groups, 1 spanning layers.

static const char* kFCu =
    "%TF.FileFunction,Copper,L1,Top*%\n"
    "%FSLAX46Y46*%\n"
    "%MOMM*%\n"
    "%ADD10C,0.300000*%\n"
    "%ADD11C,1.000000*%\n"
    "G01*\n"
    "D10*\n"
    "%TO.N,NETA*%\n"
    "X1000000Y1000000D02*\n"
    "X5000000Y1000000D01*\n"
    "X5000000Y3000000D01*\n"
    "%TD*%\n"
    "%TO.N,NETB*%\n"
    "X8000000Y1000000D02*\n"
    "X8000000Y4000000D01*\n"
    "%TD*%\n"
    "D11*\n"
    "%TO.N,NETA*%\n"
    "X1000000Y1000000D03*\n"
    "X5000000Y3000000D03*\n"
    "%TD*%\n"
    "%TO.N,NETC*%\n"
    "G36*\n"
    "X6500000Y2500000D02*\n"
    "G01*\n"
    "X7500000Y2500000D01*\n"
    "X7500000Y4500000D01*\n"
    "X6500000Y4500000D01*\n"
    "X6500000Y2500000D01*\n"
    "G37*\n"
    "X6700000Y2700000D03*\n"
    "X7300000Y4300000D03*\n"
    "%TD*%\n"
    "M02*\n";

static const char* kBCu =
    "%TF.FileFunction,Copper,L2,Bot*%\n"
    "%FSLAX46Y46*%\n"
    "%MOMM*%\n"
    "%ADD10C,0.300000*%\n"
    "G01*\n"
    "D10*\n"
    "%TO.N,NETA*%\n"
    "X1000000Y1000000D02*\n"
    "X3000000Y1000000D01*\n"
    "%TD*%\n"
    "M02*\n";

static const char* kProfile =
    "%TF.FileFunction,Profile,NP*%\n"
    "%FSLAX46Y46*%\n"
    "%MOMM*%\n"
    "%ADD10C,0.050000*%\n"
    "G01*\n"
    "D10*\n"
    "X0Y0D02*\n"
    "X9000000Y0D01*\n"
    "X9000000Y5000000D01*\n"
    "X0Y5000000D01*\n"
    "X0Y0D01*\n"
    "M02*\n";

static const char* kDrill =
    "M48\n"
    "METRIC\n"
    "T1C0.400\n"
    "%\n"
    "T1\n"
    "X1.Y1.\n"
    "M30\n";

// Strip every X2 object-attribute line -- the "plotted without advanced X2"
// package that pseudo-net inference exists for.
static std::string stripNets(const std::string& s) {
    std::istringstream in(s);
    std::ostringstream out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("%TO", 0) == 0 || line.rfind("%TD", 0) == 0) continue;
        out << line << "\n";
    }
    return out.str();
}

static fs::path writeFixture(bool withNets) {
    const fs::path dir = fs::temp_directory_path() /
                         (withNets ? "pcbview_fix_nets" : "pcbview_fix_bare");
    fs::create_directories(dir);
    const auto put = [&](const char* name, const std::string& text) {
        std::ofstream f(dir / name, std::ios::binary);
        f << text;
    };
    put("l1.gbr", withNets ? kFCu : stripNets(kFCu));
    put("l2.gbr", withNets ? kBCu : stripNets(kBCu));
    put("edge.gbr", kProfile);
    put("holes.drl", kDrill);
    return dir;
}

// Helpers over the parsed art/mesh, all coordinate-convention-immune.
static double segLen(const geom::LayerArt::NetSeg& s) {
    return std::hypot(s.bx - s.ax, s.by - s.ay);
}
static const geom::LayerArt::NetSeg* findSeg(const geom::BoardMesh& mesh,
                                             double len) {
    for (const auto& s : mesh.netSegments)
        if (std::abs(segLen(s) - len) < 1e-6) return &s;
    return nullptr;
}
static void lerp(const geom::LayerArt::NetSeg& s, double t, double& x,
                 double& y) {
    x = s.ax + (s.bx - s.ax) * t;
    y = s.ay + (s.by - s.ay) * t;
}

// ---- tests -----------------------------------------------------------------

static void testParserTagged() {
    const gerber::GerberImage img = gerber::parseGerber(kFCu, "l1.gbr");
    CHECK(img.ok);
    double netA = -1, netB = -1;
    for (const auto& na : img.nets) {
        if (na.name == "NETA") netA = na.routedMm;
        if (na.name == "NETB") netB = na.routedMm;
        // Every object in this fixture is tagged, so a no-net bucket here
        // would mean copper leaked out of its net -- the exact class of bug
        // that once deleted 19% of a board's top copper.
        CHECK(!na.name.empty());
    }
    CHECK_NEAR(netA, 6.0, 1e-6);
    CHECK_NEAR(netB, 3.0, 1e-6);
    CHECK(img.flashes.size() == 4);
    int aFlash = 0, cFlash = 0;
    for (const auto& fl : img.flashes) {
        if (fl.net == "NETA") ++aFlash;
        if (fl.net == "NETC") ++cFlash;
    }
    CHECK(aFlash == 2);
    CHECK(cFlash == 2);
    // The region marks its net as plane copper; the stroked nets stay clean.
    for (const auto& na : img.nets) {
        if (na.name == "NETC") CHECK(na.hasRegion);
        if (na.name == "NETA" || na.name == "NETB") CHECK(!na.hasRegion);
    }
}

static void testPackageTagged() {
    const geom::LayerArt art =
        gerber::importPackage(writeFixture(true).string());
    CHECK(art.nets.size() == 3);
    double routedA = -1, routedB = -1;
    int netC = -1;
    for (size_t i = 0; i < art.nets.size(); ++i) {
        if (art.nets[i].name == "NETA") routedA = art.nets[i].routedMm;
        if (art.nets[i].name == "NETB") routedB = art.nets[i].routedMm;
        if (art.nets[i].name == "NETC") netC = static_cast<int>(i);
    }
    CHECK_NEAR(routedA, 8.0, 1e-6);  // 4 + 2 on F.Cu, 2 on B.Cu
    CHECK_NEAR(routedB, 3.0, 1e-6);
    CHECK(netC >= 0);
    if (netC >= 0) {
        CHECK(art.nets[netC].hasPlane);  // the G36 region
        CHECK_NEAR(art.nets[netC].routedMm, 0.0, 1e-9);
    }
    CHECK(art.netSegments.size() == 4);
    // All four pad flashes became net-carrying snap targets.
    int netFlashes = 0;
    for (const auto& np : art.netPoints)
        if (np.net >= 0) ++netFlashes;
    CHECK(netFlashes == 4);
    CHECK(!art.netsArePseudo);

    // Plane-only net: no track route between its two pads -- the "joined
    // through the plane" case the readout now names. Pad positions come from
    // the parsed netPoints, keeping this coordinate-convention-immune.
    if (netC >= 0) {
        double cx[2], cy[2];
        int got = 0;
        for (const auto& np : art.netPoints)
            if (np.net == netC && got < 2) {
                cx[got] = np.pos[0];
                cy[got] = np.pos[1];
                ++got;
            }
        CHECK(got == 2);
        const geom::BoardMesh m2 = geom::assemble(art, {});
        if (got == 2)
            CHECK(geom::netPathLength(m2, netC, cx[0], cy[0], cx[1], cy[1]) <
                  0.0);
    }

    // Assemble and walk the measurement solver against exact ground truth.
    const geom::BoardMesh mesh = geom::assemble(art, {});
    CHECK(mesh.netSegments.size() == 4);
    int snapWithNet = 0;
    for (const auto& sp : mesh.snapPoints)
        if (sp.net >= 0) ++snapWithNet;
    CHECK(snapWithNet >= 2);

    const geom::LayerArt::NetSeg* four = findSeg(mesh, 4.0);
    const geom::LayerArt::NetSeg* two = nullptr;
    CHECK(four != nullptr);
    if (!four) return;
    // The 2mm segment SHARING four's net on the same layer chain: both NETA
    // 2mm strokes exist (F and B); the junction test only needs the one that
    // touches the 4mm stroke, so find it by connectivity through the solver
    // itself rather than by position.
    for (const auto& s : mesh.netSegments)
        if (&s != four && s.net == four->net && std::abs(segLen(s) - 2.0) < 1e-6)
            two = &s;
    CHECK(two != nullptr);

    double ax, ay, bx, by;
    // Same segment, interior to interior: |0.75 - 0.25| * 4mm = 2.0.
    lerp(*four, 0.25, ax, ay);
    lerp(*four, 0.75, bx, by);
    CHECK_NEAR(geom::netPathLength(mesh, four->net, ax, ay, bx, by), 2.0,
               1e-3);
    // Across the corner junction: mid of the 4mm + mid of the CONNECTED 2mm
    // = 2.0 + 1.0 (the B.Cu twin is 3mm+ away in 2D, so the graph resolves
    // the touching one).
    lerp(*four, 0.5, ax, ay);
    bool joined = false;
    for (const auto& s : mesh.netSegments) {
        if (s.net != four->net || std::abs(segLen(s) - 2.0) > 1e-6) continue;
        lerp(s, 0.5, bx, by);
        const double d = geom::netPathLength(mesh, four->net, ax, ay, bx, by);
        if (std::abs(d - 3.0) < 1e-3) joined = true;
    }
    CHECK(joined);
    // A point nowhere near the net: no route, honestly reported.
    CHECK(geom::netPathLength(mesh, four->net, ax, ay, ax, ay + 50.0) < 0.0);
}

static void testInference() {
    geom::LayerArt art = gerber::importPackage(writeFixture(false).string());
    CHECK(art.nets.empty());  // genuinely netless

    const geom::PseudoNetStats stats = geom::extractPseudoNets(art);
    // {NETA copper through the barrel}, {NETB stroke}, {NETC plane island}
    CHECK(stats.groups == 3);
    CHECK(stats.connecting == 1);  // only the barrel-joined group spans
    CHECK(art.netsArePseudo);
    CHECK(art.nets.size() == 3);

    // Every untagged stroke was assigned to its island's group.
    CHECK(art.netSegments.size() == 4);
    double routedSum = 0.0;
    for (const auto& n : art.nets) routedSum += n.routedMm;
    CHECK_NEAR(routedSum, 11.0, 1e-6);  // 8 + 3

    // And the solver walks the derived net exactly like a real one.
    const geom::BoardMesh mesh = geom::assemble(art, {});
    const geom::LayerArt::NetSeg* four = findSeg(mesh, 4.0);
    CHECK(four != nullptr);
    if (!four) return;
    double ax, ay, bx, by;
    lerp(*four, 0.25, ax, ay);
    lerp(*four, 0.75, bx, by);
    CHECK_NEAR(geom::netPathLength(mesh, four->net, ax, ay, bx, by), 2.0,
               1e-3);
    // Netless pad flashes still snap -- position without identity.
    bool netlessSnap = false;
    for (const auto& np : art.netPoints)
        if (np.net < 0) netlessSnap = true;
    CHECK(netlessSnap);
}

int main() {
    testParserTagged();
    testPackageTagged();
    testInference();
    if (g_failures == 0) {
        std::printf("OK: all checks passed\n");
        return 0;
    }
    std::printf("%d check(s) FAILED\n", g_failures);
    return 1;
}
