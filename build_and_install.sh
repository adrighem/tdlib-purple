#!/bin/bash

set -e

JOBS="$(nproc || echo 1)"

rm -rf build
mkdir build
pushd build
  git clone https://github.com/tdlib/td.git
  pushd td
    git checkout 1fa2a372a88c26369dcac2ce476166531df74a77 # 1.8.34
    mkdir build
    pushd build
      cmake -DCMAKE_BUILD_TYPE=Release ..
      make -j "${JOBS}"
      make install DESTDIR=destdir
    popd
  popd
  cmake -DTd_DIR="$(realpath .)"/td/build/destdir/usr/local/lib/cmake/Td/ -DNoVoip=True ..
  make -j "${JOBS}"
  echo "Now calling sudo make install"
  sudo make install
popd
