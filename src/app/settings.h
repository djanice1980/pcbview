#pragma once

#include <QSettings>
#include <QString>

namespace pcbview {

// A QSettings handle bound to  <home>/.pcbview/settings.xml  in a
// human-readable XML format, replacing Qt's default per-platform backend (the
// Windows registry). Every call returns an independent handle onto the same
// file; use it exactly like the old default QSettings() -- read with value(),
// write with setValue(). The write lands when the handle is destroyed (end of
// the full expression for a temporary, end of scope for a named local).
//
// Deleting the file resets every setting to its default: with no file present
// value() returns the fallback passed at each call site, and the next write
// recreates the file. So `rm ~/.pcbview/settings.xml` is a clean factory
// reset, honoured on the next launch.
QSettings appSettings();

// Absolute path to the settings file, for logging / status / reveal-in-explorer.
QString settingsFilePath();

}  // namespace pcbview
