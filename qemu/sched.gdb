# sched.gdb —— 跟调度器的最小可用脚本
#
# 终端 1: ./run.sh              （-s -S，CPU 停在第一条指令）
# 终端 2: gdb -x sched.gdb
# 终端 3: 等 guest 起 shell 后敲命令（每次回车都会因为 wait4 睡眠触发一次调度）
#
# 说明：不同内核版本的局部变量名不同，脚本里只下断点、不写花哨的自动打印，
# 命中后手工看，比背一堆可能失效的表达式更有用。

set pagination off
set confirm off
set $kdir = "./build/linux-6.12"
file $kdir/vmlinux
target remote :1234
source $kdir/vmlinux-gdb.py

hbreak start_kernel
continue
delete

# 等 init 起来（kernel_init 是 PID 1 的前身）
tbreak kernel_init
continue
delete

hbreak __schedule
hbreak pick_next_task_fair
hbreak task_tick_fair
hbreak try_to_wake_up
hbreak finish_task_switch

continue

# 命中后的建议动作：
#
#   bt                      看这次调度是谁触发的：
#                             syscall 返回路径 / 中断返回路径 / 主动 schedule()
#   p current->comm         老内核有 current 符号；新内核用 per-cpu 变量
#   p $lx_per_cpu("current_task")->comm     6.x 下的写法
#   p $lx_per_cpu("runqueues").nr_running   当前 CPU 就绪队列长度
#   p $lx_per_cpu("runqueues").cfs.h_nr_running
#
# 想跟一次完整的切换：在 __schedule 命中后
#   finish                  回到调用者（确认是 preempt 还是 block）
#   stepi / next             单步走 pick_next_task -> context_switch
#   disassemble /m           C 源码和汇编对照
#
# 看不到源码时检查：
#   show debug-file-directory
#   p $kdir/vmlinux 是否被 file 命令正确加载（info files）
