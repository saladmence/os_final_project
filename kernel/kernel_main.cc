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

/*
 * The kernel's own kernel_main: runs /sbin/init from the ramdisk in an
 * address space of its own, prints its exit code ("init exited: N"), and
 * shuts down once it (every thread of it) is done.
 *
 * Weak: a test with a kernel_main of its own (a kernel-side test) replaces
 * it.
 */

#include "async/async.h"
#include "lib/debug.h"
#include "filesystem/ext2.h"
#include "lib/handle_queue.h"
#include "kernel_main.h"
#include "machine/machine.h"
#include "structures/pcb.h"
#include "structures/elf.h"
#include "lib/print.h"
#include "filesystem/ramdisk.h"
#include "lib/shared.h"
#include "io/ps2.h"

namespace impl { StrongRef<Ext2> fs; }

using namespace impl;

async<void> kernel_main() {
    PS2::poll();
    StrongRef<BlockIO> ide{new RamDisk(0)};
    fs = co_await Ext2::mount(ide);

    StrongRef<Node> init = (co_await fs->find(fs->root, "/sbin/init")).file;
    ASSERT(init != nullptr);

    StrongRef<TCB> thread = co_await PCB::create(init, PROCESS_STACK_SIZE);
    ASSERT(thread != nullptr);
    StrongRef<PCB> process = thread->process;
    ASSERT(process != nullptr);

    // setup /sbin/init's fd 0, 1, 2
    StrongRef<OpenFile> COM2{new ConsoleFile{false}};
    process->fd_table.open(COM2, false); // fd 0 <- test.in
    process->fd_table.dup(0, 1, false); // fd 1 -> test.out
    process->fd_table.open(StrongRef<OpenFile>{new ConsoleFile{true}}, false); // fd 2 -> test.debug

    thread_queue.add(thread);
    thread = nullptr;

    uint64_t exit_code = co_await process->exit_code;

    KPRINT("init exited: ?\n", Dec(exit_code));
    co_await fs->flush();
    mark_system_done();
}
