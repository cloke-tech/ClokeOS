#!/bin/bash
# Startet die Desktop-VM (arm64, Image aus $CLOKEOS_WORK) und Logic Pro darin mit den Spurenhilfen; gibt die Meldungen aus (docs/logic-launch.md)
#   CLOKEOS_WORK=<Ordner> macapps/logicrun.sh [Sekunden Wartezeit nach dem Start, Standard 45]
set -euo pipefail
export CLOKEOS_ARCH=arm64 CLOKEOS_FLAVOR=desktop
cd "$(dirname "$0")/.."
source config.sh
"./vmctl.py" quit >/dev/null 2>&1 || true; pkill -f "qemu.*$CLOKE_IMG" 2>/dev/null || true; sleep 2
nohup ./run.sh --headless > "$WORK/run.log" 2>&1 &
sleep 50; ./vmctl.py key ret >/dev/null 2>&1; sleep 15
./vmctl.py send 'cd /tmp; (DISPLAY=:0 '"${LOGIC_ENV:-}"' DYLD_FRAMEWORK_PATH=/Library/ClokeOS/MacFrameworks DYLD_LIBRARY_PATH=/Library/ClokeOS/MacLibs CLOKE_AWAKE_APP=1 CLOKE_NO_UNTITLED=1 CLOKE_TRACE=1 CLOKE_LENIENT=1 CLOKE_TRACE_OSLOG=1 CRASHPC_PEEK=f2698 DYLD_INSERT_LIBRARIES=/usr/lib/libclokeos-availfix.dylib:/usr/lib/libcrashpc.dylib:/usr/lib/libclogictrace.dylib "/Applications/Logic Pro.app/Contents/MacOS/Logic Pro" > /tmp/logic.out 2>&1 &) ; sleep '"${1:-45}"'; wc -c /tmp/logic.out; grep -v "NSNull\|^  bt\|lenient" /tmp/logic.out | head -24 | cut -c1-240' 150 | tail -26
