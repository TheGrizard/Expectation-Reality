#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Build Expectation Reality: ./build.sh (the firmware), ./build.sh --host (the simulator), ./build.sh --release X.Y
# fm1-base is in base/ (a git submodule); the app in app/. The screen is drawn in DejaVu Sans (app/fonts), as After Images'.
set -e
cd "$(dirname "$0")"
export X0X_FONTSET="${X0X_FONTSET:-terminus}"
exec python3 base/tools/build.py app "$@"
