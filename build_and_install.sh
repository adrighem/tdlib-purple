#!/bin/bash

set -euo pipefail
cd -P -- "$(dirname -- "$0")"

JOBS="$(nproc || echo 1)"

if [ "${1:-}" = "uninstall" ]; then
  if [ ! -d build ]; then
    echo "No build directory found. Run ./build_and_install.sh before uninstalling." >&2
    exit 1
  fi
  echo "Now calling sudo cmake --build build --target uninstall"
  sudo cmake --build build --target uninstall
  exit 0
fi

: "${TDLIB_PURPLE_API_ID_FILE:?set this to the owner-only API ID file path}"
: "${TDLIB_PURPLE_API_HASH_FILE:?set this to the owner-only API hash file path}"

git submodule update --init --recursive
pushd td
  rm -rf build
  mkdir build
  pushd build
    cmake -DCMAKE_BUILD_TYPE=Release ..
    make -j "${JOBS}"
    make install DESTDIR=destdir
  popd
popd

rm -rf build
mkdir build
pushd build
  cmake \
    -DTd_DIR="$(realpath ../td)"/build/destdir/usr/local/lib/cmake/Td/ \
    -DNoVoip=True \
    -DTDLIB_PURPLE_API_ID_FILE="$TDLIB_PURPLE_API_ID_FILE" \
    -DTDLIB_PURPLE_API_HASH_FILE="$TDLIB_PURPLE_API_HASH_FILE" \
    ..
  make -j "${JOBS}"
  echo "Now calling sudo cmake --install ."
  sudo cmake --install .
popd
