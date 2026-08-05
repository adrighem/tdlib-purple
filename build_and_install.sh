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

require_custom_credentials="${TDLIB_PURPLE_REQUIRE_CUSTOM_CREDENTIALS:-OFF}"
case "${require_custom_credentials^^}" in
  1|ON|TRUE|YES)
    require_custom_credentials=ON
    ;;
  0|OFF|FALSE|NO)
    require_custom_credentials=OFF
    ;;
  *)
    echo "TDLIB_PURPLE_REQUIRE_CUSTOM_CREDENTIALS must be ON or OFF." >&2
    exit 2
    ;;
esac

CREDENTIAL_FLAGS=(
  "-DTDLIB_PURPLE_REQUIRE_CUSTOM_CREDENTIALS:BOOL=$require_custom_credentials"
)

if [ "$require_custom_credentials" = ON ] || \
   [ -n "${TDLIB_PURPLE_API_ID_FILE:-}" ] || \
   [ -n "${TDLIB_PURPLE_API_HASH_FILE:-}" ]; then
  : "${TDLIB_PURPLE_API_ID_FILE:?set both credential file paths or neither}"
  : "${TDLIB_PURPLE_API_HASH_FILE:?set both credential file paths or neither}"
  CREDENTIAL_FLAGS+=(
    "-DTDLIB_PURPLE_API_ID_FILE=$TDLIB_PURPLE_API_ID_FILE"
    "-DTDLIB_PURPLE_API_HASH_FILE=$TDLIB_PURPLE_API_HASH_FILE"
  )
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
  cmake \
    -DTd_DIR="$(realpath ../td)"/build/destdir/usr/local/lib/cmake/Td/ \
    -DNoVoip=True \
    "${CREDENTIAL_FLAGS[@]}" \
    ..
  make -j "${JOBS}"
  echo "Now calling sudo cmake --install ."
  sudo cmake --install .
popd
