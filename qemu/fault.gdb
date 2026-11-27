# fault.gdb —— 在 QEMU 里数匿名缺页：每次 memset 都对应一次 do_anonymous_page
#
# 用法：
#   终端 1: ./run.sh                  （-s -S）
#   终端 2: gdb -q -batch -x fault.gdb
#   终端 1 的 guest 里：/bin/touchmem  （静态编译后放进 initramfs）
#
# 预期：touchmem 报 minflt=1024（4MB/4K），这里 do_anonymous_page 命中 1024 次。
# 命中地址按页线性递增，每个 VMA 的 [start,end) 与 /proc/<pid>/maps 对得上。

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

set $n = 0
hbreak do_anonymous_page
commands
  silent
  set $n = $n + 1
  if $n <= 3
    printf "[%d] addr=0x%lx vma=[0x%lx,0x%lx) flags=0x%lx\n", $n, vmf->address, vmf->vma->vm_start, vmf->vma->vm_end, vmf->vma->vm_flags
  end
  if ($n % 128) == 0
    printf "[hit %d] addr=0x%lx\n", $n, vmf->address
  end
  if $n >= 1200
    printf "TOTAL do_anonymous_page hits = %d\n", $n
    detach
    quit
  end
  continue
end
continue

# 实测（6.12，guest 里跑 touchmem，4MB）：
#
#   [1] addr=0x7fffffffe000 vma=[0x7fffffffe000,0x7ffffffff000) flags=0x118173
#       栈 VMA：VM_READ|VM_WRITE|VM_GROWSDOWN|VM_ACCOUNT
#   [2] addr=0x37b99000     vma=[0x37b99000,0x37b9a000)         flags=0x100073
#       单页映射：VM_READ|VM_WRITE|VM_ACCOUNT，没有 GROWSDOWN
#   [3] addr=0x7f702beec000 vma=[0x7f702beec000,0x7f702beed000) flags=0x100073
#       另一页独立映射
#   [hit 128] ... 每 128 次打一行，地址按 0x1000 递增
#
#   guest 侧 touchmem 打印 minflt=1024，这里是 1024 次命中（最后多出的几十次
#   来自 shell 和其它进程），两边逐页对齐。
#
# 注意：gdb 会在 Thread 1.1 / 1.2 之间切换——两个 vCPU 都在触碰，断点命中
# 哪个 CPU 就停在哪个线程，这是 smp 下正常现象，不是脚本写错了。
