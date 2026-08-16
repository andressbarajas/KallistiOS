/* KallistiOS ##version##

   kos/kosload.h
   Copyright (C) 2025 Donald Haase
*/

/** \file      kos/kosload.h
    \brief     Functions to access the system calls provided by kosload.
    \ingroup   kosload_syscalls

    \author Donald Haase
*/

/** \defgroup  kosload_syscalls kosload system calls
    \brief     API for kosload's system calls
    \ingroup   system_calls

    This module encapsulates all the commands provided by kosload
    via its syscall function.

    @{
*/

#ifndef __KOS_KOSLOAD_H
#define __KOS_KOSLOAD_H

#include <sys/cdefs.h>
__BEGIN_DECLS

#include <fcntl.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#include <dirent.h>
#include <sys/types.h>

typedef enum {
    KOSLOAD_READ         = 0,
    KOSLOAD_WRITE        = 1,
    KOSLOAD_OPEN         = 2,
    KOSLOAD_CLOSE        = 3,
    KOSLOAD_CREAT        = 4,
    KOSLOAD_LINK         = 5,
    KOSLOAD_UNLINK       = 6,
    KOSLOAD_CHDIR        = 7,
    KOSLOAD_CHMOD        = 8,
    KOSLOAD_LSEEK        = 9,
    KOSLOAD_FSTAT        = 10,
    KOSLOAD_TIME         = 11,
    KOSLOAD_STAT         = 12,
    KOSLOAD_UTIME        = 13,
    KOSLOAD_ASSIGNWRKMEM = 14,
    KOSLOAD_EXIT         = 15,
    KOSLOAD_OPENDIR      = 16,
    KOSLOAD_CLOSEDIR     = 17,
    KOSLOAD_READDIR      = 18,
    KOSLOAD_GETHOSTINFO  = 19,
    KOSLOAD_GDBPACKET    = 20,
    KOSLOAD_REWINDDIR    = 21
} kosload_cmd_t;

typedef int (*kosload_syscall_t)(kosload_cmd_t cmd, void *param1, void *param2, void *param3);
void kosload_syscall_set(kosload_syscall_t fn);

/* Network (dcload-ip over KOS sockets) syscall backend. */
int kosload_syscall_net_init(void);
void kosload_syscall_net_shutdown(void);

ssize_t kosload_read(uint32_t hnd, uint8_t *data, size_t len);
ssize_t kosload_write(uint32_t hnd, const uint8_t *data, size_t len);
int kosload_open(const char *fn, int oflags, int mode);
int kosload_close(uint32_t hnd);
int kosload_creat(const char *path, mode_t mode);
int kosload_link(const char *fn1, const char *fn2);
int kosload_unlink(const char *fn);
int kosload_chdir(const char *path);
int kosload_chmod(const char *path, mode_t mode);
off_t kosload_lseek(uint32_t hnd, off_t offset, int whence);

/* kosload stat */
typedef struct kosload_stat {
    unsigned short st_dev;
    unsigned short st_ino;
    int st_mode;
    unsigned short st_nlink;
    unsigned short st_uid;
    unsigned short st_gid;
    unsigned short st_rdev;
    long st_size;
    long atime;
    long st_spare1;
    long mtime;
    long st_spare2;
    long ctime;
    long st_spare3;
    long st_blksize;
    long st_blocks;
    long st_spare4[2];
} kosload_stat_t;

int kosload_fstat(int fildes, kosload_stat_t *buf);
time_t kosload_time(void);
int kosload_stat(const char *restrict path, kosload_stat_t *restrict buf);
/* int kosload_utime(const char *path, const struct utimbuf *times); */
int kosload_assignwrkmem(int *buf);
void kosload_exit(void);
int kosload_opendir(const char *fn);
int kosload_closedir(uint32_t hnd);
struct dirent *kosload_readdir(uint32_t hnd);
uint32_t kosload_gethostinfo(uint32_t *ip, uint32_t *port);
size_t kosload_gdbpacket(const char* in_buf, size_t in_size, char* out_buf, size_t out_size);
int kosload_rewinddir(uint32_t hnd);

/** @} */

__END_DECLS

#endif /* __KOS_KOSLOAD_H */
