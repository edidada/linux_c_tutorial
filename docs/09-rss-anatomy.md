# 09 收口：一个真实问题的闭环——RSS 很高，但堆不大

前八篇是「按知识点走」，这一篇按真实问题走：从现象出发，一步步把数字拆到内核路径上，最后给出可复现的命令。

## 现象

一个 Java 服务，`-Xmx512m`，但容器里 RSS 是 1.5GB，K8s 的 memory limit 是 1GiB，隔几天就被 OOM kill。

## 拆解顺序（不要一上来就猜堆）

1. **先分两本账**：进程 RSS（`/proc/<pid>/status`）和容器内存（`memory.current`）。容器 `memory.stat` 里 `anon` 和 `file` 是分开的，OOM 只看 `anon` 为主的那部分，`file` 会被回收。
2. **拆进程 RSS**：`/proc/<pid>/smaps_rollup` 只看 6 个字段就够——`Rss`、`Pss`、`Private_Dirty`、`Anonymous`、`Shared_Clean`、`Swap`。`File = Rss - Anonymous`。
3. **判断共享程度**：`Pss` 远小于 `Rss` 说明大部分页被别的进程也映射着（jar、CDS、共享库），这部分不是「你的进程独吞」。
4. **回头再看堆**：jmap/jcmd 的堆占用如果和 `Anonymous` 差得远，问题就不在堆。

## 实测账本

`labs/08_rss_anatomy/rss_breakdown` 在一台 2 核 4GB 机器上跑出来的（单位 kB）：

| 阶段 | VmRSS | Rss | Anon | File | Pss |
| --- | --- | --- | --- | --- | --- |
| 初始 | 1664 | 1556 | 128 | 1428 | 235 |
| anon 8MB | 9984 | 10064 | 8328 | 1736 | 8463 |
| 私有映射 64MB 读完 | 75520 | 75600 | 8328 | 67272 | 73999 |
| 再加共享映射 | 141056 | 141136 | 8328 | 132808 | 73999 |
| COW 写 16MB | 141184 | 141136 | 24712 | 116424 | 90383 |

结论很直接：`Anon` 只有 8MB（对应「堆不大」），`Rss` 涨到 138MB 全靠 file-backed；共享映射那一步 `Rss` 再加 64MB 而 `Pss` 不动，说明这部分页被重复计了。

`labs/08_rss_anatomy/cache_probe` 展示另一本账：

```
刚写完       Cached=2585220 kB  VmRSS=1280 kB
踢缓存后     Cached=2323120 kB  VmRSS=1536 kB
第一次读(冷) 0.09 s, 2722.7 MB/s  Cached=2585164 kB
第二次读(热) 0.02 s, 11198.6 MB/s  Cached=2585164 kB
```

读 256MB 文件，进程 RSS 几乎不动，全局 `Cached` 涨了 256MB。这就是「RSS 高≠内存被独占」的最直观演示。

## 内核里对应的三行代码

- `do_anonymous_page()`（`mm/memory.c`）：匿名页缺页，分配 page 并挂到 `anon_vma`，`smaps` 记 `Anonymous`。
- `filemap_fault()`（`mm/filemap.c`）：文件映射缺页，从 page cache 取页（缺则读盘），记 file-backed `Rss`。
- `wp_page_copy()`（`mm/memory.c`）：写私有文件映射触发 COW，新页变 anon，账从 file 挪到 anon。

共享映射为什么重复计：`Rss` 统计的是「本进程页表里 resident 的 PTE 数」，同一 `struct page` 被两个 VMA 映射就占两个 PTE；`Pss` 在 `smaps` 里按 `mapcount` 均摊，所以不变。

## 和前面几篇的对接

| 前面的实验 | 这一篇用它做什么 |
| --- | --- |
| `03_memory/02_page_fault` | minflt/majflt 计数就是 `do_anonymous_page` / `filemap_fault` 的次数 |
| `04_file/01_page_cache_read` | 冷热读带宽差 8 倍，和这里 `cache_probe` 的 4 倍是同一机制 |
| `02_process/proc_inspect` | `/proc/<pid>` 字段来源，`smaps_rollup` 是同一批数据的汇总版 |
| `docs/04-memory.md` | VMA、页表、回收的整体图，这一篇是它在真实问题上的用法 |

## 复现命令

```bash
# 1. 两本账
grep -E "VmRSS|RssAnon|RssFile|RssShmem" /proc/<pid>/status
cat /sys/fs/cgroup/memory.stat | grep -E "^(anon|file) "

# 2. 拆 RSS
grep -E "^(Rss|Pss|Private_Dirty|Anonymous|Shared_Clean|Swap):" /proc/<pid>/smaps_rollup

# 3. 谁在共享
grep -B2 -A8 "Pss:" /proc/<pid>/smaps | awk '/^[0-9a-f]/{v=$0} /Pss:/{if ($2+0 > 1024) print v, $0}'

# 4. 全局缓存账
grep -E "^Cached|^Buffers|^SReclaimable" /proc/meminfo

# 5. 实验本体
cd labs/08_rss_anatomy && gcc -O2 -std=gnu11 -o rss_breakdown rss_breakdown.c && ./rss_breakdown
```

## 判定标准

- `Anon` 与 jmap 的堆占用接近 → 堆确实是主因，去看 GC 和对象泄漏。
- `Anon` 远小于 `Rss`，`File` 占大头 → 去看 jar/映射文件/CDS，`-Xmx` 调小没有意义。
- `Pss` 远小于 `Rss` → 大量共享页，先确认是不是同一个基础镜像里的多个进程在共享。
- 容器 `memory.stat` 的 `file` 很大但 `anon` 稳定 → 不是 OOM 风险，是 page cache 被算进了容器账。

## 这一篇之后

到这儿，阶段一到阶段四的链路是闭合的：一个用户态动作 → 系统调用 → 内核函数 → 数据结构 → 观测命令 → 一个能解释真实问题的数字。剩下的时间花在「自己造一个异常」上：故意把某个数字改坏（比如让一个进程 mmap 一个 8GB 稀疏文件并逐页读），看它在 `smaps_rollup`、`/proc/meminfo`、cgroup 三处账上分别长什么样，比再读一遍书有用。
