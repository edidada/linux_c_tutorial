# QEMU 实验环境

三个脚本：

| 文件 | 作用 |
| --- | --- |
| `setup.sh` | 下载 6.12 源码，生成带调试信息的内核配置，编译 `bzImage`/`vmlinux`，做 busybox initramfs |
| `run.sh` | 启动 guest，默认 `-s -S` 等 gdb 连接 |
| `gdb.cmd` / `sched.gdb` | gdb 脚本，断在 `start_kernel` 或调度器关键函数 |

## 第一次

```bash
sudo apt install -y qemu-system-x86 flex bison bc libelf-dev libssl-dev \
     cpio xz-utils gdb kmod build-essential
chmod +x setup.sh run.sh          # 从 Windows 侧 clone 过来常常丢了执行位
./setup.sh
```

内核 tarball 默认从阿里云镜像下载（约 12MB/s），可用 `KERNEL_MIRROR=...` 覆盖；
镜像失败会自动回退 kernel.org。`KVER=6.12` 可换版本。

预计 5-15 分钟（16 核；2 核云主机约 45 分钟）。产物：

```
build/linux-6.12/arch/x86/boot/bzImage   启动用
build/linux-6.12/vmlinux                 gdb 用（带符号）
build/rootfs.cpio.gz                     initramfs
```

## 启动与调试

```bash
# 终端 1
./run.sh

# 终端 2
gdb -x gdb.cmd       # 停在 start_kernel
# 或
gdb -x sched.gdb     # 断在 __schedule 等
```

guest 里只有一个 busybox shell，没有 glibc。用户态实验在 WSL 里跑，这里只做内核侧观察和改代码验证。

## 改内核的循环

```bash
cd build/linux-6.12
vim kernel/sched/fair.c
make -j"$(nproc)" bzImage     # 增量编译
cd ../..
./run.sh
```

改了配置相关的代码（Kconfig）才需要重新 `make olddefconfig`。

## 常用 guest 内命令

```sh
uname -a
cat /proc/cmdline
cat /proc/sched_debug | head -40
cat /proc/interrupts
dmesg | tail -30
```

## 注意

- 不要改宿主机的 `/boot` 或 grub，本环境完全在用户态 QEMU 里。
- `build/` 目录体积约 2-4GB，不要提交到 git（仓库 `.gitignore` 已排除）。
- 如果 `setup.sh` 下载内核失败，可以手动把 `linux-6.12.tar.xz` 放进 `build/` 再执行。
