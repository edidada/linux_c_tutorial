# 07 调度子系统：数据结构、关键函数、六个实验

选择调度做深挖的原因：它把进程、时钟中断、抢占、上下文切换、负载均衡、同步全串起来了，而且所有结论都能在两台机器上复现。

## 1. 一张指针图（先背这张图，再看代码）

```
per-cpu: runqueues (DEFINE_PER_CPU_SHARED_ALIGNED(struct rq, runqueues))
  rq
   ├── curr                 -> 当前在跑的任务（task_struct）
   ├── idle                 -> 每个 CPU 的 idle 任务
   ├── nr_running
   ├── cfs                  struct cfs_rq（公平调度队列）
   │     ├── tasks_timeline  -> 红黑树根（按 vruntime 排序）
   │     ├── curr            -> 当前跑的 sched_entity
   │     ├── min_vruntime
   │     ├── h_nr_running
   │     └── nr_running
   ├── rt                   struct rt_rq（SCHED_FIFO/RR，按优先级队列）
   ├── dl                   struct dl_rq（deadline 调度）
   └── clock / clock_task    rq 自己的时钟（用于统计）

task_struct
  ├── __state              TASK_RUNNING / INTERRUPTIBLE / ...
  ├── prio / static_prio   动态/静态优先级（CFS 下 120 + nice）
  ├── policy               SCHED_NORMAL / FIFO / RR / BATCH / IDLE / DEADLINE
  ├── sched_class          -> fair_sched_class / rt_sched_class / ...
  ├── se                   struct sched_entity（嵌入在 task_struct 里）
  │     ├── load           权重（由 nice 换算）
  │     ├── vruntime       虚拟运行时间（ns，权重归一化后）
  │     ├── deadline/ slice  EEVDF 新增
  │     ├── on_rq / cfs_rq
  │     └── parent          -> 组调度层级（cgroup 用）
  └── on_cpu / on_rq
```

关键点：`sched_entity` 是嵌在 `task_struct` 里的，不是单独分配；`rq->curr` 指向 task，`cfs_rq->curr` 指向它的 `se`。

## 2. 调度类：优先级从高到低

| 类 | 策略 | 特点 |
| --- | --- | --- |
| stop | - | 内核内部用，迁移线程等 |
| dl | SCHED_DEADLINE | 有 runtime/deadline/period 三个参数，带宽保证 |
| rt | SCHED_FIFO / SCHED_RR | 固定优先级 1-99，会饿死 CFS |
| fair | SCHED_NORMAL / BATCH | CFS/EEVDF，绝大多数进程 |
| idle | SCHED_IDLE | 只在没别的事时跑 |

`pick_next_task` 按这个顺序遍历 `sched_class` 链表，高优先级类有人就绪就一直跑高优先级类。`SCHED_FIFO` 的进程不主动让出就永不被打断——这是实时任务把系统卡死的经典原因。

## 3. CFS 到 EEVDF：机制没变，选人规则变了

**CFS（2.6.23 - 6.5）**

- 每个任务有权重 `load`，由 nice 换算：`sched_prio_to_weight[nice+20]`，nice 0 = 1024。
- 每次时钟 tick 或任务切换时 `update_curr()`：`vruntime += 实际运行时间 * (NICE_0_LOAD / load)`。
- 红黑树按 vruntime 排序，`pick_next_task_fair` 取最左（vruntime 最小）的实体。
- 效果：权重大的任务 vruntime 涨得慢，因此被选中得更频繁，长期份额正比于权重。

**EEVDF（6.6 起）**

- 每个实体有 `slice`（时间片）和 `deadline = vruntime + slice`。
- 选「eligible 且 deadline 最早」的实体；`lag = vruntime - cfs_rq->avg_vruntime` 衡量它「欠不欠 CPU」。
- 好处：延迟可控（slice 可调），睡眠任务醒来不会被欠账太久，交互性更好。
- 宏观份额仍然按权重分配，所以 `nice_effect` 的 1024:335:110 结论在 6.12 上依然成立（实测 69.8/22.8/7.4）。

不要用旧书的「红黑树最左节点」解释 6.12 的全部行为；树还在，但选人多了 deadline 和 eligibility 两个条件。

## 4. 关键函数（按执行顺序）

| 时机 | 函数 | 做了什么 |
| --- | --- | --- |
| 时钟 tick | `scheduler_tick` → `task_tick_fair` → `task_tick_fair` 内 `update_curr` + `check_preempt_tick` | 记运行时间；判断当前任务是否用完 slice/被更早 deadline 的任务超过 |
| 需要抢占 | `resched_curr(rq)` | 置 `TIF_NEED_RESCHED`，等抢占点 |
| 抢占点 | `preempt_schedule_irq`（中断返回）、`syscall_exit_to_user_mode` | 在安全点调用 `__schedule` |
| 入睡 | `schedule()` → `__schedule` | 选下一个任务，切换 |
| 选人 | `pick_next_task_fair` → `pick_eevdf` | 从 cfs_rq 里按 vruntime/deadline 选 |
| 切换 | `context_switch` → `switch_mm_irqs_off` + `switch_to` | 换页表（CR3）、换内核栈和寄存器 |
| 切换后 | `finish_task_switch` | 收尾：清 `on_cpu`、处理 rq->prev、唤醒等 |
| 入队 | `enqueue_task_fair` | 放进红黑树，更新 min_vruntime |
| 唤醒 | `try_to_wake_up` → `ttwu_queue` → `enqueue_task` | 选 CPU（wake_affine），入队，可能立即抢占当前任务 |
| 创建 | `sched_fork` | 初始化 se、设置 vruntime 基准（防止新任务占便宜） |

`__schedule` 的主干（`kernel/sched/core.c`）：

```c
rq = this_rq();
prev = rq->curr;
if (prev->__state == TASK_RUNNING) ...  // 仍在跑就只标记
next = pick_next_task(rq, prev, &rf);
if (likely(prev != next)) {
    rq->nr_switches++;
    rq->curr = next;
    context_switch(rq, prev, next, &rf);
}
```

`context_switch` 里两件事：`switch_mm` 换地址空间（如果 mm 不同），`switch_to` 换栈和 callee-saved 寄存器。`switch_to` 是宏，实际是 `__switch_to_asm`：保存 rbp/rbx/r12-r15、换 rsp、返回时从 next 的栈上继续。

## 5. 抢占发生在哪里

内核不是随时可抢占的，只有「抢占点」：

1. 从系统调用/中断返回用户态之前（`syscall_exit_to_user_mode` 检查 `TIF_NEED_RESCHED`）。
2. 中断处理返回时（`preempt_schedule_irq`）。
3. 显式 `cond_resched()` / `might_sleep()` 路径。
4. 自旋锁释放（`preempt_enable`）。

所以「一个系统调用跑了 10ms 才返回」期间，同 CPU 上更高优先级的任务只能等——除非内核配置了 `CONFIG_PREEMPT`（抢占式内核）。这也是实时性调优的第一个问题：`PREEMPT_NONE` vs `PREEMPT_VOLUNTARY` vs `PREEMPT` vs `PREEMPT_RT`。

## 6. 观测点

| 想看什么 | 用哪个 |
| --- | --- |
| 每个 CPU 的队列、当前任务、vruntime | `/proc/sched_debug` |
| 单个任务的运行时间、切换次数、迁移次数 | `/proc/<pid>/sched` |
| 全局调度统计（每秒） | `/proc/schedstat` |
| 谁切到谁、prev_state | ftrace `sched:sched_switch` |
| 唤醒延迟 | `sched:sched_wakeup` + `sched_switch` 时间差，或自己写 ping-pong |
| 迁移 | `sched:sched_migrate_task` |
| 抢占 | `sched:sched_process_exit` / `sched:sched_stat_runtime` |

## 7. 六个实验（从用户态到改内核）

**实验 1：nice 与权重（已跑通）**

`labs/06_sched/01_nice_effect.c`。三个忙循环绑到同一个 CPU，nice 0/5/10。实测 69.8/22.8/7.4，理论 69.7/22.8/7.5。
要点：不绑核就全是 100%，因为每个进程各占一个核。

**实验 2：唤醒延迟分布**

`labs/06_sched/02_wakeup_latency.c`。两个线程同一 CPU 管道 ping-pong 2 万次。实测 p50 往返 4.4us（单向 2.2us），p99 10.7us，max 702us。
要点：尾部延迟来自中断、其他任务、VM 的 vCPU 调度；实时系统看 p99.9 和 max，不看平均。

**实验 3：/proc/sched_debug 交叉验证**

在实验 1 跑的时候另开终端：

```bash
watch -n0.5 'grep -A3 "cpu#0" /proc/sched_debug | head -20'
```

看 `cfs_rq` 的 `nr_running`、`min_vruntime`、当前 se 的 `vruntime`，和你在程序里算出的份额对上。

**实验 4：SCHED_FIFO 饥饿（需要 root 或 CAP_SYS_NICE）**

写一个 `sched_setscheduler(SCHED_FIFO, 优先 80)` 的忙循环，另一个普通进程计时。会看到普通进程几乎拿不到 CPU。这一步很危险，只能在 QEMU guest 或自己的机器上短时间跑（加 `timeout`）。

**实验 5：改 sysctl 看延迟变化**

```bash
cat /proc/sys/kernel/sched_latency_ns              # 默认 6ms 左右
cat /proc/sys/kernel/sched_min_granularity_ns
echo 20000000 > /proc/sys/kernel/sched_latency_ns  # 20ms
```

再跑 `02_wakeup_latency`，p50 会变大。改回原值。这是「参数 → 行为」最便宜的闭环。

**实验 6：QEMU + gdb 单步**

按 `docs/06-qemu-gdb.md`，在 `__schedule`、`pick_next_task_fair`、`finish_task_switch` 上断点，走完一次切换。然后改 `check_preempt_tick` 的条件，重新编译，对比 `01_nice_effect` 的份额变化。

## 8. 负载均衡（别忽略）

每个 CPU 有自己的 `rq`，任务不会自动跑满所有核。`scheduler_tick` 里会触发 `trigger_load_balance`，由 `nohz` 的 idle 平衡或 `run_rebalance_domains` 的软中断做迁移：

- `sched_domain` 层级：SMT → MC（同一插槽）→ NUMA。
- 迁移成本 `sched_migration_cost_ns` 决定「多小的不均衡值得搬」。
- 唤醒时的 `select_task_rq_fair` 会优先选有 idle CPU 的核（wake_affine），这是短任务能快速铺开的原因。

实验：起 N 个忙循环不绑核，用 `/proc/sched_debug` 看它们怎么分布；再 `taskset` 绑核对比。

## 9. Java 视角的对照

- 虚拟线程（Project Loom）在 JVM 内做用户态调度，底层仍是 `ForkJoinPool` 的 carrier 线程；内核只看到十几个 carrier，调度器不感知虚拟线程。所以「虚拟线程多了会不会把内核压垮」的答案是：不会多出 task_struct，但会把 runqueue 压力转成 JVM 内的队列压力。
- `Thread.yield()` 对应 `sched_yield()`，只是把当前任务放到同优先级队列尾部，不保证让出。
- `Thread.sleep(1)` 的实际唤醒延迟就是本实验测的 p50/p99；JVM 的 `ScheduledThreadPoolExecutor` 精度受此限制。
- GC 的 stop-the-world 停顿是 JVM 自己暂停线程，不是内核调度；但 STW 期间 CPU 空转会被 CFS 记为运行时间，影响份额统计。

## 10. 易错点

- `nice` 只影响 CFS/EEVDF，不影响 RT/DL。
- `sched_setaffinity` 只限制「能跑哪些 CPU」，不保证「一定在那个 CPU」。
- `/proc/<pid>/sched` 的 `se.sum_exec_runtime` 是累计值，不是本次运行时间。
- EEVDF 下 `min_vruntime` 的语义和 CFS 不完全一样，别拿旧公式硬套。
- 在虚拟机里测绝对延迟没有意义，只能做同机对比。

## 11. 内核侧实测（6.12 + QEMU + gdb，2026-11-27）

用户态测完份额，还要把这几个数字在内核里对上。这组数据是在 2 核 guest（6.12 defconfig + DEBUG_INFO，无 KVM）上实测的，脚本在 `qemu/sched-live.gdb`，插桩补丁在 `labs/07_sched_kernel/fair_pick_dbg.patch`。

**gdb 侧的调用链**（断在 `pick_next_task_fair`）：

```
#0 pick_next_task_fair (rq=..., prev=..., rf=...) at kernel/sched/fair.c:8901
#1 __pick_next_task  at kernel/sched/core.c:5971
#2 pick_next_task   at kernel/sched/core.c:6493
#3 __schedule (sched_mode=1) at kernel/sched/core.c:6647
#4 preempt_schedule_common at kernel/sched/core.c:6872
#5 __cond_resched    at kernel/sched/core.c:7217
```

**`sched_mode` 就是「这次调度是谁发起的」**，比 `bt` 更直接：

| 值 | 宏 | 触发者 |
| --- | --- | --- |
| -1 | `SM_IDLE` | `schedule_idle()`，idle 线程让出 CPU |
| 0 | `SM_NONE` | 显式 `schedule()`，任务自己阻塞（`state` 已经改成非 RUNNING） |
| 1 | `SM_PREEMPT` | `preempt_schedule_common()` / `__cond_resched()`，在抢占点被顶掉 |
| 2 | `SM_RTLOCK_WAIT` | PREEMPT_RT 的 rtlock 等待 |

20 次调度事件的取样里有两条典型对照：`mode=-1 prev=swapper/1` 是 idle 在空转，`mode=0 prev=ksoftirqd/1 state=1` 是软中断线程把自己设成 TASK_INTERRUPTIBLE 后主动让出。同一个函数，两条完全不同的路径。

**nice → 权重 → 份额**。用 `labs/07_sched_kernel/niceload.c`（静态编译的 setpriority 忙循环）在 guest 里造 4 个 nice 0 + 2 个 nice 10，10 秒窗口读 `/proc/<pid>/stat`：

| nice | `se.load.weight`（dmesg 里的 `weight=`） | utime 增量 | 占单核 |
| --- | --- | --- | --- |
| 0 ×4 | 1048576 | 491 / 490 / 495 / 495 ticks | 约 49% |
| 10 ×2 | 112640 | 53 / 53 ticks | 约 5.3% |

权重比 1048576 : 112640 = 9.31，实测 CPU 时间比 9.30，总消耗 19.7 CPU·秒（2 核 × 10 秒）。这条链路和 `01_nice_effect` 在用户态测到的 69.7/22.8/7.5 是同一个东西，只是这里能看到内核里的那个 `weight` 字段本身。

**几个会浪费半天时间的地方**：

- `$lx_per_cpu("runqueues")` 在 6.12 + gdb 12.1 下抛 `'NoneType' object has no attribute 'cast'`，换成 `$lx_current()` 读 task_struct 字段就正常。
- `-O2` 下局部变量随时被优化掉，`next` 跨过赋值语句后 `p p->comm` 报 `value has been optimized out`。要在断点里看值，就在内核里插桩，别赌 gdb 的变量可见性。
- 只改 `fair.c` 一个文件的增量编译，在 2 核 + FUSE 文件系统的这台机器上花了 19 分 56 秒（`user` 时间只有 34 秒），瓶颈是 vmlinux 重链接和 gzip 的 I/O。本地 ext4 上通常 1-2 分钟，别拿这台机器的数字当通用结论。
- guest 里 `nice` 不存在（busybox 没编这个 applet），`taskset`/`renice` 也未必有。造负载要么自己静态编译小程序，要么在宿主机上用 `sched_setaffinity` 写进用户态实验里。

