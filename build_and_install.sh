#!/bin/bash

set -e

JOBS="$(nproc || echo 1)"

git submodule update
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
