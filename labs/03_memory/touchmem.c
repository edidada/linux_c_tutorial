/*
 * touchmem.c —— 让缺页次数可以被数出来：4MB = 1024 页 = 1024 次 minflt
 *
 * 为什么单独做这个：RSS 实验能看到「RSS 涨了多少」，但看不到「涨的过程分了
 * 多少次」。这里一个 memset 造 1024 次匿名缺页，再把计数和 QEMU 里
 * do_anonymous_page 的命中次数对一遍（见 qemu/fault.gdb）。
 *
 * 两个来源对账：
 *   getrusage() 的 ru_minflt —— 本进程视角，内核在 do_anonymous_page 里加一
 *   /proc/self/stat 的第 10 字段 minflt —— 同一本账的另一种读法
 *
 * 用法： ./touchmem
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#define MB (1UL << 20)
#define N  (4UL * MB)

static long proc_stat_minflt(void)
{
	FILE *f = fopen("/proc/self/stat", "r");
	long dummy, minflt = -1, majflt = -1;
	char comm[64], state;

	if (!f)
		return -1;
	/* 字段：pid comm state ppid ... minflt(10) ... majflt(12) */
	if (fscanf(f, "%ld %63s %c %ld %ld %ld %ld %ld %ld %ld %ld %ld",
		   &dummy, comm, &state, &dummy, &dummy, &dummy, &dummy,
		   &dummy, &dummy, &minflt, &dummy, &majflt) != 12)
		minflt = -1;
	fclose(f);
	(void)majflt;
	return minflt;
}

static unsigned long minflt_now(void)
{
	struct rusage ru;

	getrusage(RUSAGE_SELF, &ru);
	return ru.ru_minflt;
}

int main(void)
{
	char *p;
	unsigned long i;
	unsigned long t0, t1, t2;

	p = malloc(N);
	if (!p) { perror("malloc"); return 1; }

	t0 = minflt_now();
	memset(p, 0x5a, N);          /* 第一次触碰：每页一次匿名缺页 */
	t1 = minflt_now();

	printf("touchmem: 写了 %lu MB = %lu 页\n", N / MB, N / 4096);
	printf("touchmem: 首轮 minflt = %lu（每页 %.2f 次），进程累计 minflt = %ld\n",
	       t1 - t0, (double)(t1 - t0) / (N / 4096), proc_stat_minflt());

	/* 再逐页写一遍：页已经 resident，缺页计数应当为 0 */
	for (i = 0; i < N; i += 4096)
		p[i] = (char)i;
	t2 = minflt_now();
	printf("touchmem: 第二轮 minflt = %lu（页已驻留，不该再涨）\n", t2 - t1);

	return 0;
}
