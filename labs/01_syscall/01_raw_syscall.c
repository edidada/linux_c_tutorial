/*
 * 01_raw_syscall.c —— 不经过 glibc 包装，直接用 syscall 指令进内核
 *
 * 目的：
 *   1. 亲手执行 syscall 指令，确认 ABI：rax=调用号，rdi/rsi/rdx/r10/r8/r9 传参，
 *      返回值在 rax；内核返回 -errno 用负数表示。
 *   2. 对比 glibc 包装：它把负返回值转成 -1 并设置 errno，还会在参数不符合 ABI 时
 *      自己返回 EINVAL（例如 mmap 的长度非法时并不真的进内核）。
 *   3. 对比 vDSO：clock_gettime/gettimeofday 在 glibc 里优先走 vDSO（用户态内存页，
 *      由内核在 exec 时映射），完全不进内核，所以比真系统调用快一个数量级。
 *
 * 观察点：
 *   - 用本目录的 mini_strace 跟踪它，或 strace -c 统计。
 *   - 内核侧对应 arch/x86/entry/syscalls/syscall_64.tbl 里的编号，
 *     以及 SYSCALL_DEFINE3(write, ...) 展开后的 __x64_sys_write。
 *
 * 编译： make  或  gcc -O2 -g -o raw_syscall 01_raw_syscall.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/syscall.h>

/* x86-64 调用号（只列本文件用到的，完整表见 syscall_64.tbl） */
enum {
    NR_READ = 0, NR_WRITE = 1, NR_OPEN = 2, NR_CLOSE = 3,
    NR_GETPID = 39, NR_CLOCK_GETTIME = 228,
};

/* 三参数原始系统调用。注意：syscall 指令会破坏 rcx、r11（保存 rip/rflags），
 * 所以 clobber 列表里必须写上它们，否则编译器可能把值放在这两个寄存器里。 */
static inline long raw3(long nr, long a1, long a2, long a3)
{
    register long rax __asm__("rax") = nr;
    register long rdi __asm__("rdi") = a1;
    register long rsi __asm__("rsi") = a2;
    register long rdx __asm__("rdx") = a3;
    __asm__ volatile("syscall"
                     : "+r"(rax)
                     : "r"(rdi), "r"(rsi), "r"(rdx)
                     : "rcx", "r11", "memory");
    return rax;
}

static volatile long sink;

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* 把一段代码跑 N 次，返回单次耗时（纳秒）。参数是函数指针，避免循环被优化掉。 */
static double bench(long (*fn)(void *), void *arg, long n)
{
    double t0 = now_ns();
    for (long i = 0; i < n; i++)
        sink += fn(arg);
    return (now_ns() - t0) / (double)n;
}

static long f_empty(void *arg)          { sink += (long)(intptr_t)arg; return 0; }
static long f_raw_getpid(void *arg)     { (void)arg; return raw3(NR_GETPID, 0, 0, 0); }
static long f_libc_getpid(void *arg)    { (void)arg; return getpid(); }
static long f_libc_clock(void *arg)     { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_nsec; }
static long f_raw_clock(void *arg)      { struct timespec ts; return raw3(NR_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&ts, 0); }
static long f_libc_gettimeofday(void *a){ struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_usec; }

int main(void)
{
    printf("=== 1. 同一个 getpid 的两条路径 ===\n");
    printf("libc getpid()      = %d\n", getpid());
    printf("raw  syscall 39    = %ld\n", raw3(NR_GETPID, 0, 0, 0));
    printf("内核里 tsk->pid 就是这两个值来源；区别只在调用路径。\n\n");

    printf("=== 2. 原始 write 与 glibc write ===\n");
    const char *msg = "raw write: hello from syscall instruction\n";
    long r = raw3(NR_WRITE, 1 /*stdout*/, (long)msg, (long)strlen(msg));
    printf("raw write 返回 %ld（写入字节数；若为负则是 -errno）\n", r);
    write(1, "glibc write: hello\n", 19);
    printf("\n");

    printf("=== 3. 错误返回约定：负数 vs errno ===\n");
    errno = 0;
    long bad_raw = raw3(NR_OPEN, (long)"/definitely/not/here", O_RDONLY, 0);
    printf("raw open 不存在的文件 -> %ld  (= -ENOENT = -%d)\n", bad_raw, ENOENT);
    errno = 0;
    int bad_libc = open("/definitely/not/here", O_RDONLY);
    printf("libc open 同一个文件   -> %d, errno=%d (%s)\n", bad_libc, errno, strerror(errno));
    printf("这说明 errno 不是内核写的，是 glibc 包装函数在用户态设置的。\n\n");

    printf("=== 4. 一次系统调用到底多贵（本机实测，单位 ns）===\n");
    const long N = 200000;
    double e  = bench(f_empty, (void *)1, N);
    double g  = bench(f_raw_getpid, NULL, N);
    double gl = bench(f_libc_getpid, NULL, N);
    double c1 = bench(f_libc_clock, NULL, N);
    double c2 = bench(f_raw_clock, NULL, N);
    double tv = bench(f_libc_gettimeofday, NULL, N);

    printf("%-34s %10.1f\n", "空循环（被测函数本身的调用开销）", e);
    printf("%-34s %10.1f\n", "raw syscall getpid（必然进内核）", g);
    printf("%-34s %10.1f\n", "libc getpid（仍是同一个系统调用）", gl);
    printf("%-34s %10.1f\n", "libc clock_gettime（通常走 vDSO）", c1);
    printf("%-34s %10.1f\n", "raw syscall clock_gettime（真进内核）", c2);
    printf("%-34s %10.1f\n", "libc gettimeofday（通常走 vDSO）", tv);
    printf("\n解读：vDSO 版本不产生 syscall，所以明显更快；\n");
    printf("raw 与 libc 的 getpid 只差一点，说明 libc 包装层本身几乎不花钱，\n");
    printf("真正的成本是 syscall 指令触发的用户态/内核态切换（entry_SYSCALL_64 那一段）。\n");
    return (int)(sink & 0);
}
