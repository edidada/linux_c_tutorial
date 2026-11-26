# 08_rss_anatomy —— 把「RSS 很高但堆不大」拆开

这是阶段四的收口实验：一个能落到内核路径上的真实问题。Java 进程常见现象是堆只有几百 MB，RSS 却上 GB；原因通常是 file-backed 映射、共享映射重复计数、page cache 混进 RSS 账里，而不是堆真的涨了。

## 两个程序

| 程序 | 做什么 | 看什么 |
| --- | --- | --- |
| `rss_breakdown` | 依次做 anon 8MB、私有映射 64MB 读、同文件共享映射、COW 写 16MB，每步读 `/proc/self/smaps_rollup` | `Rss` / `Anonymous` / `Pss` 怎么变 |
| `cache_probe` | 写 256MB 文件、`fadvise(DONTNEED)` 踢缓存、读两遍 | `Cached` 与进程 `VmRSS` 各涨多少 |

```bash
gcc -O2 -std=gnu11 -o rss_breakdown rss_breakdown.c && ./rss_breakdown
gcc -O2 -std=gnu11 -o cache_probe  cache_probe.c  && ./cache_probe
```

`cache_probe` 不依赖 root：容器里 `/proc/sys/vm/drop_caches` 常是只读，用 `posix_fadvise` 踢自己刚写的文件就够了。

## 实测（2 核 4GB 云主机，一次采样）

`rss_breakdown`（单位 kB）：

| 阶段 | VmRSS | Rss | Anon | File | PrivDirty | Pss |
| --- | --- | --- | --- | --- | --- | --- |
| 1 初始 | 1664 | 1556 | 128 | 1428 | 140 | 235 |
| 2 anon 8MB 写完 | 9984 | 10064 | 8328 | 1736 | 8340 | 8463 |
| 3 私有映射 64MB 读完 | 75520 | 75600 | 8328 | 67272 | 8340 | 73999 |
| 4 再加共享映射 | 141056 | 141136 | 8328 | 132808 | 8340 | 73999 |
| 5 COW 写 16MB | 141184 | 141136 | 24712 | 116424 | 24724 | 90383 |

三个可以拿来对账的点：

- 第 3 步 VmRSS 涨了 65536 kB，正好是 16384 个 4K 页 = 64MB，而 `Anon` 一点没涨（8328 不变）。这 64MB 是 file-backed，来自 page cache。
- 第 4 步同一批页又映射一次，VmRSS 再加 65536 kB，`Pss` 完全没动（73999）。RSS 按 PTE 计，同一物理页被两个映射指到就记两次；PSS 按共享者均摊，所以不变。
- 第 5 步 COW 16MB：`Anon` +16384，`File` -16384，总量不变，`Pss` +16384。`wp_page_copy` 把 file 页复制成 anon 页，账从 file 挪到 anon。

`cache_probe`：

```
刚写完       Cached=2585220 kB  VmRSS=1280 kB
文件 256 MB，65536 个 4K 页
踢缓存后     Cached=2323120 kB  VmRSS=1536 kB
第一次读(冷) 0.09 s, 2722.7 MB/s  Cached=2585164 kB  VmRSS=1792 kB
第二次读(热) 0.02 s, 11198.6 MB/s  Cached=2585164 kB  VmRSS=1792 kB
```

`fadvise` 之后 Cached 掉了 262100 kB（约 256MB），读一遍又涨回来；整个过程中进程 VmRSS 从 1.2MB 到 1.8MB，几乎不动。冷读 2722 MB/s、热读 11198 MB/s，差 4 倍——这就是 page cache 命中的价值，和 `labs/04_file/01_page_cache_read` 在 WSL 上测到的 1.24 vs 10.2 GB/s 是同一件事。

GitHub Actions 的 ubuntu-24.04 runner 上是同样的形状，只是冷读慢得多（真磁盘 vs 这台机器的高速 overlay）：

```
刚写完       Cached=1102612 kB  VmRSS=1552 kB
踢缓存后     Cached=840724 kB   VmRSS=1872 kB
第一次读(冷) 0.93 s, 275.7 MB/s
第二次读(热) 0.02 s, 14479.3 MB/s
```

`rss_breakdown` 在 runner 上的五步也几乎一致：Anon 8.3MB、File 在共享映射后 132840 kB、Pss 73908 kB 不动、COW 后 Anon 涨到 24688 kB。两台机器数字差在 1% 以内，说明这套账是内核的，不是某台机器的。


## 对应的内核路径

| 用户态动作 | 内核函数 | 账记在哪 |
| --- | --- | --- |
| `malloc` + 写 | `do_anonymous_page` / `wp_page_copy` | `smaps_rollup` 的 `Anonymous` |
| 读文件映射 | `filemap_fault` → `page_cache_sync_readahead` | `Rss` 中的 file 部分 + 全局 `Cached` |
| 同文件再 `MAP_SHARED` | 同一 `struct page`，多一个 PTE | `Rss` 重复计，`Pss` 均摊 |
| `read()` 大文件 | `vfs_read` → `filemap_read` | 进程 `Rss` 不变，全局 `Cached` 涨 |
| `posix_fadvise(DONTNEED)` | `invalidate_mapping_pages` | 全局 `Cached` 回落 |

## Java 里怎么用这套账

- `-Xmx` 只约束 Java 堆，堆是 anon 内存，和 RSS 里 file 部分无关。
- JVM 映射的 jar、CDS 归档、`MappedByteBuffer` 都是 file-backed，会进 RSS，但不受 `-Xmx` 管。
- 容器里 `memory.current` / `memory.stat` 的 `file` 与 `anon` 就是这两本账，`file` 高会随内存压力被回收，`anon` 高才会 OOM。
- 排查顺序：`smaps_rollup` 看 Rss/Anonymous 差值 → `Pss` 判断共享程度 → `/proc/meminfo` 的 Cached 判断是不是全局缓存 → 再谈堆。

## 易错点

- `smaps_rollup` 没有单列 `File`，`Rss - Anonymous` 才是 file/shmem-backed，别去找 `Shared_File`。
- `Rss` 不等于物理内存占用：共享页会被每个映射它的进程各记一次，`Pss` 才是均摊值。
- 用 `MAP_PRIVATE` 映射文件后写它，账会从 file 变 anon；只想读就别给 `PROT_WRITE`，否则很容易在实验里误触发 COW。
- `posix_fadvise` 的 `DONTNEED` 只对 clean page 有效，脏页要先 `fsync`，否则踢不掉。
