# gdb.cmd —— 连上 QEMU 的 gdbstub，停在 start_kernel
#
# 用法：
#   终端 1: ./run.sh          （会卡住，等 gdb）
#   终端 2: gdb -x gdb.cmd
#
# 常用 lx- 命令来自内核自带的 scripts/gdb：lx-ps、lx-dmesg、lx-lsmod、
# lx-symbols、lx-current。若提示未定义，手动 source 一次 vmlinux-gdb.py（见下）。

set pagination off
set confirm off
set print pretty on
set print elements 200

# 路径按 setup.sh 的默认版本号改
set $kdir = "./build/linux-6.12"
file $kdir/vmlinux

target remote :1234

# 内核自带的 gdb 辅助脚本（lx- 前缀命令）
source $kdir/vmlinux-gdb.py

# KASLR 已在 config 里关掉，地址从链接时就是固定的
hbreak start_kernel
continue

# 到这里已经进入 start_kernel。接下来可以：
#   lx-dmesg                查看 printk 环形缓冲
#   hbreak kernel_init      看 init 进程怎么创建
#   lx-ps                   遍历 task 链表（对应 for_each_process）
#   bt                      看调用栈是不是 start_kernel -> ... -> kernel_init
#
# 想单步跟调度器，换用 sched.gdb：
#   gdb -x sched.gdb
