# 01 观测手段：先能看见，再谈理解

内核不会主动告诉你它在干什么。所有手段都是在不同层次上「加钩子」，从便宜到贵排下来：

| 层次 | 手段 | 能看到 | 成本 | 限制 |
| --- | --- | --- | --- | --- |
| 应用自己 | `clock_gettime`、`getrusage` | 耗时、缺页数、上下文切换数 | 几乎为零 | 只有聚合值，看不到调用链 |
| 内核导出的文件 | `/proc`、`/sys`、`/sys/kernel/debug` | 结构体字段的现场快照 | 低 | 采样式，容易错过短事件 |
| ptrace | mini_strace、strace、gdb | 每次 syscall 的参数和返回值 | 中（每次停两次） | 只能看 syscall 边界，看不到内核内部 |
| ftrace | `function`、`function_graph`、tracepoint | 内核函数调用链和耗时 | 低（内核自带） | 需要 root，函数级粒度，开销随钩子数上升 |
| perf | `perf stat`、`perf record` | 硬件计数器、采样栈 | 中 | 需要 `perf_event_paranoid` 放开 |
| eBPF | bpftrace、bcc | 任意函数入参/返回值、聚合统计 | 低-中 | 需要 root、内核支持 BTF |
| QEMU + gdb | 断点、单步、watchpoint | 寄存器、内存、每一步执行 | 高（慢） | 只能在自己编译的内核里做 |

原则：能用 `/proc` 回答的问题不要上 gdb；能用 ftrace 回答的问题不要上 QEMU。gdb 留给「为什么走了这个分支」这种必须看寄存器和局部变量的问题。

## 1. 应用自己：最便宜的证据

`getrusage` 的 `ru_minflt/ru_majflt`、`ru_nvcsw/ru_nivcsw` 就是内核计数器，和 `/proc/<pid>/stat` 的第 10、12 字段同源。阶段一的实验大部分用这个：

```c
struct rusage a, b;
getrusage(RUSAGE_SELF, &a);
/* ...被测代码... */
getrusage(RUSAGE_SELF, &b);
printf("minflt +%ld, majflt +%ld, nvcsw +%ld\n",
       b.ru_minflt - a.ru_minflt, b.ru_majflt - a.ru_majflt,
       b.ru_nvcsw - a.ru_nvcsw);
```

它告诉你「发生了多少次」，不告诉你「在哪一行发生」。

## 2. /proc：结构体的现场

```bash
cat /proc/<pid>/status            # 人读版：State/VmRSS/Threads/ctxt_switches
cat /proc/<pid>/stat              # 机器版：固定字段顺序，脚本好解析
cat /proc/<pid>/statm             # size/resident/shared/text/lib/data
cat /proc/<pid>/smaps_rollup      # RSS/PSS/Shared_Clean/Private_Dirty 汇总
cat /proc/<pid>/sched             # se.sum_exec_runtime、nr_switches、policy、prio
cat /proc/<pid>/maps              # 每个 VMA 的区间、权限、offset、inode
cat /proc/sched_debug             # 每个 CPU 的 rq、cfs_rq、当前 se 列表
cat /proc/schedstat               # 调度统计（需要 CONFIG_SCHEDSTATS）
```

要点：`/proc` 文件不是磁盘文件，每次 read 都由 procfs 现场拼字符串。所以读它本身有成本，采样频率别太高。

## 3. ptrace：syscall 边界上的显微镜

仓库自带 `labs/01_syscall/02_mini_strace`，不用装 strace 也能用：

```bash
./build/01_syscall/02_mini_strace /bin/ls /          # 全量
./build/01_syscall/02_mini_strace -f 0,1,2,3 /bin/ls /   # 只看 read/write/open/close
```

装了 strace 之后，两个更有用的用法：

```bash
strace -f -tt -T -e trace=openat,read,write ./prog    # -T 显示每个 syscall 在内核里花了多久
strace -c ./prog                                      # 按调用号汇总次数和耗时
```

注意 `-T` 的耗时包含阻塞等待（比如 read 等数据），不等于内核处理时间。要看纯内核时间，用 ftrace 的函数耗时。

## 4. ftrace：内核自带的调用链追踪

开启 tracefs（大多数发行版已经有）：

```bash
sudo mount -t tracefs tracefs /sys/kernel/tracing 2>/dev/null || true
cd /sys/kernel/tracing
```

三个最常用的文件：`available_tracers`、`set_ftrace_filter`、`trace`。

只跟一个函数及其子调用（推荐，输出可读）：

```bash
echo function_graph > current_tracer
echo do_sys_openat2 > set_graph_function
echo 1 > tracing_on
./build/04_file/03_dentry_cache
echo 0 > tracing_on
cat trace | head -80
```

看某个 tracepoint（比函数钩子稳定，不依赖符号）：

```bash
echo 1 > events/sched/sched_switch/enable
cat trace_pipe
```

`sched_switch` 的输出就是「谁换成了谁、在哪个 CPU、prev_state 是什么」，和 gdb 里看的 `__schedule` 是同一份事实的两个视角。

清理：

```bash
echo nop > current_tracer
echo > set_graph_function
echo 0 > events/sched/sched_switch/enable
```

## 5. perf：采样与硬件计数器

```bash
sudo sysctl -w kernel.perf_event_paranoid=1     # 或 -1，视发行版
perf stat -e page-faults,minor-faults,major-faults,context-switches ./prog
perf record -g ./prog && perf report
```

WSL2 里 perf 经常不可用（微软内核没开完整 perf 支持），这不是你的问题：把这部分实验放到 QEMU guest 里做，或者用 ftrace 替代。

## 6. eBPF：不重启内核也能挂钩子

```bash
sudo apt install -y bpftrace
sudo bpftrace -e 'kprobe:vfs_read { @[comm] = count(); }'
sudo bpftrace -e 'kretprobe:do_sys_openat2 { @ns = hist(retval); }'
```

适合做「按进程统计」「按返回值分布」这种聚合问题。注意：kprobe 只能挂到有符号的函数上，静态函数可能被内联掉；这也是为什么很多实验要用 ftrace 的 `function_graph`（它按编译出的 mcount 钩子走，覆盖更全）。

## 7. gdb：只有它能回答「为什么走这个分支」

三种用法，按成本排：

1. 用户态 gdb：看 libc 怎么进 syscall。`gdb ./prog`，`catch syscall write`，`bt` 能看到 `__libc_write` 和栈上的参数。
2. QEMU + gdb 的内核调试：断在 `__schedule`、`handle_mm_fault`、`filemap_fault`，看局部变量和数据结构。见 `docs/06-qemu-gdb.md`。
3. crash / drgn 分析 vmcore：生产环境常用，本教程不展开。

## 工具选择的三个判断

1. 问题关于「次数/总量」→ 应用计数或 `/proc`。
2. 问题关于「调用链/某段代码耗时」→ ftrace。
3. 问题关于「为什么进这个分支/某个字段当时是什么值」→ gdb。

## 常见坑

- `/proc/<pid>/stat` 的 comm 字段可能含空格，解析时先找最后一个 `)`。
- ftrace 的 `set_ftrace_filter` 写错函数名不会报错，只是没输出；先用 `available_filter_functions | grep` 确认。
- `strace -T` 的时间包含睡眠，不要当成内核 CPU 时间。
- WSL2 的时钟和宿主共享，跨 VM 的绝对时间不可比；只比同机同一次运行内的相对值。
- 采样类工具（perf、`/proc` 轮询）会漏掉短事件，短事件用 tracepoint。
