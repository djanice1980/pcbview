#include "render/soft/preview.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace pcbview::soft {
namespace {

struct Rgb {
    float r, g, b;
};

// Bare FR4 is tan, not green -- the green everyone pictures is the soldermask on
// top of it. Rendering the substrate green would double-count the mask and hide
// where mask is actually absent.
Rgb materialColor(geom::Material m) {
    switch (m) {
        case geom::Material::Substrate:  return Rgb{0.72f, 0.61f, 0.38f};
        case geom::Material::Soldermask: return Rgb{0.05f, 0.29f, 0.12f};
        default:                         return Rgb{0.90f, 0.66f, 0.24f};
    }
}

// Mask is a translucent film: what is underneath tints it, which is exactly why
// copper reads as a lighter, warmer green than bare laminate does.
constexpr float kMaskOpacity = 0.72f;

void writeBmp(const std::string& path, int width, int height,
              const std::vector<Rgb>& pixels) {
    // 24-bit BMP: rows are bottom-up and padded to a 4-byte boundary.
    const int rowBytes = width * 3;
    const int padding = (4 - (rowBytes % 4)) % 4;
    const int imageBytes = (rowBytes + padding) * height;
    const int fileBytes = 54 + imageBytes;

    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write: " + path);

    uint8_t header[54] = {};
    header[0] = 'B'; header[1] = 'M';
    std::memcpy(&header[2], &fileBytes, 4);
    const int offset = 54;
    std::memcpy(&header[10], &offset, 4);
    const int dibSize = 40;
    std::memcpy(&header[14], &dibSize, 4);
    std::memcpy(&header[18], &width, 4);
    std::memcpy(&header[22], &height, 4);
    const uint16_t planes = 1, bpp = 24;
    std::memcpy(&header[26], &planes, 2);
    std::memcpy(&header[28], &bpp, 2);
    std::memcpy(&header[34], &imageBytes, 4);
    out.write(reinterpret_cast<char*>(header), 54);

    const uint8_t pad[3] = {0, 0, 0};
    for (int y = height - 1; y >= 0; --y) {
        for (int x = 0; x < width; ++x) {
            const Rgb& c = pixels[static_cast<size_t>(y) * width + x];
            const auto quantise = [](float v) {
                return static_cast<uint8_t>(
                    std::clamp(std::pow(std::clamp(v, 0.0f, 1.0f), 1.0f / 2.2f) *
                                   255.0f,
                               0.0f, 255.0f));
            };
            const uint8_t bgr[3] = {quantise(c.b), quantise(c.g), quantise(c.r)};
            out.write(reinterpret_cast<const char*>(bgr), 3);
        }
        if (padding) out.write(reinterpret_cast<const char*>(pad), padding);
    }
}

}  // namespace

void renderPreview(const geom::BoardMesh& mesh, const std::string& path,
                   const PreviewOptions& opts) {
    const auto& b = mesh.bounds;
    const double spanX = b.max[0] - b.min[0];
    const double spanY = b.max[1] - b.min[1];
    if (spanX <= 0.0 || spanY <= 0.0) {
        throw std::runtime_error("degenerate bounds; nothing to preview");
    }

    const int width = opts.width;
    const int height =
        std::max(1, static_cast<int>(std::lround(width * spanY / spanX)));
    const double scale = width / spanX;

    std::vector<Rgb> color(static_cast<size_t>(width) * height,
                           Rgb{0.09f, 0.09f, 0.11f});
    std::vector<float> depth(static_cast<size_t>(width) * height,
                             -std::numeric_limits<float>::infinity());

    // Light from above and slightly off-axis, so flat top faces still separate
    // from side walls.
    const float lx = 0.35f, ly = 0.35f, lz = 0.87f;

    // Two passes: opaque geometry first, then mask blended over whatever it
    // covers. Mask always sits above copper in Z, so a plain z-test suffices --
    // no sorting needed.
    const auto rasterize = [&](const geom::Part& part, bool blend) {
        const Rgb base = materialColor(part.material);
        const std::vector<uint32_t>& idx = part.mesh.indices;

        for (size_t i = 0; i + 2 < idx.size(); i += 3) {
            const geom::Vertex& v0 = part.mesh.vertices[idx[i]];
            const geom::Vertex& v1 = part.mesh.vertices[idx[i + 1]];
            const geom::Vertex& v2 = part.mesh.vertices[idx[i + 2]];

            // Orthographic projection straight down the Z axis, or up it for a
            // bottom view (sign flips which surface wins the depth test).
            const double sign = opts.fromBottom ? -1.0 : 1.0;
            const bool mirror = opts.fromBottom && !opts.noMirror;

            const auto px = [&](const geom::Vertex& v) {
                return mirror ? (b.max[0] - v.position[0]) * scale
                              : (v.position[0] - b.min[0]) * scale;
            };
            const auto py = [&](const geom::Vertex& v) {
                return (b.max[1] - v.position[1]) * scale;
            };

            const double x0 = px(v0), y0 = py(v0);
            const double x1 = px(v1), y1 = py(v1);
            const double x2 = px(v2), y2 = py(v2);

            const double area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
            if (std::abs(area) < 1e-12) continue;

            const int minX = std::max(0, static_cast<int>(std::floor(std::min({x0, x1, x2}))));
            const int maxX = std::min(width - 1, static_cast<int>(std::ceil(std::max({x0, x1, x2}))));
            const int minY = std::max(0, static_cast<int>(std::floor(std::min({y0, y1, y2}))));
            const int maxY = std::min(height - 1, static_cast<int>(std::ceil(std::max({y0, y1, y2}))));

            for (int y = minY; y <= maxY; ++y) {
                for (int x = minX; x <= maxX; ++x) {
                    const double sx = x + 0.5, sy = y + 0.5;
                    double w0 = ((x1 - sx) * (y2 - sy) - (x2 - sx) * (y1 - sy)) / area;
                    double w1 = ((x2 - sx) * (y0 - sy) - (x0 - sx) * (y2 - sy)) / area;
                    double w2 = 1.0 - w0 - w1;
                    if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0) continue;

                    const float z = static_cast<float>(
                        sign * (w0 * v0.position[2] + w1 * v1.position[2] +
                                w2 * v2.position[2]));
                    const size_t p = static_cast<size_t>(y) * width + x;
                    if (z <= depth[p]) continue;
                    depth[p] = z;

                    float nx = static_cast<float>(w0 * v0.normal[0] + w1 * v1.normal[0] + w2 * v2.normal[0]);
                    float ny = static_cast<float>(w0 * v0.normal[1] + w1 * v1.normal[1] + w2 * v2.normal[1]);
                    float nz = static_cast<float>(w0 * v0.normal[2] + w1 * v1.normal[2] + w2 * v2.normal[2]);
                    const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                    if (len > 1e-6f) { nx /= len; ny /= len; nz /= len; }

                    // Two-sided: we are inspecting geometry, and a flipped normal
                    // should read as shading, not as a black hole.
                    float diffuse = std::abs(nx * lx + ny * ly + nz * lz);
                    const float shade = 0.15f + 0.85f * diffuse;

                    const Rgb lit{base.r * shade, base.g * shade, base.b * shade};
                    if (!blend) {
                        color[p] = lit;
                    } else {
                        const Rgb under = color[p];
                        const float a = kMaskOpacity;
                        color[p] = Rgb{lit.r * a + under.r * (1.0f - a),
                                       lit.g * a + under.g * (1.0f - a),
                                       lit.b * a + under.b * (1.0f - a)};
                    }
                }
            }
        }
    };

    for (const geom::Part& part : mesh.parts) {
        if (part.material != geom::Material::Soldermask) rasterize(part, false);
    }
    for (const geom::Part& part : mesh.parts) {
        if (part.material == geom::Material::Soldermask) rasterize(part, true);
    }

    writeBmp(path, width, height, color);
}

}  // namespace pcbview::soft
