#!/usr/bin/env bash
# Cross-compile the statusbar-avb Debian package for arm64 inside a
# native amd64 container — no qemu emulation. Produces a .deb stamped
# Architecture: arm64. Tests are not run (cross-built binaries cannot
# execute on the build host); for test runs use the emulated path
# (scripts/container-build.sh).
#
# Dependency .debs must already be cross-built in DEB_OUTPUT — run each
# dep's own container-build-cross.sh first.
set -euo pipefail

PKG="avb"
DEPS="core crypto audio"
DEBIAN_VERSION="${DEBIAN_VERSION:-trixie}"
TARGET_ARCH="${TARGET_ARCH:-arm64}"
ENGINE="${CONTAINER_ENGINE:-podman}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TREE_DIR="$(dirname "$SCRIPT_DIR")"
EXPORT_ROOT="$(dirname "$TREE_DIR")"
DEB_OUTPUT="${DEB_OUTPUT:-$EXPORT_ROOT/deb-output}"
HOST_BUILD_DIR="${HOST_BUILD_DIR:-$EXPORT_ROOT/build}"
HOST_TOOLS_DIR="$HOST_BUILD_DIR/avb/statusbar/tools"
IMAGE="localhost/statusbar-deb-builder-cross:$DEBIAN_VERSION-$TARGET_ARCH"

MOUNT_OPT=""
if [ "$(uname -s)" = "Linux" ]; then
  MOUNT_OPT=",z"
fi

mkdir -p "$DEB_OUTPUT"

if ! "$ENGINE" image exists "$IMAGE" >/dev/null 2>&1; then
  echo "=== building cross builder image $IMAGE ==="
  "$ENGINE" build -t "$IMAGE" \
    --platform linux/amd64 \
    --build-arg "DEBIAN_VERSION=$DEBIAN_VERSION" \
    --build-arg "TARGET_ARCH=$TARGET_ARCH" \
    -f "$TREE_DIR/Containerfile.cross" "$TREE_DIR"
fi

for d in $DEPS; do
  if ! ls "$DEB_OUTPUT"/statusbar-"$d"-dev_*_"$TARGET_ARCH".deb >/dev/null 2>&1; then
    echo "error: dependency statusbar-$d ($TARGET_ARCH) is not cross-built" >&2
    echo "       run each dep's container-build-cross.sh first" >&2
    exit 1
  fi
done

if [ ! -x "$HOST_TOOLS_DIR/statusbar-aem-entity-blob" ]; then
  echo "error: host amd64 tool $HOST_TOOLS_DIR/statusbar-aem-entity-blob is missing" >&2
  echo "       run ./local-build.sh first (top-level container-build-cross.sh does this automatically)" >&2
  exit 1
fi

# Mount the real lib dirs the host tool links against.
HOST_LIB_DIRS="$(ldd "$HOST_TOOLS_DIR/statusbar-aem-entity-blob" 2>/dev/null \
  | awk '/=>/ {print $3}' | xargs -r -I{} readlink -f {} | xargs -r -n1 dirname | sort -u)"
LIB_MOUNTS=()
for d in $HOST_LIB_DIRS; do
  case "$d" in
    /lib|/usr/lib|/usr/lib/x86_64-linux-gnu|/lib/x86_64-linux-gnu) ;;  # already present
    *) LIB_MOUNTS+=( -v "$d:$d:ro$MOUNT_OPT" ) ;;
  esac
done

echo "=== cross-building statusbar-$PKG .deb (target $TARGET_ARCH) ==="
"$ENGINE" run --rm \
  --platform linux/amd64 \
  -v "$TREE_DIR:/src:ro$MOUNT_OPT" \
  -v "$DEB_OUTPUT:/debs:rw$MOUNT_OPT" \
  -v "$HOST_TOOLS_DIR:/host-tools:ro$MOUNT_OPT" \
  "${LIB_MOUNTS[@]}" \
  -v "statusbar-deb-ccache-cross-$TARGET_ARCH:/root/.ccache" \
  -e "DEPS=$DEPS" \
  -e "PKG=$PKG" \
  -e "TARGET_ARCH=$TARGET_ARCH" \
  -e "HOST_LIB_DIRS=$HOST_LIB_DIRS" \
  "$IMAGE" bash -euo pipefail -c '
    debs=()
    for d in $DEPS; do
      debs+=(/debs/statusbar-"$d"_*_"$TARGET_ARCH".deb \
             /debs/statusbar-"$d"-dev_*_"$TARGET_ARCH".deb)
    done
    if [ "${#debs[@]}" -gt 0 ]; then
      apt-get update -qq
      apt-get install -y --no-install-recommends "${debs[@]}"
      # Restore clang-scan-deps if apt evicted clang-tools-19.
      if [ ! -e /usr/lib/llvm-19/bin/clang-scan-deps ] && [ -e /opt/cross-tools/clang-scan-deps ]; then
        cp /opt/cross-tools/clang-scan-deps /usr/lib/llvm-19/bin/clang-scan-deps
      fi
    fi
    # Host-arch tools on PATH for build-time add_custom_command rules.
    if [ -d /host-tools ]; then
      ln -sf /host-tools/statusbar-aem-entity-blob /usr/local/bin/aem_entity_blob_tool
      ln -sf /host-tools/statusbar-aem-entity-blob /usr/local/bin/statusbar-aem-entity-blob
    fi
    if [ -n "${HOST_LIB_DIRS:-}" ]; then
      for d in $HOST_LIB_DIRS; do
        [ -d "$d" ] && echo "$d"
      done > /etc/ld.so.conf.d/host-amd64.conf
      ldconfig
    fi
    cmake -Wno-dev -S /src -B /build -G Ninja \
      --toolchain /src/cmake/toolchain-clang-aarch64.cmake \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local \
      -DCMAKE_C_COMPILER_LAUNCHER=ccache \
      -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
      -DCPACK_DEBIAN_PACKAGE_ARCHITECTURE="$TARGET_ARCH"
    cmake --build /build
    ( cd /build && cpack -G DEB )
    rm -f /debs/statusbar-"$PKG"_*_"$TARGET_ARCH".deb \
          /debs/statusbar-"$PKG"-dev_*_"$TARGET_ARCH".deb
    cp -v /build/*.deb /debs/
  '

echo "=== statusbar-$PKG packages in $DEB_OUTPUT ==="
ls -1 "$DEB_OUTPUT"/statusbar-"$PKG"_*_"$TARGET_ARCH".deb \
      "$DEB_OUTPUT"/statusbar-"$PKG"-dev_*_"$TARGET_ARCH".deb 2>/dev/null || true
