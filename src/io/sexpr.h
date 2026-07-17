#pragma once

// Minimal s-expression reader for .kicad_pcb files.
//
// Nodes borrow string_views into the source buffer, so the buffer must outlive
// the tree. Parsing is one pass with no per-token allocation.

#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace pcbview::sexpr {

struct Node {
    bool isList = false;

    // Atoms only. Quotes are stripped and escapes resolved; when that happens
    // the text is owned by `storage` rather than borrowed from the source.
    std::string_view atom;
    bool quoted = false;

    std::vector<Node> kids;

    // First kid's atom, e.g. "segment" for (segment (start ...) ...).
    std::string_view head() const;

    // First child list whose head matches `name`, or nullptr.
    const Node* child(std::string_view name) const;

    // Every child list whose head matches `name`.
    std::vector<const Node*> childList(std::string_view name) const;

    // kids[i] as a number / string. Out-of-range or wrong-kind returns fallback.
    double num(size_t i, double fallback = 0.0) const;
    std::string_view str(size_t i, std::string_view fallback = {}) const;

    // Convenience: (thickness 1.6) -> child("thickness")->num(1).
    double childNum(std::string_view name, double fallback = 0.0) const;
    std::string_view childStr(std::string_view name,
                              std::string_view fallback = {}) const;

    bool hasAtom(std::string_view value) const;
};

struct Document {
    Node root;
    std::string source;  // owns the file text

    // Owns unescaped strings. Must be a deque, not a vector: nodes hold
    // string_views into these, and vector reallocation would dangle them all.
    std::deque<std::string> storage;
};

// Throws std::runtime_error on malformed input.
Document parseFile(const std::string& path);
Document parseString(std::string text);

}  // namespace pcbview::sexpr
