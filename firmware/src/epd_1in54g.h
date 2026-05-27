#pragma once
#include <Arduino.h>

// Arduino port of Waveshare's epaper_port.{h,c} for the
// ESP32-S3-ePaper-1.54G (200x200, 4-colour: Black/White/Yellow/Red,
// JD79667-class controller). Commands and timing taken verbatim from the
// upstream ESP-IDF demo (Example/ESP-IDF_5.5.1/09_E_Paper_Test).

#define EPD_W 200
#define EPD_H 200

// 2 bits per pixel, packed 4 pixels per byte. One row = 50 bytes.
#define EPD_BYTES_PER_ROW   (EPD_W / 4)
#define EPD_FRAMEBUF_BYTES  (EPD_BYTES_PER_ROW * EPD_H)   // 10 000 bytes

// Colour index → raw 2-bit value the controller expects.
#define EPD_BLACK   0x0
#define EPD_WHITE   0x1
#define EPD_YELLOW  0x2
#define EPD_RED     0x3

// Power-gate the panel (GPIO6 is ACTIVE-LOW on this board).
void epd_power_on();
void epd_power_off();

// Full init: SPI + GPIO + reset + controller init sequence. Must be called
// after epd_power_on() (with ≥10 ms gap, the upstream demo waits exactly that).
// Uses the panel's full-colour LUT — ~20s refresh, 5-6 visible flash cycles.
void epd_init();

// Fast-mode init — programs an alternate LUT (extra 0xE0/0xE6/0xA5 cmds from
// Waveshare's EPD_1IN54G_Init_Fast). Subsequent epd_display() / epd_clear()
// calls finish in ~15s with fewer flash cycles, at the cost of slightly
// muted colour. Recommended when the display content changes often and the
// user notices the strobing.
void epd_init_fast();

// Fill the whole panel with one colour and refresh.
void epd_clear(uint8_t color);

// Push a packed 2bpp framebuffer (EPD_FRAMEBUF_BYTES long) and refresh.
void epd_display(const uint8_t* image);

// Put the panel into deep sleep to preserve the image and save power.
void epd_sleep();
