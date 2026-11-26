/*
 * 02_page_fault.c —— 缺页异常：minor fault 与 major fault 的区别
 *
 * 内核视角（arch/x86/mm/fault.c，do_user_addr_fault）：
 *   1. 从 CR2 取故障地址，在 mm->mmap 里找到 VMA；找不到 -> SIGSEGV。
 *   2. 检查权限：VMA 只读却要写 -> COW 或 SIGSEGV。
 *   3. handle_mm_fault() -> __handle_mm_fault -> 按 PTE 状态分路：
 *        do_anonymous_page()   匿名页第一次访问：分配零页或新页，minor fault
 *        do_wp_page()          COW：复制一页，minor fault
 *        do_fault() -> filemap_fault()  文件页不在 page cache：读磁盘，major fault
 *   4. 更新 PTE，返回用户态重新执行那条指令。
 *
 * getrusage 的 minflt/majflt 就是这两个计数器（task_struct->min_flt/maj_flt），
 * 可以在 /proc/<pid>/stat 的第 10、12 字段看到同样的值。
 *
 * 本程序三段：
 *   A. mmap 256MB 匿名内存，用 MADV_NOHUGEPAGE 关掉透明大页，逐页写 -> minflt ≈ 页数；
 *   B. 打开一个 32MB 文件，MADV_RANDOM 关掉预读，读每一页 -> majflt ≈ 读到的页数；
 *   C. 再读一遍同样的文件（页已在 page cache）-> 没有 major fault。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>

#define ANON_MB  256
#define FILE_MB  32

static long minflt(void) { struct rusage ru; getrusage(RUSAGE_SELF, &ru); return ru.ru_minflt; }
static long majflt(void) { struct rusage ru; getrusage(RUSAGE_SELF, &ru); return ru.ru_majflt; }

int main(void)
{
    printf("=== A. 匿名页：第一次写触发 minor fault ===\n");
    char *anon = mmap(NULL, (size_t)ANON_MB << 20, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (anon == MAP_FAILED) { perror("mmap anon"); return 1; }
    madvise(anon, (size_t)ANON_MB << 20, MADV_NOHUGEPAGE);   /* 让计数按 4KB 算 */

    long m0 = minflt(), M0 = majflt();
    for (size_t off = 0; off < ((size_t)ANON_MB << 20); off += 4096)
        anon[off] = (char)off;
    long dm = minflt() - m0, dM = majflt() - M0;

    printf("写入 %d MB（%d 页），minflt +%ld，majflt +%ld\n", ANON_MB,
           ANON_MB * 256, dm, dM);
    printf("每页一次 minor fault，内核只是分配一个清零页并填 PTE，不读磁盘。\n\n");

    printf("=== B. 文件页：page cache 未命中触发 major fault ===\n");
    const char *path = "/tmp/linux_c_tutorial_pf.bin";
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); return 1; }
    if (ftruncate(fd, (off_t)FILE_MB << 20) < 0) { perror("ftruncate"); return 1; }
    /* 先把数据真的写进去（否则读到的是稀疏文件的零页） */
    for (size_t off = 0; off < ((size_t)FILE_MB << 20); off += 4096)
        if (pwrite(fd, "x", 1, off) < 0) { perror("pwrite"); return 1; }
    fsync(fd);

    /* 主动把这些页从 page cache 里踢掉，制造「冷缓存」 */
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);

    char *fm = mmap(NULL, (size_t)FILE_MB << 20, PROT_READ, MAP_PRIVATE, fd, 0);
    if (fm == MAP_FAILED) { perror("mmap file"); return 1; }
    madvise(fm, (size_t)FILE_MB << 20, MADV_RANDOM);       /* 关掉预读，一次一页 */

    m0 = minflt(); M0 = majflt();
    volatile char sink = 0;
    for (size_t off = 0; off < ((size_t)FILE_MB << 20); off += 4096)
        sink ^= fm[off];
    dm = minflt() - m0; dM = majflt() - M0;
    printf("冷缓存读 %d MB，minflt +%ld，majflt +%ld（major 就是要等磁盘 I/O 的那些）\n",
           FILE_MB, dm, dM);

    printf("\n=== C. 热缓存再读一遍 ===\n");
    munmap(fm, (size_t)FILE_MB << 20);
    fm = mmap(NULL, (size_t)FILE_MB << 20, PROT_READ, MAP_PRIVATE, fd, 0);
    madvise(fm, (size_t)FILE_MB << 20, MADV_RANDOM);
    m0 = minflt(); M0 = majflt();
    for (size_t off = 0; off < ((size_t)FILE_MB << 20); off += 4096)
        sink ^= fm[off];
    dm = minflt() - m0; dM = majflt() - M0;
    printf("热缓存读 %d MB，minflt +%ld，majflt +%ld（页已在 page cache，无需 I/O）\n",
           FILE_MB, dm, dM);
    printf("\nsink=%d（防止循环被优化掉）\n", sink);

    munmap(fm, (size_t)FILE_MB << 20);
    munmap(anon, (size_t)ANON_MB << 20);
    close(fd);
    unlink(path);
    return 0;
}
