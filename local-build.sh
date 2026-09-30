#! /bin/bash
# Standalone build helper for statusbar-avb.
#
# Locates each statusbar dependency's standalone build dir and threads
# the right -Dstatusbar-<dep>_DIR flags through to cmake. Extra args
# are forwarded to `cmake` configure:
#
#   ./local-build.sh                  # configure + build + ctest
#   ./local-build.sh -DENABLE_ASAN=ON # forward cmake args
#
# Dependency resolution per dep (first match wins):
#   1. $STATUSBAR_<DEP>_DIR  (env var, path to its build or install dir)
#   2. ../statusbar-<dep>/build/   (standalone sibling checkouts)
#   3. ../<dep>/build/             (sibling repo layout)
#
# IDE users: the cmake invocation below is echoed by `set -x` when run.
# Copy the printed `-Dstatusbar-<dep>_DIR=...` flags into your IDE's
# CMake configuration. The toolchain file is required; the dep flags
# are required for any statusbar dep that find_package() can't locate
# on its own.

set -e

STATUSBAR_DEPS=(core crypto audio)

here="$(cd "$(dirname "$0")" && pwd)"
parent="$(dirname "$here")"

dep_flags=()
missing=()

for dep in "${STATUSBAR_DEPS[@]}"; do
    # Skip auto-detection when the user passed -Dstatusbar-<dep>_DIR=... explicitly.
    user_override=0
    for arg in "$@"; do
        case "$arg" in
            -Dstatusbar-${dep}_DIR=*) user_override=1; break ;;
        esac
    done
    if [ "$user_override" -eq 1 ]; then
        continue
    fi

    env_var="STATUSBAR_$(echo "$dep" | tr '[:lower:]' '[:upper:]')_DIR"
    candidate="${!env_var:-}"
    if [ -z "$candidate" ]; then
        for path in "${parent}/statusbar-${dep}/build" "${parent}/${dep}/build"; do
            if [ -f "${path}/statusbar-${dep}Config.cmake" ]; then
                candidate="$path"
                break
            fi
        done
    fi
    if [ -z "$candidate" ]; then
        missing+=("$dep")
    else
        dep_flags+=("-Dstatusbar-${dep}_DIR=${candidate}")
    fi
done

if [ ${#missing[@]} -gt 0 ]; then
    {
        echo "Error: cannot locate the following statusbar dependency build dirs:"
        for dep in "${missing[@]}"; do
            env_var="STATUSBAR_$(echo "$dep" | tr '[:lower:]' '[:upper:]')_DIR"
            echo "  ${dep}: tried \$${env_var}, ${parent}/statusbar-${dep}/build/, ${parent}/${dep}/build/"
        done
        echo
        echo "Build the missing dependency first (each has its own local-build.sh),"
        echo "or set the env var to point at an existing build or install dir."
    } >&2
    exit 1
fi

# The toolchains live in statusbar-core (the single source of truth for the
# shared cmake files): prefer a sibling core checkout (the umbrella layout),
# else the installed core-dev package.
if [ -f ../core/cmake/toolchain-clang.cmake ]; then
    STATUSBAR_TOOLCHAIN_FILE=../core/cmake/toolchain-clang.cmake
else
    STATUSBAR_TOOLCHAIN_FILE=/usr/local/lib/cmake/statusbar-core/toolchain-clang.cmake
fi

set -x
cmake -S . -B build -G Ninja \
    --toolchain "$STATUSBAR_TOOLCHAIN_FILE" \
    "${dep_flags[@]}" \
    "$@"
cmake --build build
ctest --test-dir build
