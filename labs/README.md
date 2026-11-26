# 实验清单

所有实验都是独立的 `main`，用根目录 `make` 一次编完，产物在 `build/<目录>/<文件名>`。每个文件头部的注释写了「内核对应函数」，运行前的预测比运行结果更重要。

## 01_syscall

| 程序 | 做什么 | 关键观察 |
| --- | --- | --- |
| `01_raw_syscall` | 内联汇编执行 syscall，对比 libc 与 vDSO | 负数返回值 vs errno；vDSO 比真 syscall 快一个数量级 |
| `02_mini_strace` | ptrace 实现的最小 strace | 每次系统调用进/出各停一次；`/bin/echo` 有 111 次调用 |

```bash
./build/01_syscall/01_raw_syscall
./build/01_syscall/02_mini_strace -f 0,1,2,3,59 /bin/echo hi
```

## 02_process

| 程序 | 做什么 | 关键观察 |
| --- | --- | --- |
| `fork_cow` | fork 后逐页写 64MB | fork 0.1ms，COW 缺页 16384 次、41ms |
| `zombie_orphan` | 制造僵尸、制造孤儿 | 状态 Z，wait 后 /proc 消失；孤儿被 reparent |
| `exec_pipe` | fork+exec+管道+CLOEXEC | pid 不变；fd 跨 exec 保留，带 CLOEXEC 的被关掉 |
| `proc_inspect` | 打印 /proc 字段并与 task_struct 对照 | policy/prio/vruntime/nr_switches 的来源 |

## 03_memory

| 程序 | 做什么 | 关键观察 |
| --- | --- | --- |
| `01_brk_mmap` | 三个不同大小的 malloc | 小对象在 [heap]，8MB 走独立 mmap；RSS 只在写入后涨 |
| `02_page_fault` | 匿名写、文件冷读、文件热读 | minflt 65536；majflt 8192；热读 512（fault-around 16 页/次） |
| `03_mmap_shared` | SHARED 与 PRIVATE 对比 | SHARED 跨进程可见并落盘；PRIVATE 不传播、不落盘 |

## 04_file

| 程序 | 做什么 | 关键观察 |
| --- | --- | --- |
| `01_page_cache_read` | 128MB 文件的冷读/热读 | 冷 1.2GB/s，热 10GB/s，差 8 倍 |
| `02_fsync_cost` | 每条 fsync vs 批量 fsync | 1575us vs 58us vs 23.8us |
| `03_dentry_cache` | 2000 个文件的 stat | 命中、negative、失效三种情况都在微秒级 |
| `04_inotify` | 监听目录的增删改 | 事件序列对应 VFS 的 fsnotify 钩子 |

## 05_kernel

`hello_module` 需要内核头文件。宿主机 WSL 里通常没有完整头，建议在 QEMU guest 里编译（把源码树放进去，`KDIR` 指向 guest 的 `/lib/modules/$(uname -r)/build`）。

## 06_sched

| 程序 | 做什么 | 关键观察 |
| --- | --- | --- |
| `01_nice_effect` | 三个不同 nice 的忙循环，绑同一 CPU | 实测份额 69.8/22.8/7.4，等于权重比 |
| `02_wakeup_latency` | 同 CPU 管道 ping-pong 2 万次 | p50 单向 2.2us，p99 5.4us，max 351us |

## 运行建议

- 先用 `scripts/build_all.sh` 编译并冒烟。
- 内存/文件实验会产生几十到几百 MB 的 `/tmp` 文件，程序退出会自己删；中断（Ctrl-C）可能留下，手动 `rm /tmp/linux_c_tutorial_*`。
- 调度实验会跑满一个核几秒钟，笔记本上注意散热；`nice_effect` 必须绑核，否则结果无意义。
- 所有实验都不需要 root。需要 root 的只有内核模块和 QEMU 里的 insmod。
