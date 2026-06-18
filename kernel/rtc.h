// =============================================================================
//  kernel/rtc.h -- Horloge temps réel (CMOS)
// =============================================================================
#ifndef MONOS_RTC_H
#define MONOS_RTC_H

#include <stdint.h>

typedef struct {
    uint8_t  second, minute, hour;
    uint8_t  day, month;
    uint16_t year;
} rtc_time_t;

void rtc_init(void);
void rtc_now(rtc_time_t *out);
// Formate "AAAA-MM-JJ HH:MM:SS" dans buf (>= 20 octets).
void rtc_format(const rtc_time_t *t, char *buf);

#endif
