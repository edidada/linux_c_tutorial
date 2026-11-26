/*
 * 02_mini_strace.c —— 用 ptrace 自己实现一个最小 strace
 *
 * 目的：把「系统调用是一次可被外部观察的、用户态与内核态的往返」这件事变成代码。
 *
 * 原理：
 *   - 子进程 PTRACE_TRACEME 后 exec，父进程成为 tracer；
 *   - PTRACE_SYSCALL 让子进程在每次「进入系统调用」和「退出系统调用」时各停一次，
 *     两次停止之间用 PTRACE_GETREGS 读寄存器：
 *         进入时 user_regs_struct.orig_rax = 调用号，rdi/rsi/rdx/r10/r8/r9 = 参数；
 *         退出时 rax = 内核返回值（负数表示 -errno）。
 *   - 这两次停止对应内核 entry_SYSCALL_64 里 syscall_trace_enter / syscall_trace_exit
 *     对 ptrace 的处理：这正是 strace 之所以能存在的内核钩子。
 *
 * 用法：
 *   ./mini_strace /bin/ls -l /
 *   MINI_STRACE_FILTER=1,2,3 ./mini_strace /bin/echo hi   # 只看 write/open/close
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/syscall.h>

#define MAX_NR 400

static int filter_on;
static unsigned char filter[MAX_NR];
static unsigned long counts[MAX_NR];
static unsigned long total;

/* 只给常用调用起名字，避免抄整张表；完整表在内核源码
 * arch/x86/entry/syscalls/syscall_64.tbl */
static const char *nr_name(long nr)
{
    switch (nr) {
    case 0: return "read";      case 1: return "write";     case 2: return "open";
    case 3: return "close";     case 4: return "stat";      case 5: return "fstat";
    case 8: return "lseek";     case 9: return "mmap";      case 10: return "mprotect";
    case 11: return "munmap";   case 12: return "brk";      case 16: return "ioctl";
    case 21: return "access";   case 22: return "pipe";     case 24: return "sched_yield";
    case 39: return "getpid";   case 56: return "clone";    case 57: return "fork";
    case 58: return "vfork";    case 59: return "execve";   case 60: return "exit";
    case 61: return "wait4";    case 62: return "kill";     case 63: return "uname";
    case 72: return "fcntl";    case 78: return "getdents"; case 89: return "readlink";
    case 96: return "gettimeofday"; case 102: return "getuid";
    case 158: return "arch_prctl"; case 186: return "gettid"; case 202: return "futex";
    case 217: return "getdents64"; case 221: return "fadvise64";
    case 228: return "clock_gettime"; case 231: return "exit_group";
    case 257: return "openat";  case 262: return "newfstatat"; case 288: return "accept4";
    case 293: return "pipe2";   case 318: return "getrandom";
    case 332: return "statx";   case 435: return "clone3";
    default: return "?";
    }
}

static void usage(const char *p)
{
    fprintf(stderr, "用法: %s [-f nr,nr...] program [args...]\n", p);
}

int main(int argc, char **argv)
{
    int opt;
    while ((opt = getopt(argc, argv, "+f:")) != -1) {
        if (opt == 'f') {
            filter_on = 1;
            char *s = strdup(optarg);
            for (char *t = strtok(s, ","); t; t = strtok(NULL, ",")) {
                long v = strtol(t, NULL, 0);
                if (v >= 0 && v < MAX_NR) filter[v] = 1;
            }
            free(s);
        } else { usage(argv[0]); return 2; }
    }
    if (optind >= argc) { usage(argv[0]); return 2; }

    pid_t child = fork();
    if (child < 0) { perror("fork"); return 1; }

    if (child == 0) {
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) < 0) { perror("PTRACE_TRACEME"); _exit(127); }
        raise(SIGSTOP);                       /* 等父进程准备好 */
        execvp(argv[optind], &argv[optind]);
        perror("execvp");                     /* exec 失败才会走到这里 */
        _exit(127);
    }

    int status;
    if (waitpid(child, &status, 0) < 0) { perror("waitpid"); return 1; }

    /* TRACESYSGOOD：让系统调用停止点带 0x80 标志，与普通信号区分开 */
    if (ptrace(PTRACE_SETOPTIONS, child, 0,
               PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEEXEC | PTRACE_O_EXITKILL) < 0) {
        perror("SETOPTIONS"); return 1;
    }
    if (ptrace(PTRACE_SYSCALL, child, 0, 0) < 0) { perror("SYSCALL"); return 1; }

    int entering = 1;
    for (;;) {
        if (waitpid(child, &status, 0) < 0) { perror("waitpid"); break; }
        if (WIFEXITED(status) || WIFSIGNALED(status)) break;

        /* execve 成功时，内核用 PTRACE_EVENT_EXEC 取代了 syscall-exit 停止点 */
        if (status >> 8 == (SIGTRAP | (PTRACE_EVENT_EXEC << 8))) {
            fprintf(stderr, " (execve 完成，映像已替换) = 0\n");
            entering = 0;
            ptrace(PTRACE_SYSCALL, child, 0, 0);
            continue;
        }

        int sig = WSTOPSIG(status);
        if (!(sig & 0x80)) {                  /* 普通信号：原样转发给子进程 */
            ptrace(PTRACE_SYSCALL, child, 0, sig == SIGTRAP ? 0 : sig);
            continue;
        }

        struct user_regs_struct regs;
        if (ptrace(PTRACE_GETREGS, child, 0, &regs) < 0) { perror("GETREGS"); break; }

        if (entering) {
            long nr = (long)regs.orig_rax;
            total++;
            if (nr >= 0 && nr < MAX_NR) counts[nr]++;
            if (!filter_on || (nr < MAX_NR && filter[nr])) {
                fprintf(stderr, "[%d] %-16s(%#llx, %#llx, %#llx, %#llx)",
                        child, nr_name(nr), (unsigned long long)regs.rdi,
                        (unsigned long long)regs.rsi, (unsigned long long)regs.rdx,
                        (unsigned long long)regs.r10);
            }
            if (nr == 231 || nr == 60) {      /* exit_group / exit 之后不会再回来 */
            }
            entering = 0;
        } else {
            if (!filter_on || (regs.orig_rax < MAX_NR && filter[regs.orig_rax]))
                fprintf(stderr, " = %lld\n", (long long)regs.rax);
            entering = 1;
        }
        if (ptrace(PTRACE_SYSCALL, child, 0, 0) < 0) { perror("SYSCALL"); break; }
    }

    fprintf(stderr, "\n=== 总计 %lu 次系统调用，按调用号统计 ===\n", total);
    for (int i = 0; i < MAX_NR; i++)
        if (counts[i])
            fprintf(stderr, "%4d %-18s %8lu\n", i, nr_name(i), counts[i]);

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128 + WTERMSIG(status);
}
