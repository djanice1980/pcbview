#!/bin/bash
# The version lives in five files; this fails loudly when they disagree.
#
# Born of a real incident: two release sessions crossed, one bumped only
# CMakeLists, the other's sed-based bump silently no-opped on the file that
# did not match its expected old value -- and the split shipped a draft whose
# Windows assets carried one version while the Linux packaging (which reads
# CMakeLists) carried another. A sed that finds nothing to replace says
# nothing; this script is the loud part. Run by BOTH CI workflows on every
# push, and by hand before tagging:
#   packaging/check-versions.sh          # check
#   packaging/check-versions.sh 1.27.0   # check that every site says exactly this
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)

cmake_v=$(sed -n 's/^project(pcbview VERSION \([0-9.]*\).*/\1/p' "$REPO/CMakeLists.txt")
rc_v=$(sed -n 's/.*"FileVersion",[^"]*"\([0-9.]*\)".*/\1/p' "$REPO/assets/pcbview.rc" | head -1)
rc_num=$(sed -n 's/^FILEVERSION[[:space:]]*\([0-9]*\),\([0-9]*\),\([0-9]*\).*/\1.\2.\3/p' "$REPO/assets/pcbview.rc")
iss_v=$(sed -n 's/^#define AppVersion "\([0-9.]*\)".*/\1/p' "$REPO/installer/pcbview.iss")
about_v=$(sed -n 's/.*<h3>pcbview \([0-9.]*\)<\/h3>.*/\1/p' "$REPO/src/app/main_window.cpp")
pkg_v=$(sed -n 's/^pkgver=\([0-9.]*\).*/\1/p' "$REPO/packaging/linux/PKGBUILD")

echo "CMakeLists.txt        $cmake_v"
echo "pcbview.rc (string)   $rc_v"
echo "pcbview.rc (numeric)  $rc_num"
echo "pcbview.iss           $iss_v"
echo "About dialog          $about_v"
echo "PKGBUILD              $pkg_v"

want=${1:-$cmake_v}
ok=1
for v in "$cmake_v" "$rc_v" "$rc_num" "$iss_v" "$about_v" "$pkg_v"; do
    [ "$v" = "$want" ] || ok=0
done
if [ "$ok" = 1 ]; then
    echo "OK: every site says $want"
else
    echo "MISMATCH: the sites disagree (expected $want)" >&2
    exit 1
fi
