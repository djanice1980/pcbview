#include "io/altium/altium_pcb.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <numbers>
#include <set>
#include <sstream>
#include <stdexcept>

#include "io/altium/compoundfilereader.h"
#include "io/shapes.h"

using namespace Clipper2Lib;

namespace pcbview::altium {
namespace {

// ---- units -----------------------------------------------------------------
//
// Altium's internal unit is 1/10000 mil (0.1 uinch = 2.54nm). Y is NOT
// negated: Altium's board space is Y-up, and so is ours -- KiCad's importer
// negates only because KiCad is Y-down.

double rawToMm(int32_t v) { return v * 2.54e-6; }

// Property values like "12.3mil" -> mm.
double milPropToMm(const std::string& s) {
    if (s.size() < 3 || s.compare(s.size() - 3, 3, "mil") != 0) return 0.0;
    return std::atof(s.c_str()) * 0.0254;
}

// Altium layer ids (subset that matters to a viewer).
constexpr int kTopLayer = 1;
constexpr int kBottomLayer = 32;
constexpr int kTopOverlay = 33;
constexpr int kBottomOverlay = 34;
constexpr int kTopSolder = 37;
constexpr int kBottomSolder = 38;
constexpr int kMultiLayer = 74;
constexpr uint16_t kNoNet = 0xFFFF;
constexpr uint16_t kNoPoly = 0xFFFF;

bool isCopperId(int id) { return id >= kTopLayer && id <= kBottomLayer; }

// ---- binary stream reader --------------------------------------------------

class Reader {
public:
    Reader(const char* data, size_t size) : pos_(data), end_(data + size) {}

    size_t remaining() const { return end_ - pos_; }
    bool error() const { return error_; }

    template <typename T>
    T read() {
        if (remaining() < sizeof(T)) {
            pos_ = end_;
            error_ = true;
            return T{};
        }
        T v;
        std::memcpy(&v, pos_, sizeof(T));
        pos_ += sizeof(T);
        return v;
    }
    void skip(size_t n) {
        if (remaining() < n) {
            pos_ = end_;
            error_ = true;
        } else {
            pos_ += n;
        }
    }
    double readMm() { return rawToMm(read<int32_t>()); }

    size_t beginSubrecord() {
        const uint32_t len = read<uint32_t>();
        subEnd_ = (remaining() >= len) ? pos_ + len : end_;
        return len;
    }
    size_t subRemaining() const {
        return subEnd_ && subEnd_ > pos_ ? subEnd_ - pos_ : 0;
    }
    void endSubrecord() {
        if (subEnd_) pos_ = subEnd_;
        subEnd_ = nullptr;
    }

    // u8-length-prefixed string (pad names).
    std::string readShortString() {
        const uint8_t len = read<uint8_t>();
        if (remaining() < len) {
            error_ = true;
            return {};
        }
        std::string s(pos_, len);
        pos_ += len;
        return s;
    }

    // u32-length-prefixed "|KEY=VALUE|...": keys uppercased. The high byte
    // of the length flags a binary blob (ignored here).
    std::map<std::string, std::string> readProperties() {
        std::map<std::string, std::string> kv;
        uint32_t length = read<uint32_t>();
        const bool isBinary = (length & 0xff000000u) != 0;
        length &= 0x00ffffffu;
        if (length > remaining()) {
            error_ = true;
            return kv;
        }
        std::string str(pos_, length);
        pos_ += length;
        if (isBinary) return kv;
        while (!str.empty() && str.back() == '\0') str.pop_back();

        size_t i = 0;
        while (i < str.size()) {
            const size_t bar = str.find('|', i);
            const size_t start = (bar == i) ? i + 1 : i;
            size_t next = str.find('|', start);
            if (next == std::string::npos) next = str.size();
            const size_t eq = str.find('=', start);
            if (eq != std::string::npos && eq < next) {
                std::string key = str.substr(start, eq - start);
                std::transform(key.begin(), key.end(), key.begin(),
                               [](unsigned char c) { return std::toupper(c); });
                kv[key] = str.substr(eq + 1, next - eq - 1);
            }
            i = next + 1;
        }
        return kv;
    }

private:
    const char* pos_;
    const char* end_;
    const char* subEnd_ = nullptr;
    bool error_ = false;
};

std::string prop(const std::map<std::string, std::string>& kv,
                 const std::string& key, const std::string& def = {}) {
    const auto it = kv.find(key);
    return it == kv.end() ? def : it->second;
}
int propInt(const std::map<std::string, std::string>& kv,
            const std::string& key, int def = 0) {
    const auto it = kv.find(key);
    return it == kv.end() ? def : std::atoi(it->second.c_str());
}
double propMil(const std::map<std::string, std::string>& kv,
               const std::string& key) {
    return milPropToMm(prop(kv, key));
}
double propDouble(const std::map<std::string, std::string>& kv,
                  const std::string& key, double def = 0) {
    const auto it = kv.find(key);
    return it == kv.end() ? def : std::atof(it->second.c_str());
}

// ---- vertex lists (board outline, polygons) --------------------------------

struct Vertex {
    bool isRound = false;
    double x = 0, y = 0;       // mm
    double cx = 0, cy = 0;     // arc centre
    double radius = 0;         // mm
    double sa = 0, ea = 0;     // degrees, CCW, Y-up native
};

std::vector<Vertex> verticesFromProps(
    const std::map<std::string, std::string>& kv) {
    std::vector<Vertex> out;
    for (int i = 0;; ++i) {
        const std::string si = std::to_string(i);
        const auto vx = kv.find("VX" + si);
        const auto vy = kv.find("VY" + si);
        if (vx == kv.end() || vy == kv.end()) break;
        Vertex v;
        v.isRound = propInt(kv, "KIND" + si) != 0;
        v.x = milPropToMm(vx->second);
        v.y = milPropToMm(vy->second);
        v.radius = propMil(kv, "R" + si);
        v.sa = propDouble(kv, "SA" + si);
        v.ea = propDouble(kv, "EA" + si);
        v.cx = propMil(kv, "CX" + si);
        v.cy = propMil(kv, "CY" + si);
        out.push_back(v);
    }
    return out;
}

// Chain the vertices into a closed path. A round vertex contributes its arc
// (sa->ea CCW about the centre); which END joins the chain first is decided
// by which is nearer the vertex position -- the same disambiguation KiCad's
// importer uses.
Path64 pathFromVertices(const std::vector<Vertex>& verts) {
    Path64 out;
    for (const Vertex& v : verts) {
        if (!v.isRound) {
            out.push_back(io::toClip(v.x, v.y));
            continue;
        }
        const double sa = v.sa * std::numbers::pi / 180.0;
        const double ea = v.ea * std::numbers::pi / 180.0;
        const double sx = v.cx + v.radius * std::cos(sa);
        const double sy = v.cy + v.radius * std::sin(sa);
        const double ex = v.cx + v.radius * std::cos(ea);
        const double ey = v.cy + v.radius * std::sin(ea);
        const double dStart = std::hypot(sx - v.x, sy - v.y);
        const double dEnd = std::hypot(ex - v.x, ey - v.y);
        if (dStart < dEnd) {
            out.push_back(io::toClip(sx, sy));
            io::arcAppend(out, sx, sy, ex, ey, v.cx, v.cy, /*cw=*/false);
        } else {
            out.push_back(io::toClip(ex, ey));
            io::arcAppend(out, ex, ey, sx, sy, v.cx, v.cy, /*cw=*/true);
        }
    }
    return out;
}

// ---- decoded records -------------------------------------------------------

struct Track {
    int layer = 0;
    uint16_t net = kNoNet, polygon = kNoPoly;
    bool keepout = false, polygonoutline = false;
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0, width = 0;
};

struct Arc {
    int layer = 0;
    uint16_t net = kNoNet, polygon = kNoPoly;
    bool keepout = false, polygonoutline = false;
    double cx = 0, cy = 0, radius = 0, sa = 0, ea = 0, width = 0;
};

struct Via {
    uint16_t net = kNoNet;
    double x = 0, y = 0, diameter = 0, holesize = 0;
    int layerStart = kTopLayer, layerEnd = kBottomLayer;
    bool tentTop = false, tentBottom = false;
};

struct Pad {
    int layer = 0;
    uint16_t net = kNoNet;
    bool plated = true, tentTop = false, tentBottom = false;
    double x = 0, y = 0;
    double topW = 0, topH = 0, botW = 0, botH = 0, midW = 0, midH = 0;
    int topShape = 1, botShape = 1, midShape = 1;  // 1=circle 2=rect 3=oct
    double rotation = 0;  // degrees CCW
    double holesize = 0;
    double roundRadiusTop = -1;  // mm, from ROUNDRECT alt shape; <0 = none
};

struct Fill {
    int layer = 0;
    uint16_t net = kNoNet;
    bool keepout = false;
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0, rotation = 0;
};

struct Region {
    int layer = 0;
    uint16_t net = kNoNet, polygon = kNoPoly;
    bool keepout = false;
    enum class Kind { Copper, BoardCutout, PolygonCutout, Other } kind =
        Kind::Other;
    Paths64 outline;  // islands minus holes, resolved
};

struct Polygon {
    int layer = 0;
    uint16_t net = kNoNet;
    Path64 outline;
};

// The pad's flashed geometry at the origin (unrotated) for one face.
Paths64 padShape(int shape, double w, double h, double roundRadius) {
    if (w <= 0 || h <= 0) return {};
    if (roundRadius >= 0 && shape == 2) {
        const double r = std::min(roundRadius, std::min(w, h) * 0.5 - 1e-6);
        if (r > 1e-6) return io::roundedRect(w, h, r);
        return {io::rectPath(w, h)};
    }
    switch (shape) {
        case 1:  // circle -- unequal axes make it an oval
            if (std::abs(w - h) < 1e-9)
                return {io::circlePath(0, 0, w * 0.5)};
            return io::stadium(w, h);
        case 2:
            return {io::rectPath(w, h)};
        case 3: {  // octagon, Altium style: 50% corner cut of the short side
            const double cut = std::min(w, h) * 0.25;
            const double hw = w * 0.5, hh = h * 0.5;
            return {Path64{io::toClip(-hw + cut, -hh), io::toClip(hw - cut, -hh),
                           io::toClip(hw, -hh + cut), io::toClip(hw, hh - cut),
                           io::toClip(hw - cut, hh), io::toClip(-hw + cut, hh),
                           io::toClip(-hw, hh - cut),
                           io::toClip(-hw, -hh + cut)}};
        }
        default:
            return {io::circlePath(0, 0, std::max(w, h) * 0.5)};
    }
}

}  // namespace

// ---- container -------------------------------------------------------------

bool isPcbDoc(const std::string& path) {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (lower.size() < 7 || lower.compare(lower.size() - 7, 7, ".pcbdoc") != 0)
        return false;
    std::ifstream in(path, std::ios::binary);
    unsigned char magic[8] = {};
    in.read(reinterpret_cast<char*>(magic), 8);
    static const unsigned char kCfb[8] = {0xD0, 0xCF, 0x11, 0xE0,
                                          0xA1, 0xB1, 0x1A, 0xE1};
    return std::memcmp(magic, kCfb, 8) == 0;
}

geom::LayerArt importPcbDoc(const std::string& path) {
    geom::LayerArt art;
    art.sourcePath = path;

    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open: " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string buffer = ss.str();

    std::unique_ptr<CFB::CompoundFileReader> cfb;
    try {
        cfb = std::make_unique<CFB::CompoundFileReader>(buffer.data(),
                                                        buffer.size());
    } catch (const CFB::CFBException& e) {
        throw std::runtime_error(std::string("not a compound file: ") +
                                 e.what());
    }

    // Stream lookup: storages at root named e.g. "Board6" holding a "Data"
    // stream. Entry names are UTF-16; ASCII in practice, so narrow directly.
    const auto entryName = [](const CFB::COMPOUND_FILE_ENTRY* e) {
        std::string s;
        for (int i = 0; i < e->nameLen / 2 && e->name[i]; ++i)
            s.push_back(static_cast<char>(e->name[i]));
        return s;
    };
    const auto childNamed = [&](const CFB::COMPOUND_FILE_ENTRY* parent,
                                const std::string& want,
                                bool wantStream) -> const CFB::COMPOUND_FILE_ENTRY* {
        const CFB::COMPOUND_FILE_ENTRY* found = nullptr;
        cfb->EnumFiles(parent, 1,
                       [&](const CFB::COMPOUND_FILE_ENTRY* entry,
                           const CFB::utf16string&, int) -> int {
                           if (found) return 1;
                           if (cfb->IsStream(entry) == wantStream &&
                               entryName(entry) == want) {
                               found = entry;
                               return 1;
                           }
                           return 0;
                       });
        return found;
    };
    const auto findData = [&](const std::string& dir) -> std::string {
        const CFB::COMPOUND_FILE_ENTRY* storage =
            childNamed(cfb->GetRootEntry(), dir, false);
        if (!storage) return {};
        const CFB::COMPOUND_FILE_ENTRY* stream =
            childNamed(storage, "Data", true);
        if (!stream) return {};
        std::string out(static_cast<size_t>(stream->size), '\0');
        cfb->ReadFile(stream, 0, out.data(), out.size());
        return out;
    };

    // ---- FileHeader sanity -------------------------------------------------
    {
        const std::string fh = findData("FileHeader");
        if (!fh.empty()) {
            Reader r(fh.data(), fh.size());
            const auto kv = r.readProperties();
            const std::string header = prop(kv, "HEADER");
            if (header.find("PCB") == std::string::npos)
                art.warnings.push_back(
                    "PcbDoc FileHeader does not look like a PCB: '" + header +
                    "'");
        }
    }

    // ---- Board6: stackup + outline ----------------------------------------
    struct StackLayer {
        int id = 0;
        std::string name;
        double copperThick = 0.035, dielBelow = 0.0;
        // Internal plane (ids 39-54): NEGATIVE content -- the layer is a
        // net-tied copper sheet and its primitives are the clearances.
        bool plane = false;
        std::string planeNet;
    };
    std::vector<StackLayer> copperStack;
    std::vector<Vertex> boardVerts;
    {
        const std::string data = findData("Board6");
        if (data.empty())
            throw std::runtime_error("PcbDoc has no Board6 stream: " + path);
        Reader r(data.data(), data.size());
        const auto kv = r.readProperties();
        boardVerts = verticesFromProps(kv);

        // The stackup is a linked list over LAYER<n> records, TOP (id 1) to
        // BOTTOM (id 32). The chain routinely passes through INTERNAL PLANE
        // ids (39-54) on 4+ layer boards -- those are copper layers too.
        const auto layerProps = [&](int id, const char* field) {
            return prop(kv, "LAYER" + std::to_string(id) + field);
        };
        int id = kTopLayer;
        for (int guard = 0; guard < 64 && id >= kTopLayer && id <= 54; ++guard) {
            StackLayer sl;
            sl.id = id;
            sl.name = layerProps(id, "NAME");
            if (sl.name.empty()) sl.name = "Layer " + std::to_string(id);
            const double ct = milPropToMm(layerProps(id, "COPTHICK"));
            const double dh = milPropToMm(layerProps(id, "DIELHEIGHT"));
            if (ct > 0) sl.copperThick = ct;
            sl.dielBelow = dh;
            if (id >= 39 && id <= 54) {
                sl.plane = true;
                sl.planeNet =
                    prop(kv, "PLANE" + std::to_string(id - 38) + "NETNAME");
            }
            copperStack.push_back(sl);
            if (id == kBottomLayer) break;
            const int next = std::atoi(layerProps(id, "NEXT").c_str());
            if (next <= 0 || next == id) break;
            id = next;
        }
    }
    if (copperStack.empty())
        throw std::runtime_error("PcbDoc has no copper stackup: " + path);
    const auto stackIndexOf = [&](int layerId) -> int {
        for (size_t i = 0; i < copperStack.size(); ++i)
            if (copperStack[i].id == layerId) return static_cast<int>(i);
        return -1;
    };

    // ---- Nets6 -------------------------------------------------------------
    {
        const std::string data = findData("Nets6");
        Reader r(data.data(), data.size());
        while (r.remaining() >= 4 && !r.error()) {
            const auto kv = r.readProperties();
            if (kv.empty()) break;
            art.nets.push_back({prop(kv, "NAME", "~"), 0.0, 0});
        }
    }
    const auto netOf = [&](uint16_t n) -> int {
        return n != kNoNet && n < art.nets.size() ? static_cast<int>(n) : -1;
    };

    // ---- primitive streams -------------------------------------------------
    std::vector<Track> tracks;
    std::vector<Arc> arcs;
    std::vector<Via> vias;
    std::vector<Pad> pads;
    std::vector<Fill> fills;
    std::vector<Region> regions;
    std::vector<Polygon> polygons;
    int textCount = 0;

    {
        const std::string data = findData("Tracks6");
        Reader r(data.data(), data.size());
        while (r.remaining() >= 4 && !r.error()) {
            if (r.read<uint8_t>() != 4) break;  // ALTIUM_RECORD::TRACK
            r.beginSubrecord();
            Track t;
            t.layer = r.read<uint8_t>();
            const uint8_t flags1 = r.read<uint8_t>();
            t.polygonoutline = (flags1 & 0x02) != 0;
            t.keepout = r.read<uint8_t>() == 2;
            t.net = r.read<uint16_t>();
            t.polygon = r.read<uint16_t>();
            r.skip(2);  // component
            r.skip(4);
            t.x1 = r.readMm();
            t.y1 = r.readMm();
            t.x2 = r.readMm();
            t.y2 = r.readMm();
            t.width = r.readMm();
            r.endSubrecord();
            tracks.push_back(t);
        }
    }
    {
        const std::string data = findData("Arcs6");
        Reader r(data.data(), data.size());
        while (r.remaining() >= 4 && !r.error()) {
            if (r.read<uint8_t>() != 1) break;  // ALTIUM_RECORD::ARC
            r.beginSubrecord();
            Arc a;
            a.layer = r.read<uint8_t>();
            const uint8_t flags1 = r.read<uint8_t>();
            a.polygonoutline = (flags1 & 0x02) != 0;
            a.keepout = r.read<uint8_t>() == 2;
            a.net = r.read<uint16_t>();
            a.polygon = r.read<uint16_t>();
            r.skip(2);  // component
            r.skip(4);
            a.cx = r.readMm();
            a.cy = r.readMm();
            a.radius = r.readMm();
            a.sa = r.read<double>();
            a.ea = r.read<double>();
            a.width = r.readMm();
            r.endSubrecord();
            arcs.push_back(a);
        }
    }
    {
        const std::string data = findData("Vias6");
        Reader r(data.data(), data.size());
        while (r.remaining() >= 4 && !r.error()) {
            if (r.read<uint8_t>() != 3) break;  // ALTIUM_RECORD::VIA
            r.beginSubrecord();
            Via v;
            r.skip(1);
            const uint8_t flags1 = r.read<uint8_t>();
            v.tentBottom = (flags1 & 0x40) != 0;
            v.tentTop = (flags1 & 0x20) != 0;
            r.skip(1);  // flags2
            v.net = r.read<uint16_t>();
            r.skip(8);
            v.x = r.readMm();
            v.y = r.readMm();
            v.diameter = r.readMm();
            v.holesize = r.readMm();
            v.layerStart = r.read<uint8_t>();
            v.layerEnd = r.read<uint8_t>();
            r.endSubrecord();
            vias.push_back(v);
        }
    }
    {
        const std::string data = findData("Pads6");
        Reader r(data.data(), data.size());
        while (r.remaining() >= 4 && !r.error()) {
            if (r.read<uint8_t>() != 2) break;  // ALTIUM_RECORD::PAD
            Pad p;
            // Subrecord 1: designator name.
            r.beginSubrecord();
            r.readShortString();
            r.endSubrecord();
            // Subrecords 2-4: unused.
            for (int i = 0; i < 3; ++i) {
                r.beginSubrecord();
                r.endSubrecord();
            }
            // Subrecord 5: geometry.
            const size_t sub5 = r.beginSubrecord();
            if (sub5 < 110) {
                r.endSubrecord();
                r.beginSubrecord();  // consume subrecord 6 to stay in sync
                r.endSubrecord();
                continue;
            }
            p.layer = r.read<uint8_t>();
            const uint8_t flags1 = r.read<uint8_t>();
            p.tentBottom = (flags1 & 0x40) != 0;
            p.tentTop = (flags1 & 0x20) != 0;
            r.skip(1);  // flags2
            p.net = r.read<uint16_t>();
            r.skip(2);
            r.skip(2);  // component
            r.skip(4);
            p.x = r.readMm();
            p.y = r.readMm();
            p.topW = r.readMm();
            p.topH = r.readMm();
            p.midW = r.readMm();
            p.midH = r.readMm();
            p.botW = r.readMm();
            p.botH = r.readMm();
            p.holesize = r.readMm();
            p.topShape = r.read<uint8_t>();
            p.midShape = r.read<uint8_t>();
            p.botShape = r.read<uint8_t>();
            p.rotation = r.read<double>();
            p.plated = r.read<uint8_t>() != 0;
            r.endSubrecord();
            // Subrecord 6: per-layer size/shape stack incl. the ROUNDRECT
            // alt-shape and its corner-radius percentages.
            const size_t sub6 = r.beginSubrecord();
            if (sub6 >= 596) {
                r.skip(29 * 4 * 2);  // inner sizes x, y
                r.skip(29);          // inner shapes
                r.skip(1);
                r.skip(1);           // hole shape
                r.skip(4);           // slot size
                r.skip(8);           // slot rotation
                r.skip(32 * 4 * 2);  // hole offsets
                r.skip(1);
                uint8_t altShape[32];
                for (auto& s : altShape) s = r.read<uint8_t>();
                uint8_t cornerRadius[32];
                for (auto& c : cornerRadius) c = r.read<uint8_t>();
                if (altShape[0] == 9)  // ROUNDRECT on the top face
                    p.roundRadiusTop = std::min(p.topW, p.topH) * 0.5 *
                                       (cornerRadius[0] / 100.0);
            }
            r.endSubrecord();
            pads.push_back(p);
        }
    }
    {
        const std::string data = findData("Fills6");
        Reader r(data.data(), data.size());
        while (r.remaining() >= 4 && !r.error()) {
            if (r.read<uint8_t>() != 6) break;  // ALTIUM_RECORD::FILL
            r.beginSubrecord();
            Fill f;
            f.layer = r.read<uint8_t>();
            r.skip(1);
            f.keepout = r.read<uint8_t>() == 2;
            f.net = r.read<uint16_t>();
            r.skip(2);
            r.skip(2);  // component
            r.skip(4);
            f.x1 = r.readMm();
            f.y1 = r.readMm();
            f.x2 = r.readMm();
            f.y2 = r.readMm();
            f.rotation = r.read<double>();
            r.endSubrecord();
            fills.push_back(f);
        }
    }
    {
        const std::string data = findData("Polygons6");
        Reader r(data.data(), data.size());
        while (r.remaining() >= 4 && !r.error()) {
            const auto kv = r.readProperties();
            if (kv.empty()) break;
            Polygon pg;
            // Layer arrives as a NAME here ("TOP", "MID1", "BOTTOM", ...).
            const std::string layer = prop(kv, "LAYER");
            if (layer == "TOP") pg.layer = kTopLayer;
            else if (layer == "BOTTOM") pg.layer = kBottomLayer;
            else if (layer.rfind("MID", 0) == 0)
                pg.layer = kTopLayer + std::atoi(layer.c_str() + 3);
            const int net = propInt(kv, "NET", kNoNet);
            pg.net = static_cast<uint16_t>(net);
            pg.outline = pathFromVertices(verticesFromProps(kv));
            polygons.push_back(std::move(pg));
        }
    }
    {
        const std::string data = findData("Regions6");
        Reader r(data.data(), data.size());
        while (r.remaining() >= 4 && !r.error()) {
            if (r.read<uint8_t>() != 11) break;  // ALTIUM_RECORD::REGION
            r.beginSubrecord();
            Region rg;
            rg.layer = r.read<uint8_t>();
            r.skip(1);
            rg.keepout = r.read<uint8_t>() == 2;
            rg.net = r.read<uint16_t>();
            rg.polygon = r.read<uint16_t>();
            r.skip(2);  // component
            r.skip(5);
            const uint16_t holecount = r.read<uint16_t>();
            r.skip(2);
            const auto kv = r.readProperties();
            const int pkind = propInt(kv, "KIND", 0);
            const bool isCutout =
                prop(kv, "ISBOARDCUTOUT") == "TRUE" ||
                prop(kv, "ISBOARDCUTOUT") == "T";
            rg.kind = pkind == 0
                          ? (isCutout ? Region::Kind::BoardCutout
                                      : Region::Kind::Copper)
                          : (pkind == 1 ? Region::Kind::PolygonCutout
                                        : Region::Kind::Other);
            // Vertices as DOUBLES here, not i32.
            const auto readPoly = [&]() {
                const uint32_t n = r.read<uint32_t>();
                Path64 p;
                if (n > 200000) return p;  // desync guard
                p.reserve(n);
                for (uint32_t i = 0; i < n; ++i) {
                    const double x = r.read<double>() * 2.54e-6;
                    const double y = r.read<double>() * 2.54e-6;
                    p.push_back(io::toClip(x, y));
                }
                return p;
            };
            Paths64 islands{readPoly()}, holes;
            for (uint16_t h = 0; h < holecount && !r.error(); ++h)
                holes.push_back(readPoly());
            if (!islands.front().empty()) {
                for (Path64& p : islands)
                    if (Area(p) < 0) std::reverse(p.begin(), p.end());
                for (Path64& p : holes)
                    if (Area(p) < 0) std::reverse(p.begin(), p.end());
                rg.outline = holes.empty()
                                 ? Union(islands, FillRule::NonZero)
                                 : Difference(islands, holes,
                                              FillRule::NonZero);
            }
            r.endSubrecord();
            regions.push_back(std::move(rg));
        }
    }
    {
        const std::string data = findData("Texts6");
        Reader r(data.data(), data.size());
        while (r.remaining() >= 4 && !r.error()) {
            if (r.read<uint8_t>() != 5) break;
            r.beginSubrecord();
            r.endSubrecord();
            // Second subrecord holds the string.
            r.beginSubrecord();
            r.endSubrecord();
            ++textCount;
        }
    }

    // ---- stackup Z ---------------------------------------------------------
    const double maskT = 0.010;
    double copperSum = 0, dielSum = 0;
    for (size_t i = 0; i < copperStack.size(); ++i) {
        copperSum += copperStack[i].copperThick;
        if (i + 1 < copperStack.size())
            dielSum += std::max(copperStack[i].dielBelow, 0.05);
    }
    const double total = copperSum + dielSum + 2 * maskT;
    art.thickness = total;
    art.copperThickness = copperStack.front().copperThick;
    art.maskThickness = maskT;
    art.silkThickness = 0.010;
    std::vector<double> zOf(copperStack.size());
    {
        double zTop = total - maskT;
        for (size_t i = 0; i < copperStack.size(); ++i) {
            zOf[i] = zTop - copperStack[i].copperThick;
            zTop = zOf[i];
            if (i + 1 < copperStack.size())
                zTop -= std::max(copperStack[i].dielBelow, 0.05);
        }
    }
    art.notes.push_back("Altium stackup: " +
                        std::to_string(copperStack.size()) +
                        " copper layers, board " + std::to_string(total) +
                        "mm");

    // ---- compose layers ----------------------------------------------------
    // Copper per stack layer, plus mask openings and silks accumulated on
    // the way.
    struct Bucket {
        io::DarkClearAcc comp;
        std::map<int, Paths64> netDark;
    };
    std::vector<Bucket> copperB(copperStack.size());
    io::DarkClearAcc maskTopB, maskBotB, silkTopB, silkBotB;

    const auto planeNetOf = [&](int stackIdx) -> int {
        const std::string& n = copperStack[stackIdx].planeNet;
        if (n.empty() || n == "(No Net)") return -1;
        for (size_t i = 0; i < art.nets.size(); ++i)
            if (art.nets[i].name == n) return static_cast<int>(i);
        return -1;
    };
    // On a PLANE layer everything inverts: the sheet is copper and a
    // primitive is a clearance -- except split-plane copper regions, which
    // arrive with forceDark. A primitive on the plane's own net cuts
    // nothing (it is connected).
    const auto addCopper = [&](int stackIdx, int net, const Paths64& polys,
                               bool forceDark = false) {
        if (stackIdx < 0 || polys.empty()) return;
        Bucket& b = copperB[stackIdx];
        if (copperStack[stackIdx].plane && !forceDark) {
            if (net >= 0 && net == planeNetOf(stackIdx)) return;
            b.comp.clear(polys);
            return;
        }
        b.comp.dark(polys);
        Paths64& bucket = b.netDark[net];
        bucket.insert(bucket.end(), polys.begin(), polys.end());
    };

    // Board area, needed early to prime the plane sheets. Cutout regions are
    // already parsed.
    Paths64 boardArea;
    {
        Path64 outline = pathFromVertices(boardVerts);
        if (outline.size() >= 3) {
            if (Area(outline) < 0)
                std::reverse(outline.begin(), outline.end());
            boardArea = {outline};
            Paths64 cutouts;
            for (const Region& rg : regions)
                if (rg.kind == Region::Kind::BoardCutout)
                    cutouts.insert(cutouts.end(), rg.outline.begin(),
                                   rg.outline.end());
            if (!cutouts.empty())
                boardArea = Difference(boardArea, cutouts, FillRule::NonZero);
        }
    }
    const double planePullback = 0.508;   // Altium's default 20mil rule
    const double planeClearance = 0.508;  // ditto for pads/vias through it
    for (size_t i = 0; i < copperStack.size(); ++i) {
        if (!copperStack[i].plane || boardArea.empty()) continue;
        const Paths64 sheet =
            InflatePaths(boardArea, -planePullback * geom::kScale,
                         JoinType::Round, EndType::Polygon);
        const int pnet = planeNetOf(static_cast<int>(i));
        addCopper(static_cast<int>(i), pnet, sheet, /*forceDark=*/true);
        if (pnet >= 0) art.nets[pnet].hasPlane = true;
    }
    // A primitive's target: copper stack index, or a mask/silk accumulator.
    const auto simpleTarget = [&](int layerId) -> io::DarkClearAcc* {
        switch (layerId) {
            case kTopSolder: return &maskTopB;
            case kBottomSolder: return &maskBotB;
            case kTopOverlay: return &silkTopB;
            case kBottomOverlay: return &silkBotB;
            default: return nullptr;
        }
    };

    const double maskExpand = 0.1016;  // Altium's default 4mil rule

    // Which polygons got real pour geometry from Regions6.
    std::set<uint16_t> pouredPolygons;
    for (const Region& rg : regions)
        if (rg.kind == Region::Kind::Copper && rg.polygon != kNoPoly)
            pouredPolygons.insert(rg.polygon);

    for (const Track& t : tracks) {
        if (t.keepout) continue;
        // Pour outline strokes duplicate the fill; skip when the fill exists.
        if (t.polygonoutline && t.polygon != kNoPoly &&
            pouredPolygons.count(t.polygon))
            continue;
        if (t.width <= 0) continue;
        Path64 spine{io::toClip(t.x1, t.y1), io::toClip(t.x2, t.y2)};
        ClipperOffset co;
        co.ArcTolerance(geom::kScale * 0.001);
        co.AddPath(spine, JoinType::Round, EndType::Round);
        Paths64 polys;
        co.Execute(t.width * 0.5 * geom::kScale, polys);
        const int si = stackIndexOf(t.layer);
        if (si >= 0) {
            const int net = netOf(t.net);
            addCopper(si, net, polys);
            // Tracks on a plane layer are clearance strokes, not routes.
            if (net >= 0 && !t.polygonoutline && !copperStack[si].plane) {
                art.netSegments.push_back({t.x1, t.y1, t.x2, t.y2, net});
                art.nets[net].routedMm +=
                    std::hypot(t.x2 - t.x1, t.y2 - t.y1);
            }
        } else if (io::DarkClearAcc* acc = simpleTarget(t.layer)) {
            acc->dark(polys);
        }
    }
    for (const Arc& a : arcs) {
        if (a.keepout || a.width <= 0 || a.radius <= 0) continue;
        if (a.polygonoutline && a.polygon != kNoPoly &&
            pouredPolygons.count(a.polygon))
            continue;
        const double sa = a.sa * std::numbers::pi / 180.0;
        const double sx = a.cx + a.radius * std::cos(sa);
        const double sy = a.cy + a.radius * std::sin(sa);
        double sweepDeg = a.ea - a.sa;
        while (sweepDeg <= 0) sweepDeg += 360.0;
        const double ea = (a.sa + sweepDeg) * std::numbers::pi / 180.0;
        const double ex = a.cx + a.radius * std::cos(ea);
        const double ey = a.cy + a.radius * std::sin(ea);
        Path64 spine{io::toClip(sx, sy)};
        io::arcAppend(spine, sx, sy, ex, ey, a.cx, a.cy, /*cw=*/false);
        ClipperOffset co;
        co.ArcTolerance(geom::kScale * 0.001);
        co.AddPath(spine, JoinType::Round, EndType::Round);
        Paths64 polys;
        co.Execute(a.width * 0.5 * geom::kScale, polys);
        const int si = stackIndexOf(a.layer);
        if (si >= 0) {
            addCopper(si, netOf(a.net), polys);
        } else if (io::DarkClearAcc* acc = simpleTarget(a.layer)) {
            acc->dark(polys);
        }
    }
    for (const Fill& f : fills) {
        if (f.keepout) continue;
        const double cx = (f.x1 + f.x2) * 0.5, cy = (f.y1 + f.y2) * 0.5;
        const double w = std::abs(f.x2 - f.x1), h = std::abs(f.y2 - f.y1);
        // Altium fill rotation is CCW; placed() rotates CW.
        Paths64 polys =
            io::placed({io::rectPath(w, h)}, cx, cy, -f.rotation, false);
        const int si = stackIndexOf(f.layer);
        if (si >= 0) addCopper(si, netOf(f.net), polys);
        else if (io::DarkClearAcc* acc = simpleTarget(f.layer)) acc->dark(polys);
    }
    for (const Region& rg : regions) {
        if (rg.keepout || rg.outline.empty()) continue;
        if (rg.kind == Region::Kind::Copper) {
            const int si = stackIndexOf(rg.layer);
            if (si >= 0) {
                const int net = netOf(rg.net);
                // On a plane layer a copper region is a SPLIT PLANE piece --
                // positive copper even though everything else inverts.
                addCopper(si, net, rg.outline,
                          /*forceDark=*/copperStack[si].plane);
                if (net >= 0) art.nets[net].hasPlane = true;
            } else if (io::DarkClearAcc* acc = simpleTarget(rg.layer)) {
                acc->dark(rg.outline);
            }
        }
    }
    // Polygons without a poured fill (old files): fill the outline solid.
    int unpoured = 0;
    for (size_t pi = 0; pi < polygons.size(); ++pi) {
        const Polygon& pg = polygons[pi];
        if (pouredPolygons.count(static_cast<uint16_t>(pi))) continue;
        if (pg.outline.size() < 3) continue;
        const int si = stackIndexOf(pg.layer);
        if (si < 0) continue;
        Path64 outline = pg.outline;
        if (Area(outline) < 0) std::reverse(outline.begin(), outline.end());
        const int net = netOf(pg.net);
        addCopper(si, net, {outline});
        if (net >= 0) art.nets[net].hasPlane = true;
        ++unpoured;
    }
    if (unpoured > 0)
        art.notes.push_back(
            std::to_string(unpoured) +
            " polygon(s) had no stored pour; filled solid (thermal reliefs "
            "not reproduced)");

    // Pads: flash per face; MULTI_LAYER pads land on every copper layer.
    for (const Pad& p : pads) {
        const auto flash = [&](int stackIdx, int shape, double w, double h,
                               double rr) {
            const Paths64 polys = io::placed(padShape(shape, w, h, rr), p.x,
                                             p.y, -p.rotation, false);
            addCopper(stackIdx, netOf(p.net), polys);
        };
        const int net = netOf(p.net);
        if (p.layer == kMultiLayer) {
            for (size_t i = 0; i < copperStack.size(); ++i) {
                const bool outer = i == 0 || i + 1 == copperStack.size();
                if (outer) {
                    const bool top = i == 0;
                    flash(static_cast<int>(i),
                          top ? p.topShape : p.botShape,
                          top ? p.topW : p.botW, top ? p.topH : p.botH,
                          top ? p.roundRadiusTop : -1);
                } else if (copperStack[i].plane) {
                    // Through a plane, a pad is a clearance around its hole
                    // (or a connection, which addCopper resolves by net).
                    if (p.holesize > 0)
                        addCopper(static_cast<int>(i), net,
                                  {io::circlePath(
                                      p.x, p.y,
                                      p.holesize * 0.5 + planeClearance)});
                } else {
                    flash(static_cast<int>(i), p.midShape, p.midW, p.midH, -1);
                }
            }
            if (p.holesize > 0) {
                Path64 hole = io::circlePath(p.x, p.y, p.holesize * 0.5);
                art.drills.push_back(hole);
                if (p.plated) art.barrels.push_back(hole);
            }
        } else if (const int si = stackIndexOf(p.layer); si >= 0) {
            flash(si, p.topShape, p.topW, p.topH, p.roundRadiusTop);
        }
        // Mask openings from the pad's own faces.
        if (p.layer == kMultiLayer || p.layer == kTopLayer) {
            if (!p.tentTop) {
                Paths64 polys = io::placed(
                    padShape(p.topShape, p.topW + 2 * maskExpand,
                             p.topH + 2 * maskExpand, p.roundRadiusTop),
                    p.x, p.y, -p.rotation, false);
                maskTopB.dark(polys);
            }
        }
        if (p.layer == kMultiLayer || p.layer == kBottomLayer) {
            if (!p.tentBottom) {
                Paths64 polys = io::placed(
                    padShape(p.botShape, p.botW + 2 * maskExpand,
                             p.botH + 2 * maskExpand, -1),
                    p.x, p.y, -p.rotation, false);
                maskBotB.dark(polys);
            }
        }
        // Snap point on the outer face.
        geom::LayerArt::NetPoint np;
        np.pos[0] = p.x;
        np.pos[1] = p.y;
        np.pos[2] = (p.layer == kBottomLayer) ? 0.0 : total;
        np.net = net;
        art.netPoints.push_back(np);
    }

    // Vias.
    for (const Via& v : vias) {
        if (v.diameter <= 0) continue;
        const int siStart = stackIndexOf(std::min(v.layerStart, v.layerEnd));
        const int siEnd = stackIndexOf(std::max(v.layerStart, v.layerEnd));
        const int lo = siStart < 0 ? 0 : siStart;
        const int hi =
            siEnd < 0 ? static_cast<int>(copperStack.size()) - 1 : siEnd;
        const Paths64 ring{io::circlePath(v.x, v.y, v.diameter * 0.5)};
        for (int i = lo; i <= hi; ++i) {
            if (copperStack[i].plane) {
                // Clearance around the barrel unless the via is on the
                // plane's net (then it connects and cuts nothing).
                addCopper(i, netOf(v.net),
                          {io::circlePath(v.x, v.y,
                                          v.holesize * 0.5 + planeClearance)});
            } else {
                addCopper(i, netOf(v.net), ring);
            }
        }
        if (netOf(v.net) >= 0) ++art.nets[netOf(v.net)].viaCount;

        const Path64 hole = io::circlePath(v.x, v.y, v.holesize * 0.5);
        const bool fullSpan =
            lo == 0 && hi == static_cast<int>(copperStack.size()) - 1;
        if (v.holesize > 0) {
            if (fullSpan) {
                art.drills.push_back(hole);
                art.barrels.push_back(hole);
            } else {
                geom::LayerArt::PartialBore bore;
                bore.path = hole;
                bore.fromLayer = copperStack[lo].name;
                bore.toLayer = copperStack[hi].name;
                art.partialBores.push_back(std::move(bore));
            }
        }
        if (fullSpan || lo == 0) {
            if (!v.tentTop)
                maskTopB.dark({io::circlePath(v.x, v.y,
                                              v.diameter * 0.5 + maskExpand)});
        }
        if (fullSpan || hi == static_cast<int>(copperStack.size()) - 1) {
            if (!v.tentBottom)
                maskBotB.dark({io::circlePath(v.x, v.y,
                                              v.diameter * 0.5 + maskExpand)});
        }
        geom::LayerArt::NetPoint np;
        np.pos[0] = v.x;
        np.pos[1] = v.y;
        np.pos[2] = total;
        np.net = netOf(v.net);
        art.netPoints.push_back(np);
    }

    // ---- publish layers ----------------------------------------------------
    for (size_t i = 0; i < copperStack.size(); ++i) {
        geom::ArtLayer al;
        al.name = copperStack[i].name;
        al.kind = LayerKind::Copper;
        al.thickness = copperStack[i].copperThick;
        al.z = zOf[i];
        const bool hadClears = copperB[i].comp.sawClear;
        al.art = copperB[i].comp.take();
        for (auto& [net, darks] : copperB[i].netDark) {
            geom::ArtLayer::NetRegion nr;
            nr.net = net;
            // Plane layers erase clearances out of the sheet; the net split
            // must be clipped to what survived.
            nr.paths = hadClears ? Intersect(darks, al.art, FillRule::NonZero)
                                 : Union(darks, FillRule::NonZero);
            if (!nr.paths.empty()) al.netArt.push_back(std::move(nr));
        }
        art.layers.push_back(std::move(al));
    }
    const auto pushSimple = [&](io::DarkClearAcc& acc, const char* name,
                                LayerKind kind, bool top) {
        Paths64 paths = acc.take();
        if (paths.empty()) return;
        geom::ArtLayer al;
        al.name = name;
        al.kind = kind;
        if (kind == LayerKind::Soldermask) {
            al.thickness = maskT;
            al.z = top ? total - maskT : 0.0;
            al.openings = static_cast<int>(paths.size());
        } else {
            al.thickness = art.silkThickness;
            al.z = top ? total : -art.silkThickness;
        }
        al.art = std::move(paths);
        art.layers.push_back(std::move(al));
    };
    pushSimple(maskTopB, "Top Solder", LayerKind::Soldermask, true);
    pushSimple(maskBotB, "Bottom Solder", LayerKind::Soldermask, false);
    pushSimple(silkTopB, "Top Overlay", LayerKind::Silkscreen, true);
    pushSimple(silkBotB, "Bottom Overlay", LayerKind::Silkscreen, false);

    geom::normalizeWinding(art.drills);
    geom::normalizeWinding(art.barrels);

    // ---- outline -----------------------------------------------------------
    {
        Paths64 board = boardArea;  // computed before plane priming
        if (board.empty()) {
            Paths64 all;
            for (const geom::ArtLayer& al : art.layers)
                if (al.kind == LayerKind::Copper)
                    all.insert(all.end(), al.art.begin(), al.art.end());
            const Rect64 bb = GetBounds(all);
            if (bb.Width() <= 0)
                throw std::runtime_error(
                    "PcbDoc has no board outline and no copper: " + path);
            board = {bb.AsPath()};
            art.warnings.push_back(
                "no board outline in Board6; using the copper bounding box");
        }
        art.outline = std::move(board);
    }

    if (textCount > 0)
        art.notes.push_back(std::to_string(textCount) +
                            " text feature(s) not rendered");
    if (!art.nets.empty())
        art.notes.push_back("Altium netlist: " +
                            std::to_string(art.nets.size()) +
                            " nets (net highlighting and routed length "
                            "available)");
    art.notes.push_back(
        "Altium PcbDoc import is early: mask openings are derived from pads "
        "and vias (4mil expansion), text and dimensions are not rendered");
    return art;
}

}  // namespace pcbview::altium
