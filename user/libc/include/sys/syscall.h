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

#pragma once

// System call numbers, Linux x86-64's (kernel/syscall.h)

#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_poll 7
#define SYS_lseek 8
#define SYS_mmap 9
#define SYS_munmap 11
#define SYS_rt_sigaction 13
#define SYS_rt_sigreturn 15
#define SYS_sched_yield 24
#define SYS_nanosleep 35
#define SYS_alarm 37
#define SYS_getpid 39
#define SYS_fork 57
#define SYS_execve 59
#define SYS_exit 60        // ends the calling thread
#define SYS_wait4 61
#define SYS_kill 62
#define SYS_getcwd 79
#define SYS_chdir 80
#define SYS_exit_group 231 // ends the process, every thread
#define SYS_getppid 110
#define SYS_reboot 169
#define SYS_gettid 186
#define SYS_futex 202
#define SYS_openat 257
#define SYS_dup3 292
#define SYS_pipe2 293
#define SYS_execveat 322
#define SYS_clone3 435
