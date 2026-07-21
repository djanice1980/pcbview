#include "io/ipc356/ipc356.h"

#include <cctype>
#include <cstdlib>
#include <sstream>

namespace pcbview::ipc356 {
namespace {

// Coordinate fields look like "X+0022500" -- an X or Y immediately followed
// by a SIGN. Size fields ("X0350") have no sign, which is what tells the two
// apart in a record that contains both. Returns false if absent.
bool signedField(const std::string& line, char key, double& out) {
    for (size_t i = 0; i + 1 < line.size(); ++i) {
        if (line[i] != key) continue;
        const char s = line[i + 1];
        if (s != '+' && s != '-') continue;
        size_t j = i + 2;
        long long v = 0;
        bool any = false;
        while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j]))) {
            v = v * 10 + (line[j] - '0');
            ++j;
            any = true;
        }
        if (!any) continue;
        out = static_cast<double>(s == '-' ? -v : v);
        return true;
    }
    return false;
}

}  // namespace

bool looksLike(const std::string& text) {
    // A 356 file opens with C/P header records and contains 317/327 records
    // whose coordinates use the signed X/Y syntax. Sniff the first ~40 lines.
    std::istringstream in(text);
    std::string line;
    int lines = 0, hits = 0;
    while (std::getline(in, line) && ++lines <= 40) {
        if (line.rfind("317", 0) == 0 || line.rfind("327", 0) == 0) {
            double dummy;
            if (signedField(line, 'X', dummy)) ++hits;
        }
        if (line.rfind("P  UNITS", 0) == 0 || line.rfind("P UNITS", 0) == 0)
            ++hits;
    }
    return hits >= 2;
}

File parse(const std::string& text) {
    File f;
    // Default to the inch flavour KiCad writes; the P UNITS record overrides.
    double unitMm = 0.00254;  // CUST 0: 0.0001 inch
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line.rfind("P", 0) == 0 && line.find("UNITS") != std::string::npos) {
            if (line.find("CUST 1") != std::string::npos) {
                unitMm = 0.001;  // metric: 0.001 mm
            } else if (line.find("CUST 0") != std::string::npos) {
                unitMm = 0.00254;
            } else if (line.find("CUST 2") != std::string::npos) {
                unitMm = 0.00254 / 10.0;  // 0.00001 inch
            } else {
                f.warnings.push_back("unrecognised UNITS record: " + line);
            }
            continue;
        }
        const bool is317 = line.rfind("317", 0) == 0;
        const bool is327 = line.rfind("327", 0) == 0;
        if (!is317 && !is327) continue;  // comments, params, 999, tooling

        // Net name: columns 4-17 (1-based) per spec, left-justified.
        if (line.size() < 17) continue;
        std::string net = line.substr(3, 14);
        while (!net.empty() && net.back() == ' ') net.pop_back();
        // "N/C" is the spec's no-connect marker; an empty name is the same.
        if (net.empty() || net == "N/C") continue;

        double xu, yu;
        if (!signedField(line, 'X', xu) || !signedField(line, 'Y', yu)) {
            f.warnings.push_back("356 record without coordinates: " + line);
            continue;
        }
        Record r;
        r.net = std::move(net);
        r.x = xu * unitMm;
        r.y = yu * unitMm;
        r.through = is317;
        f.records.push_back(std::move(r));
    }
    f.ok = !f.records.empty();
    return f;
}

}  // namespace pcbview::ipc356
