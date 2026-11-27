# linux_c_tutorial

用「用户态现象 → 内核动作」的顺序学 Linux 内核：先在 WSL 里把系统调用、进程、内存、文件四条线跑通，每个实验都能看到具体数字；再挑调度子系统，用 QEMU + gdb 单步跟进内核函数。

面向有编程基础、写过多年 Java 的人：不铺概念，直接给可运行的代码、可对照的内核函数、可复现的测量。JVM 里的堆、线程、mmap、page cache、fsync 这些经验可以直接迁移过来，差别在于 JVM 把这些包了一层，而这里要拆到 syscall 和 task_struct。

## 三条硬规则

1. 每个实验都要跑出数字。没有数字的「理解」不算理解。
2. 每个数字都要能对上内核里的一个函数或一个字段。对不上就回去查源码。
3. 光看书记不住，必须改代码或单步跟。第三阶段的所有结论都要在 gdb 里验证过。

## 目录

```
linux_c_tutorial/
├── Makefile                  一键编译全部用户态实验
├── labs/
│   ├── 01_syscall/           syscall 指令、ABI、vDSO、mini strace
│   ├── 02_process/           fork/COW、僵尸/孤儿、exec+pipe、/proc 字段对照
│   ├── 03_memory/            brk vs mmap、缺页、SHARED/PRIVATE 映射
│   ├── 04_file/              page cache、fsync、dentry cache、inotify
│   ├── 05_kernel/            内核模块：从内核里读 task_struct
│   ├── 06_sched/             nice 权重、唤醒延迟
│   ├── 07_sched_kernel/      给内核打插桩、gdb 跟 __schedule（QEMU 里做）
│   └── 08_rss_anatomy/       RSS 拆账：anon/file/shmem，page cache vs 进程 RSS
├── qemu/                     内核源码编译 + initramfs + gdb 脚本
├── scripts/                  编译、观测、快照
└── docs/
    ├── 00-roadmap.md         12 周计划与验收标准
    ├── 01-observation.md     观测手段总览（strace/ftrace/perf/gdb）
    ├── 02-syscall.md         一次系统调用的完整路径
    ├── 03-process.md         fork/exec/wait 在内核里做了什么
    ├── 04-memory.md          VMA、缺页、page cache、回收
    ├── 05-file.md            VFS 对象、路径解析、写回
    ├── 06-qemu-gdb.md        QEMU + gdb 环境与单步方法
    ├── 07-sched-deepdive.md  调度子系统：数据结构与关键函数
    ├── 08-reading-map.md     《LKD》《ULK》章节对照 + 源码地图
    ├── 09-rss-anatomy.md     收口：RSS 高但堆不大的闭环排查
    └── 10-causal-chain.md     一页纸因果链 + 实测/抄书复盘
```

## 环境

用户态实验在 WSL2 Ubuntu 里跑（本仓库实测环境：WSL2，kernel 6.18.33.2-microsoft-standard-WSL2，16 核，/tmp 在 ext4 上）：

```bash
sudo apt install -y build-essential gdb strace   # strace 可选，仓库自带 mini_strace
make -j"$(nproc)"
./build/01_syscall/01_raw_syscall
```

内核实验（第三阶段）需要额外装：

```bash
sudo apt install -y qemu-system-x86 flex bison bc libelf-dev libssl-dev cpio xz-utils kmod
cd qemu && ./setup.sh        # 下载 6.12 源码、编译带调试信息的内核、做 initramfs
```

第一次编译内核 5-15 分钟，之后增量改一行大约几十秒。

本仓库在 2 核 4GB 云主机上实测：全量编译 66 分钟，`bzImage` 12MB、`vmlinux` 414MB、initramfs 700KB，TCG 启动约 1.6 秒进 shell，gdb 断在 `start_kernel`、`lx-version` 正常。

## CI

两个 workflow，都在 `edidada/linux_c_tutorial` 上跑过并通过：

- `labs-ci`（`.github/workflows/edidada-ci.yml`）：`make` 编译 15 个用户态实验并逐个冒烟运行，约 12 秒。
- `kernel-build`（`.github/workflows/kernel-build.yml`）：手动触发或改动 `qemu/` 时运行，ubuntu-24.04 runner 上装 `cpio`/`libelf-dev` 等依赖、缓存内核 tarball、编译 6.12、校验三件产物、QEMU 无 KVM 启动并 grep 串口 banner。实测 11 分 49 秒，产物 12M/418M/684K，`Linux version 6.12.0` 后 1.58 秒进 `Run /init`。

## 已跑通的实测（本机一次采样，仅代表这台机器）

| 实验 | 关键数字 | 说明 |
| --- | --- | --- |
| raw_syscall | getpid 157 ns，vDSO clock_gettime 17.6 ns | syscall 指令 + 模式切换的成本；vDSO 完全不进内核 |
| mini_strace | `/bin/echo hi` 共 111 次系统调用，openat 30 次，close 19 次 | 启动一个程序的真实 syscall 账单 |
| fork_cow | fork 0.101 ms；64MB 逐页写 41.4 ms（16384 次 COW 缺页） | fork 只复制页表，复制页推迟到写时 |
| brk_mmap | 8MB malloc 不在 [heap]，RSS 写入后 +8188 KB，free 后归还 | 虚拟内存与物理页是两件事 |
| page_fault | 256MB 匿名写 minflt 65536；冷读 32MB 文件 majflt 8192；热读 minflt 512 | 4KB 一页；热读的 512 次是 fault-around 批量映射 |
| 匿名缺页（QEMU + gdb） | 4MB = 1024 页 → minflt 1024，`do_anonymous_page` 命中 1024 次；栈 VMA flags=0x118173 含 `VM_GROWSDOWN` | 缺页计数与内核函数命中数 1:1 对齐 |
| 只读缺页 vs 写缺页 | 只读同样 4MB：minflt 1027，RSS 只涨 31 页（约 124 KB） | minflt 是进缺页路径的次数，不是分配页框的次数 |
| page_cache | 冷读 1.24 GB/s，热读 10.2 GB/s | page cache 命中与读盘差 8 倍 |
| fsync_cost | 每条 fsync 1575 us，每 50 条 58 us，最后刷一次 23.8 us | 等待日志提交的次数决定成本 |
| nice_effect | nice 0/5/10 实测 69.8% / 22.8% / 7.4%，理论 69.7/22.8/7.5 | 必须绑到同一个 CPU，否则 nice 无效 |
| wakeup_latency | p50 往返 4.4 us，p99 10.7 us，max 702 us | 尾部延迟远大于中位数 |
| sched 内核侧（QEMU + gdb） | nice 0 权重 1048576、nice 10 权重 112640，实测 CPU 时间比 9.30（理论 9.31）；`__schedule` 的 `sched_mode` 区分 IDLE/NONE/PREEMPT | 改 `fair.c` 插桩后在 dmesg 里直接看到 weight 与 vruntime |
| rss_breakdown | anon 只占 8MB，RSS 从 1.6MB 涨到 141MB 全是 file-backed；共享映射让 RSS 再加 64MB 而 Pss 不变 | 「RSS 高但堆不大」= file/shmem 的账，不是堆 |
| cache_probe | 读 256MB，进程 VmRSS 1.2→1.8MB，全局 Cached +256MB；冷读 2723 MB/s vs 热读 11199 MB/s | page cache 是内核的账，不是进程的账 |

数字会随机器变，结论（量级关系）不会。自己跑出来的值才是你的。

## 推荐顺序

1. 读 `docs/00-roadmap.md`，按周推进，别跳阶段。
2. 每个实验先猜输出，再运行，再读源码验证；猜错的地方最有价值。
3. 四条线（syscall/process/memory/file）跑完后，做 `docs/07-sched-deepdive.md` 的六个实验。
4. 最后进 QEMU，在 `__schedule`、`pick_next_task_fair`、`handle_mm_fault` 上打断点，把前面看到的数字在 gdb 里对一遍。
5. 想直接看结论：`docs/10-causal-chain.md`（一页纸因果链，标了哪些是实测、哪些还没跑）。

## 书怎么配合

先《Linux 内核设计与实现》（LKD）建立框架，再《深入理解 Linux 内核》（ULK）补细节。两本书的代码都是 2.6 时代的，函数名和数据结构已经变了，读的时候把「机制」和「当前实现」分开：机制不变，符号要按 6.x 源码重新找。对照表见 `docs/08-reading-map.md`。

## 约定

- 代码一律 `-O2 -g -Wall -Wextra`，注释里写「内核对应函数」，不写空话。
- 所有实验的临时文件都放 `/tmp`，退出时自己清理。
- 内核实验不碰宿主机内核：只改 `qemu/build/linux-*/` 里的源码，用 QEMU 启动验证。
