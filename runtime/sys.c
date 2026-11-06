/* Lia — wrappers finos sobre as syscalls Linux x86-64. */
#include "lia.h"

#define L(x) ((long)(x))

long lia_read(int fd, void *b, size_t n) { return lia_syscall(0, fd, L(b), L(n), 0, 0, 0); }
long lia_write(int fd, const void *b, size_t n) { return lia_syscall(1, fd, L(b), L(n), 0, 0, 0); }
long lia_close(int fd) { return lia_syscall(3, fd, 0, 0, 0, 0, 0); }
long lia_poll(struct lia_pollfd *fds, unsigned long n, int t) { return lia_syscall(7, L(fds), L(n), t, 0, 0, 0); }
long lia_dup3(int a, int n, int fl) { return lia_syscall(292, a, n, fl, 0, 0, 0); }
long lia_fork(void) { return lia_syscall(57, 0, 0, 0, 0, 0, 0); }
long lia_execve(const char *c, const char *const *av, const char *const *ev) { return lia_syscall(59, L(c), L(av), L(ev), 0, 0, 0); }
long lia_wait4(long pid, int *st, int op) { return lia_syscall(61, pid, L(st), op, 0, 0, 0); }
long lia_kill(long pid, int sig) { return lia_syscall(62, pid, sig, 0, 0, 0, 0); }
long lia_uname(struct lia_utsname *u) { return lia_syscall(63, L(u), 0, 0, 0, 0, 0); }
long lia_rename(const char *a, const char *b) { return lia_syscall(82, L(a), L(b), 0, 0, 0, 0); }
long lia_getuid(void) { return lia_syscall(102, 0, 0, 0, 0, 0, 0); }
long lia_getgid(void) { return lia_syscall(104, 0, 0, 0, 0, 0, 0); }
long lia_geteuid(void) { return lia_syscall(107, 0, 0, 0, 0, 0, 0); }
long lia_getdents64(int fd, void *b, size_t n) { return lia_syscall(217, fd, L(b), L(n), 0, 0, 0); }
long lia_clock(int r, struct lia_timespec *ts) { return lia_syscall(228, r, L(ts), 0, 0, 0, 0); }
long lia_openat(int d, const char *c, int fl, int m) { return lia_syscall(257, d, L(c), fl, m, 0, 0); }
long lia_mkdirat(int d, const char *c, int m) { return lia_syscall(258, d, L(c), m, 0, 0, 0); }
long lia_fstatat(int d, const char *c, struct lia_stat *st, int fl) { return lia_syscall(262, d, L(c), L(st), fl, 0, 0); }
long lia_readlinkat(int d, const char *c, char *b, size_t n) { return lia_syscall(267, d, L(c), L(b), L(n), 0, 0); }
long lia_pipe2(int fds[2], int fl) { return lia_syscall(293, L(fds), fl, 0, 0, 0, 0); }
long lia_faccessat2(int d, const char *c, int m, int fl) { return lia_syscall(439, d, L(c), m, fl, 0, 0); }

void lia_exit(int codigo)
{
    for (;;)
        lia_syscall(231, codigo, 0, 0, 0, 0, 0);
}
