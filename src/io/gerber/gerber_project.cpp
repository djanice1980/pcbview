#include "io/gerber/gerber_project.h"

#include <miniz.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <numbers>
#include <regex>
#include <sstream>
#include <stdexcept>

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

Paths64 parseDrills(const std::string& text, int segments) {
    Paths64 holes;
    std::map<int, double> tools;  // tool number -> diameter (mm)
    double unitScale = 1.0;       // to mm
    int currentTool = -1;

    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        if (line.rfind("METRIC", 0) == 0 || line == "M71") unitScale = 1.0;
        else if (line.rfind("INCH", 0) == 0 || line == "M72") unitScale = 25.4;

        // Tool definition: T<n>C<diameter>  (may also carry feed/speed, ignored)
        std::smatch m;
        if (std::regex_search(line, m,
                              std::regex("^T(\\d+)C([\\d.]+)"))) {
            tools[std::stoi(m[1])] = std::atof(m[2].str().c_str()) * unitScale;
            continue;
        }
        // Tool select on its own line: T<n>
        if (std::regex_match(line, m, std::regex("^T(\\d+)$"))) {
            currentTool = std::stoi(m[1]);
            continue;
        }
        // A drill hit: X..Y.. (decimal, KiCad's format). Y already up-positive,
        // matching gerber and our world -- no flip.
        if (std::regex_search(line, m,
                              std::regex("^X([-\\d.]+)Y([-\\d.]+)"))) {
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
                holes.push_back(std::move(c));
            }
        }
    }
    return holes;
}

// ---- board outline from a stroked Profile layer --------------------------
//
// The Profile gerber is drawn as a thin stroked line, not a filled region. The
// board is the area it encloses, so fill the OUTER boundary of the stroke and
// discard its interior hole. This oversizes the board by half the stroke width
// (~0.05mm on a 0.1mm pen) -- negligible on a 50mm board and invisible once
// copper is clipped to it.
Paths64 boardFromProfile(const Paths64& profileDark) {
    Clipper64 clipper;
    clipper.AddSubject(profileDark);
    PolyTree64 tree;
    clipper.Execute(ClipType::Union, FillRule::NonZero, tree);

    Paths64 out;
    // Top-level polygons are the outer boundaries; their children are the frame
    // interiors, which we intentionally drop.
    for (size_t i = 0; i < tree.Count(); ++i) {
        const PolyPath64* child = tree.Child(i);
        if (!child->IsHole()) out.push_back(child->Polygon());
    }
    geom::normalizeWinding(out);
    return out;
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
                copper.push_back({fn, std::move(img.dark)});
                break;
            case FileFunction::Kind::Soldermask:
                masks.push_back({fn, std::move(img.dark)});
                break;
            case FileFunction::Kind::Silkscreen:
                silks.push_back({fn, std::move(img.dark)});
                break;
            case FileFunction::Kind::Profile:
                profileDark = std::move(img.dark);
                haveProfile = true;
                break;
            default:
                art.warnings.push_back("unclassified gerber ignored: " + f.name);
                break;
        }
    }

    if (!haveProfile) {
        throw std::runtime_error(
            "no board outline (Profile) gerber found -- cannot place geometry");
    }
    art.outline = boardFromProfile(profileDark);

    // 4) Drills.
    for (const NamedFile& f : files) {
        if (looksLikeDrill(f.name)) {
            Paths64 h = parseDrills(f.text, 32);
            art.drills.insert(art.drills.end(), h.begin(), h.end());
        }
    }
    geom::normalizeWinding(art.drills);

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
    for (int i = 0; i < nCopper; ++i) {
        geom::ArtLayer al;
        al.name = copper[i].fn.top ? "F.Cu"
                  : copper[i].fn.bottom ? "B.Cu"
                                        : "In" + std::to_string(i) + ".Cu";
        al.kind = LayerKind::Copper;
        al.thickness = copperT;
        al.z = total - maskT - static_cast<double>(i) * pitch - copperT;
        al.art = std::move(copper[i].dark);
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
