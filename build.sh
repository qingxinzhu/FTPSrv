#!/bin/sh
# cross-compile FTPSrv for Windows x64 from Linux (mingw-w64)
#   build.sh          -> both the GUI and the console build
#   build.sh console  -> console only
#   build.sh gui      -> GUI only
set -e
cd "$(dirname "$0")"
CXX="${CXX:-x86_64-w64-mingw32-g++}"
WINDRES="${WINDRES:-x86_64-w64-mingw32-windres}"
WANT="${1:-both}"
mkdir -p bin
echo "compiler: $CXX"

# --- program icon + version info (icon.rc -> icon.res) -----------------------
# If windres is missing we simply build without the icon instead of failing.
RES=""
if command -v "$WINDRES" >/dev/null 2>&1; then
    if [ ! -f icon.res ] || [ icon.rc -nt icon.res ] || [ icon.ico -nt icon.res ]; then
        echo "--- resources (icon + version) ---"
        "$WINDRES" -i icon.rc -O coff -o icon.res
    fi
    RES="icon.res"
else
    echo "note: $WINDRES not found - building without the program icon"
fi

if [ "$WANT" = "both" ] || [ "$WANT" = "console" ]; then
    echo "--- console build ---"
    "$CXX" -O2 -std=c++17 -static -Wall -o bin/ftpsrv.exe ftpsrv.cpp $RES -lws2_32
fi

if [ "$WANT" = "both" ] || [ "$WANT" = "gui" ]; then
    echo "--- GUI build ---"
    "$CXX" -O2 -std=c++17 -static -Wall -mwindows -o bin/ftpsrv-gui.exe ftpsrv_gui.cpp $RES \
        -lws2_32 -lshell32 -lole32 -luuid -lcomdlg32
fi
cp -f bin/ftpsrv*.exe . 2>/dev/null || true
ls -l bin/ftpsrv*.exe
echo "OK -> bin/ (and copied next to the sources)"
