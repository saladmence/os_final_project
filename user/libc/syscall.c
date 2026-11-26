/* Copyright (C) 2026 Ahmed Gheith and contributors.
 *
 * Use restricted to classroom projects.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/futex.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <poll.h>

#include "pthread_impl.h"

long __syscall(long n, long a0, long a1, long a2, long a3, long a4, long a5) {
  // the arguments go in RDI, RSI, RDX, R10, R8, R9
  register long r10 asm("r10") = a3;
  register long r8 asm("r8") = a4;
  register long r9 asm("r9") = a5;
  long result;
  asm volatile("syscall"
               : "=a"(result)
               : "a"(n), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8), "r"(r9)
               : "rcx", "r11", "memory");
  return result;
}

long __syscall_result(long result) {
  if (result < 0 && result >= -4095) {
    errno = (int)-result;
    return -1;
  }
  return result;
}

long syscall(long n, long a0, long a1, long a2, long a3, long a4, long a5) {
  return __syscall_result(__syscall(n, a0, a1, a2, a3, a4, a5));
}

ssize_t read(int fd, void *buf, size_t count) {
  return syscall(SYS_read, fd, (long)buf, (long)count, 0, 0, 0);
}

ssize_t write(int fd, const void *buf, size_t count) {
  return syscall(SYS_write, fd, (long)buf, (long)count, 0, 0, 0);
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    return (int)syscall(SYS_poll, (long)fds, (long)nfds, timeout, 0, 0, 0);
}

int close(int fd) { return (int)syscall(SYS_close, fd, 0, 0, 0, 0, 0); }

int pipe2(int fd[2], int flags) { return syscall(SYS_pipe2, (long)fd, flags, 0, 0, 0, 0); }
int pipe(int fd[2]) { return pipe2(fd, 0); }
int dup3(int oldfd, int newfd, int flags) { return syscall(SYS_dup3, oldfd, newfd, flags, 0, 0, 0); }
int chdir(const char *path) { return syscall(SYS_chdir, (long)path, 0, 0, 0, 0, 0); }
char *getcwd(char *buf, size_t size) {
  return syscall(SYS_getcwd, (long)buf, size, 0, 0, 0, 0) < 0 ? NULL : buf;
}
unsigned alarm(unsigned seconds) { return syscall(SYS_alarm, seconds, 0, 0, 0, 0, 0); }
int nanosleep(const struct timespec *request, struct timespec *remaining) {
  return syscall(SYS_nanosleep, (long)request, (long)remaining, 0, 0, 0, 0);
}
unsigned sleep(unsigned seconds) {
  struct timespec request = {seconds, 0}, remaining = {0};
  return nanosleep(&request, &remaining) < 0 ? remaining.tv_sec + (remaining.tv_nsec != 0) : 0;
}
int usleep(unsigned microseconds) {
  struct timespec request = {microseconds / 1000000, (microseconds % 1000000) * 1000L};
  return nanosleep(&request, NULL);
}
int futex(uint32_t *addr, int op, uint32_t value, const struct timespec *timeout,
          uint32_t *addr2, uint32_t value3) {
  return syscall(SYS_futex, (long)addr, op, value, (long)timeout, (long)addr2, value3);
}

off_t lseek(int fd, off_t offset, int whence) {
  return syscall(SYS_lseek, fd, offset, whence, 0, 0, 0);
}

int open(const char *path, int flags, ...) {
  long mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, int);
    va_end(ap);
  }
  return (int)syscall(SYS_open, (long)path, flags, mode, 0, 0, 0);
}

int openat(int dirfd, const char *path, int flags, ...) {
  long mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, int);
    va_end(ap);
  }
  return (int)syscall(SYS_openat, dirfd, (long)path, flags, mode, 0, 0);
}

pid_t getpid(void) { return (pid_t)syscall(SYS_getpid, 0, 0, 0, 0, 0, 0); }

pid_t getppid(void) { return (pid_t)syscall(SYS_getppid, 0, 0, 0, 0, 0, 0); }

pid_t gettid(void) { return (pid_t)syscall(SYS_gettid, 0, 0, 0, 0, 0, 0); }

pid_t fork(void) { return (pid_t)syscall(SYS_fork, 0, 0, 0, 0, 0, 0); }

int execve(const char *path, char *const argv[], char *const envp[]) {
  return (int)syscall(SYS_execve, (long)path, (long)argv, (long)envp, 0, 0, 0);
}

int execveat(int dirfd, const char *path, char *const argv[],
             char *const envp[], int flags) {
  return (int)syscall(SYS_execveat, dirfd, (long)path, (long)argv,
                      (long)envp, flags, 0);
}

pid_t wait4(pid_t pid, int *status, int options, struct rusage *rusage) {
  return (pid_t)syscall(SYS_wait4, pid, (long)status, options, (long)rusage,
                        0, 0);
}

pid_t waitpid(pid_t pid, int *status, int options) { return wait4(pid, status, options, NULL); }

int reboot(int magic1, int magic2, int command, void *argument) {
  return (int)syscall(SYS_reboot, magic1, magic2, command, (long)argument, 0,
                      0);
}

int sched_yield(void) { return (int)syscall(SYS_sched_yield, 0, 0, 0, 0, 0, 0); }

void _exit(int status) {
  __syscall(SYS_exit_group, status, 0, 0, 0, 0, 0);
  __builtin_unreachable();
}
