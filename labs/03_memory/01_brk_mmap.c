/*
 * 01_brk_mmap.c —— malloc 的两个来源：brk 扩展堆，mmap 映射匿名段
 *
 * 内核视角：
 *   - brk 系统调用 -> sys_brk -> do_brk_flags：调整 mm->brk，
 *     在 [start_brk, brk] 里创建/扩展一个匿名 VMA（VM_DATA）。
 *     只是改 VMA 边界，不分配物理页；第一次读写才缺页。
 *   - mmap 系统调用 -> ksys_mmap_pgoff -> do_mmap -> mmap_region：
 *     创建一个独立 VMA（匿名或文件映射），同样不分配物理页。
 *   - glibc malloc 的策略：小于 mmap_threshold（默认 128KB，动态调整）走 brk/arena；
 *     大于阈值直接 mmap，free 时 munmap 还给内核，避免堆碎片。
 *   所以「malloc 之后 RSS 没涨」是正常的：虚拟内存和物理页是两件事。
 *
 * 本程序：分别 malloc 16B、100KB、8MB，打印地址并判断它落在哪个 VMA，
 * 然后再逐页写入，看 RSS 什么时候涨。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>

static long rss_kb(void)
{
    FILE *f = fopen("/proc/self/statm", "r");
    long size = 0, res = 0;
    if (f) { if (fscanf(f, "%ld %ld", &size, &res) != 2) res = 0; fclose(f); }
    return res * 4;
}

/* 在 /proc/self/maps 里找 addr 属于哪个 VMA，返回该行（截断到 100 字符） */
static void which_vma(void *addr, char *out, size_t outlen)
{
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    unsigned long a = (unsigned long)addr;
    snprintf(out, outlen, "(未找到)");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 && a >= lo && a < hi) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            snprintf(out, outlen, "%s", line);
            break;
        }
    }
    fclose(f);
}

int main(void)
{
    printf("初始 sbrk(0) = %p，RSS = %ld KB\n\n", sbrk(0), rss_kb());

    void *tiny = malloc(16);
    void *mid  = malloc(100 * 1024);      /* 100KB，低于默认 mmap 阈值 */
    void *big  = malloc(8u << 20);        /* 8MB，高于阈值 */
    if (!tiny || !mid || !big) { perror("malloc"); return 1; }

    char v[160];
    printf("=== 三个 malloc 的落点（只看虚拟地址，还没碰物理页）===\n");
    for (int i = 0; i < 3; i++) {
        void *p = i == 0 ? tiny : i == 1 ? mid : big;
        const char *name = i == 0 ? "malloc(16)" : i == 1 ? "malloc(100KB)" : "malloc(8MB)";
        which_vma(p, v, sizeof v);
        printf("%-14s addr=%p\n    %s\n", name, p, v);
    }
    printf("sbrk(0) 现在 = %p，RSS = %ld KB\n", sbrk(0), rss_kb());
    printf("\n注意 8MB 那块不在 [heap] 里，而是一段独立的匿名映射（glibc 直接 mmap）。\n\n");

    printf("=== 逐页写入 8MB，观察 RSS ===\n");
    long before = rss_kb();
    for (size_t off = 0; off < (8u << 20); off += 4096)
        ((char *)big)[off] = 1;
    long after = rss_kb();
    printf("写入前 RSS=%ld KB，写入后 RSS=%ld KB，增长 %ld KB（≈ 8MB）\n",
           before, after, after - before);
    printf("\n=== 释放大块后再看 ===\n");
    free(big);
    printf("free(8MB) 后 RSS=%ld KB（munmap 把页直接还给内核）\n", rss_kb());

    printf("\n内核里对应的两个入口：\n");
    printf("  sys_brk           -> do_brk_flags -> vma 合并/扩展\n");
    printf("  ksys_mmap_pgoff   -> do_mmap -> mmap_region -> vma_link\n");
    printf("物理页的分配一律推迟到缺页：handle_mm_fault -> do_anonymous_page。\n");

    free(tiny); free(mid);
    return 0;
}
