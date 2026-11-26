# 02 系统调用：从 libc 到内核函数

对应实验：`labs/01_syscall/01_raw_syscall.c`、`labs/01_syscall/02_mini_strace.c`

## 1. 用户态这边：一条指令

x86-64 的约定（不是 Linux 自己定的，是 ABI 定的）：

| 位置 | 作用 |
| --- | --- |
| rax | 系统调用号，返回时放返回值 |
| rdi, rsi, rdx, r10, r8, r9 | 第 1-6 个参数（注意第 4 个是 r10，不是 rcx） |
| rcx, r11 | 被 `syscall` 指令破坏（分别存返回地址和 rflags），不能用来传参 |
| 返回值 | 负数表示 `-errno`，范围 -1 到 -4095 |

`01_raw_syscall.c` 里那段内联汇编就是全部秘密：

```c
register long rax __asm__("rax") = nr;
register long rdi __asm__("rdi") = a1;
register long rsi __asm__("rsi") = a2;
register long rdx __asm__("rdx") = a3;
__asm__ volatile("syscall" : "+r"(rax) : "r"(rdi), "r"(rsi), "r"(rdx)
                 : "rcx", "r11", "memory");
```

跑出来的对比：

```
libc getpid()    = 244458
raw syscall 39   = 244458
raw open 不存在的文件 -> -2  (= -ENOENT)
libc open 同一个文件   -> -1, errno=2
```

最后两行说明 `errno` 不是内核写的。内核在 rax 里返回 -2，glibc 的 `open` 包装函数看到负数后把它取反写进 `errno`，自己返回 -1。这就是为什么自己写 syscall 时不能用 `errno`。

## 2. 内核入口：entry_SYSCALL_64

```
syscall 指令
  -> entry_SYSCALL_64            arch/x86/entry/entry_64.S
       swapgs                     换到内核 GS（per-cpu 基址）
       保存用户 rsp 到 percpu      cpu_current_top_of_stack
       构造 pt_regs                把用户寄存器压到内核栈上
       ...
  -> do_syscall_64               arch/x86/entry/common.c
       syscall_enter_from_user_mode()   kernel/entry/common.c
            context tracking（用户态/内核态记账）
            seccomp 过滤（不通过直接返回）
            ptrace 的 syscall-entry 停点（strace 就挂在这里）
            audit
       sys_call_table[nr]         arch/x86/entry/syscall_64.c
       syscall_exit_to_user_mode()
            处理信号、need_resched、task_work（比如 close 的延迟释放）
  -> sysret / iretq 返回用户态
```

三个容易忽略的点：

1. **`sys_call_table` 是生成的**。源表在 `arch/x86/entry/syscalls/syscall_64.tbl`，构建时生成 C 数组。所以查调用号看 `.tbl`，不要背。
2. **`SYSCALL_DEFINE3(write, ...)` 展开出三个函数**：`__x64_sys_write`（真正的入口，给表用）、`__se_sys_write`（做 32/64 位符号扩展）、`__do_sys_write`（函数体）。gdb 里断 `__x64_sys_write`，源码里看 `__do_sys_write`。
3. **`syscall_exit_to_user_mode` 不等于「马上回用户态」**。它可能在这里处理掉一个待决信号，或者因为 `TIF_NEED_RESCHED` 触发抢占——这就是为什么一次系统调用的返回可能顺便发生一次进程切换。

## 3. write 的具体路径

```
__x64_sys_write
  -> ksys_write                fs/read_write.c
       fdget_pos -> 从 current->files 里拿 struct file
  -> vfs_write
       file->f_op->write_iter == filemap_write_iter（常规文件）
  -> generic_perform_write     mm/filemap.c
       iov_iter 遍历用户缓冲区
       copy_page_from_iter_atomic   把数据拷进 page cache 页
       folio_mark_dirty              标记脏页
       按 dirty_ratio / dirty_background_ratio 触发 balance_dirty_pages
```

所以 `write` 返回时数据只在内存里。落盘由 writeback 线程或 `fsync` 负责，见 `docs/05-file.md`。

## 4. vDSO：不進内核的「系统调用」

`clock_gettime`、`gettimeofday`、`time`、`getcpu` 在 glibc 里优先走 vDSO：一段由内核在 `exec` 时映射到用户地址空间的可执行代码，加上一页 `vvar`（内核只读数据，里面是时间和时钟参数）。

实测（`01_raw_syscall` 同一台机器）：

| 调用 | 单次耗时 |
| --- | --- |
| 空循环基线 | 1.2 ns |
| raw syscall getpid | 157 ns |
| libc getpid | 169 ns |
| libc clock_gettime（vDSO） | 17.6 ns |
| raw syscall clock_gettime | 261 ns |
| libc gettimeofday（vDSO） | 17.8 ns |

结论：vDSO 版本连 `syscall` 指令都不执行，所以便宜一个数量级。这也是为什么高频打时间戳的应用（监控、trace、游戏循环）必须用 vDSO 版本，而不是自己写 `syscall(SYS_clock_gettime)`。

验证方法：

```bash
ldd ./build/01_syscall/01_raw_syscall | grep vdso    # 看不到，vDSO 由内核映射
cat /proc/self/maps | grep vdso                       # 能看到 [vdso] 和 [vvar]
```

## 5. 谁在 syscall 边界上挂钩子

| 机制 | 钩子位置 | 用途 |
| --- | --- | --- |
| seccomp | `syscall_enter_from_user_mode` | 容器/浏览器沙箱，按调用号直接拒绝 |
| ptrace | 同上，进/出各停一次 | strace、gdb、调试器 |
| audit | 同上 | 安全审计 |
| eBPF | tracepoint `sys_enter`/`sys_exit`、kprobe | 按进程统计、延迟分析 |

`02_mini_strace.c` 就是 ptrace 这一路的完整实现。它跑 `/bin/echo hi` 的结果：111 次系统调用，其中 `openat` 30 次、`close` 19 次、`read` 3 次、`write` 1 次。一个「打印两个字符」的程序，绝大部分开销在动态链接器加载和文件查找上。

## 6. 自己动手

1. 用 `catch syscall write` 在用户态 gdb 里断住，看 `bt` 里的 `__libc_write` 和寄存器参数。
2. 在 `02_mini_strace` 里加一个 `-T` 功能：记录进入和退出之间的 `clock_gettime` 差值，按调用号输出总耗时。你会看到 `read` 的「耗时」主要是在等数据，而不是内核 CPU 时间。
3. 用 ftrace 的 `function_graph` 跟一次 `do_sys_openat2`，数一数路径解析占了多少层。

## 易错点

- 参数寄存器第 4 个是 r10；写成 rcx 会得到随机参数，甚至 EFAULT。
- 内联汇编不写 `"rcx", "r11"` clobber，编译器可能把局部变量放进这两个寄存器，表现为偶发错误。
- 原始 syscall 的返回值是 `-errno`，不要和 glibc 的 `-1 + errno` 混用。
- `syscall` 不是函数调用，没有栈帧；在 gdb 里 `bt` 到 `entry_SYSCALL_64` 之后就是内核栈了。
