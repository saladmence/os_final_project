#include "ps2.h"

#include "machine/machine.h"
#include "lib/print.h"
#include <cstdint>
#include "async/async.h"
#include "ps2_keymap.h"
#include "async/event.h"
#include "structures/pcb.h"
#include "syscall.h"

namespace {

constexpr uint16_t DATA = 0x60;
constexpr uint16_t STATUS = 0x64;
constexpr uint16_t COMMAND = 0x64;
constexpr uint64_t MAX_TRIES = 1000000;
constexpr uint64_t INPUT_CAPACITY = 256;

input_event input_ring[INPUT_CAPACITY]{};
uint64_t input_head = 0;
uint64_t input_count = 0;
uint64_t input_dropped = 0;

SpinLock input_lock;
::Event input_available;

bool wait_then_write() {
    for (uint64_t i = 0; i < MAX_TRIES; i++) {
        if (!(inb(STATUS) & 0x02)) return true;
    }
    KPRINT("ps2 timed out waiting to write\n");
    return false;
}

bool wait_then_read() {
    for (uint64_t i = 0; i < MAX_TRIES; i++) {
        if (inb(STATUS) & 0x01) return true;
    }
    KPRINT("ps2 timed out waiting to read\n");
    return false;
}

// send command to the controller
void send_cmd(uint8_t cmd) {
    wait_then_write();
    outb(COMMAND, cmd);
}

// send data to the keyboard
void send_data(uint8_t byte) {
    wait_then_write();
    outb(DATA, byte);
}

// read data from the keyboard
uint8_t read_data() {
    wait_then_read();
    return inb(DATA);
}

} // namespace

void PS2::init() {
  send_cmd(0xAD);
  send_cmd(0xA7); // turn off keyboard and mouse

  while (inb(STATUS) & 0x01) { // clear port
    inb(DATA);
  } 

  send_cmd(0x20);
  uint8_t config = read_data();

  config &= ~(0x01 | 0x02); // interrupts off
  config &= ~(0x10 | 0x20); // clocks on
  config |= 0x40; // translation on

  send_cmd(0x60);
  send_data(config);

  send_cmd(0xA8); // turn on mouse

  // testing mouse sending works
  send_cmd(0xD4);
  send_data(0xF6); // reset settings to default
  uint8_t ack = read_data();
  KPRINT("mouse ack ?\n", ack);

  send_cmd(0xD4);
  send_data(0xF4); // send me movement
  ack = read_data();
  KPRINT("mouse ack ?\n", ack);

  send_cmd(0xAE);
}

void PS2::decode_keyboard(uint8_t byte) {
    // TODO: lock e0_detected if we have multiple cores
    static bool e0_detected = false;
    bool key_released = (byte & 0x80);
    uint8_t key_code = (byte & 0x7F); 
    
    if (byte == 0xE0) e0_detected = true;
    else {
        uint16_t decoded_key = 0;
        if (e0_detected) {
            decoded_key = PS2::E0_TO_SDL[key_code];
            e0_detected = false;
        } else decoded_key = PS2::SET1_TO_SDL[key_code];
        
        if (decoded_key == 0) KPRINT("ps2: unknown key byte ?\n", byte);
        else handle_event(Event{Type::KEYBOARD, decoded_key, key_released ? 0 : 1});
    }
}

void PS2::decode_mouse(uint8_t byte) {
    // TODO: lock these if we have multiple cores
    // TODO: no scrolling atm
    static uint8_t packet_no = 0;
    static uint8_t packet[3];
    static uint8_t prev_buttons = 0;

    // out of sync check
    if (packet_no == 0 && !(byte & 0x08)) {
        KPRINT("ps2: mouse resync, dropped byte ?\n", byte);
        return;
    }

    packet[packet_no] = byte;
    packet_no++;
    if (packet_no < 3) return;
    packet_no = 0;

    uint8_t flags = packet[0];

    // movement
    int16_t dx = packet[1];
    int16_t dy = packet[2];
    if (flags & 0x10) dx -= 256;
    if (flags & 0x20) dy -= 256;
    if (flags & 0xC0) dx = dy = 0; // overflow

    // dy > 0 = mouse moved up, need to resolve w graphics
    // if we should invert for them

    if (dx != 0) handle_event(Event{Type::MOTION, 0, dx});
    if (dy != 0) handle_event(Event{Type::MOTION, 1, dy});

    // buttons
    uint8_t buttons = flags & 0x07;
    uint8_t changed = buttons ^ prev_buttons;
    for (uint16_t i = 0; i < 3; i++) {
        if (changed & (1 << i)) handle_event(Event{Type::BUTTON, i, (buttons & (1 << i)) ? 1 : 0});
    }
    prev_buttons = buttons;
}

async<void> PS2::poll() {
    // TODO: loop never ends, havent decided how we know when to exit
    while (true) {
        uint8_t status_byte = inb(STATUS);
        if(!(status_byte & 0x01)){
            co_await impl::sleep_until(rdtsc() + (Sys::tsc_hz + 999) / 1000);
        }
        else {
            uint8_t data_byte = inb(DATA);
            if (status_byte & 0x20) decode_mouse(data_byte);
            else decode_keyboard(data_byte);
        }
    }
    
}

void PS2::handle_event(Event event) {
    LockGuard guard{input_lock};
    if (input_count == INPUT_CAPACITY){
        input_dropped++;
        return;
    }

    uint64_t tail = (input_head+input_count)%INPUT_CAPACITY;

    input_ring[tail] = input_event{(uint16_t)(event.type), event.code, event.value};

    input_count++;
    input_available.trigger();
}

bool PS2::events_ready() {
    LockGuard guard{input_lock};
    return input_count != 0;
}

uint64_t PS2::dropped_events() {
    LockGuard guard{input_lock};
    return input_dropped;
}

sync<uint64_t> PS2::read_events(char *buf, uint64_t len, bool nonblocking) {
    if(len == 0) co_return 0;

    if(len%sizeof(input_event) != 0){
        co_return Syscall::EINVAL;
    }

    LockGuard guard{input_lock};
    while(input_count == 0){
        if(nonblocking) co_return Syscall::EAGAIN;

        co_await input_available.wait(input_lock);

        if(impl::current_thread_dead()) co_return 0;
    }

    uint64_t limit = len/sizeof(input_event);
    uint64_t copied = 0;

    while(copied < limit && input_count != 0) {
        memcpy(buf + copied*sizeof(input_event), &input_ring[input_head], sizeof(input_event));

        input_head = (input_head+1)%INPUT_CAPACITY;
        input_count--;
        copied++;
    }

    co_return copied * sizeof(input_event);
}
