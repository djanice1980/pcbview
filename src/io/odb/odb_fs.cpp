#include "io/odb/odb_fs.h"

#include <miniz.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace pcbview::odb {
namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string readWholeFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool endsWith(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Normalise a container-relative path: forward slashes, lower case, no
// leading "./" or "/".
std::string normPath(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    while (p.rfind("./", 0) == 0) p.erase(0, 2);
    while (!p.empty() && p.front() == '/') p.erase(0, 1);
    return toLower(p);
}

}  // namespace

// ---- gzip ------------------------------------------------------------------
//
// miniz's tinfl inflates raw DEFLATE; the gzip framing (RFC 1952) is a small
// header to skip and a trailer to ignore, parsed here by hand.

bool gunzip(const std::string& data, std::string& out) {
    if (data.size() < 18) return false;
    const auto* b = reinterpret_cast<const unsigned char*>(data.data());
    if (b[0] != 0x1f || b[1] != 0x8b || b[2] != 8) return false;  // magic+CM
    const unsigned flg = b[3];
    size_t pos = 10;
    if (flg & 4) {  // FEXTRA
        if (pos + 2 > data.size()) return false;
        pos += 2 + (b[pos] | (b[pos + 1] << 8));
    }
    for (const unsigned bit : {8u, 16u}) {  // FNAME, FCOMMENT: NUL-terminated
        if (!(flg & bit)) continue;
        while (pos < data.size() && b[pos] != 0) ++pos;
        ++pos;
    }
    if (flg & 2) pos += 2;  // FHCRC
    if (pos >= data.size()) return false;

    // ISIZE (last 4 bytes) is the uncompressed length mod 2^32 -- a perfect
    // pre-allocation hint for any file under 4GB, which is all of them here.
    const size_t tail = data.size() - 4;
    const uint32_t isize = b[tail] | (b[tail + 1] << 8) | (b[tail + 2] << 16) |
                           (static_cast<uint32_t>(b[tail + 3]) << 24);
    if (isize > (1u << 30)) return false;  // corrupt header, not a 1GB member
    out.assign(isize, '\0');
    size_t outLen = isize;
    tinfl_decompressor inflator;
    tinfl_init(&inflator);
    size_t inLen = data.size() - pos;
    const tinfl_status st = tinfl_decompress(
        &inflator, b + pos, &inLen,
        reinterpret_cast<mz_uint8*>(out.data()),
        reinterpret_cast<mz_uint8*>(out.data()), &outLen,
        TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (st != TINFL_STATUS_DONE || outLen != isize) return false;
    return true;
}

// ---- .Z (Unix compress, LZW) -----------------------------------------------
//
// Still emitted by Valor-lineage exporters for individual ODB++ members.
// Classic LZW with variable code width 9..maxbits and optional block mode
// (code 256 = clear table). ~60 lines beats shipping another dependency.

bool lzwDecompress(const std::string& data, std::string& out) {
    if (data.size() < 3) return false;
    const auto* b = reinterpret_cast<const unsigned char*>(data.data());
    if (b[0] != 0x1f || b[1] != 0x9d) return false;
    const int maxBits = b[2] & 0x1f;
    const bool blockMode = (b[2] & 0x80) != 0;
    if (maxBits < 9 || maxBits > 16) return false;

    // prefix/suffix chain table. Entry i (>=256, and >=257 in block mode)
    // decodes to table[prefix[i]] + suffix[i].
    std::vector<int> prefix(1 << maxBits, 0);
    std::vector<unsigned char> suffix(1 << maxBits, 0);
    const int first = blockMode ? 257 : 256;
    int next = first;
    int bits = 9;
    int prev = -1;
    unsigned char prevFirstByte = 0;
    uint32_t buf = 0;
    int bufBits = 0;
    size_t pos = 3;
    std::vector<unsigned char> stack;
    // compress(1) refills its input in BITS-byte groups and pads the tail of
    // a group when the width changes -- the reader must do the same or it
    // desynchronises exactly at the first width bump.
    size_t groupStart = 3;
    int groupBits = 9;

    const auto resetGroup = [&](int newBits) {
        // Skip to the next group boundary of the OLD width.
        const size_t consumed = pos - groupStart;
        const size_t groupBytes = static_cast<size_t>(groupBits);
        const size_t rem = consumed % groupBytes;
        if (rem) pos += groupBytes - rem;
        buf = 0;
        bufBits = 0;
        groupStart = pos;
        groupBits = newBits;
    };

    while (true) {
        while (bufBits < bits && pos < data.size()) {
            buf |= static_cast<uint32_t>(b[pos++]) << bufBits;
            bufBits += 8;
        }
        if (bufBits < bits) break;  // end of stream
        const int code = static_cast<int>(buf & ((1u << bits) - 1));
        buf >>= bits;
        bufBits -= bits;

        if (blockMode && code == 256) {
            resetGroup(9);
            next = first;
            bits = 9;
            prev = -1;
            continue;
        }
        // Valid codes are the table (< next) plus code == next, the KwKwK
        // case, which only exists once there is a previous string.
        if (code > next || (code == next && prev < 0)) return false;
        stack.clear();
        int c = code;
        if (c == next && prev >= 0) {  // KwKwK
            stack.push_back(prevFirstByte);
            c = prev;
        }
        while (c >= first) {
            stack.push_back(suffix[c]);
            c = prefix[c];
        }
        stack.push_back(static_cast<unsigned char>(c));
        const unsigned char firstByte = stack.back();
        out.append(stack.rbegin(), stack.rend());

        if (prev >= 0 && next < (1 << maxBits)) {
            prefix[next] = prev;
            suffix[next] = firstByte;
            ++next;
        }
        prev = code;
        prevFirstByte = firstByte;
        if (next >= (1 << bits) && bits < maxBits) {
            resetGroup(bits + 1);
            ++bits;
        }
    }
    return true;
}

// ---- containers ------------------------------------------------------------

namespace {

// tar: 512-byte headers, octal sizes, content padded to block size. Enough of
// USTAR + GNU longname to read what tar(1) writes.
void loadTar(const std::string& tar,
             const std::function<void(std::string, std::string)>& insert) {
    size_t pos = 0;
    std::string pendingLongName;
    while (pos + 512 <= tar.size()) {
        const char* h = tar.data() + pos;
        if (h[0] == '\0') break;  // end-of-archive zero block
        char sizeField[13] = {};
        std::memcpy(sizeField, h + 124, 12);
        const size_t size = std::strtoull(sizeField, nullptr, 8);
        const char type = h[156];
        std::string name(h, strnlen(h, 100));
        if (h[345] != '\0') {  // USTAR prefix
            std::string prefix(h + 345, strnlen(h + 345, 155));
            name = prefix + "/" + name;
        }
        if (!pendingLongName.empty()) {
            name = pendingLongName;
            pendingLongName.clear();
        }
        const size_t body = pos + 512;
        const size_t padded = (size + 511) & ~size_t(511);
        if (type == 'L' && body + size <= tar.size()) {  // GNU longname
            pendingLongName.assign(tar.data() + body, size);
            while (!pendingLongName.empty() && pendingLongName.back() == '\0')
                pendingLongName.pop_back();
        } else if ((type == '0' || type == '\0') &&
                   body + size <= tar.size()) {
            insert(name, tar.substr(body, size));
        }
        pos = body + padded;
    }
}

}  // namespace

void OdbFs::insert(std::string path, std::string content) {
    path = normPath(std::move(path));
    if (path.empty()) return;
    // Member-level compression: serve the file under its real name.
    if (endsWith(path, ".gz")) {
        std::string plain;
        if (gunzip(content, plain)) {
            path.erase(path.size() - 3);
            content = std::move(plain);
        } else {
            warnings.push_back("could not decompress " + path);
            return;
        }
    } else if (endsWith(path, ".z")) {
        std::string plain;
        // ".z" in the wild is either compress (0x1f9d) or gzip (0x1f8b).
        if (lzwDecompress(content, plain) || gunzip(content, plain)) {
            path.erase(path.size() - 2);
            content = std::move(plain);
        } else {
            warnings.push_back("could not decompress " + path);
            return;
        }
    }
    files_[path] = std::move(content);
}

void OdbFs::stripRoot() {
    // A job archived as <jobname>/matrix/matrix... should serve paths without
    // the job name. Find the shortest prefix that puts matrix/matrix at root.
    if (files_.count("matrix/matrix")) return;
    for (const auto& [path, _] : files_) {
        const size_t at = path.find("/matrix/matrix");
        if (at == std::string::npos ||
            at + 14 != path.size())
            continue;
        const std::string prefix = path.substr(0, at + 1);
        std::map<std::string, std::string> moved;
        for (auto& [p, content] : files_) {
            if (p.rfind(prefix, 0) == 0)
                moved[p.substr(prefix.size())] = std::move(content);
            else
                moved[p] = std::move(content);
        }
        files_ = std::move(moved);
        return;
    }
}

OdbFs OdbFs::open(const std::string& path) {
    OdbFs fsys;
    const fs::path p(path);
    if (fs::is_directory(p)) {
        for (const auto& e : fs::recursive_directory_iterator(p)) {
            if (!e.is_regular_file()) continue;
            fsys.insert(fs::relative(e.path(), p).generic_string(),
                        readWholeFile(e.path()));
        }
    } else if (fs::is_regular_file(p)) {
        std::string data = readWholeFile(p);
        std::string plain;
        if (gunzip(data, plain)) data = std::move(plain);  // .tgz / .tar.gz
        if (data.size() >= 265 &&
            std::memcmp(data.data() + 257, "ustar", 5) == 0) {
            loadTar(data, [&](std::string n, std::string c) {
                fsys.insert(std::move(n), std::move(c));
            });
        } else if (data.size() >= 2 && data[0] == 'P' && data[1] == 'K') {
            mz_zip_archive zip{};
            if (!mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0))
                throw std::runtime_error("cannot open archive: " + path);
            const mz_uint n = mz_zip_reader_get_num_files(&zip);
            for (mz_uint i = 0; i < n; ++i) {
                mz_zip_archive_file_stat st;
                if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
                if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
                size_t size = 0;
                void* mem = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
                if (!mem) continue;
                fsys.insert(st.m_filename,
                            std::string(static_cast<const char*>(mem), size));
                mz_free(mem);
            }
            mz_zip_reader_end(&zip);
        } else {
            throw std::runtime_error(
                "not an ODB++ container (expected a folder, .tgz or .zip): " +
                path);
        }
    } else {
        throw std::runtime_error("cannot open: " + path);
    }
    fsys.stripRoot();
    return fsys;
}

bool OdbFs::exists(const std::string& path) const {
    return files_.count(normPath(path)) != 0;
}

std::string OdbFs::read(const std::string& path) const {
    const auto it = files_.find(normPath(path));
    return it == files_.end() ? std::string() : it->second;
}

std::vector<std::string> OdbFs::dirs(const std::string& path) const {
    const std::string prefix = normPath(path) + "/";
    std::vector<std::string> out;
    for (const auto& [p, _] : files_) {
        if (p.rfind(prefix, 0) != 0) continue;
        const size_t slash = p.find('/', prefix.size());
        if (slash == std::string::npos) continue;
        const std::string dir = p.substr(prefix.size(), slash - prefix.size());
        if (std::find(out.begin(), out.end(), dir) == out.end())
            out.push_back(dir);
    }
    return out;
}

}  // namespace pcbview::odb
