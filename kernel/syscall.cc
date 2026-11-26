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

#include "syscall.h"

#include "async/async.h"
#include "structures/ccb.h"
#include "lib/debug.h"
#include "structures/gdt.h"
#include "lib/handle_queue.h"
#include "structures/idt.h"
#include "machine/machine.h"
#include "structures/pcb.h"
#include "memory/physmem.h"
#include "async/sync.h"
#include "machine/x86_64.h"
#include "user/libc/include/poll.h"

#include <utility>

#include "kernel_main.h"

extern "C" void syscallHandler_();

namespace {

// sysret derives the user selectors from STAR[63:48]: SS = base + 8 and
// CS = base + 16
constexpr uint64_t SYSRET_BASE = (GDT::USER_DS & ~3) - 8;
static_assert((SYSRET_BASE + 8) == (GDT::USER_DS & ~3));
static_assert((SYSRET_BASE + 16) == (GDT::USER_CS & ~3));
// syscall loads CS from STAR[47:32] and SS = CS + 8
static_assert(GDT::KERNEL_DS == GDT::KERNEL_CS + 8);

// cleared on entry: TF, IF, DF, AC
constexpr uint64_t ENTRY_FLAGS_MASK = 0x100 | 0x200 | 0x400 | 0x40000;

constexpr uint8_t KILL_VECTOR = 52;

// struct clone_args up to tls (CLONE_ARGS_SIZE_VER0), what threads need
struct CloneArgs {
  uint64_t flags;
  uint64_t pidfd;
  uint64_t child_tid;
  uint64_t parent_tid;
  uint64_t exit_signal;
  uint64_t stack;
  uint64_t stack_size;
  uint64_t tls;
  uint64_t set_tid;
  uint64_t set_tid_size;
  uint64_t cgroup;
};

using impl::TCB;

bool user_range(uintptr_t start, uint64_t len) {
  if (start + len < start) return false;
  return start >= VMM::VA_START.va() && start + len <= VMM::VA_END;
}

// Caller holds vma_list_lock (used only for the futex physical-page pin).
bool readable_range(TCB *t, uintptr_t addr, uint64_t len) {
  if (!t->process->as->memory_areas) return false;
  uint64_t start = addr >> LOG_FRAME_SIZE;
  uint64_t end = (addr + len - 1) >> LOG_FRAME_SIZE;

  for (uint64_t vpn = start; vpn <= end; vpn++) {
    auto *vma = VMM::find_vma(t->process->as->memory_areas, vpn);
    if (vma->free || vpn >= vma->base + vma->length ||
           !(vma->prot & VMM::PROT_READ)) return false;
  }
  return true;
}

bool read_user(void *dst, const void *src, uint64_t len) {
  return user_range(uintptr_t(src), len) && !VMM::copy_user_(dst, src, len);
}
bool write_user(void *dst, const void *src, uint64_t len) {
  return user_range(uintptr_t(dst), len) && !VMM::copy_user_(dst, src, len);
}

constexpr uint64_t PATH_MAX = 4096;

struct Path { char bytes[PATH_MAX]; };
int64_t copy_path(const char *path, char *out) {
  for (uint64_t i = 0; i < PATH_MAX; i++) {
    uintptr_t addr = (uintptr_t)path + i;

    if (!read_user(out + i, (void*)addr, 1)) return Syscall::EFAULT;
    if (out[i] == '\0') return i == 0 ? Syscall::ENOENT : 0;
  }

  return Syscall::ENAMETOOLONG;
}

// Keep the path buffer out of dispatch's frame: every blocked syscall
// retains that frame, and thousands of sleepers must not each retain 4 KiB.
sync<uint64_t> getcwd(TCB *t, char *buf, uint64_t size) {
  if (size == 0) co_return Syscall::ERANGE;
  char path[PATH_MAX];
  uint64_t pos = PATH_MAX - 1;
  path[pos] = '\0';
  auto dir = t->process->get_cwd();
  while (dir->number != impl::fs->root->number) {
    auto parent = co_await impl::fs->find(dir, "..");
    if (parent.error) co_return parent.error;
    auto entry = co_await parent.file->entry_it([number = dir->number](DirectoryEntryHeader *e, uint32_t) {
      return e->inode == number;
    });
    if (entry == nullptr) co_return Syscall::ENOENT;
    uint64_t len = entry->name_length_low;
    if (pos < len + 1) co_return Syscall::ENAMETOOLONG;
    pos -= len;
    memcpy(path + pos, entry.operator->() + 1, len);
    path[--pos] = '/';
    dir = parent.file;
  }
  if (pos == PATH_MAX - 1) path[--pos] = '/';
  uint64_t len = PATH_MAX - pos;
  if (size < len) co_return Syscall::ERANGE;
  if (!write_user(buf, path + pos, len)) co_return Syscall::EFAULT;
  co_return len;
}

// copies a NULL-terminated array of NUL-terminated user strings (argv/envp) into kernel memory
char **copy_strings(uintptr_t list) {
  if (list == 0) return new char*[1]{nullptr};

  uint64_t n = 0;
  while (true) {
    char *s;
    if (n >= PATH_MAX || !read_user(&s, (void*)(list + n*sizeof(char*)), sizeof(s))) return nullptr;
    if (s == nullptr) break;
    n++;
  }

  char **out = new char*[n+1]();
  for (uint64_t i = 0; i < n; i++) {
    char *s;
    if (!read_user(&s, (void*)(list + i*sizeof(char*)), sizeof(s))) { impl::free_strings(out); return nullptr; }
    uint64_t len = 0;
    while (true) {
      char ch;
      if (len >= PATH_MAX || !read_user(&ch, (void*)(uintptr_t(s) + len), 1)) {
        impl::free_strings(out);
        return nullptr;
      }
      if (ch == '\x00') break;
      len++;
    }

    out[i] = new char[len+1];
    if (!read_user(out[i], s, len+1)) { impl::free_strings(out); return nullptr; }
    out[i][len] = '\0';
  }
  out[n] = nullptr;
  return out;
}

// The most one write sends: the rest is a short write.
//   Q: Why copy the bytes in instead of sending them from the user's page?
//   Q: Why does a bigger MAX_WRITE make the coroutine's frame bigger?
constexpr uint64_t MAX_WRITE = 256;

bool store_tid(uint64_t addr, int value) {
  return user_range(addr, sizeof(value)) && !VMM::store_user32_((uint32_t*)addr, value);
}

// Never touch user buffers inside a file's own locks: they may fault. Writes and
// file reads transfer everything; console reads return after one available chunk.
sync<uint64_t> buffered_io(impl::OpenFile *file, char *buf, uint64_t len, bool writing) {
  char bounce[FRAME_SIZE];
  uint64_t total = 0;
  while (total < len && !impl::current_thread_dead()) {
    uint64_t n = std::min<uint64_t>(FRAME_SIZE, len - total);
    uint64_t left = writing ? VMM::copy_user_(bounce, buf + total, n) : 0;
    if (left == n) co_return total ? total : Syscall::EFAULT;
    int64_t count = writing ? co_await file->write(bounce, n - left) : co_await file->read(bounce, n);
    if (count < 0) co_return total ? total : count;
    if (impl::current_thread_dead()) break;
    if (!writing) {
      left = VMM::copy_user_(buf + total, bounce, count);
      count -= left;
    }
    total += count;
    if (left) co_return total ? total : Syscall::EFAULT;
    if (uint64_t(count) < n || !(writing || file->type == impl::SEEKING)) break;
  }
  co_return total;
}

struct FutexKey {
  uint64_t space, address;
  bool operator==(const FutexKey &) const = default;
};

SpinLock futex_lock;
struct FutexWait;
FutexWait *futex_waiters = nullptr;

// Private mappings are keyed by address space + VA even without MAP_PRIVATE.
// Shared mappings use the physical word, so aliases and fork agree.
FutexKey futex_key(TCB *t, uintptr_t addr, bool priv) {
  auto as = t->process->as;
  auto *area = VMM::find_vma(as->memory_areas, addr >> LOG_FRAME_SIZE);
  if (!priv && (area->flags & VMM::MAP_SHARED))
    return {0, VMM::physical_address(addr)};
  return {as->cr3_phys(), addr};
}

// The syscall frame owns its waiter; no persistent per-address allocation.
struct FutexWait {
  FutexKey key;
  const uint32_t *word; // pinned by mapping until check + registration finish
  uint32_t expected;
  BlockingLock::Proof &mapping;
  impl::coroutine waiting;
  FutexWait *next = nullptr;
  int64_t result = Syscall::EAGAIN;

  bool await_ready() { return false; }
  bool await_suspend(std::coroutine_handle<> handle) {
    LockGuard g{futex_lock};
    // Compare again here, atomically with respect to WAKE and queue insertion.
    if (__atomic_load_n(word, __ATOMIC_SEQ_CST) != expected) return false;
    waiting = impl::make_coroutine(handle);
    next = futex_waiters;
    futex_waiters = this;
    result = 0;
    mapping.unlock();
    return true;
  }
  int64_t await_resume() { return result; }
};

uint32_t futex_wake(FutexKey key, uint32_t count) {
  LockGuard g{futex_lock};
  uint32_t woke = 0;
  for (auto **link = &futex_waiters; *link && woke < count;) {
    auto *waiter = *link;
    if (waiter->key == key) {
      *link = waiter->next;
      auto handle = waiter->waiting;
      // The waiter may be destroyed as soon as it is published: no more access.
      woke += !handle.current_thread->zombie; // a dead process's waiter is only resumed to free it
      impl::ready_queue.add(handle);
    } else link = &waiter->next;
  }
  return woke;
}

sync<uint64_t> poll_input_fds(TCB *t, uintptr_t user_fds, uint64_t nfds, int timeout) {
    if(nfds > impl::FileDescriptorTable::MAX_FDS) co_return Syscall::EINVAL;

    struct Storage {
        pollfd* data;
        ~Storage() { delete[] data; }
    } storage{nfds ? new pollfd[nfds] : nullptr};

    auto* fds = storage.data;
    uint64_t bytes = nfds * sizeof(pollfd);

    if(nfds && !read_user(fds, (void *)user_fds, bytes)) co_return Syscall::EFAULT;

    uint64_t step =std::max<uint64_t>(1, (Sys::tsc_hz + 999) / 1000);
    uint64_t deadline = 0;

    if(timeout > 0){
        uint64_t duration;

        if(__builtin_mul_overflow(uint64_t(timeout), step, &duration) || __builtin_add_overflow(rdtsc(), duration, &deadline)){
            co_return Syscall::EINVAL;
        }
    }

    while(true){
        if (impl::current_thread_dead()) co_return 0;

        uint64_t ready = 0;
        for(uint64_t i = 0; i < nfds; i++){
            fds[i].revents = 0;

            if(fds[i].fd < 0) continue;

            auto file = t->process->fd_table.get(fds[i].fd);

            if(file == nullptr){
                fds[i].revents = POLLNVAL;
            } else if(file->type == impl::INPUT){
                if(PS2::events_ready()){
                    fds[i].revents = fds[i].events & POLLIN;
                }
            } else{
                co_return Syscall::ENOSYS;
            }

            if (fds[i].revents != 0) ++ready;
        }

        uint64_t now = rdtsc();

        if(ready || timeout == 0 || (timeout > 0 && now >= deadline)){
            if(nfds && !write_user((void *)user_fds, fds, bytes)) co_return Syscall::EFAULT;
            co_return ready;
        }

        uint64_t wake = now + step;

        if(timeout > 0) wake = std::min(wake, deadline);

        co_await impl::sleep_until(wake);
    }
}

sync<uint64_t> dispatch(TCB *t) {
  UserContext &c = t->context;
  switch (c.rax) {
  case Syscall::READ: {
    // read(fd, buf, len): stdin only, one character from the console (COM2),
    // raw: no echo, no line editing.
    //   Q: Why does it return after one character instead of filling buf?
    //   Q: What does the thread do while it waits for a key, and its core?
    //   Q: Why read the key before touching buf?

    uint64_t fd = c.rdi;
    auto *buf = (char *)c.rsi;
    uint64_t len = c.rdx;

    if (len == 0) co_return 0;
    if (!user_range((uintptr_t)buf, len)) co_return Syscall::EFAULT;

    auto file = t->process->fd_table.get(fd);
    if (file == nullptr) co_return Syscall::EBADF;
    if (!file->rd) co_return Syscall::EBADF;

    if (file->type == impl::PIPE) co_return co_await file->read(buf, len); // copies out transactionally
    co_return co_await buffered_io(file.operator->(), buf, len, false);
  }
  case Syscall::WRITE: {
    // write(fd, buf, len): stdout (1) goes to the console (COM2), stderr (2) to
    // the debug output (COM1).
    //   Q: How much of buf does it send?
    //   Q: What does it return for any other fd?

    uint64_t fd = c.rdi;
    auto *buf = (char *)c.rsi;
    uint64_t len = c.rdx;

    if (len == 0) co_return 0;
    if (!user_range((uintptr_t)buf, len)) co_return Syscall::EFAULT;

    auto file = t->process->fd_table.get(fd);
    if (file == nullptr) co_return Syscall::EBADF;
    if (!file->wr) co_return Syscall::EBADF;

    co_return co_await buffered_io(file.operator->(), buf, len, true);
  }
  case Syscall::POLL: {
      co_return co_await poll_input_fds(t, c.rdi, c.rsi, int32_t(c.rdx));
  }
  case Syscall::SCHED_YIELD: {
    co_await yield();
    co_return 0;
  }
  case Syscall::NANOSLEEP: {
    struct Timespec { int64_t sec, nsec; } req;
    if (!read_user(&req, (void*)c.rdi, sizeof(req))) co_return Syscall::EFAULT;
    if (req.sec < 0 || req.nsec < 0 || req.nsec >= 1000000000) co_return Syscall::EINVAL;
    uint64_t ticks;
    if (__builtin_mul_overflow(uint64_t(req.sec), Sys::tsc_hz, &ticks)) co_return Syscall::EINVAL;
    uint64_t fraction = (uint64_t(req.nsec)*Sys::tsc_hz + 999999999)/1000000000;
    uint64_t deadline;
    if (__builtin_add_overflow(ticks, fraction, &ticks) || __builtin_add_overflow(rdtsc(), ticks, &deadline)) co_return Syscall::EINVAL;
    co_await impl::sleep_until(deadline);
    co_return 0;
  }
  case Syscall::ALARM: {
    LockGuard g{t->process->signal_lock};
    uint64_t now = rdtsc(), old = t->process->alarm_deadline;
    if (old && old <= now) t->process->pending |= 1ull<<13;
    t->process->alarm_deadline = uint32_t(c.rdi) ? now + uint64_t(uint32_t(c.rdi))*Sys::tsc_hz : 0;
    co_return old > now ? (old-now + Sys::tsc_hz-1)/Sys::tsc_hz : 0;
  }
  case Syscall::KILL: {
    int64_t pid = int32_t(c.rdi);
    uint32_t sig = c.rsi;
    if (pid <= 0 || (sig && (sig >= 16 || !(impl::PCB::SIGNALS & (1ull<<(sig-1)))))) co_return Syscall::EINVAL;
    co_return impl::PCB::send_signal(pid, sig, t->process->pid);
  }
  case Syscall::RT_SIGACTION: {
    uint32_t sig = c.rdi;
    if (!sig || sig >= 16 || !(impl::PCB::SIGNALS & (1ull<<(sig-1))) || c.r10 != 8) co_return Syscall::EINVAL;
    impl::PCB::SignalAction action, old;
    if (c.rsi) {
      if (!read_user(&action, (void*)c.rsi, sizeof(action))) co_return Syscall::EFAULT;
      if (action.flags & ~(4ull|0x04000000ull|0x10000000ull)) co_return Syscall::EINVAL;
      action.mask &= impl::PCB::SIGNALS;
    }
    {
      LockGuard g{t->process->signal_lock};
      old = t->process->actions[sig];
      if (c.rsi) {
        t->process->actions[sig] = action;
        if (action.handler == 1) t->process->pending &= ~(1ull<<(sig-1));
      }
    }
    if (c.rdx && !write_user((void*)c.rdx, &old, sizeof(old))) co_return Syscall::EFAULT;
    co_return 0;
  }
  case Syscall::RT_SIGRETURN:
    if (!t->restore_signal()) { t->process->terminate(11); co_return 0; }
    co_return t->context.rax;
  case Syscall::FUTEX: {
    uintptr_t addr = c.rdi;
    uint32_t op = c.rsi;
    bool priv = op & Syscall::FUTEX_PRIVATE_FLAG;
    op &= ~Syscall::FUTEX_PRIVATE_FLAG;
    if (op != Syscall::FUTEX_WAIT && op != Syscall::FUTEX_WAKE) co_return Syscall::EINVAL;
    if (op == Syscall::FUTEX_WAIT && c.r10 != 0) co_return Syscall::EINVAL;
    if (addr % sizeof(uint32_t)) co_return Syscall::EINVAL;
    if (!user_range(addr, sizeof(uint32_t))) co_return Syscall::EFAULT;
    if (op == Syscall::FUTEX_WAKE && int32_t(c.rdx) < 0) co_return Syscall::EINVAL;
    if (priv && op == Syscall::FUTEX_WAKE)
      co_return futex_wake({t->process->as->cr3_phys(), addr}, uint32_t(c.rdx));

    for (;;) {
      auto mapping = co_await t->process->as->vma_list_lock;
      if (!readable_range(t, addr, sizeof(uint32_t))) co_return Syscall::EFAULT;
      uint64_t physical = VMM::physical_address(addr);
      if (physical == 0) {
        mapping.unlock();
        // Fault in before taking the queue lock; a fault may itself block.
        uint32_t word;
        if (!read_user(&word, (void*)addr, sizeof(word))) co_return Syscall::EFAULT;
        continue;
      }
      auto key = futex_key(t, addr, priv);
      if (op == Syscall::FUTEX_WAKE) co_return futex_wake(key, uint32_t(c.rdx));
      FutexWait waiter{key, VA{PA{physical}}, uint32_t(c.rdx), mapping, {}};
      co_return co_await waiter;
    }
  }
  case Syscall::EXIT:
  case Syscall::EXIT_GROUP: {
    auto proof = co_await t->process->alive_threads;
    if (t->zombie) co_return 0;

    co_await t->clear_tid();
    t->exit_code.set(c.rdi);

    if (c.rax == Syscall::EXIT_GROUP) {
      for (TCB *s = t->process->leader; s; s = s->next_process) {
        if (s == t || s->zombie) continue;
        co_await s->clear_tid();
        s->zombie = true;
        s->exit_code.set(c.rdi);
      }
      t->zombie = true;
      proof.data() = 0;

      t->process->finish(c.rdi);
      co_return 0;
    }

    t->zombie = true;
    if (--proof.data() == 0)
    {
      TCB *z = t->process->leader; while (z) { ASSERT(z->zombie); z = z->next_process; }
      t->process->finish(c.rdi);
    } else if (t->stack_size != 0) {
      // clone3 stacks belong to the caller (pthread_join unmaps them).
      co_await VMM::munmap(VA{t->stack_base}, t->stack_size);
    }
    co_return 0;
  }
  case Syscall::MMAP: {
    // mmap(addr, length, prot, flags, fd, offset), anonymous only
    auto *addr = (char*)c.rdi;
    uint64_t length = c.rsi;
    uint64_t prot = c.rdx;
    uint64_t flags = c.r10;
    uint64_t fd = c.r8;
    uint64_t offset = c.r9;

    StrongRef<Node> file{};
    if (!(flags & VMM::MAP_ANONYMOUS)) {
      auto of = t->process->fd_table.get(fd);
      if (of == nullptr) co_return Syscall::EBADF;
      if (!of->rd) co_return Syscall::EACCES;
      if (of->type != impl::SEEKING || co_await of->seeking()->file->is_dir()) co_return Syscall::ENODEV;
      if ((flags & VMM::MAP_SHARED) && (prot & VMM::PROT_WRITE) && !of->wr) co_return Syscall::EACCES;
      file = of->seeking()->file;
    }

    if (addr != nullptr && !user_range((uintptr_t)addr, length)) co_return Syscall::EFAULT;

    void *p = co_await VMM::mmap(addr, length, prot, flags, file, offset);
    if (p == VMM::MAP_FAILED) co_return Syscall::EINVAL;
    co_return (uintptr_t)p;
  }
  case Syscall::MUNMAP: {
    // munmap(addr, length)
    auto *addr = (char*)c.rdi;
    uint64_t length = c.rsi;

    if (!user_range((uintptr_t)addr, length)) co_return Syscall::EFAULT;

    if (co_await VMM::munmap(addr, length) != 0) co_return Syscall::EINVAL;
    co_return 0;
  }
  case Syscall::CLONE3: {
    // clone3(cl_args, size)
    auto *cl_args = (char*)c.rdi;
    uint64_t size = c.rsi;

    if (!user_range((uintptr_t)cl_args, size)) co_return Syscall::EFAULT;

    CloneArgs args{};
    if (size < 64 || size > sizeof(args)) co_return Syscall::EINVAL;
    if (!read_user(&args, cl_args, size)) co_return Syscall::EFAULT;

    constexpr uint64_t REQUIRED = Syscall::CLONE_VM | Syscall::CLONE_SIGHAND | Syscall::CLONE_THREAD;
    constexpr uint64_t ALLOWED =
        REQUIRED | Syscall::CLONE_FS | Syscall::CLONE_FILES |
        Syscall::CLONE_SYSVSEM | Syscall::CLONE_SETTLS |
        Syscall::CLONE_PARENT_SETTID | Syscall::CLONE_CHILD_CLEARTID;

    // we only support stackful threads (same mem space, same signal handlers, same process)
    if ((args.flags & REQUIRED) != REQUIRED) co_return Syscall::EINVAL;

    // only support a subset
    if (args.flags & ~ALLOWED) co_return Syscall::EINVAL;

    // can't have an exit signal on a thread
    if (args.exit_signal != 0) co_return Syscall::EINVAL;

    // we're using the same VM, a stack must be specified
    if (args.stack == 0 || args.stack_size == 0) co_return Syscall::EINVAL;
    if (!user_range(args.stack, args.stack_size)) co_return Syscall::EFAULT;

    if (args.flags & Syscall::CLONE_PARENT_SETTID && !user_range(args.parent_tid, sizeof(int))) co_return Syscall::EFAULT;
    if ((args.flags & Syscall::CLONE_CHILD_CLEARTID) && !user_range(args.child_tid, sizeof(int))) co_return Syscall::EFAULT;

    auto alive = co_await t->process->alive_threads;
    if (alive.data() == 0) co_return Syscall::EAGAIN; // shouldn't ever happen...

    const uint64_t tid = TCB::provision_tid();
    StrongRef<TCB> child{new TCB{t->process, t->context, tid}};
    child->self = WeakRef{child};
    child->anchor = child;
    child->blocked_signals = t->blocked_signals;

    // same regs as parent on the new stack; rax=0 so __clone3 calls child(self)
    child->context.rax = 0;
    child->context.rsp = (args.stack + args.stack_size) & ~uintptr_t(15);

    if (args.flags & Syscall::CLONE_SETTLS) child->context.fs_base = args.tls;
    if (args.flags & Syscall::CLONE_CHILD_CLEARTID) child->clear_tid_addr = args.child_tid;
    if ((args.flags & Syscall::CLONE_PARENT_SETTID) && !store_tid(args.parent_tid, (int)tid)) {
      child->anchor = nullptr;
      co_return Syscall::EFAULT;
    }

    t->process->tail->next_process = child.operator->();
    t->process->tail = child.operator->();
    alive.data() += 1;
    alive.unlock();

    impl::thread_queue.add(child);
    co_return tid;
  }
  case Syscall::OPENAT:
  case Syscall::OPEN: {
    char *path;
    uint64_t flags, mode, dir_fd;
    if (c.rax == Syscall::OPEN) { // open(path, flags, mode)
      dir_fd = Syscall::AT_FDCWD;
      path = (char*)c.rdi;
      flags = c.rsi;
      mode = c.rdx;
    } else { // openat(dirfd, path, flags, mode)
      dir_fd = c.rdi;
      path = (char*)c.rsi;
      flags = c.rdx;
      mode = c.r10;
    }

    if (flags & Syscall::O_CREAT && flags & Syscall::O_DIRECTORY) co_return Syscall::EINVAL;

    UniqueRef<Path> pathname{new Path};
    if (auto error = copy_path(path, pathname->bytes)) co_return error;
    path = pathname->bytes;

    bool read = (flags & 0b11) != Syscall::O_WRONLY;
    bool write = (flags & 0b11) != Syscall::O_RDONLY;

    if(streq(path, "/dev/input0")){
        if((flags & 0b11) != Syscall::O_RDONLY) co_return Syscall::EACCES;
        if(flags & Syscall::O_DIRECTORY) co_return Syscall::ENOTDIR;
    
        constexpr uint64_t allowed_flags = Syscall::O_NONBLOCK | Syscall::O_CLOEXEC;
    
        if(flags & ~allowed_flags) co_return Syscall::EINVAL;
    
        StrongRef<impl::OpenFile> input{new impl::InputFile((flags & Syscall::O_NONBLOCK) != 0)};
    
        co_return t->process->fd_table.open(input, (flags & Syscall::O_CLOEXEC) != 0);
    }

    StrongRef<Node> dir = t->process->get_cwd();
    if (path[0] != '/' && dir_fd != Syscall::AT_FDCWD) {
      auto file = t->process->fd_table.get(dir_fd);
      if (file == nullptr) co_return Syscall::EBADF;
      if (file->type != impl::SEEKING) co_return Syscall::ENOTDIR;
      dir = file->seeking()->file;
    }

    auto res = co_await impl::fs->find(dir, path, !(flags & Syscall::O_NOFOLLOW));

    if (flags & Syscall::O_CREAT) {
      if (res.error == Syscall::ENOENT) {
        res = co_await impl::fs->create(dir, path, mode);
      } else if (res.error == 0 && flags & Syscall::O_EXCL) co_return Syscall::EEXIST;
    }
    if (res.error) co_return res.error;
    StrongRef<Node> file = res.file;
    if ((flags & Syscall::O_NOFOLLOW) && co_await file->is_symlink()) co_return Syscall::ELOOP;
    if (write && co_await file->is_dir()) co_return Syscall::EISDIR; // can't write to a directory file
    if (flags & Syscall::O_DIRECTORY && !co_await file->is_dir()) co_return Syscall::ENOTDIR; // can't write to a directory file

    if (flags & Syscall::O_TRUNC) co_await VMM::truncate_file(file, 0);

    co_return t->process->fd_table.open(StrongRef<impl::OpenFile>{new impl::SeekingFile(file, read, write,
      flags & Syscall::O_APPEND, flags & Syscall::O_NOATIME)}, flags & Syscall::O_CLOEXEC);
  }
  case Syscall::DUP3: {
    uint64_t oldFd = c.rdi;
    uint64_t newFd = c.rsi;
    uint64_t flags = c.rdx;

    if (oldFd == newFd) co_return Syscall::EINVAL;
    if (flags & ~Syscall::O_CLOEXEC) co_return Syscall::EINVAL;

    co_return t->process->fd_table.dup(oldFd, newFd, flags & Syscall::O_CLOEXEC);
  }
  case Syscall::PIPE2: {
    uint32_t *pipefd = VA{c.rdi};
    uint64_t flags = c.rsi;

    // we only support cloexec
    if (flags & ~Syscall::O_CLOEXEC) co_return Syscall::EINVAL;

    if (!user_range((uintptr_t) pipefd, sizeof(uint32_t)*2)) co_return Syscall::EFAULT;

    auto [readFile, writeFile] = impl::PipeFile::create();

    uint32_t fds[2];
    int64_t result = t->process->fd_table.open_pair(readFile, writeFile, flags & Syscall::O_CLOEXEC, fds);
    if (result) co_return result;
    if (!write_user(pipefd, fds, sizeof(fds))) {
      t->process->fd_table.close(fds[0]); t->process->fd_table.close(fds[1]);
      co_return Syscall::EFAULT;
    }
    co_return 0;
  }
  case Syscall::CLOSE: {
    // close(fd)
    uint64_t fd = c.rdi;

    co_return t->process->fd_table.close(fd) ? 0 : Syscall::EBADF;
  }
  case Syscall::LSEEK: {
    // lseek(fd, offset, whence)
    uint64_t fd = c.rdi;
    auto offset = (int64_t) c.rsi;
    uint64_t whence = c.rdx;

    auto file = t->process->fd_table.get(fd);
    if (file == nullptr) co_return Syscall::EBADF;
    if (file->type == impl::PIPE) co_return Syscall::ESPIPE;
    if (file->type != impl::SEEKING) co_return Syscall::EBADF;

    co_return co_await file->seeking()->seek(offset, whence);
  }
  case Syscall::GETPID: { co_return t->process->pid; }
  case Syscall::GETCWD:
    co_return co_await getcwd(t, (char*)c.rdi, c.rsi);
  case Syscall::CHDIR: {
    auto *path = (char*)c.rdi;

    UniqueRef<Path> pathname{new Path};
    if (auto error = copy_path(path, pathname->bytes)) co_return error;
    path = pathname->bytes;

    auto res = co_await impl::fs->find(t->process->get_cwd(), path);
    if (res.error) co_return res.error;

    if (!co_await res.file->is_dir()) co_return Syscall::ENOTDIR;

    t->process->set_cwd(res.file);

    co_return 0;
  }
  case Syscall::GETPPID: { co_return t->process->ppid; }
  case Syscall::GETTID: { co_return t->tid; }
  case Syscall::WAIT4: {
    // wait4(pid, status, options, rusage)
    int64_t pid = (int64_t) c.rdi;
    auto *status = (int32_t*) c.rsi;
    uint64_t options = c.rdx;
    auto *rusage = (char*) c.r10;

    if (options != 0 || pid <= 0) co_return Syscall::EINVAL; // we don't support any of these :p

    if (status && !user_range((uintptr_t)status, sizeof(int32_t))) co_return Syscall::EFAULT;
    if (rusage && !user_range((uintptr_t)rusage, Syscall::RUSAGE_STRUCT_SIZE)) co_return Syscall::EFAULT;

    bool found = false;
    uint64_t code = co_await t->process->wait_child(pid, found);
    if (!found) co_return Syscall::ECHILD;
    if (t->zombie) co_return 0; // exec stopped us while we waited: the buffers are no longer ours

    int32_t result = code;
    if (status && !write_user(status, &result, sizeof(result))) co_return Syscall::EFAULT;
    char usage[Syscall::RUSAGE_STRUCT_SIZE]{};
    if (rusage && !write_user(rusage, usage, sizeof(usage))) co_return Syscall::EFAULT;
    co_return pid;
  }
  case Syscall::REBOOT: {
    // reboot(magic, magic2, cmd, arg)
    shutdown(true);
    co_return 0;
  }
  case Syscall::FORK: {
    // fork()
    co_return (co_await t->process->fork(t->context))->pid;
  }
  case Syscall::EXECVEAT:
  case Syscall::EXECVE: {
    uint64_t dir_fd;
    char *path;
    uintptr_t argv, envp;
    uint64_t flags;
    if (c.rax == Syscall::EXECVE) { // execve(path, argv, envp)
      dir_fd = Syscall::AT_FDCWD;
      path = (char*)c.rdi;
      argv = c.rsi;
      envp = c.rdx;
      flags = 0;
    } else { // execveat(dirfd, path, argv, envp, flags)
      dir_fd = c.rdi;
      path = (char*)c.rsi;
      argv = c.rdx;
      envp = c.r10;
      flags = c.r8;
    }

    if (flags & ~Syscall::AT_SYMLINK_NOFOLLOW) co_return Syscall::EINVAL; // we don't support AT_EMPTY_PATH

    UniqueRef<Path> pathname{new Path};
    if (auto error = copy_path(path, pathname->bytes)) co_return error;
    path = pathname->bytes;

    StrongRef<Node> dir = t->process->get_cwd();
    if (path[0] != '/' && dir_fd != Syscall::AT_FDCWD) {
      auto file = t->process->fd_table.get(dir_fd);
      if (file == nullptr) co_return Syscall::EBADF;
      if (file->type != impl::SEEKING) co_return Syscall::ENOTDIR;
      dir = file->seeking()->file;
    }

    auto res = co_await impl::fs->find(dir, path, !(flags & Syscall::AT_SYMLINK_NOFOLLOW));
    if (res.error) co_return res.error;
    if (!co_await res.file->is_file()) co_return Syscall::EACCES;

    char **kargv = copy_strings(argv);
    if (kargv == nullptr) co_return Syscall::EFAULT;
    char **kenvp = copy_strings(envp);
    if (kenvp == nullptr) {
      impl::free_strings(kargv);
      co_return Syscall::EFAULT;
    }

    co_return co_await t->process->exec(t, res.file, kargv, kenvp);
  }
  default:
    co_return Syscall::ENOSYS;
  }
}

async<void> finish_syscall(StrongRef<TCB> t, sync<uint64_t> call) {
  t->context.rax = co_await call;
  if (!t->zombie) impl::thread_queue.add(t);
}

} // namespace

// I know it's odd, but it's just where all my helper functions are >.<
sync<void> TCB::clear_tid() {
  uintptr_t addr = std::exchange(clear_tid_addr, 0);

  if (!addr || !store_tid(addr, 0) || addr % sizeof(uint32_t)) co_return;
  auto mapping = co_await process->as->vma_list_lock;
  if (process->as->memory_areas) futex_wake(futex_key(this, addr, false), 1);
}

void Syscall::init_core() {
  Msr::IA32_LSTAR().write(uint64_t(syscallHandler_));
  Msr::IA32_STAR().write((SYSRET_BASE << 48) |
                         (uint64_t(GDT::KERNEL_CS) << 32));
  Msr::I32_FMASK().write(ENTRY_FLAGS_MASK);
  IDT::interrupt(KILL_VECTOR, uintptr_t(killHandler_));
}

extern "C" [[noreturn]] void syscallHandler() {
  using namespace impl;
  CCB *ccb = CCB::current();
  TCB *raw = ccb->current_thread;
  ASSERT(raw != nullptr);

  bool resume_user = false;
  {
    // All owners must leave scope before resume/bounce abandons this stack.
    StrongRef<TCB> hold{raw->self};
    sync<uint64_t> call = dispatch(raw);

    if (call.await_ready()) {
      raw->context.rax = uint64_t(call.await_resume());
      resume_user = !raw->zombie;
      if (!resume_user) {
        ccb->current_thread = nullptr;
        drop_to_kernel_cr3();
      }
    } else {
      ccb->current_thread = nullptr;
      if (raw->zombie) drop_to_kernel_cr3();
      finish_syscall(hold, std::move(call));
    }
  }

  if (resume_user) raw->resume();
  bounce_to_yield_();
}
