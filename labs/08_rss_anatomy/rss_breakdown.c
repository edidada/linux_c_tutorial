/*
 * rss_breakdown.c —— 把「RSS 很高」拆开：anon / file / shmem 各占多少
 *
 * 对应内核路径：
 *   anon 写内存        -> do_anonymous_page -> anon_vma，记 Anonymous
 *   读文件映射         -> filemap_fault / page cache，记 file-backed Rss
 *   同一文件再共享映射 -> 同一批 page cache page 被两个 PTE 指到，RSS 重复计
 *   COW 写私有映射     -> wp_page_copy，file 页变 anon 页
 *
 * 数据来自 /proc/self/smaps_rollup（内核遍历自身 VMA 汇总），不是自己估算。
 * 注意：smaps_rollup 没有单列 File，file/shmem-backed = Rss - Anonymous。
 *
 * 用法： ./rss_breakdown            （临时文件写在 /tmp，退出时删掉）
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define ANON_BYTES   (8UL  << 20)
#define FILE_BYTES   (64UL << 20)
#define COW_BYTES    (16UL << 20)
#define PAGE         4096UL

struct roll {
	long rss, pss, anon, file, priv_dirty, vm_rss;
};

static void read_roll(struct roll *r)
{
	FILE *f = fopen("/proc/self/smaps_rollup", "r");
	char line[256];

	memset(r, 0, sizeof(*r));
	if (!f) { perror("smaps_rollup"); exit(1); }
	while (fgets(line, sizeof(line), f)) {
		long v;
		if      (sscanf(line, "Rss: %ld kB", &v) == 1)           r->rss = v;
		else if (sscanf(line, "Pss: %ld kB", &v) == 1)           r->pss = v;
		else if (sscanf(line, "Anonymous: %ld kB", &v) == 1)     r->anon = v;
		else if (sscanf(line, "Private_Dirty: %ld kB", &v) == 1) r->priv_dirty = v;
	}
	r->file = r->rss - r->anon;   /* 剩下的都是 file/shmem-backed */
	fclose(f);

	f = fopen("/proc/self/status", "r");
	while (f && fgets(line, sizeof(line), f))
		if (sscanf(line, "VmRSS: %ld kB", &r->vm_rss) == 1)
			break;
	if (f) fclose(f);
}

static void row(const char *stage, struct roll *r)
{
	printf("%-22s VmRSS=%7ld  Rss=%7ld  Anon=%7ld  File=%7ld  PrivDirty=%7ld  Pss=%7ld\n",
	       stage, r->vm_rss, r->rss, r->anon, r->file, r->priv_dirty, r->pss);
}

int main(void)
{
	struct roll r;
	char path[] = "/tmp/rss_breakdown_XXXXXX";
	char page[PAGE];
	int fd = mkstemp(path);
	unsigned long i;
	volatile char sink = 0;
	char *anon, *priv, *shm;

	if (fd < 0) { perror("mkstemp"); return 1; }

	/* 先写 64MB 真实数据：这些页进 page cache，不进本进程 RSS */
	memset(page, 'A', sizeof(page));
	for (i = 0; i < FILE_BYTES / PAGE; i++)
		if (write(fd, page, PAGE) != (ssize_t)PAGE) { perror("write"); return 1; }
	fsync(fd);

	read_roll(&r);
	row("1_初始", &r);

	/* anon：malloc + 逐页触碰，每页一次缺页 */
	anon = malloc(ANON_BYTES);
	if (!anon) { perror("malloc"); return 1; }
	memset(anon, 7, ANON_BYTES);
	read_roll(&r);
	row("2_anon8MB写完", &r);

	/* 私有文件映射：页来自 page cache，计入 Rss 但算 file-backed */
	priv = mmap(NULL, FILE_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	if (priv == MAP_FAILED) { perror("mmap priv"); return 1; }
	for (i = 0; i < FILE_BYTES; i += PAGE) sink += priv[i];
	read_roll(&r);
	row("3_私有映射64MB读完", &r);

	/* 同一批页再共享映射一次：两个 PTE 指同一物理页，RSS 重复计，Pss 折半 */
	shm = mmap(NULL, FILE_BYTES, PROT_READ, MAP_SHARED, fd, 0);
	if (shm == MAP_FAILED) { perror("mmap shared"); return 1; }
	for (i = 0; i < FILE_BYTES; i += PAGE) sink += shm[i];
	read_roll(&r);
	row("4_再加共享映射", &r);

	/* 写私有映射的前 16MB：wp_page_copy，file 页变 anon 页 */
	for (i = 0; i < COW_BYTES; i += PAGE) priv[i] = (char)i;
	read_roll(&r);
	row("5_COW写16MB", &r);

	printf("\nRSS 涨得最多的是第 3 步的 file-backed，堆(anon)只有 %lu MB；\n",
	       ANON_BYTES >> 20);
	printf("第 4 步同一批物理页被两个 PTE 计了两次，Pss 没变——它才是均摊后的数。\n");

	munmap(priv, FILE_BYTES);
	munmap(shm, FILE_BYTES);
	free(anon);
	close(fd);
	unlink(path);
	(void)sink;
	return 0;
}
