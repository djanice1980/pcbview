#pragma once

#include <QApplication>

namespace pcbview::app {

// "Pro CAD" palette: neutral greys, dense, minimal chrome. Deliberately not
// tinted -- the board is the only saturated thing on screen, so copper, mask and
// laminate read true. A coloured UI competes with the thing being inspected.
namespace theme {

constexpr const char* kBg0 = "#1e1e1e";  // viewport / sunken
constexpr const char* kBg1 = "#2b2b2b";  // panels
constexpr const char* kBg2 = "#353535";  // toolbar / raised
constexpr const char* kBg3 = "#3f3f3f";  // hover
constexpr const char* kLine = "#111111";
constexpr const char* kText = "#dcdcdc";
constexpr const char* kTextDim = "#8f8f8f";
constexpr const char* kAccent = "#4a90d9";

void apply(QApplication& app);

}  // namespace theme
}  // namespace pcbview::app
