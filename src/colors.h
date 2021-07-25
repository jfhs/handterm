#pragma once

#ifndef RGB
#define RGB(r, g, b) (r | (g << 8) | (b << 16))
#endif

const int default_colors[] = {
    RGB(0x0C, 0x0C, 0x0C),
    RGB(0xC5, 0x0F, 0x1F),
    RGB(0x13, 0xA1, 0x0E),
    RGB(0xC1, 0x9C, 0x00),
    RGB(0x00, 0x37, 0xDA),
    RGB(0x88, 0x17, 0x98),
    RGB(0x3A, 0x96, 0xDD),
    RGB(0xCC, 0xCC, 0xCC),
    RGB(0x76, 0x76, 0x76),
    RGB(0xE7, 0x48, 0x56),
    RGB(0x16, 0xC6, 0x0C),
    RGB(0xF9, 0xF1, 0xA5),
    RGB(0x3B, 0x78, 0xFF),
    RGB(0xB4, 0x00, 0x9E),
    RGB(0x61, 0xD6, 0xD6),
    RGB(0xF2, 0xF, 0x2F2)
};

const int default_bg_color = 0x000000;
const int default_fg_color = 0xFFFFFF;