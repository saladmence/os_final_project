#include "ps2.h"

#include "machine/machine.h"
#include "lib/print.h"
#include <cstdint>

namespace {

constexpr uint16_t DATA = 0x60;
constexpr uint16_t STATUS = 0x64;
constexpr uint16_t COMMAND = 0x64;
constexpr uint64_t MAX_TRIES = 1000000;

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
