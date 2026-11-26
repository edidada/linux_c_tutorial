# 07_sched_kernel —— 改内核 + gdb 跟调度器

用户态那套（`labs/06_sched`）只能看到「几分之几的 CPU」，看不到调度器为什么这么选。这一组实验把 `kernel/sched/fair.c` 打上插桩，在 QEMU guest 里打印「谁被选中、它的权重和 vruntime 是多少」，再用 gdb 断点看调用链。

## 文件

| 文件 | 作用 |
| --- | --- |
| `fair_pick_dbg.patch` | 给 6.12 的 `kernel/sched/fair.c` 加 20 行插桩，在 `pick_next_task_fair` 里打印被选中的任务 |
| `niceload.c` | 静态编译的小程序：`setpriority` 后死循环。guest 里的 busybox 没有 `nice` applet，要靠它造不同 nice 的负载 |
| `../../qemu/sched-live.gdb` | 已经启动好的 guest 上直接下 `__schedule` 断点，批量打印 20 次调度事件 |

## 步骤

```bash
# 1. 打补丁
cd qemu/build/linux-6.12
git -C <源码目录> ...        # 这棵源码树是 tar 解出来的，没有 git，先备份
cp kernel/sched/fair.c /tmp/fair.c.orig
patch -p1 < /path/to/fair_pick_dbg.patch

# 2. 增量编译（只改了一个 .c）
make -j"$(nproc)" bzImage

# 3. initramfs 里塞一个静态的 niceload
gcc -O2 -static -o niceload niceload.c
# 解包 build/rootfs.cpio.gz，把 niceload 放进 bin/，再打包回去

# 4. 起 guest，造负载
cd ../.. && GDB_WAIT=0 ./run.sh
# guest 里：
echo 1 > /proc/sys/kernel/printk        # 别让 printk 把串口刷爆
for i in 1 2 3 4; do /bin/niceload 0 & done
for i in 1 2; do /bin/niceload 10 & done
sleep 10
dmesg | grep pickdbg | tail -20
```

## 实测（2 核 guest，6.12 defconfig + DEBUG_INFO）

`/proc/<pid>/stat` 的 utime，10 秒窗口，2 核共 20 CPU·秒：

| nice | 权重 `se.load.weight` | 每进程 utime 增量 | 占比 |
| --- | --- | --- | --- |
| 0（4 个） | 1048576 | 491 / 490 / 495 / 495 ticks | 各约 49% 单核 |
| 10（2 个） | 112640 | 53 / 53 ticks | 各约 5.3% |

权重比 1048576 : 112640 = 9.31，实测 CPU 时间比 492.8 : 53 = 9.30。内核里那行 `weight=` 就是 `/proc/<pid>/stat` 里 nice 换算出来的 `load.weight`，不是另一套公式。

`dmesg` 里的原始行（`cfs_nr` 是当时 rq 上 CFS 任务数）：

```
pickdbg: prev=niceload     next=niceload     cfs_nr=3 weight=112640  vruntime=12149826020 min_vr=12149826020 lag=0
pickdbg: prev=niceload     next=niceload     cfs_nr=3 weight=112640  vruntime=12684041650 min_vr=12684041650 lag=0
pickdbg: prev=rcu_preempt  next=niceload     cfs_nr=4 weight=1048576 vruntime=14260527993 min_vr=14257636345 lag=2891648
```

`lag` 一直接近 0，是因为选中任务时 `min_vruntime` 已经被拉平；要看「谁等得更久」，应该在 `enqueue_task_fair` 或 `check_preempt_wakeup_fair` 上插桩，而不是在选择点。

## gdb 侧的调用链

```
#0 pick_next_task_fair (rq=..., prev=..., rf=...) at kernel/sched/fair.c:8901
#1 __pick_next_task  at kernel/sched/core.c:5971
#2 pick_next_task   at kernel/sched/core.c:6493
#3 __schedule (sched_mode=1) at kernel/sched/core.c:6647
#4 preempt_schedule_common at kernel/sched/core.c:6872
#5 __cond_resched    at kernel/sched/core.c:7217
```

`sched_mode` 的取值（`kernel/sched/core.c:6504` 起的宏）：

| 值 | 宏 | 谁调进来 |
| --- | --- | --- |
| -1 | `SM_IDLE` | `schedule_idle()`，idle 线程让出 |
| 0 | `SM_NONE` | 显式 `schedule()`，任务自己阻塞 |
| 1 | `SM_PREEMPT` | `preempt_schedule_common()` / 抢占点 |
| 2 | `SM_RTLOCK_WAIT` | PREEMPT_RT 的 rtlock 等待 |

所以 `bt` 看到 mode=1 加 `__cond_resched`，说明这次不是「任务主动睡」，而是「在可抢占点被别人顶掉」。

## 走过的坑

- `$lx_per_cpu("runqueues")` 在 6.12 + gdb 12.1 下抛 `'NoneType' object has no attribute 'cast'`，怎么试都不行。换成 `$lx_current()` 读 task_struct 字段就正常，`$lx_current().comm/pid/__state/se.vruntime/nvcsw` 都能直接打。
- `next` 跨过 `p = pick_task_fair(rq)` 之后 `p->comm` 报 `value has been optimized out`，`-O2` 下局部变量随时可能没掉。要在断点里打印，就在内核里插桩，别赌 gdb 的变量可见性。
- 只改 `fair.c` 一个文件的增量编译在这台 2 核 FUSE 文件系统上花了 **19 分 56 秒**（`user` 只有 34 秒），瓶颈是 vmlinux 重链接和 gzip 的 I/O，不是 CPU。别拿这个时间当「改一行几十秒」的参考，本地 ext4 上通常 1-2 分钟。
- guest 里 `nice` 不存在，`nice: not found`。busybox 里连带 `taskset`/`renice` 也未必有，要造负载就自己静态编译一个小程序塞进 initramfs。
