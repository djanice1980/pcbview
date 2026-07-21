#pragma once

// The ODB++ job tree, abstracted away from how it arrived.
//
// An ODB++ product model is a DIRECTORY TREE (matrix/matrix, steps/<s>/...,
// symbols/<s>/...), and in the wild it ships four ways: a plain folder, a
// .tgz/.tar.gz (the "compressed model" the spec itself defines), a .tar, or a
// .zip somebody made by hand. Individual member files may additionally be
// compressed on their own: <name>.gz (gzip) or <name>.Z (Unix compress/LZW),
// both of which real exporters emit. OdbFs loads whichever container it is
// given, transparently decompresses members, strips the job-name root folder,
// and serves files by case-insensitive forward-slash path -- so everything
// above it can pretend the spec's clean directory layout always exists.

#include <map>
#include <string>
#include <vector>

namespace pcbview::odb {

class OdbFs {
public:
    // Load from a directory, .tgz/.tar.gz/.tar, or .zip. Throws
    // std::runtime_error when the container cannot be read at all; a readable
    // container that is not an ODB++ job loads fine and simply fails exists()
    // checks (the importer turns that into its own message).
    static OdbFs open(const std::string& path);

    // Path lookup is case-insensitive, '/'-separated, relative to the job
    // root (the directory containing matrix/). "steps/pcb/layers/top/features"
    // finds features, features.gz or features.Z, decompressed.
    bool exists(const std::string& path) const;
    // Returns the file's text, or empty when absent (use exists() to tell an
    // absent file from an empty one).
    std::string read(const std::string& path) const;

    // Immediate subdirectory names under `path` (e.g. list("steps") -> step
    // names), in first-seen order.
    std::vector<std::string> dirs(const std::string& path) const;

    // Anything odd met while loading (an unreadable member, an unsupported
    // compression) -- surfaced by the importer as warnings.
    std::vector<std::string> warnings;

private:
    // Key: lower-cased root-relative path with the .gz/.Z suffix removed.
    // Value: decompressed content.
    std::map<std::string, std::string> files_;
    void insert(std::string path, std::string content);
    void stripRoot();
};

// Decompressors, exposed for reuse and tests.
// Each returns false when `data` is not in its format or is corrupt.
bool gunzip(const std::string& data, std::string& out);
bool lzwDecompress(const std::string& data, std::string& out);  // .Z / compress

}  // namespace pcbview::odb
