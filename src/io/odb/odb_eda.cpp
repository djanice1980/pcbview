#include "io/odb/odb_eda.h"

#include <cstdlib>
#include <sstream>

namespace pcbview::odb {

EdaData parseEda(const std::string& text) {
    EdaData out;
    std::istringstream in(text);
    std::string line;
    int currentNet = -1;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t semi = line.find(';');
        if (semi != std::string::npos) line.resize(semi);

        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        if (tag == "LYR") {
            std::string name;
            while (ls >> name) out.layers.push_back(name);
        } else if (tag == "NET") {
            std::string name;
            ls >> name;
            currentNet = static_cast<int>(out.nets.size());
            out.nets.push_back(name);
        } else if (tag == "FID" && currentNet >= 0) {
            // FID <type> <layer_num> <feature_num> -- C(opper) features are
            // the ones that appear in the render; take H(ole) too so drills
            // could be net-tagged later.
            std::string type;
            int layer = -1, feature = -1;
            ls >> type >> layer >> feature;
            if (layer >= 0 && feature >= 0)
                out.featureNet[{layer, feature}] = currentNet;
        }
        // CMP/TOP/SNT and the package sections: not needed for geometry.
    }
    return out;
}

}  // namespace pcbview::odb
