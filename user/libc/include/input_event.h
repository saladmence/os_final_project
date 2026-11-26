#pragma once

#include <stdint.h>

// type: 1 = keyboard, 2 = mouse button, 3 = mouse motion
struct input_event{
  uint16_t type;
  uint16_t code;
  int32_t value;
};