#include "model/board.h"

namespace pcbview {

std::vector<int> BoardModel::copperLayers() const {
    std::vector<int> out;
    for (size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].kind == LayerKind::Copper) out.push_back(static_cast<int>(i));
    }
    return out;
}

const Layer* BoardModel::findLayer(std::string_view name) const {
    for (const Layer& layer : layers) {
        if (layer.name == name) return &layer;
    }
    return nullptr;
}

int BoardModel::layerIndex(std::string_view name) const {
    for (size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace pcbview
