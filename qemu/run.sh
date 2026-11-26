#!/usr/bin/env bash
# run.sh —— 启动 QEMU guest。默认 -s -S：先把 CPU 停在第一条指令，等 gdb 连上。
#
# 用法：
#   ./run.sh            # 停住等 gdb（gdb -x gdb.cmd 里 continue 才开跑）
#   GDB_WAIT=0 ./run.sh # 不等 gdb，直接进 shell
#   MEM=4G SMP=4 ./run.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
BUILD="$HERE/build"
KVER=${KVER:-6.12}
KDIR="$BUILD/linux-$KVER"
MEM=${MEM:-2G}
SMP=${SMP:-2}
GDB_WAIT=${GDB_WAIT:-1}

[ -f "$KDIR/arch/x86/boot/bzImage" ] || { echo "先跑 ./setup.sh"; exit 1; }
[ -f "$BUILD/rootfs.cpio.gz" ] || { echo "先跑 ./setup.sh"; exit 1; }

GDB_ARGS=()
if [ "$GDB_WAIT" = "1" ]; then
    GDB_ARGS=(-s -S)
fi

exec qemu-system-x86_64 \
    -M q35 \
    -m "$MEM" \
    -smp "$SMP" \
    -kernel "$KDIR/arch/x86/boot/bzImage" \
    -initrd "$BUILD/rootfs.cpio.gz" \
    -append "console=ttyS0 nokaslr loglevel=7 panic=-1" \
    -nographic \
    -no-reboot \
    "${GDB_ARGS[@]}"
