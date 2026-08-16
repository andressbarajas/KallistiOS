/* KallistiOS ##version##

   kernel/kosload.c

   Copyright (C) 2025 Donald Haase

*/

#include <stdio.h>

#include <kos/kosload.h>

/* Default backend, defined in kosload_syscall.c */
int kosload_syscall_native(kosload_cmd_t cmd, void *p1, void *p2, void *p3);

static kosload_syscall_t transport = kosload_syscall_native;

void kosload_syscall_set(kosload_syscall_t fp) {
    transport = fp ? fp : kosload_syscall_native;
}

static inline int kosload_syscall(kosload_cmd_t cmd, void *p1, void *p2, void *p3) {
    return transport(cmd, p1, p2, p3);
}

ssize_t kosload_read(uint32_t hnd, uint8_t *data, size_t len) {
    return kosload_syscall(KOSLOAD_READ, (void *)hnd, (void *)data, (void *)len);
}

ssize_t kosload_write(uint32_t hnd, const uint8_t *data, size_t len) {
    return kosload_syscall(KOSLOAD_WRITE, (void *)hnd, (void *)data, (void *)len);
}

int kosload_open(const char *fn, int oflags, int mode) {
    return kosload_syscall(KOSLOAD_OPEN, (void *)fn, (void *)oflags, (void *)mode);
}

int kosload_close(uint32_t hnd) {
    return kosload_syscall(KOSLOAD_CLOSE, (void *)hnd, NULL, NULL);
}

int kosload_creat(const char *path, mode_t mode) {
    return kosload_syscall(KOSLOAD_CREAT, (void *)path, (void *)mode, NULL);
}

int kosload_link(const char *fn1, const char *fn2) {
    return kosload_syscall(KOSLOAD_LINK, (void *)fn1, (void *)fn2, NULL);
}

int kosload_unlink(const char *fn) {
    return kosload_syscall(KOSLOAD_UNLINK, (void *)fn, NULL, NULL);
}

int kosload_chdir(const char *path) {
    return kosload_syscall(KOSLOAD_CHDIR, (void *)path, NULL, NULL);
}

int kosload_chmod(const char *path, mode_t mode) {
    return kosload_syscall(KOSLOAD_CHMOD, (void *)path, (void *)mode, NULL);
}

off_t kosload_lseek(uint32_t hnd, off_t offset, int whence) {
    return (off_t)kosload_syscall(KOSLOAD_LSEEK, (void *)hnd, (void *)offset, (void *)whence);
}

int kosload_fstat(int fildes, kosload_stat_t *buf) {
    return kosload_syscall(KOSLOAD_FSTAT, (void *)fildes, (void *)buf, NULL);
}

time_t kosload_time(void) {
    return (time_t)kosload_syscall(KOSLOAD_TIME, NULL, NULL, NULL);
}

int kosload_stat(const char *restrict path, kosload_stat_t *restrict buf) {
    return kosload_syscall(KOSLOAD_STAT, (void *)path, (void *)buf, NULL);
}

/* Leaving this disabled for now as kosload was written when these values would
    have been 32bit but they are now each 64 bits so they can't be sent
    transparently.
*/
/*
int kosload_utime(const char *path, const struct utimbuf *times) {
    return kosload_syscall(KOSLOAD_UTIME, (void *)path,
        (void *) (times ? times->actime : 0), (void *) (times ? times->modtime : 0));
}
*/

int kosload_assignwrkmem(int *buf) {
    return kosload_syscall_native(KOSLOAD_ASSIGNWRKMEM, (void *)buf, NULL, NULL);
}

void kosload_exit(void) {
    kosload_syscall(KOSLOAD_EXIT, NULL, NULL, NULL);
}

int kosload_opendir(const char *fn) {
    return kosload_syscall(KOSLOAD_OPENDIR, (void *)fn, NULL, NULL);
}

int kosload_closedir(uint32_t hnd) {
    return kosload_syscall(KOSLOAD_CLOSEDIR, (void *)hnd, NULL, NULL);
}

struct dirent *kosload_readdir(uint32_t hnd) {
    return (struct dirent *)kosload_syscall(KOSLOAD_READDIR, (void *)hnd, NULL, NULL);
}

size_t kosload_gdbpacket(const char* in_buf, size_t in_size, char* out_buf, size_t out_size) {
    /* we have to pack the sizes together because the kosloadsyscall handler
       can only take 4 parameters */
    return kosload_syscall(KOSLOAD_GDBPACKET, (void *)in_buf, (void *)((in_size << 16) | (out_size & 0xffff)), (void *)out_buf);
}

uint32_t kosload_gethostinfo(uint32_t *ip, uint32_t *port) {
    return kosload_syscall_native(KOSLOAD_GETHOSTINFO, (void *)ip, (void *)port, NULL);
}

int kosload_rewinddir(uint32_t hnd) {
    return kosload_syscall(KOSLOAD_REWINDDIR, (void *)hnd, NULL, NULL);
}
