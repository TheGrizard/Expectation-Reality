#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""After Images' fonts: DejaVu Sans (Bitstream Vera's open descendant: wide, open shapes, made to be read small on a
screen, as Verdana was), rendered by fm1-base's tools/gen_font.py into the same header, after fm1-base's own (fm1.toml
gen runs this once the platform's fonts are made). The faces are in app/fonts, with their licence.
  gen_font_dejavu.py OUT_HEADER"""
import importlib.util
import os
import sys
from pathlib import Path

APP = Path(__file__).resolve().parents[1]
BASE = APP.parent / "base"
os.environ["X0X_FONTSET"] = "terminus"                 # (any set the module knows, to import it: replaced below)
os.environ["X0X_FONT_DIRS"] = str(APP / "fonts")
spec = importlib.util.spec_from_file_location("gen_font", BASE / "tools" / "gen_font.py")
gf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gf)
gf.FONTDIRS = [APP / "fonts"]
# the six roles: T footers, XS labels and hints, S body, B names and titles, M the knobs' values, L the splash
gf.SIZES = [("T", "DejaVuSans-Bold.ttf", 9, 1, False, 32, 95),
            ("XS", "DejaVuSans.ttf", 10, 1, False, 32, 126),
            ("S", "DejaVuSans.ttf", 13, 1, False, 32, 126),
            ("B", "DejaVuSans-Bold.ttf", 13, 1, False, 32, 126),
            ("M", "DejaVuSans-Bold.ttf", 20, 1, False, 32, 95),
            ("L", "DejaVuSans-Bold.ttf", 28, 1, False, 32, 95)]
gf.FONTSET = "dejavu"
gf.main(sys.argv[1])
