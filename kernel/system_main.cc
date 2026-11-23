/* Copyright (C) 2025 Ahmed Gheith and contributors.
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

#include "system_main.h"

#include "core_main.h"
#include "lib/debug.h"
#include "lib/heap.h"
#include "structures/idt.h"
#include "machine/limine++.h"
#include "lib/print.h"
#include "structures/tlb.h"
#include "memory/vmm.h"
#include "machine/x86_64.h"
#include <cstdint>
#include "io/ps2.h"

using namespace limine;

extern void (*_init_array_start)();
extern void (*_init_array_end)();

char the_heap[6 * 1024 * 1024];

[[gnu::section(
    ".limine_requests")]] constexpr volatile BaseRevision base_revision{};
[[gnu::section(".limine_requests")]] constexpr volatile MultiProcessor mp{};
[[gnu::section(".limine_requests")]] constexpr volatile HHDM limine_hhdm{};
[[gnu::section(".limine_requests")]] constexpr volatile DateAtBoot limine_date{};

uint64_t Sys::core_count = 0;
volatile bool Sys::bootstraping = true;
uint64_t Sys::start_tsc = 0;
uint64_t Sys::tsc_hz = 0;
uint32_t Sys::quantum_ticks = 0;
int64_t Sys::boot_unix_time = 0;
bool Sys::normal = false;
uint64_t Sys::hhdm_offset = 0;

// Measures the TSC over 10 ms of PIT channel 2 (polled, no interrupts)
static uint64_t calibrate_tsc() {
  constexpr uint16_t PIT_TICKS = 11932; // 10 ms at 1.193182 MHz
  const uint8_t gate = inb(0x61) & ~0x03; // speaker off, gate low

  outb(0x61, gate);
  outb(0x43, 0xB0); // channel 2, lobyte/hibyte, mode 0
  outb(0x42, PIT_TICKS & 0xFF);
  outb(0x42, PIT_TICKS >> 8);

  uint64_t start = rdtsc();
  outb(0x61, gate | 0x01);
  while (!(inb(0x61) & 0x20));
  uint64_t elapsed = rdtsc() - start;
  outb(0x61, gate);

  return elapsed * 100;
}

// counts APIC timer ticks over 10 ms, with the timer masked (no interrupt fires)
static uint32_t calibrate_apic() {
  Msr::LVTTimerRegister().write(1 << 16);
  Msr::DivideConfigurationRegister().write(0xB);
  Msr::InitialCountRegister().write(0xFFFFFFFF);

  uint64_t start = rdtsc();
  while (rdtsc() - start < Sys::tsc_hz / 100) {}

  return 0xFFFFFFFF - uint32_t(rdmsr(0x839));
}

uint32_t Sys::unix_time() {
  return uint32_t(boot_unix_time + (rdtsc() - start_tsc) / tsc_hz);
}

extern "C" [[noreturn]] void system_main(void) {

  using namespace Sys;

  // Make sure we were loaded by a limine-compliant bootloader that supports the
  // base revision we require
  ASSERT(base_revision.supported());

  ///////////////////////////////////////////////////////////
  // Check supported CPU features and enable required ones //
  ///////////////////////////////////////////////////////////

  Features features{};
  features.dump();
  ASSERT(features.hasSSE3);
  enable_sse();
  ASSERT(features.hasFSGSBASE);
  CR4().setFSGSBASE();
  CR0().dump();
  CR4().dump();

  start_tsc = rdtsc();

  Sys::hhdm_offset = limine_hhdm.response->offset;

  ASSERT(limine_date.response);
  boot_unix_time = limine_date.response->timestamp;
  tsc_hz = calibrate_tsc();
  quantum_ticks = calibrate_apic();
  KPRINT("tsc ? Hz, boot time ?\n", Dec(tsc_hz), Dec(uint64_t(boot_unix_time)));

  ///////////////////////////
  // Initialize Global IDT //
  ///////////////////////////

  IDT::init_system();

  ////////////////////////////////////////
  // Initialize the TLB shootdown logic //
  ////////////////////////////////////////

  TLB::init_system();

  ////////////////////
  // Initialize VMM //
  ////////////////////

  VMM::init_system();

  ///////////////////////////
  // Initialize core count //
  ///////////////////////////

  ASSERT(mp.response);

  core_count = mp.response->cpu_count;
  // Need x2apic support to be available and enabled. This allows access
  // to the local APIC registers using MSRs instead of MMIO.
  ASSERT(mp.response->flags & LIMINE_MP_RESPONSE_X86_64_X2APIC);

  ////////////////////////////////
  // Initialize the kernel heap //
  ////////////////////////////////
  
  heap::init(uintptr_t(the_heap), sizeof(the_heap));
  
  PS2::init();
  ///////////////////////////////////////////////////////////////////////////
  // Run global initializers -- has to be done after initializing the heap //
  ///////////////////////////////////////////////////////////////////////////

  KPRINT("_init_array_start:? _init_array_end:?\n", &_init_array_start,
         &_init_array_end);

  std::size_t count = &_init_array_end - &_init_array_start;
  for (std::size_t i = 0; i < count; i++) {
    (*((&_init_array_start)[i]))();
  }

  /////////////////////////
  // Wake up other cores //
  /////////////////////////

  KPRINT("has ? cores, flags=?, bsp_lapic_id=?\n", Dec(core_count),
         mp.response->flags, mp.response->bsp_lapic_id);

  impl::start_barrier = new SpinBarrier(core_count);

  uint64_t bsp_index = UINT64_MAX;

  for (uint64_t i = 0; i < core_count; i++) {
    auto info = mp.response->cpus[i];
    // becomes the core's CCB::id
    info->extra_argument = i;
    if (mp.response->bsp_lapic_id != info->lapic_id) {
      info->goto_address = impl::core_main;
    } else {
      bsp_index = i;
    }
  }

  KPRINT("bootstrapping core index=? lapic_id=?\n", Dec(bsp_index),
         Dec(mp.response->cpus[bsp_index]->lapic_id));

  ASSERT(bsp_index != UINT64_MAX);

  impl::core_main(mp.response->cpus[bsp_index]);
}
