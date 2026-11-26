/*
 * zombie_orphan.c —— 进程的三种「非运行」状态，以及谁负责回收
 *
 * 内核视角：
 *   - exit() -> do_exit()：释放 mm（exit_mm）、关闭文件（exit_files）、
 *     把退出码写进 task_struct->exit_code，状态置 EXIT_ZOMBIE。
 *     此时不能再运行，但 task_struct 和内核栈必须保留——因为父进程要用 wait4 读退出码。
 *   - 父进程 wait4() -> kernel_wait4 -> do_wait()：找到 EXIT_ZOMBIE 的子进程，
 *     取 exit_code，然后 release_task() 真正释放 task_struct（这就是「收尸」）。
 *   - 父进程先死：子进程被 reparent 给 init（或最近的 subreaper），
 *     内核在 forget_original_parent() 里做这件事；init 会 wait 掉它们，所以不会永久僵尸。
 *
 * 本程序演示：
 *   A. 子进程退出但父进程不 wait -> 僵尸（状态 Z），仍占 PID 和 task_struct；
 *   B. 父进程 wait 之后 -> /proc/<pid> 消失；
 *   C. 父进程先退出 -> 孙进程被 reparent，getppid() 变成 1（或 subreaper 的 pid）。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

/* 读 /proc/<pid>/stat 的第三个字段：进程状态字母 */
static char proc_state(pid_t pid)
{
    char path[64], buf[512];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return '?';
    if (!fgets(buf, sizeof buf, f)) { fclose(f); return '?'; }
    fclose(f);

    /* comm 字段可能含空格和括号，所以从最后一个 ')' 之后开始切 */
    char *p = strrchr(buf, ')');
    if (!p) return '?';
    char st = 0;
    sscanf(p + 1, " %c", &st);
    return st;
}

static const char *state_desc(char c)
{
    switch (c) {
    case 'R': return "running/runnable";
    case 'S': return "interruptible sleep";
    case 'D': return "uninterruptible sleep";
    case 'T': return "stopped";
    case 'Z': return "zombie";
    case 'I': return "idle kernel thread";
    default:  return "?";
    }
}

int main(void)
{
    printf("=== A. 制造一个僵尸 ===\n");
    pid_t z = fork();
    if (z == 0) _exit(42);               /* 退出码 42，进程立刻变 EXIT_ZOMBIE */

    sleep(1);                            /* 父进程故意不 wait */
    char st = proc_state(z);
    printf("子进程 pid=%d 状态=%c (%s)\n", z, st, state_desc(st));
    printf("/proc/%d 仍然存在：", z);
    fflush(stdout);
    char cmd[64];
    snprintf(cmd, sizeof cmd, "test -d /proc/%d && echo yes || echo no", z);
    system(cmd);

    int status = 0;
    waitpid(z, &status, 0);              /* 收尸：release_task 释放 task_struct */
    printf("waitpid 得到退出码 %d，WIFEXITED=%d\n",
           WIFEXITED(status) ? WEXITSTATUS(status) : -1, WIFEXITED(status));
    printf("/proc/%d 现在还存在吗：", z);
    fflush(stdout);
    snprintf(cmd, sizeof cmd, "test -d /proc/%d && echo yes || echo no", z);
    system(cmd);

    printf("\n=== B. 父进程先死，子进程被 reparent ===\n");
    fflush(stdout);                      /* 否则子进程会把这段缓冲再打一遍 */
    pid_t p = fork();
    if (p == 0) {
        pid_t gc = fork();
        if (gc == 0) {
            sleep(2);                    /* 等父进程和祖父都退出 */
            printf("孙进程 pid=%d, getppid()=%d\n", getpid(), getppid());
            printf("（不一定是 1：内核会 reparent 给最近的 subreaper，\n");
            printf("  WSL/容器的 init 或 shell 可能就是这个角色）\n");
            fflush(stdout);              /* _exit 不刷 stdio 缓冲 */
            _exit(0);
        }
        _exit(0);                        /* 中间层父进程立刻退出 */
    }
    waitpid(p, NULL, 0);                 /* 只回收中间层，孙进程已被 reparent */

    sleep(3);
    printf("\n提示：把本程序放到 `setsid` 或容器 PID namespace 里跑，\n");
    printf("getppid() 的目标会变成该 namespace 的 pid 1，reparent 逻辑不变。\n");
    return 0;
}
