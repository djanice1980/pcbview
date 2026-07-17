#include "io/obj_export.h"

#include <fstream>
#include <stdexcept>

namespace pcbview {

void exportObj(const geom::BoardMesh& mesh, const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write: " + path);

    out << "# pcbview geometry dump\n";
    out << "# " << mesh.parts.size() << " parts, " << mesh.totalTriangles()
        << " triangles\n";

    // OBJ indices are 1-based and global across the file.
    size_t vertexBase = 1;

    for (const geom::Part& part : mesh.parts) {
        out << "\ng " << part.name << "\n";

        for (const geom::Vertex& v : part.mesh.vertices) {
            out << "v " << v.position[0] << ' ' << v.position[1] << ' '
                << v.position[2] << '\n';
        }
        for (const geom::Vertex& v : part.mesh.vertices) {
            out << "vn " << v.normal[0] << ' ' << v.normal[1] << ' '
                << v.normal[2] << '\n';
        }

        const std::vector<uint32_t>& idx = part.mesh.indices;
        for (size_t i = 0; i + 2 < idx.size(); i += 3) {
            const size_t a = vertexBase + idx[i];
            const size_t b = vertexBase + idx[i + 1];
            const size_t c = vertexBase + idx[i + 2];
            out << "f " << a << "//" << a << ' ' << b << "//" << b << ' ' << c
                << "//" << c << '\n';
        }
        vertexBase += part.mesh.vertices.size();
    }
}

}  // namespace pcbview
