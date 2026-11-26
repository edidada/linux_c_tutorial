# 04 内存：VMA 是合同，物理页是按需交付

对应实验：`labs/03_memory/01_brk_mmap.c`、`02_page_fault.c`、`03_mmap_shared.c`

## 1. 三层结构

```
task_struct
  └── mm_struct                 一个进程的地址空间
        ├── mmap             -> vm_area_struct 链表（所有 VMA）
        ├── mm_rb            -> 红黑树（按地址排序，便于查找）
        ├── pgd              -> 页表根（CR3 指向它）
        └── rss_stat         匿名/文件/共享页计数

vm_area_struct (VMA)
  vm_start/vm_end     虚拟地址区间
  vm_flags            VM_READ|VM_WRITE|VM_EXEC|VM_SHARED|VM_GROWSDOWN...
  vm_file             文件映射时指向 file
  vm_pgoff            文件内偏移（页为单位）
  vm_ops              缺页时调用的方法（filemap_fault / shmem_fault ...）
```

VMA 只是「这段地址我打算怎么用」的声明。`mmap`/`brk` 只创建或扩大 VMA，不分配物理页。

## 2. brk 与 mmap：malloc 的两个来源

- `brk`：调整 `mm->brk`，把 `[start_brk, brk]` 这段匿名 VMA 扩大。只有一个，位于堆区。
- `mmap`：创建任意位置的 VMA，匿名或文件映射。
- glibc `malloc` 策略：小于 `mmap_threshold`（默认 128KB，会动态调整）用 brk/arena；大于阈值直接 `mmap`，`free` 时 `munmap` 还给内核。

实测（`01_brk_mmap`，未写入前）：

```
malloc(16)     addr=0x5a993dff68a0  -> [heap]
malloc(100KB)  addr=0x5a993dff68c0  -> [heap]
malloc(8MB)    addr=0x721a455ff010  -> 独立匿名映射（不在 [heap]）
```

注意 100KB 仍然落在 [heap]，因为初始 brk 已经有 132KB，够用；阈值是「单次请求」判断，不是「堆总大小」。

写入 8MB 之后 RSS +8188 KB，`free` 后又降回。这验证了「虚拟地址先给，物理页后给，不用了还给内核」。

## 3. 缺页：内核真正分配内存的地方

```
CPU 访问一个 PTE 为空的地址
  -> page fault（CR2 = 故障地址）
  -> do_user_addr_fault            arch/x86/mm/fault.c
       在 mm->mmap 里查 VMA；没有 -> SIGSEGV
       检查权限；只读页要写 -> COW 或 SIGSEGV
  -> handle_mm_fault               mm/memory.c
       __handle_mm_fault -> 按 PTE 状态分路：
         do_anonymous_page()      匿名页首次访问：分配零页或新页（minor）
         do_wp_page()             COW：复制一页（minor）
         do_fault() -> filemap_fault() 文件页不在 cache：读磁盘（major）
       更新 PTE
  -> 返回用户态，重新执行那条指令
```

实测（`02_page_fault`）：

```
A. 256MB 匿名写（关掉 THP）
   minflt +65536   正好 65536 页 * 4KB
B. 32MB 文件冷读（MADV_RANDOM 关预读）
   minflt +0，majflt +8192   8192 页，每页一次磁盘 I/O
C. 32MB 文件热读（页已在 page cache）
   minflt +512，majflt +0
```

C 里的 512 值得解释：`fault-around`（`do_fault_around`）在缺页时会顺便把附近最多 16 个 PTE 一起填好（默认 `fault_around_bytes=64KB`）。8192 / 16 = 512。所以「热读只缺 512 次页」不是 bug，是批处理。

关键区分：

- minor fault：不需要 I/O，只分配页或复制页，微秒级。
- major fault：要读磁盘，毫秒级，是性能杀手。
- 统计位置：`getrusage` 的 `minflt/majflt`，`/proc/<pid>/stat` 第 10/12 字段。

## 4. SHARED 与 PRIVATE

实测（`03_mmap_shared`，两个不同文件）：

```
子进程写后父进程看到:
  MAP_SHARED  = "child wrote SHARED"   <- page cache 同一页
  MAP_PRIVATE = "init"                 <- 子进程 COW 的私有页
msync 后 shared.bin 磁盘内容 = child wrote SHARED
private.bin 磁盘内容 = init
```

两个容易踩的点：

1. **私有映射不隔离别人的写**。如果别的进程通过 SHARED 映射改了 page cache，你的 PRIVATE 映射下次缺页看到的还是新内容。PRIVATE 只保证「你自己的写不传播出去」。
2. **`msync` 对 PRIVATE 映射不写回**，因为根本没有 dirty page cache 页可写。

## 5. RSS、PSS 与「内存到底被谁占了」

| 指标 | 含义 | 陷阱 |
| --- | --- | --- |
| VmRSS | 驻留集大小，物理页数 * 4KB | 共享页全算在你头上，多进程会重复计数 |
| PSS | 按共享比例分摊后的 RSS | 需要 `CONFIG_SMAPS`，读 smaps 较慢 |
| Shared_Clean/Dirty | 与其他进程共享的页 | 文件页、共享匿名页 |
| Private_Dirty | 只有你写的匿名页 | 更接近「真正独占的内存」 |
| VmSize | 虚拟地址总量 | 和物理内存无关，JVM `-Xmx` 预申请会很大 |

排查「Java 进程 RSS 大但堆不大」的顺序：`smaps_rollup` 看 Private_Dirty 与文件页 → page cache 是否算在 RSS（不算，但 shared 文件页算在 RSS） → 是否有大量线程栈（每线程默认 8MB 虚拟、实际 RSS 几十 KB） → THP 是否放大匿名页。

## 6. 回收与 OOM

- 内存不足时先回收：`kswapd` 周期性回收，直接回收（direct reclaim）发生在分配路径上，会让分配变慢。
- 可回收的页：clean 文件页（直接丢）、dirty 文件页（先写回）、匿名页（需要 swap）。
- OOM killer 按 `oom_score` 选受害者，`oom_score_adj` 可调（容器里常用）。
- `overcommit_memory`：0 启发式、1 总是允许、2 严格。JVM 预申请大地址空间时经常受这个参数影响。

## 7. 自己动手

1. 用 `madvise(MADV_HUGEPAGE)` 再跑 `02_page_fault`，看 minflt 掉到多少（2MB 一页，预期 256MB / 2MB = 128 次左右）。
2. 在 `01_brk_mmap` 里用 `mallopt(M_MMAP_THRESHOLD, 1<<20)` 把阈值改成 1MB，看 100KB 的 malloc 是否还留在 heap。
3. `cat /proc/<pid>/smaps_rollup` 对比 `VmRSS` 与 `Pss`，找一个多进程共享库的场景。
4. QEMU 里断 `handle_mm_fault`，`p vmf->address`、`p vmf->flags`，看 `FAULT_FLAG_WRITE` 是否置位，再单步进 `do_wp_page`。
5. ftrace 跟 `filemap_fault`，看冷读时它下面挂了哪些函数（`page_cache_sync_readahead`、`read_pages`、`submit_bio`）。

## 8. 如果选内存管理做深挖（替代路线）

调度的路线在 `docs/07-sched-deepdive.md`。如果更想挖内存，按这个顺序：

1. VMA 操作：`mmap_region`、`do_munmap`、VMA 合并（`vma_merge`）。
2. 缺页：`handle_mm_fault` 四条分支各跟一遍。
3. page cache：`filemap_read`、`filemap_fault`、readahead 窗口。
4. 回收：`shrink_node`、LRU 链表（active/inactive）、`shrink_folio_list`。
5. 分配器：`buddy`（`__alloc_pages`）、slab（`kmem_cache_alloc`）、per-cpu 页帧缓存。
6. 实验：`/proc/pagetypeinfo`、`/proc/buddyinfo`、`/proc/vmstat` 在压力下的变化。

## 易错点

- `mmap` 返回成功不代表物理页已分配；`MAP_POPULATE` 才会预缺页。
- `malloc` 返回非 NULL 不代表有物理页；写入才可能 OOM。
- `madvise(MADV_DONTNEED)` 对匿名页是「丢弃内容」，下次读是 0，不是「保留」。
- `fork` 后子进程 RSS 的统计口径受共享页影响，别拿父子 RSS 直接相加。
- THP 会让缺页计数、RSS 粒度都变成 2MB，做对比实验前先关掉。
