#!/usr/bin/env bash
# Build the Release (non-ASAN) tree, package it as an RPM and install that with
# dnf, then restart the user service so the new daemon/overlay take effect.
# Going through the package keeps a single /usr copy that dnf can upgrade or
# remove. The dev `build/` tree is Debug+ASAN and is NOT what gets installed.
#
# Usage:  ./reinstall.sh
set -e
cd "$(dirname "$0")"

BUILD=build-release
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_ASAN=OFF
cmake --build "$BUILD"

echo
echo ">> packaging RPM..."
rm -f "$BUILD"/weazystroke-*.rpm
(cd "$BUILD" && cpack -G RPM)
rpm=$(ls "$BUILD"/weazystroke-*.rpm)

echo
echo ">> installing $rpm (sudo)..."
if rpm -q "$(rpm -qp "$rpm")" >/dev/null 2>&1; then
    sudo dnf reinstall -y "$rpm" # same version: `dnf install` would be a no-op
else
    sudo dnf install -y "$rpm"
fi

# Restart the daemon so the freshly installed eswl-daemon/eswl-overlay run. With
# the self-healing overlay, a later KWin restart no longer needs this step.
echo ">> restarting weazystroke.service..."
systemctl --user restart weazystroke.service

echo ">> done. processes:"
pgrep -a -f 'eswl-' || echo "   (none yet — give it a second)"
