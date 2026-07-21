#pragma once

// steps/<step>/eda/data -- the REAL netlist of an ODB++ product model.
// NET records name the nets; each net's FID records point at concrete
// features as (layer index into the LYR header list, feature index into that
// layer's features file, in record order). That pair is the whole reason the
// features parser preserves file order.

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace pcbview::odb {

struct EdaData {
    std::vector<std::string> layers;  // LYR header order, matrix layer names
    std::vector<std::string> nets;    // NET record order
    // (layer index, feature index) -> net index.
    std::map<std::pair<int, int>, int> featureNet;
};

EdaData parseEda(const std::string& text);

}  // namespace pcbview::odb
