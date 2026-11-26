# 03 进程：fork 复制了什么，exec 换掉了什么

对应实验：`labs/02_process/fork_cow.c`、`zombie_orphan.c`、`exec_pipe.c`、`proc_inspect.c`

## 1. task_struct 与「进程」的边界

内核里没有单独的「进程」和「线程」结构，只有 `task_struct`。区别在于创建时共享了哪些东西：

| clone 标志 | 共享的资源 | 效果 |
| --- | --- | --- |
| `CLONE_VM` | mm_struct（同一套页表） | 线程 |
| `CLONE_FILES` | files_struct（同一张 fd 表） | 线程 |
| `CLONE_SIGHAND` | 信号处理函数表 | 线程 |
| `CLONE_THREAD` | 同一个线程组（tgid） | `getpid()` 返回 tgid |
| `CLONE_NEWPID` | 新的 pid namespace | 容器里的 pid 1 |

`fork()` 在 glibc 里最终是 `clone(SIGCHLD, ...)`，一个标志都不共享；`pthread_create` 是 `clone(CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|...)`。

`/proc/<pid>/status` 里的 `Tgid` 就是线程组 id：主线程 pid == tgid，其他线程 pid 不同但 tgid 相同。`/proc/<pid>/task/` 目录下是同一线程组的所有 task。

## 2. fork 的完整动作（kernel/fork.c）

```
fork() -> kernel_clone()
  copy_process()
    dup_task_struct()        复制 task_struct 和内核栈（几百字节 + 16KB 栈）
    copy_sighand/copy_signal 信号相关，引用计数或复制
    copy_mm()                复制 mm_struct；copy_page_range() 遍历页表，
                             把父进程的 PTE 全部改成只读、清掉写位（COW 的关键）
    copy_files()             fd 表引用计数 +1（不是复制文件对象）
    copy_fs()                cwd/root/umask 引用计数 +1
    copy_namespaces()        namespace 引用计数
    copy_thread()            构造子进程内核栈：ip = ret_from_fork，ax = 0
    sched_fork()             设置调度实体、把状态置 TASK_NEW，暂不入队
    wake_up_new_task()       入 runqueue，等调度器选中
  父进程返回子 pid；子进程从 ret_from_fork 返回 0
```

成本结构：`fork` 的耗时 ≈ 复制页表的成本，和内存占用大小相关，但和数据内容无关。

实测（`fork_cow`，64MB 地址空间）：

```
fork 耗时              : 0.101 ms
子进程逐页写入 64MB     : 41.424 ms（≈ 16384 次 COW 缺页）
写入前 RSS             : 1144 KB
写入后 RSS             : 66800 KB
```

每页第一次写 ≈ 2.5 us，包含：缺页异常 → `do_user_addr_fault` → `handle_mm_fault` → `do_wp_page` → `wp_page_copy`（分配新页、拷贝 4KB、改 PTE）。

这解释了两个常见现象：

- `fork` 一个占了几 GB 的 JVM 很快，因为没有复制堆内容，只复制了页表（页表本身也不小，几百万个 PTE）。
- 子进程「继承」了大内存，但 RSS 一开始不高；真正开始写才逐页涨。容器里 `fork` 后 RSS 突然翻倍，通常是子进程在写父进程留下的页。

## 3. exec：换映像，不换进程

```
execve() -> do_execveat_common()
  getname/路径解析 -> open_exec -> struct file
  prepare_binprm()        读文件头，填 bprm（argv/envp/解释器信息）
  search_binary_handler() 按魔数找 binfmt：ELF 走 load_elf_binary
    flush_old_exec()      丢掉旧 mm
    exec_mmap()           装新 mm，旧 mm 的页表被彻底替换
    setup_new_exec()      设置 personality、栈上限、清掉旧信号处理
    elf_map()             按 PT_LOAD 建立 VMA（代码段只读、数据段可写）
    set_brk()             设置堆起点
    load_elf_interp()     加载 /lib64/ld-linux-x86-64.so.2
    start_thread()        ip 指向解释器入口，sp 指向新用户栈
  返回用户态，从新映像第一条指令开始
```

关键事实：

- pid 不变，`task_struct` 不变，`mm_struct` 换了一个。
- fd 表默认保留（`exec_pipe` 用这个实现管道），带 `FD_CLOEXEC` 的会在换映像前被关掉。实测输出：

```
fd 0 -> pipe, 1 -> pipe, 2 -> 文件, 3 -> /proc/<pid>/fd, 5 -> /etc/hostname
带 O_CLOEXEC 打开的 6 不在表里
```

- `fork` + `exec` 之间的窗口里，子进程只有一份父进程的拷贝；这就是为什么 shell 要在 `fork` 后、`exec` 前做重定向（`dup2`），因为 `exec` 之后就没有机会了。

## 4. 退出与回收

```
exit() -> do_exit()
  exit_mm()        释放 mm（如果线程组里还有别人，只减引用）
  exit_files()     关闭所有 fd（fput 常常延迟到 task_work）
  exit_sem/exit_fs
  exit_notify()    把子进程 reparent 给 init/subreaper；给父进程发 SIGCHLD
  __schedule()     状态 EXIT_ZOMBIE，永远不再被调度
```

僵尸的本质：`task_struct` 还占着 pid 和内核栈，因为父进程还没读 `exit_code`。父进程 `wait4` → `do_wait` 找到 `EXIT_ZOMBIE` → 取退出码 → `release_task` 真正释放。

实测（`zombie_orphan`）：

```
子进程 pid=244469 状态=Z (zombie)
/proc/244469 仍然存在：yes
waitpid 得到退出码 42
/proc/244469 现在还存在吗：no
```

孤儿则是另一个方向：父进程先死，`forget_original_parent()` 把子进程挂到 init 或最近的 subreaper（`prctl(PR_SET_CHILD_SUBREAPER)`）。所以「孤儿进程被 init 收养」不是内核在 wait，而是 init 循环 wait 的结果。

## 5. /proc 字段对照

| /proc 字段 | 内核来源 |
| --- | --- |
| status.Name | `task_struct->comm`（15 字符，`prctl(PR_SET_NAME)` 可改） |
| status.State | `task_struct->__state`（R/S/D/T/Z/I） |
| status.Tgid/Pid | `task_struct->tgid/pid` |
| status.VmRSS | `mm->rss_stat` 汇总 |
| status.Threads | 线程组内 task 数 |
| status.voluntary/nonvoluntary_ctxt_switches | `task_struct->nvcsw/nivcsw` |
| stat 第 14/15 字段 | utime/stime，`USER_HZ=100`，1 tick=10ms |
| sched.se.sum_exec_runtime | 调度实体累计运行时间（纳秒） |
| sched.nr_switches | nvcsw+nivcsw |
| sched.prio | 动态优先级（CFS 下 = 120 + nice） |
| maps 每一行 | 一个 `vm_area_struct` |

实测（`proc_inspect`）：policy=0（SCHED_NORMAL）、prio=120、`se.sum_exec_runtime=0.87ms`、`nr_switches=14`。这些不是「进程的属性」，而是调度器和 mm 的字段被 procfs 拼出来的视图。

## 6. 信号与等待

- 信号投递发生在返回用户态之前：`syscall_exit_to_user_mode` 或中断返回路径调用 `get_signal()`，把待决信号变成用户的栈帧或 `SIG_DFL` 动作。
- `wait4` 除了等退出，还能拿 `rusage`：`ru_utime/ru_stime` 是子进程消耗的 CPU 时间，`ru_maxrss` 是峰值 RSS。做性能统计时这比 `/proc` 轮询可靠。
- `SIGCHLD` 默认行为是忽略，但父进程仍必须 wait，否则僵尸。

## 7. 自己动手

1. 把 `fork_cow` 改成 `fork` 后子进程只写一个字节，观察 RSS 只涨一页；再写 1000 页，算每页成本。和 THP 打开时的行为对比（`madvise(MADV_HUGEPAGE)`）。
2. 用 `clone` 手写一个线程（共享 VM/FILES/SIGHAND，不共享 CLONE_THREAD），对比 `pthread_create` 的 `/proc/<pid>/status` 里 Tgid 是否相同。
3. 在 QEMU 里断 `copy_process`，`bt` 看调用栈，`p current->mm->mm_users` 看引用计数怎么变。
4. 用 `perf trace -e sched:sched_process_fork` 或 ftrace 的 `sched_process_fork` tracepoint，观察一次真实 fork 的参数。

## 易错点

- `fork` 在子进程里返回 0，父进程返回 pid；判断写反是最常见的 bug。
- `exec` 成功后不返回；`exec` 失败才返回 -1，此时原映像还在，可以继续执行错误处理。
- 多线程进程里 `fork` 只复制调用线程，其他线程消失；子进程里只能调用 async-signal-safe 函数（`malloc` 可能死锁）。这是 JVM 里 `Runtime.exec` 用 posix_spawn 的原因。
- `waitpid(-1, ...)` 会回收任意子进程；如果用线程池/子进程池，容易「抢」别人的退出码。
