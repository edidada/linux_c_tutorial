# sched-live.gdb —— 对「已经启动好」的 guest 直接下断点，批量打印调度事件
#
# 与 sched.gdb 的区别：sched.gdb 假设 CPU 还停在 -S（刚开机），要先过 start_kernel。
# 这个脚本只连上去设 __schedule 断点，适用于：
#   终端 1: GDB_WAIT=0 ./run.sh     （不进 -S，booting 正常跑）
#   终端 2: gdb -q -batch -x sched-live.gdb
#
# 打印 20 次调度后自动 detach 退出，guest 继续跑（不会把 VM 卡死在断点上）。

set pagination off
set confirm off
file ./build/linux-6.12/vmlinux
target remote :1234
source ./build/linux-6.12/vmlinux-gdb.py

set $n = 0
hbreak __schedule
commands
  silent
  set $n = $n + 1
  printf "[%02d] mode=%-2d prev=%-14s pid=%-5d state=%ld vruntime=%lld nvcsw=%ld\n", $n, sched_mode, $lx_current().comm, $lx_current().pid, $lx_current().__state, (long long)$lx_current().se.vruntime, $lx_current().nvcsw
  if $n >= 20
    detach
    quit
  end
  continue
end
continue

# mode 的取值（kernel/sched/core.c:6504）：
#   -1 SM_IDLE    schedule_idle()，idle 线程让出
#    0 SM_NONE    显式 schedule()，任务自己阻塞
#    1 SM_PREEMPT preempt_schedule_common() / __cond_resched，被别人顶掉
#    2 SM_RTLOCK_WAIT  PREEMPT_RT 的 rtlock 等待
#
# 注意：6.12 + gdb 12.1 下 $lx_per_cpu("runqueues") 会抛
#   'NoneType' object has no attribute 'cast'
# 所以这里只读 task_struct 字段（$lx_current()），不碰 per-cpu 变量。
