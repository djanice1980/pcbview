#include "io/sexpr.h"

#include <charconv>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace pcbview::sexpr {
namespace {

bool isAtomChar(char c) {
    return !(c == '(' || c == ')' || c == '"' || c == ' ' || c == '\t' ||
             c == '\n' || c == '\r');
}

class Parser {
public:
    Parser(std::string_view src, std::deque<std::string>& storage)
        : src_(src), storage_(storage) {}

    Node parse() {
        skipSpace();
        Node root = parseNode();
        skipSpace();
        if (pos_ != src_.size()) {
            fail("trailing content after root node");
        }
        return root;
    }

private:
    std::string_view src_;
    std::deque<std::string>& storage_;
    size_t pos_ = 0;

    [[noreturn]] void fail(const char* why) const {
        // Byte offset alone is useless in a 22k-line file; report line/column.
        size_t line = 1, col = 1;
        for (size_t i = 0; i < pos_ && i < src_.size(); ++i) {
            if (src_[i] == '\n') { ++line; col = 1; } else { ++col; }
        }
        std::ostringstream os;
        os << "sexpr parse error at line " << line << ", column " << col << ": "
           << why;
        throw std::runtime_error(os.str());
    }

    void skipSpace() {
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    Node parseNode() {
        if (pos_ >= src_.size()) fail("unexpected end of input");
        if (src_[pos_] == '(') return parseList();
        if (src_[pos_] == '"') return parseQuoted();
        if (src_[pos_] == ')') fail("unexpected ')'");
        return parseBareAtom();
    }

    Node parseList() {
        ++pos_;  // consume '('
        Node node;
        node.isList = true;
        for (;;) {
            skipSpace();
            if (pos_ >= src_.size()) fail("unterminated list");
            if (src_[pos_] == ')') {
                ++pos_;
                return node;
            }
            node.kids.push_back(parseNode());
        }
    }

    Node parseQuoted() {
        ++pos_;  // consume opening '"'
        const size_t start = pos_;
        bool needsUnescape = false;

        while (pos_ < src_.size() && src_[pos_] != '"') {
            if (src_[pos_] == '\\') {
                needsUnescape = true;
                ++pos_;
                if (pos_ >= src_.size()) fail("unterminated escape");
            }
            ++pos_;
        }
        if (pos_ >= src_.size()) fail("unterminated string");

        const std::string_view raw = src_.substr(start, pos_ - start);
        ++pos_;  // consume closing '"'

        Node node;
        node.quoted = true;
        if (!needsUnescape) {
            node.atom = raw;  // borrow directly from the source buffer
            return node;
        }

        std::string out;
        out.reserve(raw.size());
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\\' && i + 1 < raw.size()) ++i;
            out.push_back(raw[i]);
        }
        storage_.push_back(std::move(out));
        node.atom = storage_.back();  // stable: deque never moves elements
        return node;
    }

    Node parseBareAtom() {
        const size_t start = pos_;
        while (pos_ < src_.size() && isAtomChar(src_[pos_])) ++pos_;
        if (pos_ == start) fail("empty atom");

        Node node;
        node.atom = src_.substr(start, pos_ - start);
        return node;
    }
};

}  // namespace

std::string_view Node::head() const {
    if (!isList || kids.empty() || kids[0].isList) return {};
    return kids[0].atom;
}

const Node* Node::child(std::string_view name) const {
    for (const Node& kid : kids) {
        if (kid.isList && kid.head() == name) return &kid;
    }
    return nullptr;
}

std::vector<const Node*> Node::childList(std::string_view name) const {
    std::vector<const Node*> out;
    for (const Node& kid : kids) {
        if (kid.isList && kid.head() == name) out.push_back(&kid);
    }
    return out;
}

double Node::num(size_t i, double fallback) const {
    if (i >= kids.size() || kids[i].isList) return fallback;
    const std::string_view text = kids[i].atom;
    double value = fallback;

    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) return fallback;
    return value;
}

std::string_view Node::str(size_t i, std::string_view fallback) const {
    if (i >= kids.size() || kids[i].isList) return fallback;
    return kids[i].atom;
}

double Node::childNum(std::string_view name, double fallback) const {
    const Node* c = child(name);
    return c ? c->num(1, fallback) : fallback;
}

std::string_view Node::childStr(std::string_view name,
                                std::string_view fallback) const {
    const Node* c = child(name);
    return c ? c->str(1, fallback) : fallback;
}

bool Node::hasAtom(std::string_view value) const {
    for (const Node& kid : kids) {
        if (!kid.isList && kid.atom == value) return true;
    }
    return false;
}

Document parseString(std::string text) {
    Document doc;
    doc.source = std::move(text);
    Parser parser(doc.source, doc.storage);
    doc.root = parser.parse();
    return doc;
}

Document parseFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open file: " + path);

    std::ostringstream buffer;
    buffer << in.rdbuf();
    return parseString(buffer.str());
}

}  // namespace pcbview::sexpr
