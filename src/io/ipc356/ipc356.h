#pragma once

// IPC-D-356 netlist import.
//
// A 356 file is the fab-side net list: one record per pad/via test point,
// each carrying a NET NAME and a board position. It contains no geometry --
// but a Gerber package plus a 356 file is exactly the pairing fabs receive,
// and it is what lets a bare package get REAL net names:
//
//   356 supplies names at positions; copper connectivity (extractPseudoNets)
//   supplies exact galvanic groups; a test point inside an island names the
//   whole group. Names nobody had to invent, connectivity nobody had to
//   assert -- and disagreements between the two are real findings (a net
//   split across groups is an open; two names in one group is a short).
//
// Format: fixed-ish column text. Record types read here: 317 (plated
// through hole) and 327 (surface mount). The P UNITS parameter selects
// inches (x0.0001") or metric (x0.001mm). Everything else is skipped.

#include <string>
#include <vector>

namespace pcbview::ipc356 {

struct Record {
    std::string net;     // as written; "N/C" and empties are filtered out
    double x = 0.0;      // mm
    double y = 0.0;      // mm
    bool through = false;  // 317: plated through -- present on every layer
};

struct File {
    std::vector<Record> records;
    std::vector<std::string> warnings;
    bool ok = false;
};

File parse(const std::string& text);

// Cheap content sniff for package loaders: does this text look like IPC-D-356?
bool looksLike(const std::string& text);

}  // namespace pcbview::ipc356
