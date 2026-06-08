#include "epd.h"
#include "board.h"
#include <SPI.h>

// Pins come from board.h:
//   EPD_PWR  = GPIO6  (ACTIVE-LOW power enable)
//   EPD_BUSY = GPIO8 / EPD_RST = GPIO9 / EPD_DC = GPIO10
//   EPD_CS   = GPIO11 / EPD_SCLK = GPIO12 / EPD_MOSI = GPIO13
// Controller commands/timing are verbatim from Waveshare's epaper_port.c
// (ESP-IDF demo 09_E_Paper_Test) for the JD79667-class 4-colour panel.

static SPIClass&   s_spi = SPI;
static SPISettings s_spi_cfg(20000000, MSBFIRST, SPI_MODE0);  // 20 MHz

static inline void cs_low()  { digitalWrite(EPD_CS, LOW); }
static inline void cs_high() { digitalWrite(EPD_CS, HIGH); }
static inline void dc_cmd()  { digitalWrite(EPD_DC, LOW);  }
static inline void dc_data() { digitalWrite(EPD_DC, HIGH); }

static void epd_reset() {
    digitalWrite(EPD_RST, HIGH); delay(200);
    digitalWrite(EPD_RST, LOW);  delay(20);
    digitalWrite(EPD_RST, HIGH); delay(200);
}

// Waveshare "readbusyh": wait for BUSY to go HIGH (ready). Deliberately has
// NO deadline — a JD79667 full refresh takes ~15 s; a bounded wait (like the
// SSD1681 port's 5 s) would abort mid-frame and corrupt the image.
static void epd_wait_idle() {
    delay(100);
    while (digitalRead(EPD_BUSY) == LOW) delay(50);
}

static void send_cmd(uint8_t cmd)   { dc_cmd();  cs_low(); s_spi.transfer(cmd);  cs_high(); }
static void send_data(uint8_t data) { dc_data(); cs_low(); s_spi.transfer(data); cs_high(); }

static void send_data_buffer(const uint8_t* buf, size_t len) {
    dc_data(); cs_low();
    // transferBytes is NON-destructive; transfer(void*, len) would overwrite
    // the source buffer with MISO bytes and corrupt the framebuffer mid-push.
    s_spi.transferBytes(buf, nullptr, len);
    cs_high();
}

void epd_power_on() {
    pinMode(EPD_PWR, OUTPUT);
    digitalWrite(EPD_PWR, LOW);   // ACTIVE-LOW — 0 = panel powered
    delay(10);
}

void epd_power_off() { digitalWrite(EPD_PWR, HIGH); }

// Shared controller bring-up (SPI + reset + init sequence up to power-on).
static void epd_common_init() {
    pinMode(EPD_CS,   OUTPUT);
    pinMode(EPD_DC,   OUTPUT);
    pinMode(EPD_RST,  OUTPUT);
    pinMode(EPD_BUSY, INPUT_PULLUP);
    cs_high();

    s_spi.begin(EPD_SCLK, /*MISO*/-1, EPD_MOSI, /*ss handled manually*/-1);
    s_spi.beginTransaction(s_spi_cfg);

    epd_reset();

    send_cmd(0x4D); send_data(0x78);

    send_cmd(0x00);                              // PSR
    send_data(0x0F); send_data(0x29);

    send_cmd(0x06);                              // BTST_P
    send_data(0x0D); send_data(0x12); send_data(0x30); send_data(0x20);
    send_data(0x19); send_data(0x2A); send_data(0x22);

    send_cmd(0x50); send_data(0x37);             // CDI

    send_cmd(0x61);                              // resolution
    send_data(EPD_W / 256); send_data(EPD_W % 256);
    send_data(EPD_H / 256); send_data(EPD_H % 256);

    send_cmd(0xE9); send_data(0x01);
    send_cmd(0x30); send_data(0x08);

    send_cmd(0x04);                              // power on
    epd_wait_idle();
}

void epd_init() { epd_common_init(); }

void epd_init_fast() {
    epd_common_init();
    // Fast-LUT enable (extra 0xE0/0xE6/0xA5 from Waveshare's *_Init_Fast).
    send_cmd(0xE0); send_data(0x02);
    send_cmd(0xE6); send_data(0x5D);
    send_cmd(0xA5); send_data(0x00);             // engage fast LUT
    epd_wait_idle();
}

static void epd_turn_on_display() {
    send_cmd(0x12);                              // DISPLAY_REFRESH
    send_data(0x00);
    epd_wait_idle();
}

void epd_clear(uint8_t color) {
    uint8_t packed = (color << 6) | (color << 4) | (color << 2) | color;
    send_cmd(0x10);
    dc_data(); cs_low();
    for (int j = 0; j < EPD_H; j++)
        for (int i = 0; i < EPD_BYTES_PER_ROW; i++) s_spi.transfer(packed);
    cs_high();
    epd_turn_on_display();
}

void epd_display(const uint8_t* image) {
    send_cmd(0x10);
    send_data_buffer(image, EPD_FRAMEBUF_BYTES);
    epd_turn_on_display();
}

void epd_sleep() {
    send_cmd(0x02); send_data(0x00); epd_wait_idle();
    send_cmd(0x07); send_data(0xA5);
}
