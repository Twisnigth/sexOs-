// =============================================================================
//  kernel/rtc.c -- Lecture de l'horloge CMOS
// =============================================================================
#include "rtc.h"
#include "io.h"
#include "klib.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static int update_in_progress(void) {
    outb(CMOS_ADDR, 0x0A);
    return inb(CMOS_DATA) & 0x80;
}

static uint8_t bcd_to_bin(uint8_t v) { return (v & 0x0F) + ((v >> 4) * 10); }

void rtc_init(void) {
    rtc_time_t t;
    rtc_now(&t);
    char buf[24];
    rtc_format(&t, buf);
    kprintf("[rtc] date/heure : %s\n", buf);
}

void rtc_now(rtc_time_t *out) {
    while (update_in_progress()) {}      // attend la fin d'une mise à jour

    uint8_t sec = cmos_read(0x00);
    uint8_t min = cmos_read(0x02);
    uint8_t hr  = cmos_read(0x04);
    uint8_t day = cmos_read(0x07);
    uint8_t mon = cmos_read(0x08);
    uint8_t yr  = cmos_read(0x09);
    uint8_t reg_b = cmos_read(0x0B);

    if (!(reg_b & 0x04)) {               // valeurs en BCD -> binaire
        sec = bcd_to_bin(sec);
        min = bcd_to_bin(min);
        hr  = bcd_to_bin(hr & 0x7F) | (hr & 0x80);
        day = bcd_to_bin(day);
        mon = bcd_to_bin(mon);
        yr  = bcd_to_bin(yr);
    }

    out->second = sec;
    out->minute = min;
    out->hour   = hr;
    out->day    = day;
    out->month  = mon;
    out->year   = 2000 + yr;
}

void rtc_format(const rtc_time_t *t, char *buf) {
    // AAAA-MM-JJ HH:MM:SS
    char *p = buf;
    int y = t->year;
    *p++ = '0' + (y / 1000) % 10;
    *p++ = '0' + (y / 100) % 10;
    *p++ = '0' + (y / 10) % 10;
    *p++ = '0' + y % 10;
    *p++ = '-';
    *p++ = '0' + t->month / 10; *p++ = '0' + t->month % 10;
    *p++ = '-';
    *p++ = '0' + t->day / 10;   *p++ = '0' + t->day % 10;
    *p++ = ' ';
    *p++ = '0' + t->hour / 10;  *p++ = '0' + t->hour % 10;
    *p++ = ':';
    *p++ = '0' + t->minute / 10;*p++ = '0' + t->minute % 10;
    *p++ = ':';
    *p++ = '0' + t->second / 10;*p++ = '0' + t->second % 10;
    *p = 0;
}
