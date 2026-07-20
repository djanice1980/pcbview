// pcbview -- standalone 3D PCB viewer.
//
// Phase 0: report GPU / ray tracing capability.
// Phase 1 (current): import a .kicad_pcb into BoardModel and dump it.
//
//   pcbview                 -- GPU capability report
//   pcbview <board.kicad_pcb>  -- import and dump the board

#include <clipper2/clipper.h>

#include <cstdio>
#include <exception>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "geom/layer_art.h"

#ifdef _WIN32
// windows.h defines min/max as macros, which collide with std::min/std::max.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "app/viewer.h"
#include "geom/tessellate.h"
#include "render/cpu/cpu_tracer.h"
#include "io/gerber/gerber_parser.h"
#include "io/gerber/gerber_project.h"
#include "io/kicad/kicad_importer.h"
#include "render/soft/preview.h"
#include "io/obj_export.h"
#include "model/board.h"
#include "model/validate.h"
#include "render/common/device.h"
#include "render/soft/preview.h"

namespace {

// Reconnect stdout/stderr to the console that launched us, if any.
//
// The executable is built /SUBSYSTEM:WINDOWS so the GUI does not drag a black
// console window along with it. That alone would silence every CLI mode, so we
// attach to the PARENT console when there is one: run it from a terminal and
// output appears there; double-click it and no console is created, because
// AttachConsole simply fails.
//
// The guard matters. If stdout is ALREADY connected -- redirected to a file, or
// piped by a shell that captures output -- then reopening CONOUT$ would rip that
// redirection away and send everything to the console instead, so a caller doing
// `pcbview board.kicad_pcb > out.txt` (or any shell capturing the output) would
// silently receive nothing. Only claim the console when nothing else owns stdout.
// Register the software (CPU) Vulkan driver we ship beside the exe, so a
// "llvmpipe" CPU device appears in the picker without a system-wide Mesa install.
//
// VK_ADD_DRIVER_FILES *adds* to the drivers the loader finds on its own, unlike
// VK_ICD_FILENAMES which REPLACES them -- using the latter would hide the real
// GPUs. The loader reads this at vkCreateInstance, which every path here reaches
// only after main() starts, so setting it now is early enough. If the driver was
// not staged (a bare dev build), the file is absent and we do nothing.
void registerBundledVulkanDriver() {
#ifdef _WIN32
    wchar_t exePath[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    std::wstring dir(exePath, exePath + n);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    const std::wstring icd = dir.substr(0, slash + 1) + L"lvp_icd.x86_64.json";
    if (GetFileAttributesW(icd.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    // Preserve anything the user already set; the loader accepts ';'-separated.
    std::wstring value = icd;
    wchar_t existing[4096];
    const DWORD got =
        GetEnvironmentVariableW(L"VK_ADD_DRIVER_FILES", existing, 4096);
    if (got > 0 && got < 4096) value = std::wstring(existing) + L";" + icd;
    SetEnvironmentVariableW(L"VK_ADD_DRIVER_FILES", value.c_str());
#endif
}

void attachParentConsole() {
#ifdef _WIN32
    const HANDLE existing = GetStdHandle(STD_OUTPUT_HANDLE);
    if (existing && existing != INVALID_HANDLE_VALUE &&
        GetFileType(existing) != FILE_TYPE_UNKNOWN) {
        return;  // a pipe, a file, or an inherited console -- leave it be
    }
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;

    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    freopen_s(&f, "CONIN$", "r", stdin);
#endif
}

void printGpu(const pcbview::GpuInfo& gpu, bool selected) {
    std::printf("  %s %-34s [%s] Vulkan %u.%u.%u\n", selected ? "->" : "  ",
                gpu.name.c_str(), gpu.typeName(),
                VK_API_VERSION_MAJOR(gpu.apiVersion),
                VK_API_VERSION_MINOR(gpu.apiVersion),
                VK_API_VERSION_PATCH(gpu.apiVersion));

    const auto mark = [](bool b) { return b ? "yes" : "NO "; };
    std::printf("       ray_tracing_pipeline    %s\n",
                mark(gpu.hasRayTracingPipeline));
    std::printf("       acceleration_structure  %s\n",
                mark(gpu.hasAccelerationStructure));
    std::printf("       deferred_host_ops       %s\n",
                mark(gpu.hasDeferredHostOperations));
    std::printf("       buffer_device_address   %s   (RT rule 2)\n",
                mark(gpu.hasBufferDeviceAddress));
    std::printf("       descriptor_indexing     %s   (RT rule 3)\n",
                mark(gpu.hasDescriptorIndexing));
    std::printf("       ray_query               %s\n", mark(gpu.hasRayQuery));
    std::printf("       => ray-query ready:     %s   (pcbview RT path)\n",
                gpu.rayQueryReady() ? "YES" : "no");
    std::printf("       => rt-pipeline ready:   %s\n",
                gpu.rayTracingReady() ? "YES" : "no");
}

int gpuReport() {
    VkInstance instance = pcbview::createInstance(/*enableValidation=*/true);

    const auto gpus = pcbview::enumerateGpus(instance);
    if (gpus.empty()) {
        std::fprintf(stderr, "No Vulkan devices found.\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    const pcbview::GpuInfo* best = pcbview::pickBestGpu(gpus);
    std::printf("Vulkan devices (%zu):\n\n", gpus.size());
    for (const auto& gpu : gpus) {
        printGpu(gpu, best && best->handle == gpu.handle);
        std::printf("\n");
    }

    if (!best) {
        std::fprintf(stderr, "No usable device (none has a graphics queue).\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    pcbview::Device device = pcbview::createDevice(*best);
    std::printf("Created device on: %s\n", device.gpu.name.c_str());
    std::printf("Ray-query RT mode: %s\n",
                device.rayQueryEnabled
                    ? "yes -- ray-traced shadows/AO available"
                    : "no -- raster only, which is a supported fallback");

    pcbview::destroyDevice(device);
    vkDestroyInstance(instance, nullptr);
    return 0;
}

const char* padShapeName(pcbview::PadShape s) {
    switch (s) {
        case pcbview::PadShape::Circle:    return "circle";
        case pcbview::PadShape::Rect:      return "rect";
        case pcbview::PadShape::RoundRect: return "roundrect";
        case pcbview::PadShape::Oval:      return "oval";
        default:                           return "custom";
    }
}

const char* padTypeName(pcbview::PadType t) {
    switch (t) {
        case pcbview::PadType::Smd:        return "smd";
        case pcbview::PadType::ThruHole:   return "thru_hole";
        case pcbview::PadType::NpThruHole: return "np_thru_hole";
        default:                           return "connect";
    }
}

int tessellateBoard(const std::string& path, const std::string& objPath,
                    const std::string& previewPath, bool fromBottom,
                    bool noMirror) {
    const pcbview::BoardModel board = pcbview::kicad::importPcb(path);
    const pcbview::geom::BoardMesh mesh = pcbview::geom::tessellate(board);

    std::printf("%s\n\n", path.c_str());
    const auto materialName = [](pcbview::geom::Material m) {
        switch (m) {
            case pcbview::geom::Material::Substrate:  return "substrate";
            case pcbview::geom::Material::Soldermask: return "mask";
            case pcbview::geom::Material::Silkscreen: return "silk";
            default:                                  return "copper";
        }
    };

    std::printf("  Parts:\n");
    for (const pcbview::geom::Part& part : mesh.parts) {
        std::printf("    %-10s %-9s %8zu tris  %8zu verts", part.name.c_str(),
                    materialName(part.material), part.mesh.triangleCount(),
                    part.mesh.vertices.size());
        if (part.maskOpenings >= 0) {
            std::printf("   %d openings", part.maskOpenings);
        }
        std::printf("\n");
    }
    std::printf("\n    total      %8zu tris  %8zu verts\n", mesh.totalTriangles(),
                mesh.totalVertices());

    const auto& b = mesh.bounds;
    std::printf("\n  Bounds (mm):\n");
    std::printf("    x  %8.3f .. %8.3f   (%.3f)\n", b.min[0], b.max[0],
                b.max[0] - b.min[0]);
    std::printf("    y  %8.3f .. %8.3f   (%.3f)\n", b.min[1], b.max[1],
                b.max[1] - b.min[1]);
    std::printf("    z  %8.3f .. %8.3f   (%.3f)\n", b.min[2], b.max[2],
                b.max[2] - b.min[2]);

    if (!objPath.empty()) {
        pcbview::exportObj(mesh, objPath);
        std::printf("\n  Wrote %s\n", objPath.c_str());
    }
    if (!previewPath.empty()) {
        pcbview::soft::PreviewOptions opts;
        opts.fromBottom = fromBottom;
        opts.noMirror = noMirror;
        pcbview::soft::renderPreview(mesh, previewPath, opts);
        std::printf("  Wrote %s (%s view)\n", previewPath.c_str(),
                    !fromBottom      ? "top"
                    : noMirror       ? "bottom, x-ray (not mirrored)"
                                     : "bottom, mirrored");
    }
    return 0;
}

int dumpBoard(const std::string& path) {
    const pcbview::BoardModel board = pcbview::kicad::importPcb(path);

    std::printf("%s\n", board.sourcePath.c_str());
    std::printf("  thickness      %.3f mm  (finished: includes mask + copper)\n",
                board.thickness);
    std::printf("  copper foil    %.4f mm\n", board.copperThickness);
    std::printf("  mask film      %.4f mm\n", board.maskThickness);
    // Say WHERE the numbers came from. With an explicit block the per-film
    // thicknesses are read and the single dielectric figure is only a mean
    // over an asymmetric stack; without one, everything below is derived.
    std::printf("  dielectric     %.4f mm  (%s)\n",
                board.dielectricThickness,
                board.stackup.empty() ? "derived"
                                      : "mean; per-film from (setup (stackup))");
    if (!board.stackup.empty())
        std::printf("  stackup        explicit, %zu entries%s%s\n",
                    board.stackup.size(),
                    board.copperFinish.empty() ? "" : "; finish ",
                    board.copperFinish.c_str());
    std::printf("\n");

    std::printf("  Stackup (physical order, top to bottom; z = bottom of film):\n");
    for (const pcbview::Layer& layer : board.layers) {
        if (layer.kind == pcbview::LayerKind::Soldermask &&
            layer.name == "F.Mask") {
            std::printf("        %-8s          z=%6.3f .. %6.3f\n",
                        layer.name.c_str(), layer.z, layer.z + layer.thickness);
        }
    }
    for (const pcbview::Layer& layer : board.layers) {
        if (layer.kind != pcbview::LayerKind::Copper) continue;
        std::printf("    [%d] %-8s id=%-3d z=%6.3f .. %6.3f\n", layer.stackIndex,
                    layer.name.c_str(), layer.kicadId, layer.z,
                    layer.z + layer.thickness);
    }
    for (const pcbview::Layer& layer : board.layers) {
        if (layer.kind == pcbview::LayerKind::Soldermask &&
            layer.name == "B.Mask") {
            std::printf("        %-8s          z=%6.3f .. %6.3f\n",
                        layer.name.c_str(), layer.z, layer.z + layer.thickness);
        }
    }

    std::printf("\n  Counts:\n");
    std::printf("    layers       %zu (%zu copper)\n", board.layers.size(),
                board.copperLayers().size());
    std::printf("    tracks       %zu\n", board.tracks.size());
    std::printf("    vias         %zu\n", board.vias.size());
    std::printf("    components   %zu\n", board.components.size());
    std::printf("    pads         %zu\n", board.pads.size());
    std::printf("    zones        %zu\n", board.zones.size());
    std::printf("    drills       %zu\n", board.drills.size());
    std::printf("    outline      %zu loop(s)\n", board.outline.size());

    std::map<std::string, int> shapes;
    for (const pcbview::Pad& pad : board.pads) {
        shapes[std::string(padTypeName(pad.type)) + " " + padShapeName(pad.shape)]++;
    }
    std::printf("\n  Pad shapes:\n");
    for (const auto& [name, count] : shapes) {
        std::printf("    %-24s %d\n", name.c_str(), count);
    }

    std::printf("\n  Outline loops:\n");
    for (size_t i = 0; i < board.outline.size(); ++i) {
        const auto& poly = board.outline[i];
        double minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9;
        for (const pcbview::Vec2& p : poly.outer) {
            minx = std::min(minx, p.x); maxx = std::max(maxx, p.x);
            miny = std::min(miny, p.y); maxy = std::max(maxy, p.y);
        }
        std::printf("    [%zu] %zu pts, bbox %.3f x %.3f mm\n", i,
                    poly.outer.size(), maxx - minx, maxy - miny);
    }

    std::printf("\n  Zone fills:\n");
    for (const pcbview::ZoneFill& zone : board.zones) {
        size_t pts = 0;
        for (const auto& poly : zone.polygons) pts += poly.outer.size();
        std::printf("    %-8s net=%-8s %zu polygon(s), %zu pts\n",
                    board.layers[zone.layer].name.c_str(), zone.net.c_str(),
                    zone.polygons.size(), pts);
    }

    std::printf("\n  Components that are rotated and/or on the bottom:\n");
    int plain = 0;
    for (const pcbview::Component& comp : board.components) {
        if (comp.rotation == 0.0 && !comp.bottom) { ++plain; continue; }
        std::printf("    %-6s %-28s at (%8.3f,%8.3f) rot=%6.1f %s\n",
                    comp.reference.c_str(), comp.footprint.c_str(), comp.at.x,
                    comp.at.y, comp.rotation, comp.bottom ? "BOTTOM" : "top");
    }
    std::printf("    (%d more with rot=0 on top)\n", plain);

    const pcbview::GeometryReport geom = pcbview::validateNetGeometry(board);
    std::printf("\n  Net geometry check:\n");
    std::printf("    track endpoints  %d\n", geom.trackEndpoints);
    std::printf("    orphaned         %d  (%.2f%%)\n", geom.orphanEndpoints,
                geom.trackEndpoints ? 100.0 * geom.orphanEndpoints /
                                          geom.trackEndpoints
                                    : 0.0);
    for (const std::string& example : geom.examples) {
        std::printf("      %s\n", example.c_str());
    }

    const pcbview::PadReport pads = pcbview::validatePadConnectivity(board);
    std::printf("\n  Pad connectivity (compare the two rates, not their values):\n");
    std::printf("    top     %4d/%-4d reached by track or via  (%.1f%%)\n",
                pads.topTouched, pads.topPads, pads.topRate());
    std::printf("    bottom  %4d/%-4d reached by track or via  (%.1f%%)\n",
                pads.bottomTouched, pads.bottomPads, pads.bottomRate());

    const pcbview::OverlapReport overlaps = pcbview::validatePadOverlaps(board);
    std::printf("\n  Pad overlap check (different nets = short; must be 0):\n");
    std::printf("    candidate pairs  %d\n", overlaps.pairsChecked);
    std::printf("    shorts           %d\n", overlaps.shorts);
    for (const std::string& example : overlaps.examples) {
        std::printf("      %s\n", example.c_str());
    }

    if (board.warnings.empty()) {
        std::printf("\n  No warnings.\n");
    } else {
        std::printf("\n  Warnings (%zu):\n", board.warnings.size());
        std::map<std::string, int> grouped;
        for (const std::string& w : board.warnings) grouped[w]++;
        for (const auto& [text, count] : grouped) {
            std::printf("    x%-4d %s\n", count, text.c_str());
        }
    }
    return 0;
}

}  // namespace

// TEMPORARY: parse one gerber file and report its dark-image area, so the parser
// can be checked against the KiCad path's copper before the project layer exists.
int gerberProbe(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return 1; }
    std::ostringstream buf;
    buf << in.rdbuf();

    const pcbview::gerber::GerberImage img =
        pcbview::gerber::parseGerber(buf.str(), path);

    double area = 0.0;
    for (const auto& p : img.dark) area += Clipper2Lib::Area(p);
    area /= (pcbview::geom::kScale * pcbview::geom::kScale);

    double minx = 1e18, miny = 1e18, maxx = -1e18, maxy = -1e18;
    for (const auto& p : img.dark)
        for (const auto& pt : p) {
            minx = std::min(minx, pcbview::geom::toMm(pt.x));
            maxx = std::max(maxx, pcbview::geom::toMm(pt.x));
            miny = std::min(miny, pcbview::geom::toMm(pt.y));
            maxy = std::max(maxy, pcbview::geom::toMm(pt.y));
        }

    std::printf("%s\n", path.c_str());
    std::printf("  file function : %s (index %d, %s)\n",
                img.hasFunction ? "yes" : "no", img.function.copperIndex,
                img.function.top ? "top" : img.function.bottom ? "bottom" : "-");
    std::printf("  dark polygons : %zu\n", img.dark.size());
    std::printf("  copper area   : %.4f mm^2\n", area);
    std::printf("  bbox          : %.3f,%.3f .. %.3f,%.3f\n", minx, miny, maxx, maxy);
    for (const std::string& w : img.warnings) std::printf("  warning: %s\n", w.c_str());
    return 0;
}

// --cpu-trace <board> <out.bmp>: import a board, build the Embree scene, trace a
// primary-ray image from an iso view, and write a BMP. De-risks the Embree CPU
// tracer (does its BVH hold the whole ~1M-triangle board? -- lavapipe's did not)
// without any Vulkan/display plumbing.
int cpuTrace(const std::string& boardPath, const std::string& outBmp) {
    pcbview::geom::BoardMesh mesh;
    const bool isKicad = boardPath.size() > 10 &&
                         boardPath.substr(boardPath.size() - 10) == ".kicad_pcb";
    if (isKicad) {
        const pcbview::BoardModel board = pcbview::kicad::importPcb(boardPath);
        mesh = pcbview::geom::tessellate(board);
    } else {
        const pcbview::geom::LayerArt art =
            pcbview::gerber::importPackage(boardPath);
        mesh = pcbview::geom::assemble(art);
    }

    pcbview::cpu::CpuTracer tracer;
    tracer.setScene(mesh);
    if (!tracer.ready()) {
        std::fprintf(stderr, "cpu-trace: empty scene\n");
        return 1;
    }

    const int W = 1024, H = 800;
    const auto& b = mesh.bounds;
    const double cx = 0.5 * (b.min[0] + b.max[0]);
    const double cy = 0.5 * (b.min[1] + b.max[1]);
    const double cz = 0.5 * (b.min[2] + b.max[2]);
    const double dx = b.max[0] - b.min[0], dy = b.max[1] - b.min[1];
    const double span = std::max(dx, dy);

    // iso basis, matching vulkan_window's cameraBasis(yaw=0.6, pitch=0.62).
    const float yaw = 0.6f, pitch = 0.62f;
    const float cosP = std::cos(pitch), sinP = std::sin(pitch);
    const float offx = cosP * std::sin(yaw), offy = -cosP * std::cos(yaw),
                offz = sinP;
    const float dist = static_cast<float>(span) * 1.6f + 20.0f;
    const float fovY = 45.0f * 3.14159265f / 180.0f;
    const float tanY = std::tan(fovY * 0.5f);
    const float tanX = tanY * (static_cast<float>(W) / H);

    const float rightx = std::cos(yaw), righty = std::sin(yaw), rightz = 0.0f;
    // up = cross(right, forward), forward = -offset
    const float fwdx = -offx, fwdy = -offy, fwdz = -offz;
    const float upx = righty * fwdz - rightz * fwdy;
    const float upy = rightz * fwdx - rightx * fwdz;
    const float upz = rightx * fwdy - righty * fwdx;

    pcbview::cpu::TraceCamera cam;
    cam.eye = {static_cast<float>(cx) + dist * offx,
               static_cast<float>(cy) + dist * offy,
               static_cast<float>(cz) + dist * offz};
    cam.fwd = {fwdx, fwdy, fwdz};
    cam.right = {rightx * tanX, righty * tanX, rightz * tanX};
    cam.up = {upx * tanY, upy * tanY, upz * tanY};

    const std::vector<uint8_t> rgba = tracer.renderImage(cam, W, H, 64, true);

    // Minimal 24-bit BMP (bottom-up, 4-byte-padded rows) from the RGBA8 buffer,
    // which is already gamma-encoded.
    const int rowBytes = W * 3;
    const int padding = (4 - (rowBytes % 4)) % 4;
    const int imageBytes = (rowBytes + padding) * H;
    const int fileBytes = 54 + imageBytes;
    std::ofstream out(outBmp, std::ios::binary);
    if (!out) { std::fprintf(stderr, "cannot write %s\n", outBmp.c_str()); return 1; }
    uint8_t hdr[54] = {};
    hdr[0] = 'B'; hdr[1] = 'M';
    std::memcpy(&hdr[2], &fileBytes, 4);
    const int off = 54, dib = 40; std::memcpy(&hdr[10], &off, 4);
    std::memcpy(&hdr[14], &dib, 4);
    std::memcpy(&hdr[18], &W, 4);
    std::memcpy(&hdr[22], &H, 4);
    const uint16_t planes = 1, bpp = 24;
    std::memcpy(&hdr[26], &planes, 2); std::memcpy(&hdr[28], &bpp, 2);
    std::memcpy(&hdr[34], &imageBytes, 4);
    out.write(reinterpret_cast<char*>(hdr), 54);
    const uint8_t pad[3] = {0, 0, 0};
    for (int y = H - 1; y >= 0; --y) {
        for (int x = 0; x < W; ++x) {
            const size_t i = (static_cast<size_t>(y) * W + x) * 4;
            const uint8_t bgr[3] = {rgba[i + 2], rgba[i + 1], rgba[i]};
            out.write(reinterpret_cast<const char*>(bgr), 3);
        }
        if (padding) out.write(reinterpret_cast<const char*>(pad), padding);
    }
    std::printf("cpu-trace: %zu triangles -> %s\n", mesh.totalTriangles(),
                outBmp.c_str());
    return 0;
}

int main(int argc, char** argv) {
    attachParentConsole();
    registerBundledVulkanDriver();
    try {
        // No arguments: open the viewer with an empty board rather than printing
        // a report and exiting. Double-clicking the exe should give you the app.
        if (argc < 2) return pcbview::app::runViewer("");
        // Bare --view opens the empty viewer (which may then load PCBVIEW_OPEN),
        // rather than being mistaken for a board filename.
        if (std::string(argv[1]) == "--view" && argc == 2)
            return pcbview::app::runViewer("");
        if (std::string(argv[1]) == "--gpu-info") return gpuReport();
        if (std::string(argv[1]) == "--cpu-trace" && argc >= 4)
            return cpuTrace(argv[2], argv[3]);
        if (std::string(argv[1]) == "--gerber-probe" && argc >= 3)
            return gerberProbe(argv[2]);
        if (std::string(argv[1]) == "--gerber" && argc >= 4) {
            // --gerber <package> <out.bmp>: import gerbers, tessellate, preview.
            const pcbview::geom::LayerArt art =
                pcbview::gerber::importPackage(argv[2]);
            const pcbview::geom::BoardMesh mesh = pcbview::geom::assemble(art);
            std::printf("gerber package: %zu layers, %zu triangles\n",
                        art.layers.size(), mesh.totalTriangles());
            for (const std::string& w : art.warnings)
                std::printf("  warning: %s\n", w.c_str());
            pcbview::soft::renderPreview(mesh, argv[3], {});
            std::printf("wrote %s\n", argv[3]);
            return 0;
        }

        std::string board = argv[1];
        std::string objPath;
        std::string previewPath;
        bool tessellate = false;
        bool fromBottom = false;
        bool noMirror = false;
        bool view = false;

        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--tessellate") {
                tessellate = true;
            } else if (arg == "--obj" && i + 1 < argc) {
                objPath = argv[++i];
                tessellate = true;
            } else if (arg == "--preview" && i + 1 < argc) {
                previewPath = argv[++i];
                tessellate = true;
            } else if (arg == "--bottom") {
                fromBottom = true;
            } else if (arg == "--no-mirror") {
                noMirror = true;
            } else if (arg == "--view") {
                view = true;
            } else {
                std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
                return 2;
            }
        }

        // The viewer imports the board itself so File->Open can replace it.
        if (view) return pcbview::app::runViewer(board);
        if (tessellate) {
            return tessellateBoard(board, objPath, previewPath, fromBottom,
                                   noMirror);
        }
        return dumpBoard(board);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
}
