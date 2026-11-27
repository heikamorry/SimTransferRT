#!/usr/bin/env bash
# Build a Pi installation package on Debian amd64. The guest owns its dpkg database.
set -euo pipefail
project=$(cd -- "$(dirname -- "$0")/.." && pwd)
base=/var/cache/simtransferrt-arm64
guest="$base/trixie"
mode=${1:-release}

if [[ "$mode" != --guest ]]; then
    [[ $(dpkg --print-architecture) == amd64 ]] || { echo "Use linux-release.sh on a native Pi."; exit 2; }
    case "$mode" in release|package) ;; *) echo "Usage: bash scripts/linux-arm64-package.sh [release|package]" >&2; exit 2 ;; esac
    sudo apt-get install -y debootstrap qemu-user qemu-user-binfmt g++-aarch64-linux-gnu
    cache=${SIM_LINUX_CACHE:-"${XDG_CACHE_HOME:-$HOME/.cache}/SimTransferRT"}
    # All bind mounts disappear when this private mount namespace exits.
    sudo unshare --mount --propagation private --fork /bin/bash "$project/scripts/linux-arm64-package.sh" \
        --guest "$mode" "$cache" "${CMAKE_BUILD_PARALLEL_LEVEL:-}"
    output="$project/build/linux-release-arm64"
    mkdir -p "$output/packages"
    cp -- "$guest"/work/build/packages/simtransferrt_*_arm64.deb{,.sha256} "$output/packages/"
    if [[ -f "$guest/work/build/Testing/Temporary/LastTest.log" ]]; then
        cp -- "$guest/work/build/Testing/Temporary/LastTest.log" "$output/arm64-ctest.log"
    fi
    (cd "$output/packages" && sha256sum -c ./*_arm64.deb.sha256)
    echo "ARM64 package directory: $output/packages"
    exit
fi

mode=$2
cache=$3
jobs=$4
install -d -m755 "$base"
exec 9>"$base/build.lock"
flock 9
if ! grep -qs '^enabled' /proc/sys/fs/binfmt_misc/qemu-aarch64; then
    if [[ ! -e /proc/sys/fs/binfmt_misc/register ]]; then
        mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc
    fi
    rule=$(dpkg-query -L qemu-user-binfmt | awk '/\/qemu-aarch64\.conf$/ {print; exit}')
    [[ -n "$rule" ]] || { echo "Debian qemu-aarch64 binfmt configuration missing" >&2; exit 1; }
    /usr/lib/systemd/systemd-binfmt "$rule"
fi
if [[ ! -d "$guest" ]]; then
    stage=$(mktemp -d "$base/bootstrap.XXXXXXXX")
    debootstrap --arch=arm64 --variant=minbase --include=ca-certificates \
        trixie "$stage" https://deb.debian.org/debian
    chmod 755 "$stage"
    mv -- "$stage" "$guest"
fi
[[ $(chroot "$guest" dpkg --print-architecture) == arm64 ]]
mount -t proc proc "$guest/proc"
mount --rbind /dev "$guest/dev"
mount --bind /sys "$guest/sys"
mount -o remount,bind,ro "$guest/sys"
install -d "$guest/project"
bind_readonly() {
    mkdir -p "$2"
    mount --bind "$1" "$2"
    mount -o remount,bind,ro "$2"
}
bind_readonly "$project" "$guest/project"
# Bash reads long scripts incrementally. Snapshot the recipe so edits to the
# checkout during an SDK build cannot change the running shell's input.
recipe=/opt/simtransfer-build/linux-release.sh
install -Dm755 "$project/scripts/linux-release.sh" "$guest$recipe"
# No services need starting when installing packages in a build root.
printf '#!/bin/sh\nexit 101\n' > "$guest/usr/sbin/policy-rc.d"
chmod 755 "$guest/usr/sbin/policy-rc.d"
chroot "$guest" /usr/bin/env DEBIAN_FRONTEND=noninteractive SIM_PROJECT_ROOT=/project /bin/bash "$recipe" prerequisites
if ! chroot "$guest" id simbuild >/dev/null 2>&1; then
    chroot "$guest" useradd -m -s /bin/bash simbuild
fi
uid=$(chroot "$guest" id -u simbuild)
gid=$(chroot "$guest" id -g simbuild)
guest_cache=/home/simbuild/.cache/SimTransferRT
install -d -m755 -o "$uid" -g "$gid" "$guest/work/build" "$guest/home/simbuild/.cache" \
    "$guest$guest_cache" "$guest$guest_cache/sources"
for name in boost_1_88_0 hdf5-1.14.6 VTK-9.4.2; do
    if [[ -f "$cache/sources/$name/.sim-sha256" ]]; then
        bind_readonly "$cache/sources/$name" "$guest$guest_cache/sources/$name"
    fi
done

# Run target configuration, Qt tools, tests and packaging as ARM64 processes.
# GCC's host executables can generate the same AArch64 objects natively and save
# emulating thousands of compiler invocations. Keep their own linker/plugin together.
cross=/opt/sim-cross/bin
install -d "$guest$cross" "$guest/lib64"
bind_readonly /usr/lib/x86_64-linux-gnu "$guest/usr/lib/x86_64-linux-gnu"
bind_readonly /usr/lib/gcc-cross "$guest/usr/lib/gcc-cross"
bind_readonly /usr/aarch64-linux-gnu "$guest/usr/aarch64-linux-gnu"
if [[ ! -L "$guest/lib64/ld-linux-x86-64.so.2" && ! -f "$guest/lib64/ld-linux-x86-64.so.2" ]]; then
    ln -s ../usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 "$guest/lib64/ld-linux-x86-64.so.2"
fi
for tool in gcc g++ as ld ar ranlib nm objcopy objdump strip; do
    install -m755 "$(command -v "aarch64-linux-gnu-$tool")" "$guest$cross/$tool"
done
gcc_libexec=$(dirname "$(realpath "$(aarch64-linux-gnu-g++ -print-prog-name=cc1plus)")")
gcc_lib=$(dirname "$(aarch64-linux-gnu-gcc -print-libgcc-file-name)")
if [[ "$gcc_libexec" != /usr/lib/gcc-cross/* ]]; then
    bind_readonly "$gcc_libexec" "$guest$gcc_libexec"
fi
compile_env=("CC=$cross/gcc -B$cross/" "CXX=$cross/g++ -B$cross/"
             "COMPILER_PATH=$cross:$gcc_libexec:$gcc_lib")
run_guest=(chroot --userspec="$uid:$gid" "$guest" /usr/bin/env -i HOME=/home/simbuild USER=simbuild
           PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 SIM_PROJECT_ROOT=/project SIM_LINUX_BUILD_DIR=/work/build
           QT_XCB_GL_INTEGRATION=xcb_egl LIBGL_ALWAYS_SOFTWARE=1
           GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=safe.directory GIT_CONFIG_VALUE_0=/project)
if "${run_guest[@]}" "${compile_env[@]}" /bin/bash -c \
    'printf "#ifndef __aarch64__\n#error wrong target\n#endif\n#include <iostream>\nint main(){std::cout << 42;}\n" |
     $CXX -x c++ - -o /tmp/sim-compiler-check && [[ $(/tmp/sim-compiler-check) == 42 ]]'; then
    echo "Validated native GCC cross compiler and ARM64 executable"
else
    echo "Using Debian ARM64 GCC under QEMU"
    compile_env=()
fi
if [[ -z "$jobs" ]]; then
    jobs=$(awk '/^MemAvailable:/ {j=int($2/(1536*1024)); print (j<1?1:(j>6?6:j))}' /proc/meminfo)
fi
"${run_guest[@]}" "${compile_env[@]}" CMAKE_BUILD_PARALLEL_LEVEL="$jobs" \
    /bin/bash "$recipe" "$mode"
