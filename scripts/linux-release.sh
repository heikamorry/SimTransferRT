#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "${SIM_PROJECT_ROOT:-$(dirname -- "$0")/..}" && pwd)
cd "$root"
mode=${1:-release}
build=${SIM_LINUX_BUILD_DIR:-"$root/build/linux-release"}
jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-}
if [[ -z "$jobs" ]]; then
    jobs=2
    if [[ $(uname -m) == x86_64 ]]; then
        available_kib=$(awk '/^MemAvailable:/ { print $2 }' /proc/meminfo)
        cores=$(nproc)
        # Leave memory for the desktop while compiling VTK's large C++ units.
        jobs=$((available_kib / (1536 * 1024)))
        ((jobs >= 1)) || jobs=1
        ((jobs <= cores)) || jobs=$cores
        ((jobs <= 6)) || jobs=6
    fi
fi
package_release() (
    local package stage
    cd "$build"
    cpack --config CPackConfig.cmake -B packages
    cd packages
    package=$(find . -maxdepth 1 -name "simtransferrt_*_$(dpkg --print-architecture).deb" -printf '%T@ %f\n' |
        sort -nr | head -n 1 | cut -d' ' -f2-)
    [[ -n "$package" ]] || { echo "No native Debian package in $PWD" >&2; exit 1; }
    sha256sum -c "$package.sha256"
    stage=$(mktemp -d)
    trap 'rm -rf -- "$stage"' EXIT
    dpkg-deb -x "$package" "$stage"
    "$stage/usr/bin/SimRtProbe" --help >/dev/null
    "$stage/usr/bin/SimTransferCli" --help >/dev/null
    if [[ -n ${DISPLAY:-} || -n ${WAYLAND_DISPLAY:-} ]]; then
        "$stage/usr/bin/SimTransfer" --smoke-test
    else
        QT_QPA_PLATFORM=xcb xvfb-run -a -s '-screen 0 1280x800x24' "$stage/usr/bin/SimTransfer" --smoke-test
    fi
    echo "Validated package: $PWD/$package"
)
case "$mode" in
    prerequisites)
        apt=(apt-get)
        if ((EUID != 0)); then apt=(sudo apt-get); fi
        "${apt[@]}" update
        exec "${apt[@]}" install -y build-essential cmake ninja-build git curl ca-certificates bzip2 \
            python3 pkg-config zlib1g-dev qt6-base-dev qt6-base-dev-tools libqt6opengl6-dev \
            qt6-wayland qt6-qpa-plugins libglvnd-dev libgles-dev libx11-dev libxt-dev \
            libgl1-mesa-dri xvfb xauth dpkg-dev file fonts-noto-cjk
        ;;
    package) package_release; exit ;;
    soak)
        exec python3 -B tools/run_release_loopback.py --build "$build" --visible --seconds 600 \
            --ndi-handoff-guard-us 0
        ;;
    core|release) ;;
    *) echo "Usage: bash scripts/linux-release.sh {prerequisites|core|release|package|soak}" >&2; exit 2 ;;
esac
[[ $(uname -s) == Linux ]] || { echo "Run this script in Linux." >&2; exit 1; }
cache=${SIM_LINUX_CACHE:-"${XDG_CACHE_HOME:-$HOME/.cache}/SimTransferRT"}
mkdir -p "$cache"/{archives,sources}
# Serialise source extraction and SDK installation across builds owned by this user.
exec 9>"$cache/native-build.lock"
flock 9
stage=$(mktemp -d "$cache/stage.XXXXXXXX")
trap 'rm -rf -- "$stage"' EXIT

source_archive() {
    local name=$1 archive=$2 digest=$3 url=$4 bundled=${5:-}
    local dir="$cache/sources/$name" file="$cache/archives/$archive"
    if [[ -f "$dir/.sim-sha256" && $(<"$dir/.sim-sha256") == "$digest" ]]; then return; fi
    if [[ -e "$dir" ]]; then echo "Incomplete source tree: $dir" >&2; return 1; fi
    if [[ -n ${SIM_ARCHIVE_DIR:-} && -f "$SIM_ARCHIVE_DIR/$archive" ]]; then
        file="$SIM_ARCHIVE_DIR/$archive"
    elif [[ ! -f "$file" && -f "$bundled" ]]; then
        file=$bundled
    elif [[ ! -f "$file" ]]; then
        curl --fail --location --proto '=https' --proto-redir '=https' --retry 2 \
            --output "$stage/$archive" "$url"
        file="$stage/$archive"
    fi
    echo "Verifying $archive"
    if ! printf '%s  %s\n' "$digest" "$file" | sha256sum --check --status; then
        echo "SHA-256 mismatch: $file" >&2; return 1
    fi
    if [[ "$file" == "$stage/$archive" ]]; then
        mv -- "$file" "$cache/archives/$archive"
        file="$cache/archives/$archive"
    fi
    if [[ "$name" == boost_* ]]; then
        tar -xf "$file" -C "$stage" "$name/boost" "$name/LICENSE_1_0.txt"
    else
        tar -xf "$file" -C "$stage"
    fi
    printf '%s\n' "$digest" > "$stage/$name/.sim-sha256"
    mv -- "$stage/$name" "$dir"
}

source_archive boost_1_88_0 boost_1_88_0.tar.bz2 \
    46d9d2c06637b219270877c9e16155cbd015b6dc84349af064c088e9b5b12f7b \
    https://archives.boost.io/release/1.88.0/source/boost_1_88_0.tar.bz2
boost="$cache/sources/boost_1_88_0"
if [[ "$mode" == core ]]; then
    core_build="${build}-core"
    cmake -S . -B "$core_build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DSIM_BOOST_ROOT="$boost" -DSIM_WITH_ASIO=ON -DSIM_WITH_QT=OFF -DSIM_BUILD_TESTS=ON
    cmake --build "$core_build" --parallel "$jobs"
    ctest --test-dir "$core_build" --output-on-failure --no-tests=error
    exit
fi

source_archive hdf5-1.14.6 hdf5-1.14.6.tar.gz \
    e4defbac30f50d64e1556374aa49e574417c9e72c6b1de7a4ff88c4b1bea6e9b \
    https://github.com/HDFGroup/hdf5/releases/download/hdf5_1.14.6/hdf5-1.14.6.tar.gz
source_archive VTK-9.4.2 VTK-9.4.2.tar.gz \
    36c98e0da96bb12a30fe53708097aa9492e7b66d5c3b366e1c8dc251e2856a02 \
    https://vtk.org/files/release/9.4/VTK-9.4.2.tar.gz "$root/build/model-deps/official-9.4.2.tar.gz"

arch=$(uname -m)
gles=${SIM_VTK_GLES:-OFF}
if [[ "$arch" == aarch64 && -z ${SIM_VTK_GLES:-} ]]; then gles=ON; fi
case "$gles" in ON) vtk_x=OFF ;; OFF) vtk_x=ON ;; *) echo "SIM_VTK_GLES must be ON or OFF" >&2; exit 2 ;; esac
sdk_build="$cache/native/$arch-gles$gles-vtk9.4.2-hdf1.14.6"
if [[ -n ${CC:-}${CXX:-} ]]; then
    compiler_key=$(printf '%s\n' "${CC:-cc}" "${CXX:-c++}" | sha256sum | cut -c1-12)
    sdk_build="$sdk_build-$compiler_key"
fi
sdk="$sdk_build/install"
common=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$sdk" -DCMAKE_INSTALL_LIBDIR=lib
        -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON)
cmake -S "$cache/sources/hdf5-1.14.6" -B "$sdk_build/hdf5" "${common[@]}" \
    -DBUILD_STATIC_LIBS=ON -DBUILD_TESTING=OFF -DHDF5_BUILD_TOOLS=OFF -DHDF5_BUILD_EXAMPLES=OFF \
    -DHDF5_BUILD_HL_LIB=OFF -DHDF5_BUILD_CPP_LIB=OFF -DHDF5_ENABLE_Z_LIB_SUPPORT=ON \
    -DHDF5_ENABLE_SZIP_SUPPORT=OFF -DHDF5_INSTALL_CMAKE_DIR=lib/cmake/hdf5
cmake --build "$sdk_build/hdf5" --target install --parallel "$jobs"
install -Dm644 "$cache/sources/hdf5-1.14.6/COPYING" "$sdk/share/licenses/HDF5/COPYING"
cmake -S "$cache/sources/VTK-9.4.2" -B "$sdk_build/vtk" "${common[@]}" \
    -DVTK_BUILD_TESTING=OFF -DVTK_BUILD_EXAMPLES=OFF -DVTK_ENABLE_WRAPPING=OFF \
    -DVTK_GROUP_ENABLE_StandAlone=DONT_WANT -DVTK_GROUP_ENABLE_Rendering=DONT_WANT \
    -DVTK_GROUP_ENABLE_Imaging=DONT_WANT -DVTK_GROUP_ENABLE_MPI=NO -DVTK_GROUP_ENABLE_Qt=DONT_WANT \
    -DVTK_MODULE_ENABLE_VTK_GUISupportQt=YES -DVTK_MODULE_ENABLE_VTK_RenderingOpenGL2=YES \
    -DVTK_MODULE_ENABLE_VTK_RenderingAnnotation=YES -DVTK_MODULE_ENABLE_VTK_RenderingFreeType=YES \
    -DVTK_MODULE_ENABLE_VTK_InteractionStyle=YES -DVTK_MODULE_ENABLE_VTK_InteractionWidgets=YES \
    -DVTK_MODULE_ENABLE_VTK_IOXML=YES -DVTK_MODULE_ENABLE_VTK_IOLegacy=YES \
    -DVTK_MODULE_ENABLE_VTK_hdf5=NO -DVTK_SMP_IMPLEMENTATION_TYPE=Sequential -DVTK_QT_VERSION=6 \
    -DVTK_OPENGL_USE_GLES="$gles" -DVTK_USE_X="$vtk_x" -DVTK_OPENGL_HAS_EGL=ON
cmake --build "$sdk_build/vtk" --target install --parallel "$jobs"
cmake --preset linux-release -B "$build" -DCMAKE_PREFIX_PATH="$sdk" -DSIM_BOOST_ROOT="$boost"
cmake --build "$build" --parallel "$jobs" --target all SimAppMemoryTests
if [[ -n ${DISPLAY:-} || -n ${WAYLAND_DISPLAY:-} ]]; then
    ctest --test-dir "$build" --no-tests=error --output-on-failure
else
    QT_QPA_PLATFORM=xcb xvfb-run -a -s '-screen 0 1280x800x24' \
        ctest --test-dir "$build" --no-tests=error --output-on-failure
fi
package_release
