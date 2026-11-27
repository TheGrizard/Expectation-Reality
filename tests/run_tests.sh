#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The simulator's checks: the pages, the knobs, REC, saving, the keys, the clock, After Images heard. Each script fails the run on
# a FAIL line.
set -e
cd "$(dirname "$0")/.."
out=app/build/host/shots
mkdir -p "$out"
fail=0
# the app's own code compiles with no warning (one read of a value never set drew nothing in the browser's emulator)
./build.sh --host 2> "$out/build.log" || { cat "$out/build.log"; exit 1; }
if grep -E "^[^ ]*app/[^ ]*: warning:" "$out/build.log"; then echo "FAIL: warnings in the app's code"; fail=1; fi
# the sound engine (app/dsp, built quietly into the simulator) with every warning an error, through the desktop renderer
make -s -C app/tools clean-render >/dev/null 2>&1 || true
if ! make -s -C app/tools WERROR=-Werror > "$out/dsp.log" 2>&1; then cat "$out/dsp.log"; echo "FAIL: warnings in the sound engine"; fail=1; fi
# the device's compiler is freestanding: no size_t, no memmove, no stdio. Caught here (v0.3's build failed on one)
if grep -nE "\bsize_t\b|\bmemmove *\(|\b(printf|fprintf|sprintf|snprintf) *\(|#include <(stdio|stdlib)\.h>" app/app.c app/dsp/*.c app/dsp/*.h; then
    echo "FAIL: the app uses something the FM-1's compiler doesn't have"; fail=1
fi
# the device build's own source check (fm1-base tools/build.py mmio_check): no number that looks like a hardware
# register's address in the app's code (v0.4's build failed on a seed written 0x40000)
python3 - <<'PY' || fail=1
import re, sys, pathlib
sys.path.insert(0, "base/tools")
src = pathlib.Path("base/tools/build.py").read_text()
lit = re.search(r'MMIO_LIT = (re\.compile\(.*?\))\n', src, re.S).group(1)
MMIO_LIT = eval(lit)
bad = 0
for f in sorted(pathlib.Path("app").rglob("*.[ch]")):
    if "build" in f.parts: continue
    s = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), f.read_text(), flags=re.S)
    s = re.sub(r"//[^\n]*", "", s)
    for no, ln in enumerate(s.splitlines(), 1):
        m = MMIO_LIT.search(ln)
        if m: print(f"FAIL: {f}:{no}: {m.group(0)} looks like a hardware address to the FM-1's build"); bad = 1
sys.exit(bad)
PY
for f in app/dsp/drums.c app/dsp/groove.c app/dsp/engine.c app/dsp/fx.c; do
    clang -ffreestanding -nostdlibinc -fsyntax-only -Wno-everything -Ibase/include -Iapp/dsp -Iapp "$f" 2>"$out/free.log" || { cat "$out/free.log"; echo "FAIL: $f won't build freestanding"; fail=1; }
done
for t in app/tests/*.txt; do
    echo "== $t"
    log="$out/$(basename "$t" .txt).log"
    rc=0; app/build/host/expectation-reality_host "$t" "$out" > "$log" 2>&1 || rc=$?
    if [ "$rc" -gt 1 ]; then echo "FAIL: $t crashed (exit $rc)"; fail=1; fi
    cat "$log"
    if grep -q FAIL "$log"; then fail=1; fi
done
exit $fail
