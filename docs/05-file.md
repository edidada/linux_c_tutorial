# 05 文件：四个对象和一条路径

对应实验：`labs/04_file/01_page_cache_read.c`、`02_fsync_cost.c`、`03_dentry_cache.c`、`04_inotify.c`

## 1. VFS 的四个对象

| 对象 | 生命周期 | 对应什么 |
| --- | --- | --- |
| `super_block` | 一次 mount | 一个文件系统实例（ext4 的一个分区） |
| `inode` | 一个文件（可被缓存/回收） | 元数据：大小、权限、块位置、时间戳 |
| `dentry` | 一个路径分量 | 名字 → inode 的绑定，构成目录树缓存 |
| `file` | 一次 open | 打开实例：当前偏移、打开模式、`f_op` 指针 |

关系：`file->f_path.dentry` → `dentry->d_inode` → `inode->i_sb`。同一个文件可以有多个 `file`（多次 open），但通常只有一个 inode、一个 dentry（硬链接会有多个 dentry 指向同一 inode）。

## 2. open 的路径

```
__x64_sys_openat
  -> do_sys_openat2            fs/open.c
       get_unused_fd_flags     先在 fd 表里占一个号
       do_filp_open
         path_openat           fs/namei.c
           getname_flags       把用户态路径拷进内核（PATH_MAX 4096）
           link_path_walk      逐级解析：
             每个分量先在 dcache 里按 (parent, name) 查找
             命中 -> 校验 d_seq（是否在解析期间被删/改名）
             未命中 -> inode->i_op->lookup()（ext4_lookup）读目录块
           do_last             最后一级：O_CREAT 时创建，权限检查，调用 f_op->open
       fd_install              把 file 挂到 fd 表
```

两个成本点：

- 路径解析：每一级一次 dcache 查找。目录层级深、mount 层多（overlayfs/容器）时线性变贵。
- 权限检查：`inode_permission` 走 `generic_permission`，还有 LSM（SELinux/AppArmor）钩子。

实测（`03_dentry_cache`，2000 个文件）：

```
stat 已存在（dcache 命中）        1.77 us
stat 不存在（negative dentry）    5.86 us
unlink 后 stat                    0.90 us
```

三者都在微秒级，说明路径一旦不碰磁盘，成本主要是 syscall 本身。真正的 lookup 代价要 drop caches 或冷目录才看得到。negative dentry 的意义是：反复探测不存在的路径不会每次都读目录块。

## 3. read/write 与 page cache

```
read
  -> vfs_read -> filemap_read         mm/filemap.c
       先在 inode->i_mapping 的 xarray 里找页
       没有 -> page_cache_sync_readahead / read_pages -> submit_bio
       有   -> copy_page_to_iter 拷到用户缓冲区（仍是一次内存拷贝）

write
  -> vfs_write -> filemap_write_iter -> generic_perform_write
       copy_page_from_iter_atomic 拷进 page cache 页
       folio_mark_dirty
       balance_dirty_pages 按 dirty_background_ratio 触发后台回写
```

所以 buffered I/O 永远有「用户缓冲区 ↔ page cache」这一次拷贝。`O_DIRECT` 跳过 page cache，用 DMA 直接读写用户缓冲区，代价是对齐要求和失去缓存。

实测（`01_page_cache_read`，128MB 文件）：

```
写完后立刻读（页还在 cache）      18459 MB/s
fadvise DONTNEED 后读（冷，读盘）  1241 MB/s
紧接着再读（热，page cache）      10210 MB/s
```

热读 10GB/s 是内存拷贝的上限量级；冷读 1.2GB/s 是本机虚拟磁盘的顺序读带宽。两者差 8 倍，这就是为什么数据库要自己管 buffer pool——它不想和内核的 page cache 抢同一份内存，还要自己控制刷盘时机。

`posix_fadvise(POSIX_FADV_DONTNEED)` 不需要 root，只丢指定 range 的 clean 页，是做冷/热对比最干净的办法（对比 `drop_caches` 是全局的）。

## 4. fsync 与写回

- 脏页由 flusher 线程按 `dirty_expire_centisecs`（默认 30s）和 `dirty_writeback_centisecs`（默认 5s）回写。
- `fsync` = 提交该文件的脏页 + 等 I/O 完成 + 刷元数据（ext4 走 jbd2 日志提交）。
- `fdatasync` 省略不必需的元数据（如 mtime），少一次日志事务。

实测（`02_fsync_cost`，200 条 4KB）：

```
每条 write 后 fsync        315.1 ms，每条 1575 us
每 50 条 fsync 一次         11.6 ms，每条 58 us
只 write，最后 fsync 一次    4.8 ms，每条 23.8 us
```

同样的数据量，差 27 倍。差距全部来自「等设备 flush + 等日志提交」的次数。这就是 WAL 组提交、Kafka 的 `flush.messages`、MySQL `innodb_flush_log_at_trx_commit` 的本质。

## 5. inotify：把 VFS 的钩子变成事件

实测（`04_inotify`）：

```
CREATE -> OPEN -> MODIFY -> CLOSE_WRITE -> ATTRIB -> MOVED_FROM/MOVED_TO -> DELETE
```

每个事件对应 VFS 里的一次 `fsnotify` 调用：

| 事件 | 内核位置 |
| --- | --- |
| CREATE | `vfs_create` / `vfs_mkdir` |
| OPEN/CLOSE_WRITE | `do_dentry_open` / `filp_close` |
| MODIFY | `file_modified` → `fsnotify_modify` |
| ATTRIB | `notify_change`（chmod/chown/utimes） |
| MOVED_FROM/TO | `vfs_rename` |
| DELETE | `vfs_unlink` |

inotify watch 挂在 inode 上（`i_fsnotify_marks`），不是路径，所以 rename 之后 watch 仍有效。事件进内核队列，用户态用 `read` 取——它也是一个 fd。

## 6. 文件系统层往下

ext4 的一次 `write` 落盘大概经过：

```
page cache 脏页
  -> writeback 控制（wb_writeback）
  -> ext4_writepages           构建 extent、分配块
  -> jbd2 日志事务             元数据先写日志（ordered 模式下数据先落盘）
  -> 块层 submit_bio           合并、排序
  -> I/O 调度器（mq-deadline/bfq/none）
  -> 设备驱动 / virtio-blk
```

想验证顺序，用 ftrace 跟 `ext4_writepages` 和 `jbd2_journal_commit_transaction`，或者用 `blktrace`/`bpftrace` 看块层。

## 7. 自己动手

1. `01_page_cache_read` 加一个 `O_DIRECT` 分支，缓冲区和长度按 4KB 对齐，对比吞吐和 CPU 占用（`/usr/bin/time -v` 看 system time）。
2. 把 `02_fsync_cost` 改成 `fdatasync`，看每条成本降多少；再把文件放到 `/mnt/d`（9p/drvfs）上跑一次，量级会完全不同——这是「虚拟磁盘 vs 真实磁盘」的差别。
3. 用 `ftrace function_graph` 跟 `do_sys_openat2`，数 `link_path_walk` 下有几层 `walk_component`。
4. 在 `04_inotify` 里把 watch 加到文件本身，再 rename，验证 watch 跟着 inode 走。
5. QEMU 里断 `filemap_fault`，`p vmf->pgoff` 看读到第几页，`finish` 看返回的是不是 `VM_FAULT_RETRY`。

## 易错点

- `write` 成功只代表进了 page cache；掉电会丢，需要 `fsync`。
- `read` 返回 0 是 EOF，不是错误；短读（返回值小于请求）在管道/终端/信号中断时都正常。
- `fsync` 刷的是「这个文件」，父目录的元数据（新建文件）需要 `fsync` 目录 fd 才保证落盘。
- 同一文件多个 fd 的偏移各自独立（`file` 对象各自一份 `f_pos`），`O_APPEND` 除外。
- `/tmp` 在容器里可能是 tmpfs，`fsync` 是空操作，测出来的数字没有意义。
