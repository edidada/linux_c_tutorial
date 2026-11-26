/*
 * cache_probe.c —— 读文件不会让进程 RSS 变大，但会让 page cache 变大
 *
 * 对应内核路径：read() -> vfs_read -> filemap_read -> page cache 命中或回填
 * 观测量：/proc/meminfo 的 Cached（内核全局）、/proc/self/status 的 VmRSS（本进程）
 *
 * 没 root 也能跑：用 posix_fadvise(POSIX_FADV_DONTNEED) 把自己刚写的文件从
 * page cache 踢掉，代替 echo 3 > /proc/sys/vm/drop_caches（容器里 /proc/sys 常是只读）。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#define MB (1UL << 20)
#define FILE_SIZE (256UL * MB)
#define CHUNK (64UL << 10)

static long meminfo_cached(void)
{
	FILE *f = fopen("/proc/meminfo", "r");
	char line[128];
	long v = -1;

	while (f && fgets(line, sizeof(line), f))
		if (sscanf(line, "Cached: %ld kB", &v) == 1)
			break;
	if (f) fclose(f);
	return v;
}

static long vm_rss(void)
{
	FILE *f = fopen("/proc/self/status", "r");
	char line[128];
	long v = -1;

	while (f && fgets(line, sizeof(line), f))
		if (sscanf(line, "VmRSS: %ld kB", &v) == 1)
			break;
	if (f) fclose(f);
	return v;
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double read_all(const char *path)
{
	int fd = open(path, O_RDONLY);
	char *buf = malloc(CHUNK);
	ssize_t n;
	double t0, t1;

	if (fd < 0 || !buf) { perror("open/malloc"); exit(1); }
	t0 = now();
	while ((n = read(fd, buf, CHUNK)) > 0)
		;
	t1 = now();
	close(fd);
	free(buf);
	return t1 - t0;
}

int main(void)
{
	const char *path = "/tmp/cache_probe.bin";
	char *buf = malloc(CHUNK);
	size_t pages = FILE_SIZE / 4096;
	double cold, warm;
	int fd, i;

	if (!buf) { perror("malloc"); return 1; }

	memset(buf, 'B', CHUNK);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) { perror("open"); return 1; }
	for (i = 0; i < (int)(FILE_SIZE / CHUNK); i++)
		if (write(fd, buf, CHUNK) != (ssize_t)CHUNK) { perror("write"); return 1; }
	fsync(fd);
	close(fd);
	printf("刚写完       Cached=%ld kB  VmRSS=%ld kB\n", meminfo_cached(), vm_rss());

	/* 把这 256MB 从 page cache 里踢掉，模拟「文件很久没被读过」 */
	fd = open(path, O_RDONLY);
	posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
	close(fd);

	printf("文件 %lu MB，%zu 个 4K 页\n", FILE_SIZE / MB, pages);
	printf("踢缓存后     Cached=%ld kB  VmRSS=%ld kB\n", meminfo_cached(), vm_rss());

	cold = read_all(path);
	printf("第一次读(冷) %.2f s, %.1f MB/s  Cached=%ld kB  VmRSS=%ld kB\n",
	       cold, FILE_SIZE / MB / cold, meminfo_cached(), vm_rss());

	warm = read_all(path);
	printf("第二次读(热) %.2f s, %.1f MB/s  Cached=%ld kB  VmRSS=%ld kB\n",
	       warm, FILE_SIZE / MB / warm, meminfo_cached(), vm_rss());

	printf("\nCached 涨了约 %lu MB，而进程 VmRSS 一直是几 MB：\n", FILE_SIZE / MB);
	printf("page cache 是内核的账，不是进程的账。\n");

	unlink(path);
	free(buf);
	return 0;
}
