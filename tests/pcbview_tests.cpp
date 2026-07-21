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

#include <clipper2/clipper.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <set>

#include <miniz.h>

#include "geom/connectivity.h"
#include "geom/layer_art.h"
#include "geom/tessellate.h"
#include "io/gerber/gerber_parser.h"
#include "io/gerber/gerber_project.h"
#include "io/ipc2581/ipc2581.h"
#include "io/ipc356/ipc356.h"
#include "io/odb/odb_features.h"
#include "io/odb/odb_fs.h"
#include "io/odb/odb_project.h"

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

// ---- IPC-D-356 -------------------------------------------------------------
//
// A netlist for the SAME fixture board, netless flavour: real names at test
// points sitting on the fixture's known copper. Metric (CUST 1 = 0.001mm),
// so (1,1)mm is X+0001000. The N/C record must be dropped; the 317 record is
// a through-hole point (the plated (1,1) pad).

static const char* k356 =
    "C  IPC-D-356 fixture netlist\n"
    "P  UNITS CUST 1\n"
    "317NETA          TP1   D0400PA00X+0001000Y+0001000X0600Y0600\n"
    "327NETA          TP2        A01X+0005000Y+0003000X1000Y1000\n"
    "327NETB          TP3        A01X+0008000Y+0004000X0300Y0300\n"
    "327NETC          TP4        A01X+0006700Y+0002700X0600Y0600\n"
    "317N/C           TP5   D0400PA00X+0002000Y+0004500X0600Y0600\n"
    "999\n";

static void test356Parser() {
    CHECK(ipc356::looksLike(k356));
    CHECK(!ipc356::looksLike(kFCu));  // a gerber must not sniff as a netlist

    const ipc356::File f = ipc356::parse(k356);
    CHECK(f.ok);
    CHECK(f.warnings.empty());
    CHECK(f.records.size() == 4);  // N/C filtered out
    if (f.records.size() != 4) return;
    CHECK(f.records[0].net == "NETA");
    CHECK(f.records[0].through);
    CHECK_NEAR(f.records[0].x, 1.0, 1e-9);
    CHECK_NEAR(f.records[0].y, 1.0, 1e-9);
    CHECK(!f.records[1].through);
    CHECK_NEAR(f.records[1].x, 5.0, 1e-9);
    CHECK_NEAR(f.records[1].y, 3.0, 1e-9);
    CHECK(f.records[3].net == "NETC");
    CHECK_NEAR(f.records[3].x, 6.7, 1e-9);
    CHECK_NEAR(f.records[3].y, 2.7, 1e-9);

    // Inch flavour: CUST 0 = 0.0001 inch, and negative coordinates.
    const ipc356::File in = ipc356::parse(
        "P  UNITS CUST 0\n"
        "327INCHNET       TP1        A01X+0010000Y-0005000\n");
    CHECK(in.ok);
    if (in.ok && !in.records.empty()) {
        // 10000 units x 0.0001" = 1.0" = 25.4mm.
        CHECK_NEAR(in.records[0].x, 25.4, 1e-9);
        CHECK_NEAR(in.records[0].y, -12.7, 1e-9);
    }
}

static fs::path write356Fixture() {
    const fs::path dir = fs::temp_directory_path() / "pcbview_fix_356";
    fs::create_directories(dir);
    const auto put = [&](const char* name, const std::string& text) {
        std::ofstream f(dir / name, std::ios::binary);
        f << text;
    };
    put("l1.gbr", stripNets(kFCu));
    put("l2.gbr", stripNets(kBCu));
    put("edge.gbr", kProfile);
    put("holes.drl", kDrill);
    put("board.ipc", k356);
    return dir;
}

// The marriage: a netless package plus a 356 file arrives as REAL names at
// test points (netsFromTestPoints), and extractPseudoNets then names its
// copper groups from the contained points instead of inventing "~1".
static void test356Package() {
    geom::LayerArt art = gerber::importPackage(write356Fixture().string());
    CHECK(art.netsFromTestPoints);
    CHECK(!art.netsArePseudo);  // names are real, just unbound
    CHECK(art.nets.size() == 3);
    int named = 0;
    for (const auto& np : art.netPoints)
        if (np.net >= 0) ++named;
    CHECK(named == 5);  // 4 records; the through-hole point at both faces

    const size_t warningsBefore = art.warnings.size();
    const geom::PseudoNetStats stats = geom::extractPseudoNets(art);
    CHECK(stats.groups == 3);
    CHECK(art.netsArePseudo);  // connectivity is still derived...
    CHECK(art.nets.size() == 3);
    std::set<std::string> names;
    for (const auto& n : art.nets) names.insert(n.name);
    // ...but every group carries its netlist name, no "~k" placeholders.
    CHECK(names == std::set<std::string>({"NETA", "NETB", "NETC"}));
    // The marriage itself raised no findings: no opens, no shorts, no orphan
    // test points on this board. (The import may warn about other things.)
    CHECK(art.warnings.size() == warningsBefore);
    for (size_t i = warningsBefore; i < art.warnings.size(); ++i)
        std::printf("  marriage warning: %s\n", art.warnings[i].c_str());

    // Routed lengths still accumulate from the assigned strokes.
    double routedSum = 0.0;
    for (const auto& n : art.nets) routedSum += n.routedMm;
    CHECK_NEAR(routedSum, 11.0, 1e-6);

    // Test points now index the published table under their own names.
    for (const auto& np : art.netPoints)
        if (np.net >= 0)
            CHECK(art.nets[np.net].name[0] != '~');

    // And the path solver walks the REAL-named net.
    const geom::BoardMesh mesh = geom::assemble(art, {});
    const geom::LayerArt::NetSeg* four = findSeg(mesh, 4.0);
    CHECK(four != nullptr);
    if (!four) return;
    CHECK(art.nets[four->net].name == "NETA");
    double ax, ay, bx, by;
    lerp(*four, 0.25, ax, ay);
    lerp(*four, 0.75, bx, by);
    CHECK_NEAR(geom::netPathLength(mesh, four->net, ax, ay, bx, by), 2.0,
               1e-3);
}

// The findings the marriage exists to raise: the same board with a DEFECTIVE
// netlist. NETA is claimed at (1,1) and also on the (8,x) stroke -- two
// unconnected groups, an OPEN. NETB and NETC both land on the plane island --
// one group, a SHORT. Every group still gets a name (opens suffix #2).
static void test356Findings() {
    static const char* kBad =
        "P  UNITS CUST 1\n"
        "317NETA          TP1   D0400PA00X+0001000Y+0001000X0600Y0600\n"
        "327NETA          TP2        A01X+0008000Y+0004000X0300Y0300\n"
        "327NETC          TP3        A01X+0006700Y+0002700X0600Y0600\n"
        "327NETB          TP4        A01X+0007300Y+0004300X0600Y0600\n"
        "999\n";
    const fs::path dir = fs::temp_directory_path() / "pcbview_fix_356b";
    fs::create_directories(dir);
    const auto put = [&](const char* name, const std::string& text) {
        std::ofstream f(dir / name, std::ios::binary);
        f << text;
    };
    put("l1.gbr", stripNets(kFCu));
    put("l2.gbr", stripNets(kBCu));
    put("edge.gbr", kProfile);
    put("holes.drl", kDrill);
    put("board.ipc", kBad);

    geom::LayerArt art = gerber::importPackage(dir.string());
    CHECK(art.netsFromTestPoints);
    const size_t before = art.warnings.size();
    const geom::PseudoNetStats stats = geom::extractPseudoNets(art);
    CHECK(stats.groups == 3);

    bool open = false, shorted = false;
    for (size_t i = before; i < art.warnings.size(); ++i) {
        if (art.warnings[i].find("open?") != std::string::npos) open = true;
        if (art.warnings[i].find("short?") != std::string::npos)
            shorted = true;
    }
    CHECK(open);
    CHECK(shorted);
    std::set<std::string> names;
    for (const auto& n : art.nets) names.insert(n.name);
    CHECK(names.count("NETA") == 1);    // first group claimed keeps the name
    CHECK(names.count("NETA#2") == 1);  // the open's second piece is visible
}

// Arc interpolation, both quadrant modes, strokes and regions -- exact
// analytic ground truth (chord-vs-arc error at the parser's segment count is
// well under the tolerances used).
static void testArcs() {
    // G75 multi-quadrant: one D01 with start == end is a FULL circle.
    // r = 2mm about (5,5): routed length 2*pi*2 = 12.566.
    const char* fullCircle =
        "%FSLAX46Y46*%\n%MOMM*%\n%ADD10C,0.200000*%\n"
        "G01*\nD10*\nG75*\nG03*\n"
        "%TO.N,ARC*%\n"
        "X7000000Y5000000D02*\n"
        "X7000000Y5000000I-2000000J0D01*\n"
        "%TD*%\nM02*\n";
    const gerber::GerberImage a = gerber::parseGerber(fullCircle, "arc75");
    CHECK(a.ok);
    double routed = -1;
    for (const auto& na : a.nets)
        if (na.name == "ARC") routed = na.routedMm;
    CHECK_NEAR(routed, 2.0 * 3.14159265358979 * 2.0, 0.02);

    // G74 single-quadrant: unsigned I/J, arc <= 90 degrees. Quarter circle,
    // r = 2mm: routed pi = 3.1416.
    const char* quarter =
        "%FSLAX46Y46*%\n%MOMM*%\n%ADD10C,0.200000*%\n"
        "G01*\nD10*\nG74*\nG03*\n"
        "%TO.N,ARC*%\n"
        "X7000000Y5000000D02*\n"
        "X5000000Y7000000I2000000J0D01*\n"
        "%TD*%\nM02*\n";
    const gerber::GerberImage b = gerber::parseGerber(quarter, "arc74");
    CHECK(b.ok);
    routed = -1;
    for (const auto& na : b.nets)
        if (na.name == "ARC") routed = na.routedMm;
    CHECK_NEAR(routed, 3.14159265358979, 0.01);

    // A region with an arc edge: a semicircular pour, r = 2mm about (4,2).
    // Area = pi * r^2 / 2 = 6.283 mm^2.
    const char* halfDisc =
        "%FSLAX46Y46*%\n%MOMM*%\n"
        "G01*\nG75*\n"
        "%TO.N,POUR*%\n"
        "G36*\n"
        "X2000000Y2000000D02*\n"
        "G01*X6000000Y2000000D01*\n"
        "G03*X2000000Y2000000I-2000000J0D01*\n"
        "G37*\n"
        "%TD*%\nM02*\n";
    const gerber::GerberImage c = gerber::parseGerber(halfDisc, "arcRegion");
    CHECK(c.ok);
    double area = -1;
    for (const auto& na : c.nets)
        if (na.name == "POUR") {
            area = 0.0;
            for (const auto& p : na.area)
                area += std::abs(Clipper2Lib::Area(p));
            area /= geom::kScale * geom::kScale;
        }
    CHECK_NEAR(area, 3.14159265358979 * 2.0, 0.05);  // pi*r^2/2, r=2
}

// %SR step-and-repeat: the SAME content parsed plain and inside a 2x2 grid
// must replicate exactly -- geometry, net segments, routed length and pad
// flashes all x4, in non-overlapping cells.
static void testStepRepeat() {
    const std::string content =
        "%TO.N,NETS*%\n"
        "X1000000Y1000000D02*\n"
        "X3000000Y1000000D01*\n"
        "D11*\n"
        "X1000000Y1000000D03*\n"
        "%TD*%\n";
    const std::string header =
        "%FSLAX46Y46*%\n%MOMM*%\n%ADD10C,0.300000*%\n%ADD11C,1.000000*%\n"
        "G01*\nD10*\n";
    const gerber::GerberImage plain =
        gerber::parseGerber(header + content + "M02*\n", "plain");
    const gerber::GerberImage sr = gerber::parseGerber(
        header + "%SRX2Y2I10.0J10.0*%\n" + content + "%SR*%\nM02*\n", "sr");
    CHECK(plain.ok);
    CHECK(sr.ok);

    const auto routedOf = [](const gerber::GerberImage& img) {
        for (const auto& na : img.nets)
            if (na.name == "NETS") return na.routedMm;
        return -1.0;
    };
    const auto segsOf = [](const gerber::GerberImage& img) {
        for (const auto& na : img.nets)
            if (na.name == "NETS") return na.segments.size();
        return size_t(0);
    };
    const auto areaOf = [](const gerber::GerberImage& img) {
        double a = 0.0;
        for (const auto& p : img.dark) a += Clipper2Lib::Area(p);
        return a / (geom::kScale * geom::kScale);
    };
    CHECK_NEAR(routedOf(sr), 4.0 * routedOf(plain), 1e-6);
    CHECK(segsOf(sr) == 4 * segsOf(plain));
    CHECK(sr.flashes.size() == 4 * plain.flashes.size());
    CHECK_NEAR(areaOf(sr), 4.0 * areaOf(plain), 1e-3);  // cells don't overlap
    // And the replicated flashes still carry the net.
    for (const auto& fl : sr.flashes) CHECK(fl.net == "NETS");
}

// Corpus: every package committed under tests/corpus/<name>/ is imported and
// held to survival invariants -- no throw, a real outline, copper present,
// and a successful assemble. This is deliberately weaker than the fixture
// assertions above: the corpus exists to feed the parser the WILD, where the
// ground truth is unknown but "imports sane geometry without crashing" is
// still a strong regression net. Growing coverage = dropping a folder in.
static void testCorpus() {
    const fs::path corpus = fs::path(__FILE__).parent_path() / "corpus";
    if (!fs::exists(corpus)) return;
    int packages = 0;
    for (const auto& entry : fs::directory_iterator(corpus)) {
        if (!entry.is_directory()) continue;
        ++packages;
        const std::string name = entry.path().filename().string();
        try {
            // A corpus folder holding matrix/matrix is an ODB++ job; anything
            // else is a Gerber package.
            const geom::LayerArt art =
                odb::isOdbJob(entry.path().string())
                    ? odb::importJob(entry.path().string())
                    : gerber::importPackage(entry.path().string());
            CHECK(!art.outline.empty());
            bool copper = false;
            for (const auto& al : art.layers)
                if (al.kind == LayerKind::Copper && !al.art.empty())
                    copper = true;
            CHECK(copper);
            const geom::BoardMesh mesh = geom::assemble(art, {});
            bool tris = false;
            for (const auto& part : mesh.parts)
                if (!part.mesh.indices.empty()) tris = true;
            CHECK(tris);
        } catch (const std::exception& e) {
            ++g_failures;
            std::printf("FAIL corpus package '%s' threw: %s\n", name.c_str(),
                        e.what());
        }
    }
    std::printf("corpus: %d package(s) imported\n", packages);
}

// ---- ODB++ -----------------------------------------------------------------
//
// The same 9x5 fixture board expressed as an ODB++ product model: matrix,
// profile surface, two signal layers, a plated drill, and an eda/data netlist
// carrying the same NETA/NETB/NETC ground truth as the Gerber fixture --
// including feature-index-addressed net membership (FID), the part most worth
// pinning down.

static const std::map<std::string, std::string>& odbFixtureFiles() {
    static const std::map<std::string, std::string> files = {
        {"matrix/matrix",
         "STEP {\n"
         "   COL=1\n"
         "   NAME=pcb\n"
         "}\n"
         "LAYER {\n"
         "   ROW=1\n"
         "   CONTEXT=BOARD\n"
         "   TYPE=SOLDER_MASK\n"
         "   NAME=mask_top\n"
         "   POLARITY=POSITIVE\n"
         "}\n"
         "LAYER {\n"
         "   ROW=2\n"
         "   CONTEXT=BOARD\n"
         "   TYPE=SIGNAL\n"
         "   NAME=top\n"
         "   POLARITY=POSITIVE\n"
         "}\n"
         "LAYER {\n"
         "   ROW=3\n"
         "   CONTEXT=BOARD\n"
         "   TYPE=SIGNAL\n"
         "   NAME=bottom\n"
         "   POLARITY=POSITIVE\n"
         "}\n"
         "LAYER {\n"
         "   ROW=4\n"
         "   CONTEXT=BOARD\n"
         "   TYPE=DRILL\n"
         "   NAME=drill\n"
         "   POLARITY=POSITIVE\n"
         "   START_NAME=\n"
         "   END_NAME=\n"
         "}\n"},
        // Feature indices (the FIDs below index these, in order):
        //  0: L (1,1)-(5,1)   NETA   4mm
        //  1: L (5,1)-(5,3)   NETA   2mm
        //  2: L (8,1)-(8,4)   NETB   3mm
        //  3: P (1,1)         NETA
        //  4: P (5,3)         NETA
        //  5: S plane 1x2mm   NETC
        //  6: P (6.7,2.7)     NETC
        //  7: P (7.3,4.3)     NETC
        // Symbol dims are MICROMETERS in a metric file (KiCad style, no M).
        {"steps/pcb/layers/top/features",
         "UNITS=MM\n"
         "$0 r300\n"
         "$1 r1000\n"
         "L 1 1 5 1 0 P 0\n"
         "L 5 1 5 3 0 P 0\n"
         "L 8 1 8 4 0 P 0\n"
         "P 1 1 1 P 0 0\n"
         "P 5 3 1 P 0 0\n"
         "S P 0\n"
         "OB 6.5 2.5 I\n"
         "OS 7.5 2.5\n"
         "OS 7.5 4.5\n"
         "OS 6.5 4.5\n"
         "OE\n"
         "SE\n"
         "P 6.7 2.7 1 P 0 0\n"
         "P 7.3 4.3 1 P 0 0\n"},
        {"steps/pcb/layers/bottom/features",
         "UNITS=MM\n"
         "$0 r300\n"
         "$1 r1000\n"
         "L 1 1 3 1 0 P 0\n"
         "P 1 1 1 P 0 0\n"},
        // The explicit M (microns) suffix, the other spelling of the same.
        {"steps/pcb/layers/mask_top/features",
         "UNITS=MM\n"
         "$0 r1200 M\n"
         "P 1 1 0 P 0 0\n"},
        {"steps/pcb/layers/drill/features",
         "UNITS=MM\n"
         "$0 r400\n"
         "P 1 1 0 P 0 0\n"},
        {"steps/pcb/layers/drill/tools",
         "TOOLS {\n"
         "   NUM=1\n"
         "   TYPE=VIA\n"
         "   DRILL_SIZE=15.748\n"
         "}\n"},
        {"steps/pcb/profile",
         "UNITS=MM\n"
         "S P 0\n"
         "OB 0 0 I\n"
         "OS 9 0\n"
         "OS 9 5\n"
         "OS 0 5\n"
         "OE\n"
         "SE\n"},
        {"steps/pcb/eda/data",
         "UNITS=MM\n"
         "LYR top bottom drill\n"
         "NET NETA\n"
         "FID C 0 0\n"
         "FID C 0 1\n"
         "FID C 0 3\n"
         "FID C 0 4\n"
         "FID C 1 0\n"
         "FID C 1 1\n"
         "NET NETB\n"
         "FID C 0 2\n"
         "NET NETC\n"
         "FID C 0 5\n"
         "FID C 0 6\n"
         "FID C 0 7\n"},
    };
    return files;
}

static void checkOdbArt(geom::LayerArt& art, const char* label) {
    CHECK(art.nets.size() == 3);
    if (art.nets.size() != 3) return;
    CHECK(art.nets[0].name == "NETA");
    CHECK(art.nets[1].name == "NETB");
    CHECK(art.nets[2].name == "NETC");
    CHECK_NEAR(art.nets[0].routedMm, 8.0, 1e-6);
    CHECK_NEAR(art.nets[1].routedMm, 3.0, 1e-6);
    CHECK(art.nets[2].hasPlane);
    CHECK(art.netSegments.size() == 4);

    // 45mm^2 board from the profile surface.
    CHECK_NEAR(std::abs(Clipper2Lib::Area(art.outline)) /
                   (geom::kScale * geom::kScale),
               45.0, 1e-3);
    CHECK(art.drills.size() == 1);
    CHECK(art.barrels.size() == 1);  // TYPE=VIA tool -> plated

    int copper = 0, mask = 0;
    for (const auto& al : art.layers) {
        if (al.kind == LayerKind::Copper) ++copper;
        if (al.kind == LayerKind::Soldermask) ++mask;
    }
    CHECK(copper == 2);
    CHECK(mask == 1);

    // Every pad is a net-carrying snap point (5 pads: 3 NETA, 2 NETC).
    int neta = 0, netc = 0;
    for (const auto& np : art.netPoints) {
        if (np.net == 0) ++neta;
        if (np.net == 2) ++netc;
    }
    CHECK(neta == 3);
    CHECK(netc == 2);

    // The measurement solver walks ODB++ nets like any others.
    const geom::BoardMesh mesh = geom::assemble(art, {});
    const geom::LayerArt::NetSeg* four = findSeg(mesh, 4.0);
    CHECK(four != nullptr);
    if (!four) {
        std::printf("  (%s: no 4mm segment)\n", label);
        return;
    }
    CHECK(art.nets[four->net].name == "NETA");
    double ax, ay, bx, by;
    lerp(*four, 0.25, ax, ay);
    lerp(*four, 0.75, bx, by);
    CHECK_NEAR(geom::netPathLength(mesh, four->net, ax, ay, bx, by), 2.0,
               1e-3);
}

static void testOdbDirectory() {
    const fs::path dir = fs::temp_directory_path() / "pcbview_fix_odb";
    for (const auto& [rel, text] : odbFixtureFiles()) {
        const fs::path p = dir / rel;
        fs::create_directories(p.parent_path());
        std::ofstream f(p, std::ios::binary);
        f << text;
    }
    CHECK(odb::isOdbJob(dir.string()));
    geom::LayerArt art = odb::importJob(dir.string());
    checkOdbArt(art, "dir");
}

// The same job as a .tgz with a job-name root folder -- the container format
// ODB++ actually travels in. Exercises the tar reader, gzip inflate, and
// root stripping in one pass.
static void testOdbTgz() {
    std::string tar;
    const auto octal = [](char* dst, size_t width, uint64_t v) {
        std::snprintf(dst, width, "%0*llo", static_cast<int>(width - 1),
                      static_cast<unsigned long long>(v));
    };
    for (const auto& [rel, text] : odbFixtureFiles()) {
        char h[512] = {};
        const std::string name = "job1/" + rel;
        std::memcpy(h, name.c_str(), name.size());
        octal(h + 100, 8, 0644);
        octal(h + 108, 8, 0);
        octal(h + 116, 8, 0);
        octal(h + 124, 12, text.size());
        octal(h + 136, 12, 0);
        std::memset(h + 148, ' ', 8);  // checksum computed over spaces
        h[156] = '0';
        std::memcpy(h + 257, "ustar", 5);
        std::memcpy(h + 263, "00", 2);
        unsigned sum = 0;
        for (unsigned char c : std::string(h, 512)) sum += c;
        octal(h + 148, 7, sum);
        h[155] = ' ';
        tar.append(h, 512);
        tar += text;
        tar.append(512 - text.size() % 512, '\0');
    }
    tar.append(1024, '\0');

    // gzip wrap: fixed header + raw deflate (zlib output minus its 2-byte
    // header and 4-byte adler) + crc32 + isize.
    mz_ulong zlen = mz_compressBound(static_cast<mz_ulong>(tar.size()));
    std::string zbuf(zlen, '\0');
    CHECK(mz_compress(reinterpret_cast<unsigned char*>(zbuf.data()), &zlen,
                      reinterpret_cast<const unsigned char*>(tar.data()),
                      static_cast<mz_ulong>(tar.size())) == MZ_OK);
    std::string gz("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x00", 10);
    gz.append(zbuf.data() + 2, zlen - 6);
    const uint32_t crc = static_cast<uint32_t>(
        mz_crc32(MZ_CRC32_INIT,
                 reinterpret_cast<const unsigned char*>(tar.data()),
                 tar.size()));
    const uint32_t isz = static_cast<uint32_t>(tar.size());
    for (const uint32_t v : {crc, isz})
        for (int i = 0; i < 4; ++i)
            gz.push_back(static_cast<char>((v >> (8 * i)) & 0xff));

    const fs::path tgz = fs::temp_directory_path() / "pcbview_fix_odb.tgz";
    {
        std::ofstream f(tgz, std::ios::binary);
        f << gz;
    }
    CHECK(odb::isOdbJob(tgz.string()));
    geom::LayerArt art = odb::importJob(tgz.string());
    checkOdbArt(art, "tgz");
}

// ---- IPC-2581 --------------------------------------------------------------
//
// The fixture board once more, as IPC-2581 XML: same NETA/NETB/NETC ground
// truth, plus the one thing only this format carries -- a real stackup with
// thicknesses.

static void testIpc2581() {
    static const char* kXml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<IPC-2581 revision=\"C\">\n"
        " <Content>\n"
        "  <DictionaryLineDesc units=\"MILLIMETER\">\n"
        "   <EntryLineDesc id=\"LINE_1\">\n"
        "    <LineDesc lineWidth=\"0.3\" lineEnd=\"ROUND\"/>\n"
        "   </EntryLineDesc>\n"
        "  </DictionaryLineDesc>\n"
        "  <DictionaryStandard units=\"MILLIMETER\">\n"
        "   <EntryStandard id=\"CIRCLE_1\">\n"
        "    <Circle diameter=\"1.0\"/>\n"
        "   </EntryStandard>\n"
        "   <EntryStandard id=\"CIRCLE_M\">\n"
        "    <Circle diameter=\"1.2\"/>\n"
        "   </EntryStandard>\n"
        "  </DictionaryStandard>\n"
        " </Content>\n"
        " <Ecad>\n"
        "  <CadHeader units=\"MILLIMETER\"/>\n"
        "  <CadData>\n"
        "   <Layer name=\"F.Mask\" layerFunction=\"SOLDERMASK\" side=\"TOP\"/>\n"
        "   <Layer name=\"top\" layerFunction=\"CONDUCTOR\" side=\"TOP\"/>\n"
        "   <Layer name=\"DIEL_1\" layerFunction=\"DIELCORE\" side=\"INTERNAL\"/>\n"
        "   <Layer name=\"bottom\" layerFunction=\"CONDUCTOR\" side=\"BOTTOM\"/>\n"
        "   <Layer name=\"drill_all\" layerFunction=\"DRILL\">\n"
        "    <Span fromLayer=\"top\" toLayer=\"bottom\"/>\n"
        "   </Layer>\n"
        "   <Stackup>\n"
        "    <StackupGroup>\n"
        "     <StackupLayer layerOrGroupRef=\"F.Mask\" thickness=\"0.010\" sequence=\"0\"/>\n"
        "     <StackupLayer layerOrGroupRef=\"top\" thickness=\"0.035\" sequence=\"1\"/>\n"
        "     <StackupLayer layerOrGroupRef=\"DIEL_1\" thickness=\"1.510\" sequence=\"2\"/>\n"
        "     <StackupLayer layerOrGroupRef=\"bottom\" thickness=\"0.035\" sequence=\"3\"/>\n"
        "     <StackupLayer layerOrGroupRef=\"B.Mask\" thickness=\"0.010\" sequence=\"4\"/>\n"
        "    </StackupGroup>\n"
        "   </Stackup>\n"
        "   <Step name=\"fixture\">\n"
        "    <Profile>\n"
        "     <Polygon>\n"
        "      <PolyBegin x=\"0\" y=\"0\"/>\n"
        "      <PolyStepSegment x=\"9\" y=\"0\"/>\n"
        "      <PolyStepSegment x=\"9\" y=\"5\"/>\n"
        "      <PolyStepSegment x=\"0\" y=\"5\"/>\n"
        "      <PolyStepSegment x=\"0\" y=\"0\"/>\n"
        "     </Polygon>\n"
        "    </Profile>\n"
        "    <LayerFeature layerRef=\"top\">\n"
        "     <Set net=\"NETA\">\n"
        "      <Features>\n"
        "       <Line startX=\"1\" startY=\"1\" endX=\"5\" endY=\"1\">\n"
        "        <LineDescRef id=\"LINE_1\"/>\n"
        "       </Line>\n"
        "       <Line startX=\"5\" startY=\"1\" endX=\"5\" endY=\"3\">\n"
        "        <LineDescRef id=\"LINE_1\"/>\n"
        "       </Line>\n"
        "      </Features>\n"
        "      <Pad>\n"
        "       <Location x=\"1\" y=\"1\"/>\n"
        "       <StandardPrimitiveRef id=\"CIRCLE_1\"/>\n"
        "      </Pad>\n"
        "      <Pad>\n"
        "       <Location x=\"5\" y=\"3\"/>\n"
        "       <StandardPrimitiveRef id=\"CIRCLE_1\"/>\n"
        "      </Pad>\n"
        "     </Set>\n"
        "     <Set net=\"NETB\">\n"
        "      <Features>\n"
        "       <Line startX=\"8\" startY=\"1\" endX=\"8\" endY=\"4\">\n"
        "        <LineDescRef id=\"LINE_1\"/>\n"
        "       </Line>\n"
        "      </Features>\n"
        "     </Set>\n"
        "     <Set net=\"NETC\">\n"
        "      <Features>\n"
        "       <Contour>\n"
        "        <Polygon>\n"
        "         <PolyBegin x=\"6.5\" y=\"2.5\"/>\n"
        "         <PolyStepSegment x=\"7.5\" y=\"2.5\"/>\n"
        "         <PolyStepSegment x=\"7.5\" y=\"4.5\"/>\n"
        "         <PolyStepSegment x=\"6.5\" y=\"4.5\"/>\n"
        "         <PolyStepSegment x=\"6.5\" y=\"2.5\"/>\n"
        "        </Polygon>\n"
        "       </Contour>\n"
        "      </Features>\n"
        "      <Pad>\n"
        "       <Location x=\"6.7\" y=\"2.7\"/>\n"
        "       <StandardPrimitiveRef id=\"CIRCLE_1\"/>\n"
        "      </Pad>\n"
        "      <Pad>\n"
        "       <Location x=\"7.3\" y=\"4.3\"/>\n"
        "       <StandardPrimitiveRef id=\"CIRCLE_1\"/>\n"
        "      </Pad>\n"
        "     </Set>\n"
        "    </LayerFeature>\n"
        "    <LayerFeature layerRef=\"bottom\">\n"
        "     <Set net=\"NETA\">\n"
        "      <Features>\n"
        "       <Line startX=\"1\" startY=\"1\" endX=\"3\" endY=\"1\">\n"
        "        <LineDescRef id=\"LINE_1\"/>\n"
        "       </Line>\n"
        "      </Features>\n"
        "      <Pad>\n"
        "       <Location x=\"1\" y=\"1\"/>\n"
        "       <StandardPrimitiveRef id=\"CIRCLE_1\"/>\n"
        "      </Pad>\n"
        "     </Set>\n"
        "    </LayerFeature>\n"
        "    <LayerFeature layerRef=\"F.Mask\">\n"
        "     <Set>\n"
        "      <Pad>\n"
        "       <Location x=\"1\" y=\"1\"/>\n"
        "       <StandardPrimitiveRef id=\"CIRCLE_M\"/>\n"
        "      </Pad>\n"
        "     </Set>\n"
        "    </LayerFeature>\n"
        "    <LayerFeature layerRef=\"drill_all\">\n"
        "     <Set net=\"NETA\">\n"
        "      <Hole diameter=\"0.4\" platingStatus=\"VIA\" x=\"1\" y=\"1\"/>\n"
        "     </Set>\n"
        "    </LayerFeature>\n"
        "   </Step>\n"
        "  </CadData>\n"
        " </Ecad>\n"
        "</IPC-2581>\n";

    const fs::path xml = fs::temp_directory_path() / "pcbview_fix_2581.xml";
    {
        std::ofstream f(xml, std::ios::binary);
        f << kXml;
    }
    CHECK(ipc2581::isIpc2581(xml.string()));
    geom::LayerArt art = ipc2581::importFile(xml.string());

    // NETA pad appears on both faces (top pad + bottom pad), so counts match
    // the ODB++ fixture exactly and the shared checker applies as-is.
    checkOdbArt(art, "2581");

    // What only IPC-2581 carries: the real stackup.
    CHECK_NEAR(art.thickness, 1.6, 1e-9);
    for (const auto& al : art.layers)
        if (al.name == "top") CHECK_NEAR(al.z, 1.555, 1e-9);
    bool stackNote = false;
    for (const auto& n : art.notes)
        if (n.find("stackup") != std::string::npos) stackNote = true;
    CHECK(stackNote);
}

// LZW (.Z) decoder sanity: round-trip against a minimal single-width
// compressor (data short enough that no code-width bump occurs).
static void testOdbLzw() {
    const std::string plain = "TOBEORNOTTOBEORTOBEORNOT";
    std::string z = "\x1f\x9d";
    z += static_cast<char>(0x80 | 16);  // block mode, maxbits 16
    uint32_t buf = 0;
    int bufBits = 0;
    const auto emit = [&](int code) {
        buf |= static_cast<uint32_t>(code) << bufBits;
        bufBits += 9;
        while (bufBits >= 8) {
            z.push_back(static_cast<char>(buf & 0xff));
            buf >>= 8;
            bufBits -= 8;
        }
    };
    std::map<std::string, int> dict;
    int next = 257;
    std::string w;
    for (const char c : plain) {
        const std::string wc = w + c;
        if (w.empty() || dict.count(wc)) {
            w = wc;
            continue;
        }
        emit(w.size() == 1 ? static_cast<unsigned char>(w[0]) : dict[w]);
        dict[wc] = next++;
        w = std::string(1, c);
    }
    if (!w.empty())
        emit(w.size() == 1 ? static_cast<unsigned char>(w[0]) : dict[w]);
    if (bufBits > 0) z.push_back(static_cast<char>(buf & 0xff));

    std::string round;
    CHECK(odb::lzwDecompress(z, round));
    CHECK(round == plain);
}

int main() {
    testParserTagged();
    testPackageTagged();
    testInference();
    test356Parser();
    test356Package();
    test356Findings();
    testArcs();
    testStepRepeat();
    testOdbDirectory();
    testOdbTgz();
    testOdbLzw();
    testIpc2581();
    testCorpus();
    if (g_failures == 0) {
        std::printf("OK: all checks passed\n");
        return 0;
    }
    std::printf("%d check(s) FAILED\n", g_failures);
    return 1;
}
