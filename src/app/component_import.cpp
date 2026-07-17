#include "app/component_import.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>

// cgltf: single-header glTF/GLB loader (MIT). The implementation is compiled
// here, in exactly one translation unit.
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

namespace pcbview::app {
namespace {

// kicad-cli location, in order of preference: an explicit override, then PATH,
// then the standard per-user and machine install roots (newest version first).
// Returns an empty string if none is found -- components are then simply skipped.
QString findKicadCli() {
    const QByteArray override = qgetenv("PCBVIEW_KICAD_CLI");
    if (!override.isEmpty()) {
        const QString p = QString::fromLocal8Bit(override);
        if (QFileInfo::exists(p)) return p;
    }

    const QString onPath = QStandardPaths::findExecutable("kicad-cli");
    if (!onPath.isEmpty()) return onPath;

    QStringList roots;
    auto addRoot = [&](const QByteArray& base, const char* sub) {
        if (!base.isEmpty()) roots << QString::fromLocal8Bit(base) + sub;
    };
    addRoot(qgetenv("LOCALAPPDATA"), "/Programs/KiCad");
    addRoot(qgetenv("ProgramFiles"), "/KiCad");
    addRoot(qgetenv("ProgramW6432"), "/KiCad");

    for (const QString& root : roots) {
        QDir dir(root);
        if (!dir.exists()) continue;
        // Version dirs like "10.0", "9.0". Sort descending so the newest wins.
        QStringList versions =
            dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        std::sort(versions.begin(), versions.end(), std::greater<QString>());
        for (const QString& v : versions) {
            const QString cli = root + "/" + v + "/bin/kicad-cli.exe";
            if (QFileInfo::exists(cli)) return cli;
        }
    }
    return {};
}

// A stable cache path for this board's components GLB. Keyed by the board's
// absolute path and last-modified time, so editing the board re-exports while an
// unchanged board is a cache hit. Lives under the per-user cache dir, not beside
// the board -- generated data does not belong in the user's project folder.
QString cachePathFor(const QString& pcbPath) {
    const QFileInfo info(pcbPath);
    const QString key = info.absoluteFilePath() + "|" +
                        QString::number(info.lastModified().toMSecsSinceEpoch());
    const QString hash =
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex();

    QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty()) base = QDir::tempPath();
    const QString dir = base + "/components";
    QDir().mkpath(dir);
    return dir + "/" + info.completeBaseName() + "-" + hash + ".glb";
}

// glTF world point (metres, Y-up) -> pcbview world (mm). Derived from a known
// component: KiCad C6 at (47, 61.5) exports to glTF (0.047, height, 0.0615).
//   world X = +gltf X          (KiCad X)
//   world Y = -gltf Z          (world Y = -KiCad Y, and gltf Z = KiCad Y)
//   world Z = +gltf Y          (glTF Y-up is our board-normal height)
// The linear part has determinant +1, so winding and normals are preserved.
inline void gltfPointToWorld(float x, float y, float z, float out[3]) {
    out[0] = x * 1000.0f;
    out[1] = -z * 1000.0f;
    out[2] = y * 1000.0f;
}
inline void gltfDirToWorld(float x, float y, float z, float out[3]) {
    out[0] = x;
    out[1] = -z;
    out[2] = y;
}

// Apply a cgltf column-major 4x4 to a point / direction (gltf local -> gltf
// world), before the axis change above.
inline void mulPoint(const float m[16], const float in[3], float out[3]) {
    out[0] = m[0] * in[0] + m[4] * in[1] + m[8] * in[2] + m[12];
    out[1] = m[1] * in[0] + m[5] * in[1] + m[9] * in[2] + m[13];
    out[2] = m[2] * in[0] + m[6] * in[1] + m[10] * in[2] + m[14];
}
inline void mulDir(const float m[16], const float in[3], float out[3]) {
    out[0] = m[0] * in[0] + m[4] * in[1] + m[8] * in[2];
    out[1] = m[1] * in[0] + m[5] * in[1] + m[9] * in[2];
    out[2] = m[2] * in[0] + m[6] * in[1] + m[10] * in[2];
}

struct Accum {
    geom::Mesh mesh;
    float color[4] = {0.8f, 0.8f, 0.8f, 1.0f};
    int mountSide = 1;
};

// Load a components GLB into world-space parts, grouped by (material, mounting
// side) so every part carries one colour and one explode side.
std::vector<geom::Part> loadGlb(const QString& glbPath,
                                const geom::Bounds& bounds, std::string& err) {
    std::vector<geom::Part> parts;
    const QByteArray pathBytes = QFile::encodeName(glbPath);

    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, pathBytes.constData(), &data) !=
        cgltf_result_success) {
        err = "could not parse component GLB";
        return parts;
    }
    if (cgltf_load_buffers(&options, data, pathBytes.constData()) !=
        cgltf_result_success) {
        cgltf_free(data);
        err = "could not load component GLB buffers";
        return parts;
    }

    const double boardMidZ = 0.5 * (bounds.min[2] + bounds.max[2]);

    // Key: material index (materials_count == "no material"), then mounting side.
    std::map<std::pair<size_t, int>, Accum> groups;

    for (size_t ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node& node = data->nodes[ni];
        if (!node.mesh) continue;

        float world[16];
        cgltf_node_transform_world(&node, world);

        // Mounting side from where this node actually sits in Z (our space).
        float origin[3];
        const float zero[3] = {0, 0, 0};
        mulPoint(world, zero, origin);
        float ow[3];
        gltfPointToWorld(origin[0], origin[1], origin[2], ow);
        const int side = (ow[2] >= boardMidZ) ? 1 : -1;

        for (size_t pi = 0; pi < node.mesh->primitives_count; ++pi) {
            const cgltf_primitive& prim = node.mesh->primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) continue;

            const cgltf_accessor* pos = nullptr;
            const cgltf_accessor* nrm = nullptr;
            for (size_t a = 0; a < prim.attributes_count; ++a) {
                if (prim.attributes[a].type == cgltf_attribute_type_position)
                    pos = prim.attributes[a].data;
                else if (prim.attributes[a].type == cgltf_attribute_type_normal)
                    nrm = prim.attributes[a].data;
            }
            if (!pos) continue;

            const size_t matIndex =
                prim.material ? static_cast<size_t>(prim.material - data->materials)
                              : data->materials_count;
            Accum& acc = groups[{matIndex, side}];
            acc.mountSide = side;
            if (prim.material && prim.material->has_pbr_metallic_roughness) {
                const cgltf_float* c =
                    prim.material->pbr_metallic_roughness.base_color_factor;
                acc.color[0] = c[0];
                acc.color[1] = c[1];
                acc.color[2] = c[2];
                acc.color[3] = c[3];
            }

            const uint32_t base =
                static_cast<uint32_t>(acc.mesh.vertices.size());
            for (size_t v = 0; v < pos->count; ++v) {
                float p[3] = {0, 0, 0};
                cgltf_accessor_read_float(pos, v, p, 3);
                float pw[3];
                mulPoint(world, p, pw);
                geom::Vertex vert{};
                gltfPointToWorld(pw[0], pw[1], pw[2], vert.position);

                float n[3] = {0, 0, 1};
                if (nrm) cgltf_accessor_read_float(nrm, v, n, 3);
                float nw[3];
                mulDir(world, n, nw);
                float nworld[3];
                gltfDirToWorld(nw[0], nw[1], nw[2], nworld);
                const float len = std::sqrt(nworld[0] * nworld[0] +
                                            nworld[1] * nworld[1] +
                                            nworld[2] * nworld[2]);
                const float inv = len > 1e-8f ? 1.0f / len : 0.0f;
                vert.normal[0] = nworld[0] * inv;
                vert.normal[1] = nworld[1] * inv;
                vert.normal[2] = nworld[2] * inv;
                acc.mesh.vertices.push_back(vert);
            }

            if (prim.indices) {
                for (size_t i = 0; i < prim.indices->count; ++i) {
                    acc.mesh.indices.push_back(
                        base +
                        static_cast<uint32_t>(cgltf_accessor_read_index(
                            prim.indices, i)));
                }
            } else {
                for (size_t i = 0; i < pos->count; ++i)
                    acc.mesh.indices.push_back(base + static_cast<uint32_t>(i));
            }
        }
    }

    cgltf_free(data);

    for (auto& [key, acc] : groups) {
        if (acc.mesh.indices.empty()) continue;
        geom::Part part;
        part.material = geom::Material::Component;
        // One shared name, so the single "Components" tree toggle drives every
        // colour/side group at once (onLayerToggled matches all parts by name).
        part.name = "Components";
        part.mesh = std::move(acc.mesh);
        part.color[0] = acc.color[0];
        part.color[1] = acc.color[1];
        part.color[2] = acc.color[2];
        part.color[3] = acc.color[3];
        part.mountSide = acc.mountSide;
        parts.push_back(std::move(part));
    }

    if (parts.empty()) err = "no component geometry in export";
    return parts;
}

}  // namespace

ComponentImport importComponents(const std::string& pcbPath,
                                 const geom::Bounds& boardBounds) {
    ComponentImport result;
    const QString board = QString::fromStdString(pcbPath);

    const QString cli = findKicadCli();
    if (cli.isEmpty()) {
        result.message =
            "components skipped: kicad-cli not found (install KiCad, or set "
            "PCBVIEW_KICAD_CLI)";
        return result;
    }

    const QString cache = cachePathFor(board);
    if (!QFileInfo::exists(cache)) {
        QProcess proc;
        QStringList args{"pcb",     "export",         "glb",
                         "--no-board-body",  // we render our own board
                         "--subst-models",   // use STEP where no VRML exists
                         "--no-dnp",         // fab does not place DNP parts
                         "--force",   "-o",  cache,   board};
        proc.start(cli, args);
        if (!proc.waitForStarted(10000)) {
            result.message = "components skipped: could not start kicad-cli";
            return result;
        }
        // STEP tessellation of a full board can take a few seconds.
        proc.waitForFinished(120000);
        if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0 ||
            !QFileInfo::exists(cache)) {
            const QString errOut =
                QString::fromLocal8Bit(proc.readAllStandardError()).trimmed();
            QString msg = "components skipped: kicad-cli export failed";
            if (!errOut.isEmpty()) msg += " (" + errOut.left(200) + ")";
            result.message = msg.toStdString();
            return result;
        }
    }

    std::string err;
    result.parts = loadGlb(cache, boardBounds, err);
    if (result.parts.empty()) {
        result.message = err.empty() ? "no components" : ("components: " + err);
        return result;
    }

    size_t tris = 0;
    for (const geom::Part& p : result.parts) tris += p.mesh.triangleCount();
    result.available = true;
    result.message = "components: " + std::to_string(tris) + " triangles";
    return result;
}

}  // namespace pcbview::app
