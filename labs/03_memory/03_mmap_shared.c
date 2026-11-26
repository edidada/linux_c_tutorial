/*
 * 03_mmap_shared.c —— MAP_SHARED / MAP_PRIVATE 与 page cache 的关系
 *
 * 内核视角：
 *   - MAP_SHARED 文件映射：VMA 的 vm_file 指向同一个 file，缺页时走 filemap_fault，
 *     PTE 直接指向 page cache 里的页。多个进程映射同一文件，看到的是同一批物理页
 *     （page cache 是全局的，inode->i_mapping 的 xarray 里存）。
 *     写这种页会标 dirty，由 writeback 或 msync 写回磁盘。
 *   - MAP_PRIVATE 文件映射：缺页时也读 page cache，但 PTE 是只读的；
 *     一旦写就 COW 出私有副本，改动只在本进程可见，不会回到 page cache，也不会落盘。
 *     注意：私有映射并不隔离别人对同一文件的修改——别的进程通过 SHARED 映射
 *     改了 page cache，你下次缺页看到的仍是新内容。私有只保护「你自己的写」。
 *   - fork 之后 mm 是复制品，VMA 仍指向同一个 file，所以 SHARED 区域天然跨进程可见。
 *
 * 本程序用两个独立文件，避免上面那个「互相污染」的干扰：
 *   A. shared.bin  做 MAP_SHARED，子进程写，父进程读 -> 能看到；
 *   B. private.bin 做 MAP_PRIVATE，子进程写，父进程读 -> 看不到，磁盘也不变。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>

#define LEN 4096

static int make_file(const char *path)
{
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); exit(1); }
    if (ftruncate(fd, LEN) < 0) { perror("ftruncate"); exit(1); }
    if (pwrite(fd, "init", 4, 0) < 0) { perror("pwrite"); exit(1); }
    return fd;
}

int main(void)
{
    const char *sp = "/tmp/linux_c_tutorial_shared.bin";
    const char *pp = "/tmp/linux_c_tutorial_private.bin";
    int sfd = make_file(sp);
    int pfd = make_file(pp);

    char *sh = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0);
    char *pr = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_PRIVATE, pfd, 0);
    if (sh == MAP_FAILED || pr == MAP_FAILED) { perror("mmap"); return 1; }

    printf("映射地址: MAP_SHARED=%p  MAP_PRIVATE=%p\n", sh, pr);
    printf("初始内容: sh=\"%s\"  pr=\"%s\"\n\n", sh, pr);

    fflush(stdout);
    pid_t child = fork();
    if (child == 0) {
        strcpy(sh, "child wrote SHARED");
        strcpy(pr, "child wrote PRIVATE");
        printf("子进程写完两个映射，直接退出（没有 msync）\n");
        fflush(stdout);
        _exit(0);
    }
    waitpid(child, NULL, 0);

    printf("\n子进程退出后，父进程看到:\n");
    printf("  MAP_SHARED  = \"%s\"   <- 同一个 page cache 页，改动立即可见\n", sh);
    printf("  MAP_PRIVATE = \"%s\"   <- 子进程 COW 出的私有页，父进程看不到\n\n", pr);

    msync(sh, LEN, MS_SYNC);
    printf("msync(MAP_SHARED) 之后的磁盘内容: ");
    fflush(stdout);
    char cmd[256];
    snprintf(cmd, sizeof cmd, "head -c 32 %s; echo", sp);
    if (system(cmd) != 0) perror("system");

    printf("private.bin 的磁盘内容（应该还是 init）: ");
    fflush(stdout);
    snprintf(cmd, sizeof cmd, "head -c 32 %s; echo", pp);
    if (system(cmd) != 0) perror("system");

    printf("\n内核路径：filemap_fault（缺页）-> page cache 页；\n");
    printf("SHARED 的 PTE 可写且指向 cache 页，脏页由 writeback 线程按 dirty_expire_centisecs 回写；\n");
    printf("PRIVATE 的写走 do_wp_page，复制一页，vm_file 只用于读。\n");
    printf("\n可以再做一个实验：两个进程分别 mmap 同一个文件，一个 SHARED 一个 PRIVATE，\n");
    printf("让 SHARED 那边先写，再看 PRIVATE 那边重新缺页后读到的是不是新内容（会是）。\n");

    munmap(sh, LEN); munmap(pr, LEN);
    close(sfd); close(pfd);
    unlink(sp); unlink(pp);
    return 0;
}
