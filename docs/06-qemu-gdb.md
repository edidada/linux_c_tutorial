# 06 QEMU + gdb：把内核放进显微镜

用户态实验能告诉你「发生了什么」，只有 gdb 能告诉你「为什么」。这一章的目标是：在自己的内核里，单步走完一次调度切换，并且能随时停下来检查任意数据结构。

## 1. 为什么要自己编译内核

发行版内核能用 `gdb` 连，但没有两个关键条件：源码版本必须完全对得上（否则行号和结构体偏移错位），以及 `CONFIG_DEBUG_INFO` + 关 KASLR。自己编译一份 6.12，成本 5-15 分钟，换来的是随意改代码、加 printk、加断点的自由。

## 2. 准备（`qemu/setup.sh` 做了什么）

```bash
sudo apt install -y qemu-system-x86 flex bison bc libelf-dev libssl-dev \
     cpio xz-utils gdb kmod build-essential
chmod +x setup.sh run.sh     # 从 Windows 侧 clone 常丢执行位，报 Permission denied 就是它
cd qemu
./setup.sh
```

脚本下载内核默认走阿里云镜像（`KERNEL_MIRROR` 可覆盖，失败自动回退 kernel.org）。

脚本步骤：

1. 下载 `linux-6.12.tar.xz` 到 `qemu/build/`，解压。
2. `make defconfig` 后用 `scripts/config` 打开调试项：

```bash
scripts/config --enable DEBUG_INFO
scripts/config --disable DEBUG_INFO_DWARF5
scripts/config --enable DEBUG_INFO_DWARF4
scripts/config --enable DEBUG_KERNEL
scripts/config --enable GDB_SCRIPTS
scripts/config --enable FRAME_POINTER
scripts/config --enable UNWINDER_FRAME_POINTER
scripts/config --disable UNWINDER_ORC
scripts/config --disable RANDOMIZE_BASE
scripts/config --disable DEBUG_INFO_BTF
scripts/config --enable VIRTIO_PCI
scripts/config --enable VIRTIO_BLK
```

为什么每一项：

- `DEBUG_INFO`：vmlinux 里带 DWARF，gdb 才能显示源码和变量。
- 关 DWARF5 用 DWARF4：老 gdb 对 DWARF5 支持差，容易读不出变量。
- `GDB_SCRIPTS`：提供 `lx-ps`、`lx-dmesg`、`lx-current` 等命令。
- `FRAME_POINTER` + 关 ORC：gdb 的 `bt` 需要 frame pointer 链；ORC 是内核自己的展开器，gdb 用不了。
- 关 `RANDOMIZE_BASE`：地址固定，断点不用换算（启动参数里也加了 `nokaslr`，双保险）。
- 关 `DEBUG_INFO_BTF`：省掉 `pahole` 依赖。
- `VIRTIO_*`：QEMU 的 virtio 磁盘/网卡。

3. `make -j$(nproc) bzImage`，产出 `arch/x86/boot/bzImage` 和带符号的 `vmlinux`。
4. 下载静态 busybox，做 initramfs：`/init` 挂 proc/sys/devtmpfs，然后 `exec sh`。
5. 打包成 `rootfs.cpio.gz`。

## 3. 启动

```bash
./run.sh            # 默认 -s -S：CPU 停在第一条指令，等 gdb 连
GDB_WAIT=0 ./run.sh # 不等，直接进 shell
```

`-s` 在 1234 端口开 gdbstub，`-S` 让 CPU 暂停。启动参数：

```
console=ttyS0 nokaslr loglevel=7 panic=-1
```

`console=ttyS0` 是因为用了 `-nographic`，串口就是你的终端。

## 4. 连 gdb

```bash
gdb -x gdb.cmd
```

`gdb.cmd` 的关键三行：

```
file $kdir/vmlinux
target remote :1234
hbreak start_kernel
```

注意：

- 必须用 `vmlinux`（带符号的 ELF），不能用 `bzImage`。
- `hbreak` 是硬件断点。内核代码段在启动早期是只读的，软件断点写不进去；后期虽然可以，但用 hbreak 更稳。
- 连上后 `info registers` 能看到 `rip=0x1000000` 左右（QEMU 的入口），`c` 之后才会走到 `start_kernel`。

常用动作：

```
c                 继续
bt                调用栈（需要 frame pointer）
lx-dmesg          打印内核 log buffer（比串口干净）
lx-ps             遍历 task 链表
p current->comm   老内核写法；6.x 用 p $lx_per_cpu("current_task")->comm
info locals       当前函数的局部变量
disassemble /m    源码和汇编对照
watch *(int *)0xffff...   硬件 watchpoint（数量有限，通常 4 个）
```

## 5. 跟一次调度切换（阶段三的核心练习）

1. `qemu/sched.gdb` 在 `__schedule`、`pick_next_task_fair`、`task_tick_fair`、`try_to_wake_up`、`finish_task_switch` 上打断点。
2. 等 guest 起 shell，敲一个命令（比如 `ls`）。每次等待输入都会因为 `wait4` 睡眠触发一次主动调度。
3. 在 `__schedule` 命中时：

```
bt                          # 看是谁调的：syscall 返回 / 中断 / 主动 schedule
p $lx_per_cpu("runqueues").nr_running
p $lx_per_cpu("runqueues").cfs.h_nr_running
p $lx_per_cpu("runqueues").curr->comm
```

4. `finish` 回到调用者，确认 `prev->__state` 已经是 `TASK_INTERRUPTIBLE`（说明是主动睡眠，不是被抢占）。
5. 在 `finish_task_switch` 停下，看 `prev` 和 `rq->curr` 分别是谁，`switch_to` 已经改完栈和寄存器。

要验证「抢占」而不是「主动让出」：起两个 CPU 密集型进程绑到同一个 CPU，在 `task_tick_fair` 命中，看 `check_preempt_tick` 返回真、`resched_curr` 被调用，然后在中断返回路径进 `__schedule`。

## 6. 改内核并验证

四个由浅入深的改法：

1. 加 printk：在 `task_tick_fair` 里打印 `curr->comm` 和 `se->vruntime`，重新 `make bzImage`，启动看串口。注意 printk 本身会改变时序，只用于确认路径，不用来测性能。
2. 加 tracepoint：`trace_sched_switch` 已有，自己加一个 `TRACE_EVENT` 更麻烦但更干净（不阻塞、可开关）。
3. 改参数：`/proc/sys/kernel/sched_latency_ns`、`sched_min_granularity_ns`、`sched_migration_cost_ns`，在 guest 里 `echo` 改，跑 `nice_effect` 看份额和延迟怎么变。
4. 改逻辑：比如把 `check_preempt_tick` 的判断条件放宽，观察 `wakeup_latency` 的 p99 变化。这一步必须备份原文件，改完用 `git diff` 看差异。

## 7. 常见问题

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| gdb 连上但 `bt` 是问号 | 没有 frame pointer 或 vmlinux 不对 | 检查 config 的 `UNWINDER_FRAME_POINTER`，确认 file 加载的是同一版本 vmlinux |
| 断点打不上 | 用了 `break` 而代码段只读 | 改 `hbreak` |
| 源码行号错位 | 源码树被改过但没重新编译，或路径变了 | `make` 后重启 QEMU；gdb 里 `set substitute-path` |
| 启动就 panic | initramfs 缺 `/init` 或没有执行权限 | 检查 `cpio` 打包和 `chmod +x` |
| 看不到串口输出 | 忘了 `console=ttyS0` 或没加 `-nographic` | 补参数 |
| `lx-ps` 未定义 | `vmlinux-gdb.py` 没加载 | `source $kdir/vmlinux-gdb.py`，或 `add-auto-load-safe-path` |
| gdb 单步极慢 | 每步都要和 QEMU 通信 | 正常；用断点+`finish` 代替逐条 `stepi` |
| 时间测量不可信 | QEMU 的 TSC 和宿主不同步 | 只比较 guest 内部的相对值 |

## 8. 验收

能在 10 分钟内完成这套动作，才算阶段三过关：

1. 启动 QEMU，gdb 停在 `start_kernel`。
2. 让 guest 起 shell，断在 `__schedule`，说出 prev/next 和触发原因。
3. 在 `handle_mm_fault` 断一次，说出是匿名页、COW 还是文件页。
4. 改一行内核代码，重新编译，在串口看到效果。

## 9. 不想编译内核的替代方案

可以用发行版的 debug 内核（Ubuntu 的 `linux-image-*-dbgsym` + ddebs 源），`gdb vmlinux` 直接连 `/proc/kcore` 或 QEMU。但源码版本必须和包完全一致，且不能改代码。做「单步看」够用，做「改了验证」不行。本教程选后者，因为改代码是重点。
