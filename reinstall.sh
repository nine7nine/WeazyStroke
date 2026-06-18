#!/usr/bin/env bash
# Build the Release (non-ASAN) tree and install the four binaries to /usr, then
# restart the user service so the new daemon/overlay take effect. The dev `build/`
# tree is Debug+ASAN and is NOT what gets installed.
#
# Usage:  ./reinstall.sh
set -e
cd "$(dirname "$0")"

BUILD=build-release
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_ASAN=OFF
cmake --build "$BUILD"

echo
echo ">> installing to /usr (sudo)..."
sudo cmake --install "$BUILD"

# Restart the daemon so the freshly installed eswl-daemon/eswl-overlay run. With
# the self-healing overlay, a later KWin restart no longer needs this step.
echo ">> restarting weazystroke.service..."
systemctl --user restart weazystroke.service

echo ">> done. processes:"
pgrep -a -f 'eswl-' || echo "   (none yet — give it a second)"
