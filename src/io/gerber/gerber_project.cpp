#include "io/gerber/gerber_project.h"

#include <miniz.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <numbers>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <cstdlib>

#include "io/gerber/gerber_parser.h"

namespace pcbview::gerber {
namespace {

namespace fs = std::filesystem;
using namespace Clipper2Lib;

struct NamedFile {
    std::string name;  // basename
    std::string text;
};

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// ---- loading: zip / folder / job -----------------------------------------

std::vector<NamedFile> loadZip(const std::string& path) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, path.c_str(), 0)) {
        throw std::runtime_error("cannot open zip: " + path);
    }
    std::vector<NamedFile> files;
    const mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;

        size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        if (!data) continue;

        NamedFile f;
        // Flatten any interior directory: gerber packages are sometimes zipped
        // with a top folder.
        f.name = fs::path(st.m_filename).filename().string();
        f.text.assign(static_cast<const char*>(data), size);
        mz_free(data);
        files.push_back(std::move(f));
    }
    mz_zip_reader_end(&zip);
    return files;
}

std::vector<NamedFile> loadFolder(const fs::path& dir) {
    std::vector<NamedFile> files;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        NamedFile f;
        f.name = entry.path().filename().string();
        f.text = readFile(entry.path());
        files.push_back(std::move(f));
    }
    return files;
}

// ---- .gbrjob manifest ----------------------------------------------------
//
// Not a full JSON parse; a targeted scan for the two arrays we need. The format
// is machine-generated and regular, so this is robust in practice and avoids a
// JSON dependency.

struct JobFile {
    std::string path;
    FileFunction function;
    bool hasFunction = false;
};

struct StackEntry {
    std::string type;  // Copper, Dielectric, SolderMask, Legend, SolderPaste
    std::string name;  // F.Cu, In1.Cu, ...
    double thickness = 0.0;
    bool hasThickness = false;
};

struct JobManifest {
    std::vector<JobFile> files;
    std::vector<StackEntry> stack;
    bool ok = false;
};

std::string jsonString(const std::string& s, const std::string& key,
                       size_t from, size_t to) {
    std::regex re("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch m;
    std::string sub = s.substr(from, to - from);
    if (std::regex_search(sub, m, re)) return m[1].str();
    return {};
}

bool jsonNumber(const std::string& s, const std::string& key, size_t from,
                size_t to, double& out) {
    std::regex re("\"" + key + "\"\\s*:\\s*([-\\d.eE]+)");
    std::smatch m;
    std::string sub = s.substr(from, to - from);
    if (std::regex_search(sub, m, re)) {
        out = std::atof(m[1].str().c_str());
        return true;
    }
    return false;
}

// Split the [ ... ] after `arrayKey` into top-level { ... } object spans.
std::vector<std::pair<size_t, size_t>> jsonObjects(const std::string& s,
                                                   const std::string& arrayKey) {
    std::vector<std::pair<size_t, size_t>> spans;
    const size_t k = s.find("\"" + arrayKey + "\"");
    if (k == std::string::npos) return spans;
    const size_t lb = s.find('[', k);
    if (lb == std::string::npos) return spans;

    int depth = 0;
    size_t objStart = std::string::npos;
    for (size_t i = lb; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '[') { if (depth == 0 && i != lb) {} ++depth; }
        else if (c == ']') { --depth; if (depth == 0) break; }
        else if (c == '{') { if (objStart == std::string::npos) objStart = i; }
        else if (c == '}') {
            if (objStart != std::string::npos) {
                spans.emplace_back(objStart, i + 1);
                objStart = std::string::npos;
            }
        }
    }
    return spans;
}

JobManifest parseJob(const std::string& text) {
    JobManifest job;
    for (const auto& [a, b] : jsonObjects(text, "FilesAttributes")) {
        JobFile jf;
        jf.path = jsonString(text, "Path", a, b);
        const std::string fn = jsonString(text, "FileFunction", a, b);
        if (!fn.empty()) {
            jf.function = parseFileFunctionPublic(fn);
            jf.hasFunction = true;
        }
        if (!jf.path.empty()) job.files.push_back(std::move(jf));
    }
    for (const auto& [a, b] : jsonObjects(text, "MaterialStackup")) {
        StackEntry se;
        se.type = jsonString(text, "Type", a, b);
        se.name = jsonString(text, "Name", a, b);
        se.hasThickness = jsonNumber(text, "Thickness", a, b, se.thickness);
        if (!se.type.empty()) job.stack.push_back(std::move(se));
    }
    job.ok = !job.files.empty();
    return job;
}

// ---- Excellon drills -----------------------------------------------------

// Parses every hole. If platedOut is given, PLATED holes (PTH / vias) are also
// appended there -- read per TOOL from the Excellon `TA.AperFunction,Plated/
// NonPlated` comments, so a single MERGED (MixedPlating) drill program is split
// correctly instead of being taken as all-one-kind by a file-level guess. The
// `TF.FileFunction` header sets the file-level DEFAULT (an NPTH program with no
// per-tool attributes must not default to plated and grow barrels in its
// mounting holes). Anything this parser cannot represent lands in `warnings`
// rather than failing silently -- silent wrong guesses are how the mounting-hole
// and barrel bugs happened.
Paths64 parseDrills(const std::string& text, int segments,
                    Paths64* platedOut = nullptr,
                    std::vector<std::string>* warnings = nullptr) {
    Paths64 holes;
    std::map<int, double> tools;  // tool number -> diameter (mm)
    std::map<int, bool> toolPlated;
    bool currentPlated = true;    // refined by FileFunction / AperFunction below
    double unitScale = 1.0;       // to mm
    int currentTool = -1;
    size_t modalSkipped = 0;      // X-only / Y-only hits we cannot place
    size_t integerCoords = 0;     // decimal-less coords (implied-decimal format)
    bool dotsSeen = false;        // decimal format legitimately allows "X152Y-90",
                                  // so only an ALL-integer file is implied-decimal

    const auto emitPlated = [&](const Path64& p) {
        const bool plated = toolPlated.count(currentTool) ? toolPlated[currentTool]
                                                          : currentPlated;
        if (plated && platedOut) platedOut->push_back(p);
    };

    // ---- Excellon ROUT mode ----------------------------------------------
    // Some CAM tools emit slots as MILLED PATHS instead of G85 obrounds:
    //   G00X..Y..  rapid move, tool up (enters rout mode)
    //   M15        plunge -- cutting starts at the current position
    //   G01X..Y..  linear cut; G02/G03 arc cuts (I/J centre or A radius)
    //   M16        retract -- the chain so far becomes one swept slot
    //   G05        back to drill mode
    // While the tool is down, a bare X..Y.. line CONTINUES the cut (G01 is
    // modal) -- it must not fall through to the plain drill-hit case.
    bool routMode = false;
    bool toolDown = false;
    bool havePos = false;
    double curX = 0.0, curY = 0.0;
    std::vector<std::array<double, 2>> routPts;  // mm, while tool is down

    // The swept slot is the tool disc dragged along the chain: offset the open
    // polyline outward by the tool radius (round joins, round end caps). A
    // single-point chain (plunge + retract, no move) is just a drilled hole.
    const auto sweepRout = [&]() {
        auto it = tools.find(currentTool);
        const double r = (it != tools.end() ? it->second : 0.0) * 0.5;
        if (routPts.empty()) return;
        if (r <= 0.0) {
            if (warnings)
                warnings->push_back(
                    "routed slot with no tool diameter SKIPPED -- this slot "
                    "is missing");
            routPts.clear();
            return;
        }
        Paths64 swept;
        if (routPts.size() == 1) {
            Path64 c;
            for (int i = 0; i < segments; ++i) {
                const double t = 2.0 * std::numbers::pi * i / segments;
                c.push_back(
                    Point64{geom::toInt(routPts[0][0] + r * std::cos(t)),
                            geom::toInt(routPts[0][1] + r * std::sin(t))});
            }
            swept.push_back(std::move(c));
        } else {
            Path64 chain;
            chain.reserve(routPts.size());
            for (const auto& p : routPts)
                chain.push_back(Point64{geom::toInt(p[0]), geom::toInt(p[1])});
            ClipperOffset co;
            co.ArcTolerance(geom::kScale * 0.005);
            co.AddPath(chain, JoinType::Round, EndType::Round);
            co.Execute(r * geom::kScale, swept);
        }
        bool droppedIsland = false;
        for (Path64& p : swept) {
            // A CLOSED rout loop sweeps to an annulus: outer boundary plus an
            // inner hole around the surviving island. The drill set is
            // winding-normalised downstream, which would flip the hole into a
            // cut -- keep only the outers and say so, rather than silently
            // cutting away an island.
            if (Area(p) < 0) {
                droppedIsland = true;
                continue;
            }
            emitPlated(p);
            holes.push_back(std::move(p));
        }
        if (droppedIsland && warnings)
            warnings->push_back(
                "closed routed path: the island inside it was CUT instead of "
                "kept (annulus holes are not preserved)");
        routPts.clear();
    };

    // A rout move/cut to (x, y): while the tool is down the point extends the
    // chain; tool up it just moves the head.
    const auto routTo = [&](double x, double y) {
        if (toolDown) routPts.push_back({x, y});
        curX = x;
        curY = y;
        havePos = true;
    };

    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        // File-level plating default. KiCad: `TF.FileFunction,NonPlated,...`
        // (NPTH), `,Plated,...` (PTH) or `,MixedPlating,...` (merged; per-tool
        // attributes then refine).
        if (line.find("FileFunction") != std::string::npos) {
            if (line.find("NonPlated") != std::string::npos) currentPlated = false;
            continue;
        }
        // Per-tool plating attribute (applies to the tools defined after it).
        if (line.find("AperFunction") != std::string::npos) {
            currentPlated = line.find("NonPlated") == std::string::npos;
            continue;
        }

        if (line.rfind("METRIC", 0) == 0 || line == "M71") unitScale = 1.0;
        else if (line.rfind("INCH", 0) == 0 || line == "M72") unitScale = 25.4;

        // Tool definition: T<n>C<diameter>  (may also carry feed/speed, ignored)
        std::smatch m;
        if (std::regex_search(line, m,
                              std::regex("^T(\\d+)C([\\d.]+)"))) {
            const int t = std::stoi(m[1]);
            tools[t] = std::atof(m[2].str().c_str()) * unitScale;
            toolPlated[t] = currentPlated;
            continue;
        }
        // Tool select on its own line: T<n>
        if (std::regex_match(line, m, std::regex("^T(\\d+)$"))) {
            currentTool = std::stoi(m[1]);
            continue;
        }

        // ---- Rout-mode commands (see the state block above) ----
        if (std::regex_search(line, m,
                              std::regex("^G0?0X([-\\d.]+)Y([-\\d.]+)"))) {
            if (toolDown) {
                // A rapid move with the tool down is malformed; close the cut
                // rather than dragging it across the board.
                sweepRout();
                toolDown = false;
                if (warnings)
                    warnings->push_back(
                        "rout: G00 rapid move with the tool down -- cut ended "
                        "at the previous point");
            }
            routMode = true;
            curX = std::atof(m[1].str().c_str()) * unitScale;
            curY = std::atof(m[2].str().c_str()) * unitScale;
            havePos = true;
            continue;
        }
        if (line == "M15") {
            if (havePos) {
                toolDown = true;
                routPts.assign(1, {curX, curY});
            } else if (warnings) {
                warnings->push_back(
                    "rout: M15 plunge with no known position SKIPPED");
            }
            continue;
        }
        if (line == "M16") {
            if (toolDown) sweepRout();
            toolDown = false;
            continue;
        }
        if (line == "G05") {
            if (toolDown) {
                sweepRout();
                toolDown = false;
            }
            routMode = false;
            continue;
        }
        if (std::regex_search(line, m,
                              std::regex("^G0?1X([-\\d.]+)Y([-\\d.]+)"))) {
            routTo(std::atof(m[1].str().c_str()) * unitScale,
                   std::atof(m[2].str().c_str()) * unitScale);
            continue;
        }
        if (std::regex_search(
                line, m, std::regex("^G0?([23])X([-\\d.]+)Y([-\\d.]+)"))) {
            // Arc cut. The centre comes as I/J (offset from the START point,
            // the standard form) or as A (radius; two candidate centres, the
            // one giving the <=180-degree arc in the commanded direction).
            const bool cw = (m[1].str() == "2");
            const double x2 = std::atof(m[2].str().c_str()) * unitScale;
            const double y2 = std::atof(m[3].str().c_str()) * unitScale;
            double cx = 0.0, cy = 0.0;
            bool haveCentre = false;
            std::smatch cm;
            if (std::regex_search(line, cm,
                                  std::regex("I([-\\d.]+)J([-\\d.]+)"))) {
                cx = curX + std::atof(cm[1].str().c_str()) * unitScale;
                cy = curY + std::atof(cm[2].str().c_str()) * unitScale;
                haveCentre = true;
            } else if (std::regex_search(line, cm,
                                         std::regex("A([\\d.]+)"))) {
                const double radius =
                    std::atof(cm[1].str().c_str()) * unitScale;
                const double dx = x2 - curX, dy = y2 - curY;
                const double chord = std::hypot(dx, dy);
                if (chord > 1e-9 && radius >= chord * 0.5) {
                    const double h = std::sqrt(
                        std::max(0.0, radius * radius - chord * chord * 0.25));
                    // Left normal of the chord; a CW minor arc keeps its
                    // centre on the RIGHT of the travel direction, CCW left.
                    const double nx = -dy / chord, ny = dx / chord;
                    const double sign = cw ? -1.0 : 1.0;
                    cx = (curX + x2) * 0.5 + sign * h * nx;
                    cy = (curY + y2) * 0.5 + sign * h * ny;
                    haveCentre = true;
                }
            }
            if (haveCentre && toolDown) {
                const double radius = std::hypot(curX - cx, curY - cy);
                double a1 = std::atan2(curY - cy, curX - cx);
                double a2 = std::atan2(y2 - cy, x2 - cx);
                if (cw) {
                    while (a2 >= a1 - 1e-12) a2 -= 2.0 * std::numbers::pi;
                } else {
                    while (a2 <= a1 + 1e-12) a2 += 2.0 * std::numbers::pi;
                }
                const int steps = std::max(
                    2, static_cast<int>(std::ceil(std::abs(a2 - a1) /
                                                  (2.0 * std::numbers::pi /
                                                   segments))));
                for (int i = 1; i <= steps; ++i) {
                    const double t = a1 + (a2 - a1) * i / steps;
                    routPts.push_back({cx + radius * std::cos(t),
                                       cy + radius * std::sin(t)});
                }
                curX = x2;
                curY = y2;
                havePos = true;
            } else {
                if (toolDown && warnings)
                    warnings->push_back(
                        "rout: arc without a resolvable centre approximated "
                        "as a straight line");
                routTo(x2, y2);
            }
            continue;
        }
        // In rout mode a bare X..Y.. continues the current motion (G01 is
        // modal) -- it is NOT a drill hit, so intercept it before that case.
        if (routMode &&
            std::regex_search(line, m,
                              std::regex("^X([-\\d.]+)Y([-\\d.]+)")) &&
            line.find("G85") == std::string::npos) {
            routTo(std::atof(m[1].str().c_str()) * unitScale,
                   std::atof(m[2].str().c_str()) * unitScale);
            continue;
        }

        // An Excellon routed slot: X<x1>Y<y1>G85X<x2>Y<y2>. The tool is swept
        // from A to B, so the hole is an obround (the Minkowski sum of the
        // segment with a disc of the tool radius), not a point. This MUST be
        // tested before the plain-hole case: a plain X..Y.. search matches the
        // slot's leading coordinate and would cut only a dot at one end --
        // exactly the "holes not cut" symptom on KiCad boards with oblong pads.
        std::smatch g85;
        if (std::regex_search(
                line, g85,
                std::regex("^X([-\\d.]+)Y([-\\d.]+)G85X([-\\d.]+)Y([-\\d.]+)"))) {
            auto it = tools.find(currentTool);
            const double r = (it != tools.end() ? it->second : 0.0) * 0.5;
            if (r > 0.0) {
                const double x1 = std::atof(g85[1].str().c_str()) * unitScale;
                const double y1 = std::atof(g85[2].str().c_str()) * unitScale;
                const double x2 = std::atof(g85[3].str().c_str()) * unitScale;
                const double y2 = std::atof(g85[4].str().c_str()) * unitScale;
                // Obround = a rounded-rectangle capsule: a semicircle cap of
                // radius r centred on each endpoint, joined by the two straight
                // sides. Built directly (no offset library) so a 2-point slot
                // can never trip an edge case in ClipperOffset.
                const double a = std::atan2(y2 - y1, x2 - x1);
                const int caps = std::max(3, segments / 2);
                Path64 poly;
                poly.reserve(2 * (caps + 1));
                // Leading cap around B, sweeping a-90deg .. a+90deg.
                for (int i = 0; i <= caps; ++i) {
                    const double t = a - std::numbers::pi / 2 +
                                     std::numbers::pi * i / caps;
                    poly.push_back(Point64{geom::toInt(x2 + r * std::cos(t)),
                                           geom::toInt(y2 + r * std::sin(t))});
                }
                // Trailing cap around A, sweeping a+90deg .. a+270deg.
                for (int i = 0; i <= caps; ++i) {
                    const double t = a + std::numbers::pi / 2 +
                                     std::numbers::pi * i / caps;
                    poly.push_back(Point64{geom::toInt(x1 + r * std::cos(t)),
                                           geom::toInt(y1 + r * std::sin(t))});
                }
                emitPlated(poly);
                holes.push_back(std::move(poly));
            }
            continue;
        }
        // A round drill hit: X..Y.. (decimal, KiCad's format). Y already
        // up-positive, matching gerber and our world -- no flip.
        if (std::regex_search(line, m,
                              std::regex("^X([-\\d.]+)Y([-\\d.]+)"))) {
            // Decimal-less coordinates MAY mean an implied-decimal Excellon
            // format (leading/trailing-zero suppressed), where parsing them as
            // plain numbers places holes at absurd positions. Decimal format
            // also legitimately emits integer values, so this only becomes a
            // warning if the WHOLE file never shows a decimal point.
            if (m[1].str().find('.') == std::string::npos &&
                m[2].str().find('.') == std::string::npos) {
                ++integerCoords;
            } else {
                dotsSeen = true;
            }
            const double x = std::atof(m[1].str().c_str()) * unitScale;
            const double y = std::atof(m[2].str().c_str()) * unitScale;
            auto it = tools.find(currentTool);
            const double r = (it != tools.end() ? it->second : 0.0) * 0.5;
            if (r > 0.0) {
                Path64 c;
                for (int i = 0; i < segments; ++i) {
                    const double t = 2.0 * std::numbers::pi * i / segments;
                    c.push_back(Point64{geom::toInt(x + r * std::cos(t)),
                                        geom::toInt(y + r * std::sin(t))});
                }
                emitPlated(c);
                holes.push_back(std::move(c));
            }
        } else if (line[0] == 'X' || line[0] == 'Y') {
            // An X-only or Y-only hit: Excellon MODAL coordinates (the missing
            // axis repeats the previous value). Not supported -- count it so the
            // user learns holes are missing instead of never noticing.
            ++modalSkipped;
        }
    }

    // A file that ends mid-cut still owes its slot.
    if (toolDown) {
        sweepRout();
        if (warnings)
            warnings->push_back("rout: file ended with the tool down");
    }

    if (warnings) {
        if (modalSkipped > 0) {
            warnings->push_back(
                std::to_string(modalSkipped) +
                " drill hits use modal (X-only/Y-only) coordinates and were "
                "SKIPPED -- these holes are missing");
        }
        if (integerCoords > 0 && !dotsSeen) {
            warnings->push_back(
                std::to_string(integerCoords) +
                " drill hits use implied-decimal (integer) coordinates, which "
                "are not supported -- hole positions are likely WRONG");
        }
    }
    return holes;
}

// ---- board outline from a stroked Profile layer --------------------------
//
// The Profile gerber is drawn as thin stroked lines, not filled regions -- one
// closed loop for the board perimeter, plus one for every internal cutout drawn
// on Edge_Cuts (mounting holes, slots, voids). The board is the perimeter's area
// MINUS every cutout, so a single perimeter-outer fill (dropping the rest) leaves
// mounting holes uncut.
//
// Union the strokes into ribbons, then keep each ribbon's OUTER boundary and
// even-odd fill them: Clipper orients nested boundaries alternately, so all outer
// boundaries (perimeter + every cutout) share one sign while the inner boundaries
// take the other. Even-odd filling the outers gives the board with cutouts
// removed -- a point inside the perimeter alone is enclosed once (solid), inside a
// mounting hole twice (empty), inside an island within a cutout three times
// (solid), and so on to any depth. Oversizes every edge by half the pen width
// (~0.05mm) -- negligible, and invisible once copper is clipped to it.
Paths64 boardFromProfile(const Paths64& profileDark) {
    const Paths64 ribbons = Union(profileDark, FillRule::NonZero);

    // The perimeter is the largest-area contour; its orientation defines "outer".
    double maxAbs = 0.0, perimeterArea = 1.0;
    for (const Path64& p : ribbons) {
        const double a = Area(p);
        if (std::abs(a) > maxAbs) { maxAbs = std::abs(a); perimeterArea = a; }
    }
    const bool perimeterPositive = perimeterArea > 0.0;

    Paths64 outers;
    for (const Path64& p : ribbons) {
        const double a = Area(p);
        if (a != 0.0 && (a > 0.0) == perimeterPositive) outers.push_back(p);
    }

    // Even-odd fill yields the board as a polygon-WITH-HOLES: outer boundaries
    // positive, cutouts negative. Do NOT normalizeWinding here -- that forces
    // every path positive, which turns the holes back into solid islands (they
    // then merge into the board under assemble()'s NonZero union and the mounting
    // holes vanish). assemble() consumes the opposite-wound holes correctly.
    return Union(outers, FillRule::EvenOdd);
}

bool endsWithNoCase(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    return toLower(s.substr(s.size() - suffix.size())) == toLower(suffix);
}

bool looksLikeDrill(const std::string& name) {
    const std::string n = toLower(name);
    return endsWithNoCase(n, ".drl") || endsWithNoCase(n, ".xln") ||
           endsWithNoCase(n, ".txt") || n.find("drill") != std::string::npos;
}

}  // namespace

bool isGerberPackage(const std::string& path) {
    const std::string p = toLower(path);
    if (endsWithNoCase(p, ".zip") || endsWithNoCase(p, ".gbrjob")) return true;
    return fs::is_directory(path);
}

geom::LayerArt importPackage(const std::string& path) {
    // 1) Gather the files, whatever the container.
    std::vector<NamedFile> files;
    fs::path baseDir;
    if (endsWithNoCase(path, ".zip")) {
        files = loadZip(path);
    } else if (endsWithNoCase(path, ".gbrjob")) {
        baseDir = fs::path(path).parent_path();
        files = loadFolder(baseDir);
    } else if (fs::is_directory(path)) {
        baseDir = path;
        files = loadFolder(baseDir);
    } else {
        throw std::runtime_error("not a gerber package: " + path);
    }
    if (files.empty()) throw std::runtime_error("no files found in " + path);

    geom::LayerArt art;
    art.sourcePath = path;

    // 2) The manifest, if present.
    JobManifest job;
    std::map<std::string, FileFunction> fnByFile;  // basename -> function
    for (const NamedFile& f : files) {
        if (endsWithNoCase(f.name, ".gbrjob")) {
            job = parseJob(f.text);
        }
    }
    if (job.ok) {
        for (const JobFile& jf : job.files) {
            if (jf.hasFunction) {
                fnByFile[fs::path(jf.path).filename().string()] = jf.function;
            }
        }
    }

    // 3) Parse each gerber; classify by manifest, else its own X2 attribute.
    struct ParsedLayer {
        FileFunction fn;
        Paths64 dark;
        std::vector<NetArea> nets;  // from %TO.N%, empty when absent
        std::vector<GerberImage::Flash> flashes;  // D03 centres (pads)
    };
    std::vector<ParsedLayer> copper, masks, silks;
    Paths64 profileDark;
    bool haveProfile = false;

    for (const NamedFile& f : files) {
        if (endsWithNoCase(f.name, ".gbrjob")) continue;
        if (looksLikeDrill(f.name)) continue;
        // Cheap sniff: gerber files start with attribute/format commands.
        if (f.text.find('%') == std::string::npos &&
            f.text.find("G04") == std::string::npos) {
            continue;
        }

        GerberImage img = parseGerber(f.text, f.name);
        for (const std::string& w : img.warnings) art.warnings.push_back(w);
        if (!img.ok) continue;

        FileFunction fn = img.function;
        auto it = fnByFile.find(f.name);
        if (it != fnByFile.end()) fn = it->second;  // manifest wins

        switch (fn.kind) {
            case FileFunction::Kind::Copper:
                copper.push_back({fn, std::move(img.dark), std::move(img.nets),
                                  std::move(img.flashes)});
                break;
            case FileFunction::Kind::Soldermask:
                masks.push_back({fn, std::move(img.dark), {}});
                break;
            case FileFunction::Kind::Silkscreen:
                silks.push_back({fn, std::move(img.dark), {}});
                break;
            case FileFunction::Kind::Profile:
                profileDark = std::move(img.dark);
                haveProfile = true;
                break;
            case FileFunction::Kind::Paste:
                // Recognised, deliberately unused: solder paste is a stencil
                // aperture, not something on the finished board.
                art.notes.push_back("solder paste layer not rendered (it is "
                                    "a stencil, not board copper): " +
                                    f.name);
                break;
            case FileFunction::Kind::Documentation:
                // Also recognised and deliberately unused. These files declare
                // themselves via TF.FileFunction, so they are identified, not
                // mysterious -- they simply describe the board rather than
                // being part of it.
                art.notes.push_back(
                    "drill map / fab drawing not rendered (it documents the "
                    "board, it is not a layer of it): " + f.name);
                break;
            default:
                art.warnings.push_back(
                    "gerber not identified, ignored (no usable "
                    "TF.FileFunction and the filename matches no known "
                    "layer): " + f.name);
                break;
        }
    }

    if (!haveProfile) {
        throw std::runtime_error(
            "no board outline (Profile) gerber found -- cannot place geometry");
    }
    art.outline = boardFromProfile(profileDark);

    // Sanity: a healthy board fills a large share of its bounding box. If the
    // Edge_Cuts strokes do not CLOSE, the fill degenerates to the thin stroke
    // ribbon itself -- a sliver of area -- and everything downstream quietly
    // clips to almost nothing. Warn instead of letting that fail silently.
    {
        int64_t xmin = INT64_MAX, ymin = INT64_MAX;
        int64_t xmax = INT64_MIN, ymax = INT64_MIN;
        for (const Path64& p : art.outline)
            for (const Point64& pt : p) {
                xmin = std::min(xmin, pt.x); xmax = std::max(xmax, pt.x);
                ymin = std::min(ymin, pt.y); ymax = std::max(ymax, pt.y);
            }
        const double bboxArea = static_cast<double>(xmax - xmin) *
                                static_cast<double>(ymax - ymin);
        if (bboxArea > 0.0 && Area(art.outline) < 0.05 * bboxArea) {
            art.warnings.push_back(
                "board outline fills <5% of its bounding box -- the Profile "
                "strokes may not form a closed loop");
        }
    }

    // 4) Drills. Every hole is subtracted from the board; PLATED holes (PTH /
    // vias) additionally get a copper barrel (see assemble()). Plated vs not is
    // read from the Excellon FileFunction header, falling back to the filename
    // (NPTH = non-plated) -- KiCad emits separate PTH and NPTH programs.
    for (const NamedFile& f : files) {
        if (!looksLikeDrill(f.name)) continue;
        // Per-tool plating (handles merged MixedPlating programs); a filename that
        // says NPTH forces the whole file non-plated as a fallback for files with
        // no AperFunction attributes.
        Paths64 plated;
        Paths64 h = parseDrills(f.text, 32, &plated, &art.warnings);
        art.drills.insert(art.drills.end(), h.begin(), h.end());
        if (toLower(f.name).find("npth") == std::string::npos)
            art.barrels.insert(art.barrels.end(), plated.begin(), plated.end());
    }
    geom::normalizeWinding(art.drills);
    geom::normalizeWinding(art.barrels);

    // 5) Stackup Z. Prefer the manifest's real thicknesses; else fall back to
    // KiCad defaults and an even split, exactly like the KiCad importer.
    std::sort(copper.begin(), copper.end(), [](const auto& a, const auto& b) {
        return a.fn.copperIndex < b.fn.copperIndex;  // L1 (top) first
    });
    const int nCopper = static_cast<int>(copper.size());

    double copperT = 0.035, maskT = 0.010, dielT = 0.0, total = 1.6;
    if (job.ok && !job.stack.empty()) {
        double sum = 0.0;
        int dielCount = 0;
        for (const StackEntry& se : job.stack) {
            if (!se.hasThickness) continue;
            sum += se.thickness;
            if (se.type == "Copper") copperT = se.thickness;
            else if (se.type == "SolderMask") maskT = se.thickness;
            else if (se.type == "Dielectric") { dielT = se.thickness; ++dielCount; }
        }
        if (sum > 0.0) total = sum;
        (void)dielCount;
    } else {
        if (nCopper > 1)
            dielT = (total - nCopper * copperT - 2 * maskT) / (nCopper - 1);
        art.warnings.push_back(
            "no stackup manifest; using derived thicknesses");
    }
    art.thickness = total;
    art.copperThickness = copperT;
    art.maskThickness = maskT;
    art.silkThickness = 0.010;

    const double pitch = copperT + dielT;
    // Net table across every copper layer. A net normally appears on several
    // layers, so the index must be global to the board -- the same identity the
    // KiCad path produces, which is what net highlighting, the Nets panel and
    // the measure tool all key off.
    std::map<std::string, int> netIndex;
    for (const ParsedLayer& pl : copper) {
        for (const NetArea& na : pl.nets) {
            if (na.name.empty()) continue;  // the no-net bucket has no entry
            auto [it, inserted] = netIndex.emplace(na.name,
                                                   static_cast<int>(art.nets.size()));
            if (inserted) art.nets.push_back({na.name, 0.0, 0});
            art.nets[it->second].routedMm += na.routedMm;
            for (const NetArea::Seg& sg : na.segments)
                art.netSegments.push_back({sg.ax, sg.ay, sg.bx, sg.by, it->second});
        }
    }
    if (!art.nets.empty())
        art.notes.push_back(
            "Gerber X2 net attributes found: " + std::to_string(art.nets.size()) +
            " nets (net highlighting and routed length available)");

    for (int i = 0; i < nCopper; ++i) {
        geom::ArtLayer al;
        al.name = copper[i].fn.top ? "F.Cu"
                  : copper[i].fn.bottom ? "B.Cu"
                                        : "In" + std::to_string(i) + ".Cu";
        al.kind = LayerKind::Copper;
        al.thickness = copperT;
        al.z = total - maskT - static_cast<double>(i) * pitch - copperT;
        al.art = std::move(copper[i].dark);
        // Untagged strokes, kept per layer for pseudo-net inference (which
        // assigns each to the island containing it). Nothing reads these when
        // the package carries real nets.
        for (const NetArea& na : copper[i].nets) {
            if (!na.name.empty()) continue;
            for (const NetArea::Seg& sg : na.segments)
                al.looseSegments.push_back({sg.ax, sg.ay, sg.bx, sg.by});
        }
        // Every pad flash is a measurement snap target -- the magnetic pad
        // clicking KiCad boards get from their pad table, which a Gerber
        // otherwise lacks entirely. The marker sits on the layer's outward
        // face; the net comes from the flash's X2 tag when present.
        for (const GerberImage::Flash& fl : copper[i].flashes) {
            geom::LayerArt::NetPoint np;
            np.pos[0] = fl.x;
            np.pos[1] = fl.y;
            np.pos[2] = copper[i].fn.bottom ? al.z : al.z + al.thickness;
            const auto nit = netIndex.find(fl.net);
            np.net = (fl.net.empty() || nit == netIndex.end()) ? -1
                                                               : nit->second;
            art.netPoints.push_back(np);
        }
        // Per-net split of this layer's copper, so the mesh keeps net identity
        // per triangle. Anything with no TO.N stays in the -1 bucket and
        // behaves exactly as Gerber always has.
        for (NetArea& na : copper[i].nets) {
            geom::ArtLayer::NetRegion nr;
            // Untagged copper still has to be EXTRUDED, just not highlightable:
            // net -1 is the "belongs to no net" bucket, and omitting it would
            // delete that copper from the board entirely.
            const auto it = netIndex.find(na.name);
            nr.net = (it == netIndex.end()) ? -1 : it->second;
            nr.paths = std::move(na.area);
            al.netArt.push_back(std::move(nr));
        }
        art.layers.push_back(std::move(al));
    }

    // Masks: art holds the OPENINGS, which is exactly the gerber dark image.
    for (ParsedLayer& m : masks) {
        geom::ArtLayer al;
        al.name = m.fn.top ? "F.Mask" : "B.Mask";
        al.kind = LayerKind::Soldermask;
        al.thickness = maskT;
        al.z = m.fn.top ? total - maskT : 0.0;
        al.openings = static_cast<int>(m.dark.size());
        al.art = std::move(m.dark);
        art.layers.push_back(std::move(al));
    }

    for (ParsedLayer& s : silks) {
        geom::ArtLayer al;
        al.name = s.fn.top ? "F.SilkS" : "B.SilkS";
        al.kind = LayerKind::Silkscreen;
        al.thickness = art.silkThickness;
        al.z = s.fn.top ? total : -art.silkThickness;
        al.art = std::move(s.dark);
        art.layers.push_back(std::move(al));
    }

    return art;
}

}  // namespace pcbview::gerber
