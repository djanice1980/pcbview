#pragma once

// ODB++ "structured text" -- the ARRAY { KEY=VALUE ... } format shared by
// matrix/matrix, layer tools files and stephdr -- plus the matrix model
// built from it.

#include <map>
#include <string>
#include <vector>

namespace pcbview::odb {

// One "NAME { k=v ... }" block. Top-level bare "KEY=VALUE" lines are returned
// as a block with an empty name so headers (UNITS=MM) are not lost.
struct Block {
    std::string name;
    std::map<std::string, std::string> kv;
    std::string get(const std::string& key) const {
        const auto it = kv.find(key);
        return it == kv.end() ? std::string() : it->second;
    }
};
std::vector<Block> parseStructured(const std::string& text);

// The matrix: every layer of the product model, in board order.
struct MatrixLayer {
    std::string name;      // directory name under steps/<s>/layers/
    std::string type;      // SIGNAL, POWER_GROUND, MIXED, SOLDER_MASK, ...
    std::string context;   // BOARD or MISC
    bool positive = true;  // POLARITY=POSITIVE
    // DRILL layers: the copper span (empty = through all).
    std::string startName, endName;
    int row = 0;
};

struct Matrix {
    std::vector<MatrixLayer> layers;  // sorted by ROW = physical order
    std::vector<std::string> steps;   // by COL order
};
Matrix parseMatrix(const std::string& text);

}  // namespace pcbview::odb
