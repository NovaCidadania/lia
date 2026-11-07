/* Lia — runtime freestanding (Linux x86-64, sem libc). */
#ifndef LIA_H
#define LIA_H

#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t i32;
typedef int64_t i64;

/* nucleo/x86_64/entrada.S — retorna -errno em caso de erro */
long lia_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6);

#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_EACCESS 0x200

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT 0100
#define O_TRUNC 01000
#define O_DIRECTORY 0200000
#define O_CLOEXEC 02000000

#define S_IFMT 0170000
#define S_IFREG 0100000
#define S_IFLNK 0120000

#define EINTR 4
#define EEXIST 17

struct lia_stat {
    u64 dev, ino, nlink;
    u32 mode, uid, gid, pad0;
    u64 rdev;
    i64 size, blksize, blocks;
    i64 atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns;
    i64 reservado[3];
};

struct lia_dirent {
    u64 ino;
    i64 off;
    u16 reclen;
    u8 tipo;
    char nome[];
};

struct lia_timespec { i64 s, ns; };
struct lia_pollfd { int fd; short events, revents; };
struct lia_utsname { char sysname[65], nodename[65], release[65], version[65], machine[65], domain[65]; };

#define POLLIN 1
#define POLLERR 8
#define POLLHUP 16

/* runtime/sys.c */
long lia_read(int fd, void *b, size_t n);
long lia_write(int fd, const void *b, size_t n);
long lia_openat(int dirfd, const char *caminho, int flags, int modo);
long lia_close(int fd);
long lia_fstatat(int dirfd, const char *caminho, struct lia_stat *st, int flags);
long lia_readlinkat(int dirfd, const char *caminho, char *b, size_t n);
long lia_faccessat2(int dirfd, const char *caminho, int modo, int flags);
long lia_getdents64(int fd, void *b, size_t n);
long lia_mkdirat(int dirfd, const char *caminho, int modo);
long lia_rename(const char *de, const char *para);
long lia_fork(void);
long lia_execve(const char *caminho, const char *const *argv, const char *const *envp);
long lia_wait4(long pid, int *status, int opcoes);
long lia_pipe2(int fds[2], int flags);
long lia_dup3(int antigo, int novo, int flags);
long lia_poll(struct lia_pollfd *fds, unsigned long n, int timeout_ms);
long lia_kill(long pid, int sinal);
long lia_clock(int relogio, struct lia_timespec *ts);
long lia_uname(struct lia_utsname *u);
long lia_getuid(void);
long lia_geteuid(void);
long lia_getgid(void);
void lia_exit(int codigo) __attribute__((noreturn));

/* runtime/texto.c */
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t lia_strlen(const char *s);
int lia_strcmp(const char *a, const char *b);
int lia_strncmp(const char *a, const char *b, size_t n);

int lia_escrever_tudo(int fd, const void *p, size_t n);

/* nucleo/x86_64/lia-cpuid.S — nunca assume suporte; checa CPUID (e XGETBV
 * para AVX2) antes de qualquer uso de SSE2/AVX2. Ver docs/abi.md §6. */
int lia_cpuid_sse2(void);
int lia_cpuid_avx2(void);

/* nucleo/x86_64/lia-mem.S — backends de memcpy/memset/memcmp. Contrato
 * completo em docs/abi.md §6: memcpy não aceita sobreposição; n == 0 nunca
 * toca memória; sem exigência de alinhamento. */
void *lia_memcpy_escalar(void *dst, const void *src, size_t n);
void *lia_memcpy_sse2(void *dst, const void *src, size_t n);
void *lia_memcpy_avx2(void *dst, const void *src, size_t n);
void *lia_memset_escalar(void *dst, int c, size_t n);
void *lia_memset_sse2(void *dst, int c, size_t n);
void *lia_memset_avx2(void *dst, int c, size_t n);
int lia_memcmp_escalar(const void *a, const void *b, size_t n);
int lia_memcmp_sse2(const void *a, const void *b, size_t n);
int lia_memcmp_avx2(const void *a, const void *b, size_t n);

/* runtime/lia_mem.c — despacho automático (AVX2 > SSE2 > escalar) com
 * seleção manual só para testes/benchmark. */
#define LIA_MEM_AUTO    (-1)
#define LIA_MEM_ESCALAR 0
#define LIA_MEM_SSE2    1
#define LIA_MEM_AVX2    2

int lia_mem_capacidade(void);           /* bit0 = SSE2, bit1 = AVX2 */
int lia_mem_selecionar(int modo);       /* 0 = ok; -1 = backend indisponível/inválido, nada é alterado */
int lia_mem_backend_ativo(void);        /* resolve LIA_MEM_AUTO para o backend concreto em uso */
const char *lia_mem_backend_nome(int backend);
void *lia_memcpy(void *dst, const void *src, size_t n);
void *lia_memset(void *dst, int c, size_t n);
int lia_memcmp(const void *a, const void *b, size_t n);

/* nucleo/referencia/lia_mem_ref.c — oráculo escalar em C, só para testes. */
void *lia_memcpy_ref(void *dst, const void *src, size_t n);
void *lia_memset_ref(void *dst, int c, size_t n);
int lia_memcmp_ref(const void *a, const void *b, size_t n);

/* Buffer de saída: fd >= 0 descarrega ao encher; fd < 0 acumula em memória. */
typedef struct {
    char *p;
    size_t cap, n;
    int fd;
    int err;
} Buf;

void b_init(Buf *b, char *mem, size_t cap, int fd);
void b_flush(Buf *b);
void b_put(Buf *b, const void *s, size_t n);
void b_s(Buf *b, const char *s);
void b_c(Buf *b, char c);
void b_u(Buf *b, u64 v);
void b_i(Buf *b, i64 v);
void b_oct(Buf *b, u32 v);
void b_pad(Buf *b, const char *s, size_t largura);
void b_json(Buf *b, const char *s);
void b_data_utc(Buf *b, i64 epoch);

#endif
