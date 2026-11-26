/*
 * fork_cow.c —— fork 到底复制了什么？写时复制什么时候才真复制？
 *
 * 内核视角（kernel/fork.c）：
 *   fork() -> _do_fork/kernel_clone -> copy_process()
 *     dup_task_struct()   复制 task_struct（几百字节，新内核栈）
 *     copy_mm()           复制 mm_struct，页表项逐个标记只读并清 COW 位（copy_page_range）
 *     copy_files()        fdtable 引用计数 +1，不复制文件对象
 *     copy_fs()           cwd/root/umask 引用计数 +1
 *     sched_fork()        设置状态、初始化调度实体，放进 runqueue
 *   父进程从 kernel_clone 返回子 pid，子进程从 ret_from_fork 返回 0。
 *   所以 fork 的成本是「复制页表 + 复制元数据」，不是「复制内存内容」。
 *
 * 本程序做的事：
 *   1. 打印全局区/堆/栈的地址，对照进程地址空间布局；
 *   2. 分配 64MB 但不写，fork 前后看 RSS —— 数字几乎不变；
 *   3. 子进程逐页写入，RSS 才涨上来，同时测「写 64MB」的耗时；
 *   4. 用管道把子进程的 RSS 和耗时带回父进程打印。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/wait.h>

#define ALLOC_MB 64

static char g_buf[4096];                 /* .bss */

static long rss_pages(pid_t pid)
{
    char path[64], buf[256];
    long size, resident;
    snprintf(path, sizeof path, "/proc/%d/statm", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (fscanf(f, "%ld %ld", &size, &resident) != 2) resident = -1;
    fclose(f);
    return resident;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(void)
{
    int stack_var = 0;
    char *heap = malloc(64);
    char *big = malloc((size_t)ALLOC_MB << 20);
    if (!heap || !big) { perror("malloc"); return 1; }

    printf("=== 1. 同一进程里的三类地址 ===\n");
    printf("全局 .bss  : %p\n", (void *)g_buf);
    printf("堆 (malloc): %p\n", (void *)heap);
    printf("大块 malloc: %p\n", (void *)big);
    printf("栈         : %p\n", (void *)&stack_var);
    printf("（地址从低到高大致是 text/rodata -> data/bss -> heap -> mmap -> stack）\n\n");

    printf("父进程 fork 前 RSS = %ld 页 (%ld KB)\n",
           rss_pages(getpid()), rss_pages(getpid()) * 4);

    int p2c[2], c2p[2];
    if (pipe(p2c) || pipe(c2p)) { perror("pipe"); return 1; }

    double t0 = now_ms();
    pid_t child = fork();
    double fork_ms = now_ms() - t0;

    if (child < 0) { perror("fork"); return 1; }

    if (child == 0) {
        close(p2c[1]); close(c2p[0]);
        char c;
        read(p2c[0], &c, 1);             /* 等父进程先读一次 RSS */

        long rss_after_fork = rss_pages(getpid());

        /* 逐页写入，每一页第一次写都会触发一次 COW 缺页：分配新页 + 拷贝 4KB + 改 PTE */
        double t1 = now_ms();
        for (size_t off = 0; off < ((size_t)ALLOC_MB << 20); off += 4096)
            big[off] = (char)off;
        double touch_ms = now_ms() - t1;

        long rss_after_touch = rss_pages(getpid());
        dprintf(c2p[1], "%ld %ld %.3f", rss_after_fork, rss_after_touch, touch_ms);
        _exit(0);
    }

    close(p2c[0]); close(c2p[1]);
    long rss_fork = rss_pages(child);
    write(p2c[1], "x", 1);               /* 放行子进程 */

    long rf = 0, rt = 0;
    double touch_ms = 0;
    char buf[128];
    ssize_t n = read(c2p[0], buf, sizeof buf - 1);
    if (n > 0) { buf[n] = 0; sscanf(buf, "%ld %ld %lf", &rf, &rt, &touch_ms); }

    wait(NULL);

    printf("\n=== 2. fork 本身 ===\n");
    printf("fork 耗时              : %.3f ms（64MB 地址空间，只复制页表）\n", fork_ms);
    printf("父进程读到的子进程 statm（仅供参考）: %ld 页\n", rss_fork);

    printf("\n=== 3. 子进程逐页写入 64MB ===\n");
    printf("写入前 RSS             : %ld 页 (%ld KB)\n", rf, rf * 4);
    printf("写入后 RSS             : %ld 页 (%ld KB)\n", rt, rt * 4);
    printf("写入耗时               : %.3f ms（≈ %ld 次 COW 缺页）\n",
           touch_ms, (long)(((size_t)ALLOC_MB << 20) / 4096));
    printf("\n结论：fork 便宜是因为没有复制数据；贵的那部分被推迟到第一次写，\n");
    printf("每次写缺页在内核里走 do_user_addr_fault -> wp_page_copy，\n");
    printf("分配一个新页、拷贝旧页 4KB、把 PTE 改成可写。\n");

    free(heap); free(big);
    return 0;
}
