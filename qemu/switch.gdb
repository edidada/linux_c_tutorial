# switch.gdb —— 走完一次完整切换：__schedule 挑人 → __switch_to 换栈 → finish_task_switch 收尾
#
# 用法：
#   终端 1: ./run.sh                 （-s -S，CPU 停在第一条指令）
#   终端 2: gdb -q -batch -x switch.gdb
#
# 为什么断在 __switch_to 而不是 __schedule：__schedule 里 prev/next 是局部变量，
# -O2 下断在函数入口时还没赋值；而 __switch_to(prev_p, next_p) 是真实函数，
# 两个参数一定能打出来，且它就是 switch_to 宏里真正换内核栈的那一步。

set pagination off
set confirm off
file ./build/linux-6.12/vmlinux
target remote :1234
source ./build/linux-6.12/vmlinux-gdb.py

hbreak start_kernel
continue
delete
tbreak kernel_init
continue
delete

hbreak __switch_to
continue
echo "=== 命中 __switch_to（上下文真正换栈的地方）===\n"
bt 4
p prev_p->comm
p prev_p->pid
p next_p->comm
p next_p->pid
p prev_p->__state
p next_p->__state
p prev_p->thread.sp
p next_p->thread.sp
p/x prev_p->thread.fsbase
p/x next_p->thread.fsbase
x/12i $pc

hbreak finish_task_switch
continue
echo "=== 命中 finish_task_switch（新任务已经在跑了）===\n"
bt 4
p prev->comm
p prev->pid
p $lx_current().comm
p $lx_current().pid
p $lx_current().__state
detach
quit

# 实测（6.12，开机时 kernel_init 让位给 kthreadd 的一次切换）：
#
#   __switch_to(prev_p=0xffff888003598000, next_p=0xffff888003598e80)
#   prev_p->pid = 1（kernel_init）   next_p->pid = 2（kthreadd）
#   两个 comm 都是 "swapper/0" —— fork 出来的任务继承父进程的 comm，
#   所以 comm 不能用来区分任务，认 pid。
#
#   prev_p->thread.sp = 0xffffc9000001...   next_p->thread.sp = 0xffffc9000003...
#   这是内核栈切换的落点：__switch_to 末尾把 next 的 sp 交给 next 的返回路径，
#   ret 一弹就跑到 next 上次停下的地方。
#
#   finish_task_switch 的 bt 显示栈底是 ret_from_fork(fn=kthreadd)：
#   新任务不是「跳」进去的，而是被伪装成刚从 fork 系统调用返回，从
#   ret_from_fork_asm 开始跑。这就是 fork 出来的线程第一次被调度的样子。
#
#   两个 task 的 thread.fsbase 都是 0：内核线程没有用户态 TLS。
