#!/bin/sh
# g++ (mingw / Linux). OpenMP makes generation scale with cores.
#   sh build.sh            -> jp_lpr_image(.exe)
set -e
OUT=jp_lpr_image
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) OUT=jp_lpr_image.exe ;; esac
g++ -std=c++17 -O2 -fopenmp -Isrc -Ithird_party src/main.cpp -o "$OUT"
echo "built $OUT"
