#!/usr/bin/env bash
# Builds the command line tool of ViSQOL, which has no ready-made binaries, for eval/evaluate.py.
# Tested on Ubuntu with GCC 15. It takes up about 3 GB, and the build took about 15 minutes here.
# The folder must be on a Linux file system, because Bazel needs symlinks and executable bits.
#
# Usage: eval/build_visqol.sh [folder], by default ~/visqol-build
set -euo pipefail

ROOT="${1:-$HOME/visqol-build}"
VISQOL_COMMIT=38d0b0163e441047d4429bf07ad09e5b9031d02c
BAZEL_VERSION=5.3.2
BAZEL_FILE="bazel-$BAZEL_VERSION-linux-x86_64"
BAZEL_URL="https://github.com/bazelbuild/bazel/releases/download/$BAZEL_VERSION/$BAZEL_FILE"

mkdir -p "$ROOT"
cd "$ROOT"

if [ ! -x bazel ]; then
  curl -fsSL -o bazel.part "$BAZEL_URL"
  curl -fsSL -o bazel.sha256 "$BAZEL_URL.sha256"
  if [ "$(cut -d' ' -f1 bazel.sha256)" != "$(sha256sum bazel.part | cut -d' ' -f1)" ]; then
    echo "The checksum of Bazel does not match." >&2
    exit 1
  fi
  mv bazel.part bazel
  chmod +x bazel
fi

if [ ! -d visqol/.git ]; then
  git init --quiet visqol
  git -C visqol fetch --quiet --depth 1 https://github.com/google/visqol.git "$VISQOL_COMMIT"
  git -C visqol -c advice.detachedHead=false checkout --quiet FETCH_HEAD
fi

# The dependencies of ViSQOL are too old for GCC 15: it rejects a template that is never used, and
# some files rely on an include of <cstdint> that newer standard libraries no longer provide.
flags=()
for flag in -Wno-template-body -Wno-error -include cstdint; do
  flags+=("--cxxopt=$flag" "--host_cxxopt=$flag")
done

cd visqol
"$ROOT/bazel" --output_user_root="$ROOT/cache" build //:visqol -c opt --curses=no "${flags[@]}"

# ViSQOL's own conformance test expects 2.346868205375293 for this pair of files.
score=$(./bazel-bin/visqol \
  --reference_file testdata/conformance_testdata_subset/contrabassoon48_stereo.wav \
  --degraded_file testdata/conformance_testdata_subset/contrabassoon48_stereo_24kbps_aac.wav |
  sed -n 's/^MOS-LQO:[[:space:]]*//p')
if [ "$score" != "2.34687" ]; then
  echo "ViSQOL gave $score instead of the expected 2.34687." >&2
  exit 1
fi
echo "ViSQOL is ready in $ROOT/visqol and reaches its expected score of $score."
