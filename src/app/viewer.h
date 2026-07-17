#pragma once

// Qt application shell. Owns the QApplication and the main window.

#include <string>

namespace pcbview::app {

struct ViewerOptions {
    int width = 1600;
    int height = 950;
};

// Opens the viewer and runs until closed. `path` may be empty -- the window
// starts with no board and File->Open loads one. Returns the Qt exit code.
int runViewer(const std::string& path, const ViewerOptions& opts = {});

}  // namespace pcbview::app
