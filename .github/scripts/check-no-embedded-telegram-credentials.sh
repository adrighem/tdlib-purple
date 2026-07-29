#!/bin/sh

set -eu

for variable in API_ID API_HASH; do
    if ! grep -Eq "^set\\(${variable}[[:space:]]+\"\"[[:space:]]+CACHE[[:space:]]+STRING" CMakeLists.txt; then
        echo "CMakeLists.txt must not contain a default ${variable} value." >&2
        exit 1
    fi
done
