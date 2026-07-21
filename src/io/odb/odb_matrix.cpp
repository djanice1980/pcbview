#include "io/odb/odb_matrix.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace pcbview::odb {
namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return s;
}

}  // namespace

std::vector<Block> parseStructured(const std::string& text) {
    std::vector<Block> blocks;
    Block* current = nullptr;
    Block toplevel;  // bare KEY=VALUE lines outside any block
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line == "}") {
            current = nullptr;
            continue;
        }
        const size_t brace = line.find('{');
        if (brace != std::string::npos &&
            line.find('=') == std::string::npos) {
            blocks.push_back({upper(trim(line.substr(0, brace))), {}});
            current = &blocks.back();
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = upper(trim(line.substr(0, eq)));
        const std::string val = trim(line.substr(eq + 1));
        (current ? *current : toplevel).kv[key] = val;
    }
    if (!toplevel.kv.empty()) blocks.insert(blocks.begin(), toplevel);
    return blocks;
}

Matrix parseMatrix(const std::string& text) {
    Matrix m;
    struct Col {
        int col = 0;
        std::string name;
    };
    std::vector<Col> steps;
    for (const Block& b : parseStructured(text)) {
        if (b.name == "STEP") {
            steps.push_back({std::atoi(b.get("COL").c_str()), b.get("NAME")});
        } else if (b.name == "LAYER") {
            MatrixLayer l;
            l.name = b.get("NAME");
            l.type = upper(b.get("TYPE"));
            l.context = upper(b.get("CONTEXT"));
            l.positive = upper(b.get("POLARITY")) != "NEGATIVE";
            l.startName = b.get("START_NAME");
            l.endName = b.get("END_NAME");
            l.row = std::atoi(b.get("ROW").c_str());
            m.layers.push_back(std::move(l));
        }
    }
    std::sort(m.layers.begin(), m.layers.end(),
              [](const MatrixLayer& a, const MatrixLayer& b) {
                  return a.row < b.row;
              });
    std::sort(steps.begin(), steps.end(),
              [](const Col& a, const Col& b) { return a.col < b.col; });
    for (const Col& s : steps) m.steps.push_back(s.name);
    return m;
}

}  // namespace pcbview::odb
