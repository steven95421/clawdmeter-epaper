#pragma once
#include <Arduino.h>

// JD79667 4-colour (Black/White/Yellow/Red) e-paper driver for the
// Waveshare ESP32-S3-ePaper-1.54G (200x200). Board-private to
// waveshare_epaper_154g; vendored from our standalone firmware's epd_1in54g
// (an Arduino port of Waveshare's epaper_port.c). Pins come from board.h.
//
// NOTE: this panel is FULL-REFRESH-ONLY — ~15 s fast-LUT, no partial mode.
// The framebuffer is 2 bits/pixel, 4 px/byte, MSB-first, row-major.

#define EPD_W 200
#define EPD_H 200

// 2 bits per pixel, packed 4 pixels per byte. One row = 50 bytes.
#define EPD_BYTES_PER_ROW   (EPD_W / 4)
#define EPD_FRAMEBUF_BYTES  (EPD_BYTES_PER_ROW * EPD_H)   // 10 000 bytes

// Colour index -> raw 2-bit value the controller expects.
#define EPD_BLACK   0x0
#define EPD_WHITE   0x1
#define EPD_YELLOW  0x2
#define EPD_RED     0x3

void epd_power_on();    // GPIO6 is ACTIVE-LOW (drive LOW = panel powered)
void epd_power_off();
void epd_init();        // full-colour LUT (~20 s, 5-6 flash cycles)
void epd_init_fast();   // fast LUT (~15 s, slightly muted colour)
void epd_clear(uint8_t color);
void epd_display(const uint8_t* image);   // push 10000-byte 2bpp buffer + refresh
void epd_sleep();       // deep sleep, retains the image
