#pragma once

#include <cstdint>
#include "async/async.h"

namespace PS2 {

enum class Type : uint64_t {
    KEYBOARD = 1, BUTTON = 2, MOTION = 3
};

struct Event {
    Type type;
    uint16_t code; // sdl scancode, button, axis
    int32_t value; // how much mouse movement, pressed, released (also if we add scroll wheel)
};

// sets up ps2 controller
void init();

void decode_keyboard(uint8_t byte);

void decode_mouse(uint8_t byte);

void handle_event(Event event);

async<void> poll();

} // namespace PS2
