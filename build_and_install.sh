#!/bin/bash

set -e
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
  cmake -DTd_DIR="$(realpath ../td)"/build/destdir/usr/local/lib/cmake/Td/ -DNoVoip=True ..
  make -j "${JOBS}"
  echo "Now calling sudo make install"
  sudo make install
popd
