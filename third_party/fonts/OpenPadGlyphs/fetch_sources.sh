#!/bin/sh
# Fetch the one input of build_padglyphs.py: Baloo 2 (SIL OFL 1.1) from Google Fonts' repository, pinned and checked.
set -eu
cd "$(dirname "$0")"
mkdir -p sources
C=8e0cd05881f3ae5e933ba4161103005a3e841ce0   # google/fonts commit
U=https://raw.githubusercontent.com/google/fonts/$C/ofl/baloo2
curl -fsSL -o 'sources/Baloo2[wght].ttf' "$U/Baloo2%5Bwght%5D.ttf"
curl -fsSL -o sources/Baloo2-OFL.txt "$U/OFL.txt"
echo "d47a6852548059b1db49a1319d06d499d546c3fa2237cf9eee9c43c8abb025c2  sources/Baloo2[wght].ttf" | sha256sum -c -
echo "ad09b05dc8bc678c9daf7c4c5f7ef1f55e5726127f4330b2e98e40b9dffcb860  sources/Baloo2-OFL.txt" | sha256sum -c -
