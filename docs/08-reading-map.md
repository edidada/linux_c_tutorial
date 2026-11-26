# 08 书与源码对照：怎么读才不白读

## 1. 两本书的定位

| 书 | 适合 | 不适合 |
| --- | --- | --- |
| 《Linux 内核设计与实现》LKD 3e | 建立框架：有哪些子系统、彼此怎么配合、为什么这样设计 | 代码细节；例子是 2.6.34，函数名已过时 |
| 《深入理解 Linux 内核》ULK 3e | 补细节：数据结构字段、函数调用链、算法实现 | 当参考手册逐页啃；2.6.11 的代码和 6.x 差得远 |

建议用法：LKD 通读一遍（一到两周），ULK 只读你正在挖的那两章。两本书都别抄笔记里的函数名，改成「当前源码里的对应函数」，边读边在 `build/linux-6.12` 里 grep。

## 2. 章节对照

### LKD

| 章 | 主题 | 配合本仓库 |
| --- | --- | --- |
| 3 | 进程管理 | `labs/02_process`、`docs/03-process.md` |
| 4 | 进程调度 | `docs/07-sched-deepdive.md`、`labs/06_sched` |
| 5 | 系统调用 | `labs/01_syscall`、`docs/02-syscall.md` |
| 6 | 内核数据结构 | 读源码时的链表/红黑树/radix tree 基础 |
| 7-8 | 中断与下半部 | 理解为什么抢占点在中断返回处 |
| 9 | 内核同步 | 自旋锁/RCU 与 `rq->lock`、`mmap_lock` |
| 10 | 定时器与时间管理 | jiffies、tick、`scheduler_tick` |
| 11 | 内存管理 | `labs/03_memory`、`docs/04-memory.md` |
| 12 | 虚拟文件系统 | `labs/04_file`、`docs/05-file.md` |
| 13 | 块 I/O 层 | fsync 落盘的下一层 |
| 14 | 进程地址空间 | VMA、页表、COW |
| 15 | 页缓存与写回 | page cache、writeback、readahead |

第 16 章之后（模块、kobject、调试、可移植性、社区）按需读；调试那一章可以配合 `docs/06-qemu-gdb.md`。

### ULK

| 章 | 主题 | 说明 |
| --- | --- | --- |
| 2 | 内存寻址 | 分段/分页、页表层级；x86-64 下分段基本被扁平化，重点看分页 |
| 3 | 进程 | task_struct 字段、内核栈、do_fork 流程（现在叫 kernel_clone） |
| 4 | 中断与异常 | 缺页异常也是一种异常，对应 `do_user_addr_fault` |
| 5 | 内核同步 | 锁、RCU、per-cpu 变量 |
| 6 | 定时与度量 | jiffies、时钟中断 |
| 7 | 进程调度 | CFS 前身的 O(1) 调度器；看「机制」别抠函数名 |
| 8 | 内存管理 | buddy、slab、per-cpu 页帧缓存 |
| 9 | 进程地址空间 | VMA、缺页处理、堆栈 |
| 10 | 系统调用 | 入口、参数传递、`sys_call_table` |
| 12 | VFS | super_block/inode/dentry/file 四对象 |
| 15 | 页缓存 | address_space、readahead、writeback |
| 16 | 访问文件 | 路径解析、打开、读写 |
| 17 | 页框回收 | LRU、kswapd、OOM |

两本书的章节号在不同版本可能差一，按标题找。

## 3. 2.6 到 6.x 的主要差异（读旧书时必须知道）

| 旧书里的 | 6.x 里的 |
| --- | --- |
| `_do_fork` / `do_fork` | `kernel_clone` |
| `struct task_struct->state` | `__state`（`state` 宏还在，但字段改名过） |
| `current` 宏直接读 `current_task` | 仍是 per-cpu `current_task`，写法 `$lx_per_cpu("current_task")` |
| CFS 红黑树选最左 | EEVDF：vruntime + deadline + slice |
| `struct mm_struct->mmap_sem` | `mmap_lock`（读写锁） |
| `address_space->page_tree`（radix tree） | `i_pages`（xarray） |
| `do_generic_file_read` | `filemap_read` |
| `sys_open` | `__x64_sys_openat` + `do_sys_openat2` |
| `struct file->f_op->read/write` | `read_iter/write_iter`（iov_iter） |
| `jiffies` 直接算超时 | 大量改用 `ktime_t`/`timer_list` |
| `struct page` 到处传 | 大量改用 `struct folio`（6.x 的内存管理单位） |

这张表建议边读边补，遇到旧名字就在源码里 grep 新名字，记到自己的笔记里。

## 4. 源码地图（只列本教程用到的）

```
kernel/
  fork.c             kernel_clone / copy_process / copy_mm
  exit.c             do_exit / do_wait / release_task
  sched/
    core.c           __schedule / context_switch / try_to_wake_up / sched_fork
    fair.c           update_curr / check_preempt_tick / pick_next_task_fair
    rt.c             RT 队列
    deadline.c       DL 调度
    stats.c          /proc/schedstat
  entry/common.c     syscall_enter_from_user_mode / syscall_exit_to_user_mode
  time/tick-*.c      scheduler_tick 的调用者

arch/x86/
  entry/entry_64.S         entry_SYSCALL_64
  entry/common.c           do_syscall_64
  entry/syscalls/syscall_64.tbl  系统调用号表
  mm/fault.c               do_user_addr_fault
  mm/mmap.c                页表操作、COW 的 PTE 修改

mm/
  memory.c                 handle_mm_fault / do_anonymous_page / do_wp_page
  filemap.c                filemap_read / filemap_fault / readahead
  mmap.c                   do_mmap / mmap_region / do_brk_flags
  vmscan.c                 shrink_node / LRU
  page_alloc.c             __alloc_pages（buddy）

fs/
  open.c                   do_sys_openat2 / vfs_open
  namei.c                  link_path_walk / walk_component / do_last
  read_write.c             vfs_read / vfs_write
  file.c                   vfs_fsync_range
  exec.c                   do_execveat_common / load_elf_binary 在 binfmt_elf.c
  dcache.c                 dentry 哈希与 LRU
  inode.c                  inode 生命周期
  ext4/                    ext4_lookup / ext4_writepages
  jbd2/                    日志提交

include/linux/
  sched.h                  task_struct / sched_entity
  sched/sched.h            rq / cfs_rq / sched_class
  mm_types.h               vm_area_struct / mm_struct
  fs.h                     inode / file / address_space
  dcache.h                 dentry
```

## 5. 读代码的方法

1. **带着问题进**。不要「今天读 fair.c」，要「为什么 nice 10 只拿 7.4%」。问题越具体，读得越深。
2. **先看调用者**。`git grep -n "check_preempt_tick("` 找到谁调用，比从头读函数定义快十倍。
3. **看数据结构，不只看函数**。函数是动作，结构体是状态。把 `rq`、`cfs_rq`、`sched_entity` 的字段列出来，很多问题自然有答案。
4. **用 bootlin Elixir 交叉引用**。本地 grep 只能查当前版本；Elixir 能看跨版本和引用关系。
5. **一图胜千行**。每读完一个子系统画一张「谁调用谁、改了哪个字段」的图，比抄代码有用。
6. **改一行验证**。加 printk、改条件、加 tracepoint，重新编译进 QEMU，这是唯一能确认「你真的懂了」的方式。

## 6. 推荐的最小源码阅读清单（按阶段）

阶段一（每条线读 3-5 个函数就够）：

- `__x64_sys_write` → `ksys_write` → `vfs_write` → `generic_perform_write`
- `kernel_clone` → `copy_process` → `copy_mm`
- `do_execveat_common` → `load_elf_binary` → `start_thread`
- `do_exit` → `do_wait` → `release_task`
- `handle_mm_fault` → `do_anonymous_page` / `do_wp_page` / `filemap_fault`
- `do_sys_openat2` → `path_openat` → `link_path_walk` → `do_last`

阶段三（调度）：

- `__schedule` → `pick_next_task` → `context_switch` → `switch_to`
- `task_tick_fair` → `update_curr` / `check_preempt_tick`
- `try_to_wake_up` → `ttwu_queue` → `enqueue_task_fair`
- `sched_fork` → `place_entity`
- `pick_eevdf`（6.6+）

## 7. 常见误区

- 把书上的函数名当事实。书是 2.6，源码是 6.x，名字对不上是常态。
- 只读调度不看中断。没有 tick 和中断返回，就没有抢占。
- 只看用户态 API。`nice()` 只是写一个字段，真正生效在 `enqueue_task_fair` 和 `check_preempt_tick`。
- 用「性能测试」代替「机制验证」。跑分告诉你快慢，gdb 告诉你为什么。
- 读得太顺就以为懂了。每隔一章做一次「不看书画流程图」的自测。
