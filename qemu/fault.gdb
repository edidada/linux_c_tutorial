# fault.gdb —— 在 QEMU 里数匿名缺页：每写一页就命中一次 do_anonymous_page
#
# 用法：
#   终端 1: ./run.sh                  （-s -S）
#   终端 2: gdb -q -batch -x fault.gdb
#   终端 1 的 guest 里：/bin/touchmem  （静态编译后放进 initramfs）
#
# 预期：touchmem 报 minflt=1024（mmap 4MB / 4K），这里 do_anonymous_page 正好
# 命中 1024 次覆盖这块映射，随后几十次属于 shell/init，可以忽略。
# 命中地址按 0x1000 线性递增，VMA 的 [start,end) 与 /proc/<pid>/maps 对得上。

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
    printf "[%d] addr=0x%lx vma=[0x%lx,0x%lx) len=0x%lx flags=0x%lx\n", $n, vmf->address, vmf->vma->vm_start, vmf->vma->vm_end, vmf->vma->vm_end - vmf->vma->vm_start, vmf->vma->vm_flags
  end
  if $n == 1024
    printf "=== 第 1024 次命中 addr=0x%lx，正好覆盖 4MB 映射 ===\n", vmf->address
  end
  if $n >= 1050
    printf "TOTAL do_anonymous_page hits = %d\n", $n
    detach
    quit
  end
  continue
end
continue

# 实测（6.12，guest 里跑 touchmem，mmap 4MB）：
#
#   touchmem: 首轮 getrusage minflt = 1024（每页 1.00 次）
#   [1] addr=0x7fffffffe000 vma=[0x7fffffffe000,0x7ffffffff000) len=0x1000  flags=0x118173
#   [2] addr=0x7ffcdeb6a000 vma=[0x7ffcdeb4b000,0x7ffcdeb6c000) len=0x21000 flags=0x100173
#   [3] addr=0x2a747000     vma=[0x2a747000,0x2a748000)         len=0x1000  flags=0x100073
#   === 第 1024 次命中 addr=0x7f2fe5b38000，正好覆盖 4MB 映射 ===
#   TOTAL do_anonymous_page hits = 1050
#
# flags 解读：
#   0x118173 = READ|WRITE|GROWSDOWN|ACCOUNT（+madvise 留下的 SEQ/RAND 位），栈顶 VMA
#   0x100173 = READ|WRITE|GROWSDOWN|ACCOUNT，栈向下扩展出来的 0x21000 区间
#   0x100073 = READ|WRITE|ACCOUNT，没有 GROWSDOWN：堆或单页匿名映射
#
# 注意：gdb 会在 Thread 1.1 / 1.2 之间切换——两个 vCPU 都在缺页，断在哪核就
# 停在哪个线程，是 smp 下正常现象。用 $n 自己收尾，别让 batch 模式一直卡住 guest。
