# 10 因果链：一页纸

这一页是整个仓库的压缩版。左边是你在用户态做的一个动作，中间是它进内核后经过的函数与数据结构，右边是能看到它的命令和这台机器上实测出的数字。凡是标了「实测」的，都在 `labs/` 里有对应的程序和输出；标「书」的是还没自己跑过的部分。

## 一、动作 → 内核 → 观测 → 数字

| 用户态动作 | 系统调用 | 内核入口 | 关键数据结构 | 观测命令 | 实测 |
| --- | --- | --- | --- | --- | --- |
| `getpid()` | 无（vDSO） | 不进内核 | — | `strace -c` 看不到它 | 157 ns；vDSO `clock_gettime` 17.6 ns |
| 跑 `/bin/echo hi` | 111 次 | `do_sys_openat2` → `vfs_open` | `struct file` / `dentry` | `mini_strace -f ...` | openat 30 次、close 19 次 |
| `fork()` | `clone` | `kernel_clone` → `copy_process` | `task_struct` + 页表复制 | `/proc/<pid>/status` | 0.101 ms |
| 写 64MB 子进程内存 | 缺页 | `do_wp_page` → `wp_page_copy` | `anon_vma` / PTE | `/proc/<pid>/stat` minflt | 16384 次 COW，41.4 ms |
| `malloc(8MB)` + 写 | `brk`/`mmap` | `do_mmap` → `mm_populate` | `vm_area_struct` | `smaps`、`[heap]` 是否存在 | RSS 只涨 8188 KB，8MB 不在 [heap] |
| 读 32MB 文件 | `read` / `mmap` | `filemap_fault` → `page_cache_sync_readahead` | `address_space` / `struct page` | `minflt` vs `majflt` | 冷读 majflt 8192；热读 minflt 512 |
| 冷热读同一文件 | `read` | `filemap_read` | page cache | `/proc/meminfo Cached` | 1.24 GB/s vs 10.2 GB/s |
| `fsync` 循环 | `fsync` | `vfs_fsync` → 日志提交 | `journal_head` | `perf`/`strace -T` | 单条 1575 us，批量 58 us，收尾 23.8 us |
| 三个 nice 忙循环绑同核 | 无 | `pick_next_task_fair` | `sched_entity.se.load.weight` | `/proc/<pid>/stat` utime | 69.8 / 22.8 / 7.4% |
| 管道 ping-pong | `write`/`read` | `try_to_wake_up` → `ttwu_do_activate` | `rq` / `cfs_rq` | 自建计时 | p50 4.4 us、p99 10.7 us、max 702 us |

## 二、内核里直接看到的（QEMU + gdb，不是推算）

| 问题 | 证据 | 数字/符号 |
| --- | --- | --- |
| 这次调度是谁发起的 | `__schedule(sched_mode=...)` | -1 IDLE、0 NONE、1 PREEMPT、2 RTLOCK_WAIT |
| nice 在内核里是什么 | `fair.c` 插桩打印 `se.load.weight` | nice 0 = 1048576，nice 10 = 112640（=1024/110 × 1024） |
| 权重怎么变成份额 | 6 个进程 10 秒 utime | 权重比 9.31，CPU 时间比 9.30，总 19.7 CPU·秒 |
| 切换时到底换什么 | `__switch_to(prev_p, next_p)` | `thread.sp` 从 0xffffc9000001… 换到 …0003…；`fsbase` 全 0 |
| 新任务怎么开始跑 | `finish_task_switch` 的栈 | `ret_from_fork(fn=<kthreadd>)` ← `ret_from_fork_asm` |
| 为什么两个 comm 一样 | `prev_p->pid=1`、`next_p->pid=2` | comm 从 `init_task` 继承成 `swapper/0`，只有 pid 能区分 |

## 三、内存两本账（阶段四收口）

| 现象 | 内核侧 | 数字 |
| --- | --- | --- |
| 堆只有 8MB，RSS 141MB | `do_anonymous_page` vs `filemap_fault` | Anon 8328 KB，File 132808 KB |
| 同一文件再共享映射，RSS 又 +64MB | 一个 `struct page` 被两个 PTE 指到 | Pss 73999 KB 不变，Rss 141136 KB |
| COW 写 16MB | `wp_page_copy` | Anon +16384 KB，File −16384 KB，总量不变 |
| 读 256MB，进程 RSS 不动 | `filemap_read` 填 page cache | VmRSS 1.2→1.8 MB，Cached +256 MB（踢缓存后 −262100 KB） |
| 冷热读差 4 倍 | page cache 命中 | 2722 MB/s vs 11198 MB/s（本机）；runner 上 275.7 vs 14479 |

## 四、实测 vs 书

**真的跑出来、有数字的**：

- vDSO 不进内核（`mini_strace` 里看不到 `getpid`），157 ns vs 17.6 ns。
- fork 只复制页表，COW 缺页次数等于页数：64MB → 16384 次。
- 热读文件的 512 次 minflt 是 fault-around，不是 8192 次（对比冷读）。
- fsync 的成本由「等待日志提交的次数」决定，不是数据量决定。
- EEVDF 下 nice 仍然按 `load.weight` 折算份额，权重比和实测份额差 0.1 个百分点以内。
- `sched_mode` 四个取值各自对应哪条调用路径。
- 切换的真实动作是换 `thread.sp` + `fsbase`，新任务从 `ret_from_fork` 起跑。
- RSS 按 PTE 计、Pss 按 mapcount 均摊；`Rss - Anonymous` 就是 file/shmem 部分。

**只在文档里写了、没自己跑的**（下一步补）：

- EEVDF 的 eligibility / lag 到底怎么算（只验证了权重折算这一层）。
- `sched_domain` 的负载均衡（`trigger_load_balance`、`sched_migration_cost_ns`）没有实测。
- THP / `khugepaged` 的合并阈值与 `AnonHugePages` 计数。
- cgroup v2 的 `memory.stat` 回收行为（这台 vm 的 `/proc/sys` 只读，没造容器环境，`docs/09` 里那套容器账是照内核文档描述的）。
- ext4/jbd2 的提交路径（只测了 fsync 的时间成本，没在 `jbd2_journal_commit_transaction` 上断点）。
- JVM 虚拟线程 → carrier 线程 → `task_struct` 的对应关系（没有在这台机器上跑 Java）。

## 五、12 周对照

| 阶段 | 计划 | 状态 |
| --- | --- | --- |
| 一（1-4 周） | syscall / 进程 / 内存 / 文件四条线各跑出数字 | 完成，15 个程序 + 实测表 |
| 二（5-6 周） | ftrace/perf/bpftrace 换视角重看四条线 | 部分：文档写了方法，只有 strace/`/proc` 是本仓库内跑的 |
| 三（7-10 周） | 调度深挖：读源码 → 用户态测份额 → QEMU+gdb 跟 → 改内核验证 | 完成，含插桩与完整切换单步 |
| 四（11-12 周） | 一个真实问题闭环 + 一页因果链 + 复盘 | 完成：`docs/09` 的 RSS 闭环，这一页即复盘 |

## 六、下一步最值得补的三个

1. **cgroup v2 实测**：造一个 `memory.max=256M` 的 cgroup，里面跑 `rss_breakdown`，看 `memory.stat` 的 `anon`/`file` 怎么变、`file` 被回收时 `Pss` 与 `Rss` 怎么动。这一步能把 `docs/09` 里「照文档写」的部分变成实测。
2. **THP**：同一个 `rss_breakdown`，打开 `always` 与 `madvise`，对比 `AnonHugePages`、缺页次数、`smaps` 里的 `AnonHugePages` 字段与 COW 成本。
3. **jbd2 断点**：在 QEMU 里断 `jbd2_journal_commit_transaction`，解释 `fsync_cost` 里 1575 us 到底花在等谁。

判断标准始终是同一条：这个结论能不能用一条命令在另一台机器上复现。能，才算学会。
