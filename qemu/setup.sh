#!/usr/bin/env bash
# setup.sh —— 准备 QEMU + gdb 的内核实验环境（在 WSL/Linux 里跑，不要用 root）
#
# 做三件事：
#   1. 下载并编译一个带调试信息的内核（默认 6.12 LTS），关掉 KASLR、打开 GDB scripts；
#   2. 用静态 busybox 做一个最小 initramfs；
#   3. 打印启动命令（run.sh）。
#
# 依赖（Ubuntu/Debian）：
#   sudo apt install -y build-essential flex bison bc libelf-dev libssl-dev \
#        cpio xz-utils gdb qemu-system-x86 kmod
# 如果 CONFIG_DEBUG_INFO_BTF 打开还需要 pahole，本脚本显式关掉它，省一次安装。
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
KVER=${KVER:-6.12}
BUILD="$HERE/build"
KDIR="$BUILD/linux-$KVER"
# 默认走阿里云镜像（国内通常比 kernel.org 快一个数量级），可用 KERNEL_MIRROR 覆盖
MIRROR=${KERNEL_MIRROR:-https://mirrors.aliyun.com/linux-kernel}
KURL="$MIRROR/v${KVER%%.*}.x/linux-$KVER.tar.xz"
FALLBACK="https://cdn.kernel.org/pub/linux/kernel/v${KVER%%.*}.x/linux-$KVER.tar.xz"

need() {
    for t in "$@"; do
        command -v "$t" >/dev/null 2>&1 || {
            echo "缺少命令: $t"
            echo "请先 apt install（见脚本头部注释）"
            exit 1
        }
    done
}
need gcc make flex bison bc cpio xz curl
command -v qemu-system-x86_64 >/dev/null 2>&1 || echo "警告: 未找到 qemu-system-x86_64"

# objtool 和签名需要 libelf / openssl 头文件，缺了会在编译到一半才报错
missing=0
[ -f /usr/include/libelf.h ] || { echo "缺 libelf 头文件: sudo apt install -y libelf-dev"; missing=1; }
[ -f /usr/include/openssl/ssl.h ] || { echo "缺 openssl 头文件: sudo apt install -y libssl-dev"; missing=1; }
[ "$missing" = 0 ] || { echo "装完依赖再跑本脚本"; exit 1; }

mkdir -p "$BUILD"

if [ ! -d "$KDIR" ]; then
    if [ ! -f "$BUILD/linux-$KVER.tar.xz" ]; then
        echo "下载 $KURL"
        curl -L --retry 3 --fail -o "$BUILD/linux-$KVER.tar.xz" "$KURL" || {
            echo "镜像下载失败，回退 kernel.org"
            rm -f "$BUILD/linux-$KVER.tar.xz"
            curl -L --retry 3 -o "$BUILD/linux-$KVER.tar.xz" "$FALLBACK"
        }
    fi
    echo "解压..."
    tar -xf "$BUILD/linux-$KVER.tar.xz" -C "$BUILD"
fi

cd "$KDIR"
if [ ! -f .config ]; then
    echo "生成 defconfig + 调参..."
    make defconfig
    # 调试必须项
    scripts/config --enable DEBUG_INFO
    scripts/config --disable DEBUG_INFO_DWARF5
    scripts/config --enable DEBUG_INFO_DWARF4
    scripts/config --enable DEBUG_KERNEL
    scripts/config --enable GDB_SCRIPTS
    scripts/config --enable FRAME_POINTER
    scripts/config --enable UNWINDER_FRAME_POINTER
    scripts/config --disable UNWINDER_ORC
    scripts/config --disable RANDOMIZE_BASE          # nokaslr 的替代，地址稳定
    scripts/config --disable DEBUG_INFO_BTF          # 免装 pahole
    scripts/config --enable BLK_DEV_INITRD
    scripts/config --enable VIRTIO_PCI
    scripts/config --enable VIRTIO_BLK
    scripts/config --enable VIRTIO_NET
    scripts/config --enable NET_9P
    scripts/config --enable 9P_FS
    scripts/config --enable DEVTMPFS
    scripts/config --enable DEVTMPFS_MOUNT
    scripts/config --enable SERIAL_8250
    scripts/config --enable SERIAL_8250_CONSOLE
    scripts/config --enable MAGIC_SYSRQ
    scripts/config --enable MODULES
    scripts/config --enable MODULE_UNLOAD
    scripts/config --disable MODULE_SIG
    make olddefconfig
fi

echo "编译内核（$(nproc) 并发，首次约 5-15 分钟）..."
make -j"$(nproc)" bzImage
# gdb 辅助脚本（lx-ps/lx-dmesg）需要这一步生成的符号常量与软链接，只 build bzImage 不会跑它
make -j"$(nproc)" scripts_gdb

# ---------- initramfs ----------
ROOTFS="$BUILD/rootfs"
BB="$BUILD/busybox"
if [ ! -x "$BB" ]; then
    echo "下载静态 busybox..."
    curl -L --retry 3 -o "$BB" \
        "https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox" || {
        echo "下载失败，可改用: sudo apt install -y busybox-static && cp /bin/busybox $BB"
        exit 1
    }
    chmod +x "$BB"
fi

rm -rf "$ROOTFS"
mkdir -p "$ROOTFS"/{bin,dev,proc,sys,etc,tmp,root,lib/modules}
cp "$BB" "$ROOTFS/bin/busybox"
for c in sh ls cat mount umount echo dmesg insmod rmmod lsmod ps top sleep mkdir rm chmod \
         ifconfig ip free dd grep sed head tail vi wc uname; do
    ln -sf /bin/busybox "$ROOTFS/bin/$c"
done

cat > "$ROOTFS/init" <<'EOF'
#!/bin/sh
mount -t proc  none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev 2>/dev/null || true
echo
echo "=== linux_c_tutorial guest ==="
echo "kernel: $(uname -a)"
echo "cmdline: $(cat /proc/cmdline)"
echo "（这个 shell 是 busybox，guest 里没有 glibc 用户态实验，那些在 WSL 里做）"
echo
exec sh
EOF
chmod +x "$ROOTFS/init"

echo "打包 initramfs..."
( cd "$ROOTFS" && find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -9 ) \
    > "$BUILD/rootfs.cpio.gz"

echo
echo "完成："
echo "  内核      $KDIR/arch/x86/boot/bzImage"
echo "  vmlinux   $KDIR/vmlinux"
echo "  initramfs $BUILD/rootfs.cpio.gz"
echo "启动：$HERE/run.sh          （带 -s -S，等 gdb 连）"
echo "调试：gdb -x $HERE/gdb.cmd"
