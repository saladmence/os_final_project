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

#include <algorithm>
#include <utility>

#include "async/future.h"
#include "lib/shared.h"
#include "async/spin_lock.h"
#include "machine/user_context.h"
#include "system_main.h"
#include <cstdint>

#include "ccb.h"
#include "elf.h"
#include "async/event.h"
#include "io/console.h"
#include "memory/vmm.h"
#include "io/ps2.h"

namespace impl
{
class AddressSpace;
struct TCB;
bool current_thread_dead();

enum FileType { SEEKING, CONSOLE, PIPE, INPUT };

class SeekingFile;

class OpenFile {
protected:
    OpenFile(FileType type, bool rd, bool wr) : rd(rd), wr(wr), type(type) {}

public:
    const bool rd;
    const bool wr;

    virtual ~OpenFile() = default;

    const FileType type;

    SeekingFile* seeking() { return type == SEEKING ? (SeekingFile*)this : nullptr; }
    virtual sync<uint64_t> read(char *buf, uint64_t len);
    virtual sync<uint64_t> write(char *buf, uint64_t len);
};

class InputFile : public OpenFile {
    bool nonblocking;

public:
    explicit InputFile(bool nonblocking) : OpenFile(INPUT, true, false), nonblocking(nonblocking) {}

    sync<uint64_t> read(char *buf, uint64_t len) override{
        co_return co_await PS2::read_events(buf, len, nonblocking);
    }

    sync<uint64_t> write(char *, uint64_t) override{
        co_return Syscall::EBADF;
    }
};

class SeekingFile : public OpenFile {
    Mutex<uint64_t> cursor{0};
    bool append;
    bool no_atime;

public:
    const StrongRef<Node> file;
    SeekingFile(StrongRef<Node> file, bool rd, bool wr, bool append, bool no_atime) : OpenFile(SEEKING, rd, wr),
    append(append), no_atime(no_atime), file(file) {}

    sync<int64_t> seek(int64_t offset, uint64_t whence) {
        auto proof = co_await cursor;

        int64_t target;
        switch (whence) {
            case Syscall::SEEK_SET: target = offset; break;
            case Syscall::SEEK_CUR:
                if (__builtin_add_overflow(proof.data(), offset, &target)) co_return Syscall::EOVERFLOW;
                break;
            case Syscall::SEEK_END:
                if (__builtin_add_overflow(co_await file->size_in_bytes(), offset, &target)) co_return Syscall::EOVERFLOW;
                break;
            default: co_return Syscall::EINVAL;
        }
        if (target < 0) co_return Syscall::EINVAL;

        proof.data() = target;
        co_return target;
    }

    sync<uint64_t> read(char *buf, uint64_t len) override {
        if (!no_atime) {
            co_await file->edit_inode([] (Ext2Inode& inode) {
                inode.atime = std::max(inode.atime, Sys::unix_time());
            });
        }

        auto proof = co_await cursor;
        if (proof.data() >= UINT32_MAX) co_return 0;
        uint64_t read = co_await file->read_all(proof.data(), std::min<uint64_t>(len, UINT32_MAX - proof.data()), buf);
        proof.data() += read;

        co_return read;
    }

    sync<uint64_t> write(char *buf, uint64_t len) override {
        co_await file->edit_inode([] (Ext2Inode& inode) {
            inode.mtime = std::max(inode.mtime, Sys::unix_time());
        });

        auto proof = co_await cursor;
        if (append) proof.data() = co_await file->size_in_bytes();
        if (proof.data() >= UINT32_MAX) co_return 0;
        int64_t written = co_await file->write_all(proof.data(), std::min<uint64_t>(len, UINT32_MAX - proof.data()), buf);
        if (written > 0) proof.data() += written;

        co_return written;
    }
};

class ConsoleFile : public OpenFile {
    bool debug;
public:
    ConsoleFile(bool debug) : OpenFile(CONSOLE, !debug, true), debug(debug) {}

    sync<uint64_t> read(char *buf, uint64_t len) override {
        if (debug) co_return -1;

        if (len == 0) co_return 0;
        char ch = co_await Console::getch();
        if (ch == '\4') co_return 0;
        buf[0] = ch;
        co_return 1;
    }

    sync<uint64_t> write(char *buf, uint64_t len) override {
        if (debug) {
            print_lock();
            for (uint64_t i = 0; i < len; i++) putch(buf[i]);
            print_unlock();
        } else co_await Console::write(buf, len);

        co_return len;
    }
};

struct PipePage {
    PipePage *next;
    PPN page;
    uint64_t readOffset = 0;
    uint64_t writeOffset = 0;
};

class PipeFile : public OpenFile {
    static constexpr uint16_t PIPE_CAPACITY = 16; // in pages
    static constexpr uint64_t PIPE_BUF = 256; // atomic writes

    struct State {
        SpinLock lock;
        BlockingLock reader_lock;
        Event readable, writable;
        PipePage *head = nullptr, *tail = nullptr;
        uint64_t pages = 0;
        bool reader = true, writer = true;

        ~State() {
            while (head) {
                auto *old = head;
                head = head->next;
                physMem.free(old->page);
                delete old;
            }
        }
    };
    StrongRef<State> state;

    PipeFile(StrongRef<State> state, bool read) : OpenFile(PIPE, read, !read), state(state) {}

public:
    ~PipeFile() override {
        LockGuard g{state->lock};
        if (rd) {
            state->reader = false;
            state->writable.trigger();
        } else {
            state->writer = false;
            state->readable.trigger();
        }
    }

    static std::pair<StrongRef<OpenFile>, StrongRef<OpenFile>> create() {
        StrongRef state{new State};
        return {StrongRef<OpenFile>{new PipeFile{state, true}},
                StrongRef<OpenFile>{new PipeFile{state, false}}};
    }

    sync<uint64_t> read(char *buf, uint64_t len) override {
        if (len == 0) co_return 0;
        auto &s = *state.operator->();
        auto reader = co_await s.reader_lock;
        char bounce[FRAME_SIZE];
        uint64_t available = 0;
        {
            LockGuard g{s.lock};
            while (s.head == nullptr && s.writer) {
                co_await s.readable.wait(s.lock);
                if (current_thread_dead()) co_return 0;
            }
            for (auto *p = s.head; p && available < std::min<uint64_t>(len, FRAME_SIZE); p = p->next) {
                uint64_t n = std::min<uint64_t>(std::min<uint64_t>(len, FRAME_SIZE) - available,
                                                p->writeOffset - p->readOffset);
                memcpy(bounce + available, (char*)VA{p->page}.va() + p->readOffset, n);
                available += n;
            }
        }
        if (current_thread_dead()) co_return 0;
        uint64_t left = VMM::copy_user_(buf, bounce, available);
        len = available - left;
        uint64_t read = 0;
        LockGuard g{s.lock};
        while (len > 0 && s.head) {
            auto *pipe = s.head;
            uint64_t to_read = std::min(len, pipe->writeOffset-pipe->readOffset);
            read += to_read;
            len -= to_read;
            pipe->readOffset += to_read;

            // we've read all the data; may as well clean up, even if it didn't use the whole page
            if (pipe->readOffset == pipe->writeOffset) {
                physMem.free(pipe->page);
                s.head = pipe->next;
                delete pipe;
                --s.pages;
                if (s.head == nullptr) s.tail = nullptr;
                s.writable.trigger();
            }
        }

        co_return read ? read : left ? Syscall::EFAULT : 0;
    }

    sync<uint64_t> write(char *buf, uint64_t len) override {
        auto &s = *state.operator->();
        uint64_t written = 0;
        while (len > 0) {
            uint64_t writing = std::min(len, PIPE_BUF);
            LockGuard g{s.lock};
            // Reserve enough room before copying any of this atomic chunk.
            while (s.reader && s.pages == PIPE_CAPACITY &&
                   (s.tail == nullptr || FRAME_SIZE - s.tail->writeOffset < writing)) {
                co_await s.writable.wait(s.lock);
                if (current_thread_dead()) co_return written;
            }
            if (!s.reader) co_return written == 0 ? Syscall::EPIPE : written;

            while (writing > 0) {
                if (s.tail == nullptr || s.tail->writeOffset == FRAME_SIZE) {
                    auto *page = new PipePage{nullptr, physMem.alloc()};
                    if (s.tail) s.tail->next = page;
                    else s.head = page;
                    s.tail = page;
                    ++s.pages;
                }
                auto *pipe = s.tail;
                uint16_t to_write = std::min(writing, FRAME_SIZE - pipe->writeOffset);

                memcpy(VA{VA{pipe->page}.va() + pipe->writeOffset}, buf, to_write);
                pipe->writeOffset += to_write;
                buf += to_write;

                written += to_write;
                writing -= to_write;
                len -= to_write;
            }
            s.readable.trigger();
        }

        co_return written;
    }
};

struct FileDescriptor {
    StrongRef<OpenFile> file;
    bool close_on_exec = false;
};

struct FileDescriptorTable {
    static constexpr uint64_t MAX_FDS = 1024;
    SpinLock lock;

    uint64_t next_fd;
    uint64_t capacity = 8;
    FileDescriptor *table;
    uint64_t *free_fds;

    [[nodiscard]] uint64_t chunks() const { return (capacity + 64 + 1) / 64; }

    void exec_triggered() {
        LockGuard g{lock};
        for (uint64_t i = 0; i < capacity; i++) {
            if (!is_free(i) && table[i].close_on_exec) do_close(i);
        }
        find_next();
    }

    FileDescriptorTable() {
        next_fd = 0;
        table = new FileDescriptor[capacity];
        free_fds = new uint64_t[chunks()];
        memset(free_fds, 0xFF, chunks() * sizeof(uint64_t));
    }

    ~FileDescriptorTable() {
        delete[] table;
        delete[] free_fds;
    }

    void resize(uint64_t newCapacity = 0) {
        auto old_capacity = capacity;
        auto old_chunks = chunks();
        auto *old_table = table;
        auto *old_free_fds = free_fds;

        if (newCapacity == 0) capacity *= 2;
        else capacity = 1ull << (64 - __builtin_clzll(newCapacity - 1));

        table = new FileDescriptor[capacity];
        free_fds = new uint64_t[chunks()];
        memset(free_fds, 0xFF, chunks() * sizeof(uint64_t));

        for (uint64_t fd = 0; fd < old_capacity; fd++) table[fd] = old_table[fd];
        memcpy(free_fds, old_free_fds, old_chunks * sizeof(uint64_t));

        delete[] old_table;
        delete[] old_free_fds;
    }

    bool is_free(uint64_t fd) const { return free_fds[fd/64] & (1ull << (fd % 64)); }

    void destroy() {
        LockGuard g{lock};
        for (uint64_t i = 0; i < capacity; i++) {
            if (!is_free(i)) do_close(i);
        }
        find_next();
    }

    void find_next() {
        next_fd = capacity;
        for (uint64_t fd = 0; fd < capacity; fd++) {
            if (is_free(fd)) { next_fd = fd; return; }
        }
    }

    uint64_t install(StrongRef<OpenFile> file, bool cloexec) {
        if (next_fd == MAX_FDS) return Syscall::EMFILE;
        if (next_fd == capacity) resize();
        uint64_t fd = next_fd;
        table[fd] = { .file = file, .close_on_exec = cloexec };
        free_fds[fd/64] &= ~(1ull << (fd % 64));
        find_next();

        return fd;
    }

    uint64_t open(StrongRef<OpenFile> file, bool cloexec) {
        LockGuard g{lock};
        return install(file, cloexec);
    }

    int64_t open_pair(StrongRef<OpenFile> read, StrongRef<OpenFile> write, bool cloexec, uint32_t *fds) {
        LockGuard g{lock};
        if (capacity == MAX_FDS) {
            uint64_t available = 0;
            for (uint64_t fd = 0; fd < capacity && available < 2; ++fd) available += is_free(fd) != 0;
            if (available < 2) return Syscall::EMFILE;
        }
        fds[0] = install(read, cloexec);
        fds[1] = install(write, cloexec);
        return 0;
    }

    uint64_t dup(uint64_t oldFd, uint64_t newFd, bool cloexec) {
        LockGuard g{lock};
        if (oldFd >= capacity) return Syscall::EBADF;
        if (is_free(oldFd)) return Syscall::EBADF;
        if (newFd >= MAX_FDS) return Syscall::EBADF;

        if (newFd >= capacity) resize(newFd+1);
        if (!is_free(newFd)) do_close(newFd);

        table[newFd] = { .file = table[oldFd].file, .close_on_exec = cloexec };
        free_fds[newFd/64] &= ~(1ull << (newFd % 64));

        if (newFd == next_fd) find_next();

        return newFd;
    }

    // Copy ownership under the table lock; the caller may safely suspend.
    StrongRef<OpenFile> get(uint64_t fd) {
        LockGuard g{lock};
        if (fd >= capacity || is_free(fd)) return {};
        return table[fd].file;
    }

    void do_close(uint64_t fd) {
        ASSERT(!is_free(fd));

        table[fd].file = nullptr;

        free_fds[fd/64] |= 1ull << (fd % 64);
    }

    bool close(uint64_t fd) {
        LockGuard g{lock};
        if (fd >= capacity) return false;

        if (is_free(fd)) return false;

        do_close(fd);

        find_next();

        return true;
    }

    void copy_from(FileDescriptorTable &parent) {
        LockGuard g{parent.lock};

        delete[] table;
        delete[] free_fds;

        capacity = parent.capacity;
        table = new FileDescriptor[capacity];
        free_fds = new uint64_t[chunks()];
        memset(free_fds, 0xFF, chunks() * sizeof(uint64_t));

        for (uint64_t fd = 0; fd < parent.capacity; fd++) {
            if (parent.is_free(fd)) continue;

            table[fd] = parent.table[fd];
            free_fds[fd/64] &= ~(1ull << (fd % 64));
        }
        find_next();

        next_fd = parent.next_fd;
    }
};

/*
 * Ownership (no PCB↔TCB strong cycles):
 *
 * PCB
 *   - StrongRef<AddressSpace> as: owns the mm
 *   - WeakRef<PCB> self: upgrade for free_mm / mmap without StrongRef{this}
 *   - process_table: WeakRef (lookup only)
 *   - leader/tail: non-owning intrusive list
 *
 * TCB
 *   - StrongRef<PCB> process: thread keeps process alive
 *   - WeakRef<TCB> self: upgrade from raw TCB* (current_thread, list walk)
 *   - StrongRef<TCB> anchor: process membership pin; cleared in free_mm
 *   - StrongRef<TCB> queue_pin: held while on thread_queue
 *
 * CCB::current_thread stays a raw TCB* for assembly. Never leave a StrongRef
 * on the stack across bounce_to_yield_ / resume_user_ (those abandon the stack).
 * Blocking syscalls hold StrongRef in the finish_syscall coroutine frame.
 */
struct PCB {
    StrongRef<AddressSpace> const as{};
    WeakRef<PCB> self;
    uint64_t const pid;
    uint64_t ppid = 0;
    FileDescriptorTable fd_table;

    struct SignalAction { uint64_t handler = 0, flags = 0, restorer = 0, mask = 0; };
    static constexpr uint64_t SIGNALS = (1ull<<1)|(1ull<<6)|(1ull<<9)|(1ull<<10)|(1ull<<13)|(1ull<<14);
    SpinLock signal_lock;
    SignalAction actions[16];
    uint64_t pending = 0, alarm_deadline = 0;
    uint32_t signal_sender[16]{};
    uint32_t death_signal = 0;
    bool finished = false;
    static int64_t send_signal(int64_t pid, uint32_t sig, uint32_t sender);
    void terminate(uint32_t sig);

private:
    SpinLock cwd_lock;
    StrongRef<Node> cwd;
public:
    StrongRef<Node> get_cwd() {
        LockGuard g{cwd_lock};
        return cwd;
    }
    void set_cwd(StrongRef<Node> dir) {
        LockGuard g{cwd_lock};
        cwd = dir;
    }

    // owned by the parent until wait_child reaps it
    StrongRef<PCB> children{};
    StrongRef<PCB> sibling{};

private:
    static ChainingHashMap<WeakRef<PCB>> process_table;
    static uint64_t current_pid;
    static bool has_rolled_over;
    static SpinLock pid_lock;

    static uint64_t provision_pid();

    explicit PCB(uint64_t pid)
        : as(AddressSpace::create()), pid(pid) {}

public:
    Mutex<uint32_t> alive_threads;
    // Non-owning. Lifetime: each TCB's anchor until free_mm clears it.
    TCB *leader = nullptr;
    TCB *tail = nullptr;

    Future<uint64_t> exit_code;

    static sync<StrongRef<TCB>> create(StrongRef<Node> init, uint64_t stack_size);

private:
    SpinLock forkLock;
public:
    sync<StrongRef<PCB>> fork(const UserContext &ctx);
    sync<uint64_t> exec(TCB *t, StrongRef<Node> file, char **argv, char **envp);
    sync<uint64_t> wait_child(uint64_t pid, bool &found);
    sync<StrongRef<TCB>> create_thread(uint64_t stack_size);
    void finish(uint64_t exit_code);

    PCB(const PCB &) = delete;
    PCB &operator=(const PCB &) = delete;

    ~PCB() = default;
};

struct DrainDeadMm;
struct MmKillNode
{
    MmKillNode *next;

    uint64_t kill_target_cr3;
    DrainDeadMm *kill_drain_request;
};
inline SpinLock kill_list_lock{};
inline MmKillNode *kill_list = nullptr;

void drop_to_kernel_cr3();

constexpr uint64_t USER_FLAGS = 0xCD5;
constexpr uint64_t FORCED_FLAGS = 0x202;
constexpr uint64_t PROCESS_STACK_SIZE = 64 * 1024;

void free_strings(char **v);
sync<void> sleep_until(uint64_t deadline);
void poll_sleepers();

struct TCB {
    static SpinLock tid_lock;
    static uint64_t current_tid;
    static uint64_t provision_tid();

    // Must stay first — syscallHandler_ / offsetof.
    UserContext context{};

    Future<uint64_t> exit_code;

    const uint64_t tid;
    StrongRef<PCB> const process{};

    WeakRef<TCB> self{};
    // Process-list pin (keeps TCB alive in user / between queue ops).
    StrongRef<TCB> anchor{};
    // thread_queue pin.
    StrongRef<TCB> queue_pin{};

    TCB *next_process = nullptr;
    TCB *next_ready = nullptr;

    bool zombie = false;
    bool preempted = false;   // set by the timer; picks the thread's queue on its next add
    uint64_t ready_at = 0;    // TSC when it was last queued
    uint64_t clear_tid_addr = 0;
    sync<void> clear_tid();
    uint64_t blocked_signals = 0, signal_frame = 0;
    bool deliver_signal(int forced = 0, uintptr_t address = 0, int code = 0);
    bool restore_signal();

    // Only stacks allocated by setup_stack are kernel-owned. clone3 stacks
    // are caller-owned and must not be unmapped by the thread-exit path.
    uint64_t stack_base = 0, stack_size = 0;

    TCB(StrongRef<PCB> process, uint64_t tid)
        : tid(tid), process(process) {
        context.rflags = FORCED_FLAGS;
    }

    sync<bool> setup_stack(uint64_t stack_size) {
        CCB::current()->current_thread = this;

        this->stack_size = stack_size;
        stack_base = (uintptr_t)co_await VMM::mmap(
            (void*)(0x700000000000ull - tid * 0x100000ull), stack_size, VMM::PROT_READ | VMM::PROT_WRITE,
            VMM::MAP_PRIVATE | VMM::MAP_ANONYMOUS, {}, 0);
        if ((void *)stack_base == VMM::MAP_FAILED) co_return false;

        uintptr_t stack_top = (stack_base + stack_size) & ~uintptr_t(15);
        context.rsp = stack_top;
        co_return true;
    }

    TCB(StrongRef<PCB> process, const UserContext &parent_ctx, uint64_t tid)
        : context(parent_ctx), tid(tid), process(process) {}

    sync<void> load_and_enter(StrongRef<Node> file) {
        CCB::current()->current_thread = this; // all memory operations should operate with this thread
        context.rip = co_await ELF::load(file);
        constexpr char name[] = "/sbin/init";
        uintptr_t arg = context.rsp - sizeof(name);
        memcpy((void*)arg, name, sizeof(name));
        // argc, argv[0], NULL, envp NULL, AT_NULL.
        uint64_t *sp = (uint64_t*)((arg - 6 * sizeof(uint64_t)) & ~uintptr_t(15));
        for (int i = 0; i < 6; i++) sp[i] = 0;
        sp[0] = 1;
        sp[1] = arg;
        context.rsp = (uintptr_t)sp;
    }

    [[noreturn]] void resume();
};
} // namespace impl
