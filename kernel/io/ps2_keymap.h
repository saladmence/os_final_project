#pragma once

#include <cstdint>

// scancode set to SDL3 scancodes
// TODO: ask graphics team if they're using sdl3

namespace PS2 {

// no prefix keys
// 0 = unknown
constexpr uint16_t SET1_TO_SDL[128] = {
      0, // 0x00
     41, // 0x01 Esc
     30, // 0x02 1
     31, // 0x03 2
     32, // 0x04 3
     33, // 0x05 4
     34, // 0x06 5
     35, // 0x07 6
     36, // 0x08 7
     37, // 0x09 8
     38, // 0x0A 9
     39, // 0x0B 0
     45, // 0x0C -
     46, // 0x0D =
     42, // 0x0E Backspace
     43, // 0x0F Tab
     20, // 0x10 Q
     26, // 0x11 W
      8, // 0x12 E
     21, // 0x13 R
     23, // 0x14 T
     28, // 0x15 Y
     24, // 0x16 U
     12, // 0x17 I
     18, // 0x18 O
     19, // 0x19 P
     47, // 0x1A [
     48, // 0x1B ]
     40, // 0x1C Enter
    224, // 0x1D Left Ctrl
      4, // 0x1E A
     22, // 0x1F S
      7, // 0x20 D
      9, // 0x21 F
     10, // 0x22 G
     11, // 0x23 H
     13, // 0x24 J
     14, // 0x25 K
     15, // 0x26 L
     51, // 0x27 ;
     52, // 0x28 '
     53, // 0x29 `
    225, // 0x2A Left Shift
     49, // 0x2B backslash
     29, // 0x2C Z
     27, // 0x2D X
      6, // 0x2E C
     25, // 0x2F V
      5, // 0x30 B
     17, // 0x31 N
     16, // 0x32 M
     54, // 0x33 ,
     55, // 0x34 .
     56, // 0x35 /
    229, // 0x36 Right Shift
     85, // 0x37 Keypad *
    226, // 0x38 Left Alt
     44, // 0x39 Space
     57, // 0x3A Caps Lock
     58, // 0x3B F1
     59, // 0x3C F2
     60, // 0x3D F3
     61, // 0x3E F4
     62, // 0x3F F5
     63, // 0x40 F6
     64, // 0x41 F7
     65, // 0x42 F8
     66, // 0x43 F9
     67, // 0x44 F10
     83, // 0x45 Num Lock
     71, // 0x46 Scroll Lock
     95, // 0x47 Keypad 7
     96, // 0x48 Keypad 8
     97, // 0x49 Keypad 9
     86, // 0x4A Keypad -
     92, // 0x4B Keypad 4
     93, // 0x4C Keypad 5
     94, // 0x4D Keypad 6
     87, // 0x4E Keypad +
     89, // 0x4F Keypad 1
     90, // 0x50 Keypad 2
     91, // 0x51 Keypad 3
     98, // 0x52 Keypad 0
     99, // 0x53 Keypad .
    154, // 0x54 SysRq (Alt+Print Screen)
      0, // 0x55
    100, // 0x56 extra key left of Z (ISO keyboards)
     68, // 0x57 F11
     69, // 0x58 F12
    103, // 0x59 Keypad =
      0, // 0x5A
      0, // 0x5B
    140, // 0x5C International 6
      0, // 0x5D
      0, // 0x5E
      0, // 0x5F
      0, // 0x60
      0, // 0x61
      0, // 0x62
      0, // 0x63
    104, // 0x64 F13
    105, // 0x65 F14
    106, // 0x66 F15
    107, // 0x67 F16
    108, // 0x68 F17
    109, // 0x69 F18
    110, // 0x6A F19
    111, // 0x6B F20
    112, // 0x6C F21
    113, // 0x6D F22
    114, // 0x6E F23
      0, // 0x6F
    136, // 0x70 Katakana/Hiragana
      0, // 0x71
      0, // 0x72
    135, // 0x73 Ro
      0, // 0x74
      0, // 0x75
    115, // 0x76 F24
      0, // 0x77
      0, // 0x78
    138, // 0x79 Henkan
      0, // 0x7A
    139, // 0x7B Muhenkan
      0, // 0x7C
    137, // 0x7D Yen
    133, // 0x7E Keypad , (Brazil)
      0, // 0x7F
};

// keys that come after an E0 byte
constexpr uint16_t E0_TO_SDL[128] = {
      0, // 0x00
      0, // 0x01
      0, // 0x02
      0, // 0x03
      0, // 0x04
      0, // 0x05
      0, // 0x06
      0, // 0x07
      0, // 0x08
      0, // 0x09
      0, // 0x0A
      0, // 0x0B
      0, // 0x0C
      0, // 0x0D
      0, // 0x0E
      0, // 0x0F
    268, // 0x10 Previous Track
      0, // 0x11
      0, // 0x12
      0, // 0x13
      0, // 0x14
      0, // 0x15
      0, // 0x16
      0, // 0x17
      0, // 0x18
    267, // 0x19 Next Track
      0, // 0x1A
      0, // 0x1B
     88, // 0x1C Keypad Enter
    228, // 0x1D Right Ctrl
      0, // 0x1E
      0, // 0x1F
    127, // 0x20 Mute
      0, // 0x21 Calculator (no SDL3 code)
    271, // 0x22 Play/Pause
      0, // 0x23
    269, // 0x24 Stop
      0, // 0x25
      0, // 0x26
      0, // 0x27
      0, // 0x28
      0, // 0x29
      0, // 0x2A fake Left Shift (part of Print Screen), ignore
      0, // 0x2B
      0, // 0x2C
      0, // 0x2D
    129, // 0x2E Volume Down
      0, // 0x2F
    128, // 0x30 Volume Up
      0, // 0x31
    281, // 0x32 Browser Home
      0, // 0x33
      0, // 0x34
     84, // 0x35 Keypad /
      0, // 0x36 fake Right Shift, ignore
     70, // 0x37 Print Screen
    230, // 0x38 Right Alt
      0, // 0x39
      0, // 0x3A
      0, // 0x3B
      0, // 0x3C
      0, // 0x3D
      0, // 0x3E
      0, // 0x3F
      0, // 0x40
      0, // 0x41
      0, // 0x42
      0, // 0x43
      0, // 0x44
      0, // 0x45
     72, // 0x46 Ctrl+Break
     74, // 0x47 Home
     82, // 0x48 Up
     75, // 0x49 Page Up
      0, // 0x4A
     80, // 0x4B Left
      0, // 0x4C
     79, // 0x4D Right
      0, // 0x4E
     77, // 0x4F End
     81, // 0x50 Down
     78, // 0x51 Page Down
     73, // 0x52 Insert
     76, // 0x53 Delete
      0, // 0x54
      0, // 0x55
      0, // 0x56
      0, // 0x57
      0, // 0x58
      0, // 0x59
      0, // 0x5A
    227, // 0x5B Left Windows/Command
    231, // 0x5C Right Windows/Command
    101, // 0x5D Menu
    102, // 0x5E Power
    258, // 0x5F Sleep
      0, // 0x60
      0, // 0x61
      0, // 0x62
    259, // 0x63 Wake
      0, // 0x64
    280, // 0x65 Browser Search
    286, // 0x66 Browser Favorites
    285, // 0x67 Browser Refresh
    284, // 0x68 Browser Stop
    283, // 0x69 Browser Forward
    282, // 0x6A Browser Back
      0, // 0x6B My Computer (no SDL3 code)
      0, // 0x6C Mail (no SDL3 code)
    272, // 0x6D Media Select
      0, // 0x6E
      0, // 0x6F
      0, // 0x70
      0, // 0x71
      0, // 0x72
      0, // 0x73
      0, // 0x74
      0, // 0x75
      0, // 0x76
      0, // 0x77
      0, // 0x78
      0, // 0x79
      0, // 0x7A
      0, // 0x7B
      0, // 0x7C
      0, // 0x7D
      0, // 0x7E
      0, // 0x7F
};

} // namespace PS2
