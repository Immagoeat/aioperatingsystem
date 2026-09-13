/* rtc.c - reads the wall-clock time from the CMOS Real-Time Clock chip,
 * present on every x86 PC since the original IBM AT (it keeps time even
 * while the machine is powered off, backed by a small battery). Accessed
 * via I/O ports 0x70 (register index) / 0x71 (register data) - no IRQ
 * needed here, we just poll it whenever the current time is wanted. */
#include "kernel.h"

#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71

#define CMOS_REG_SECONDS 0x00
#define CMOS_REG_MINUTES 0x02
#define CMOS_REG_HOURS   0x04
#define CMOS_REG_STATUS_A 0x0A
#define CMOS_REG_STATUS_B 0x0B

static uint8_t cmos_read(uint8_t reg) {
	outb(CMOS_INDEX, reg);
	io_wait();
	return inb(CMOS_DATA);
}

static bool rtc_update_in_progress(void) {
	return (cmos_read(CMOS_REG_STATUS_A) & 0x80) != 0;
}

static uint8_t bcd_to_bin(uint8_t val) {
	return (uint8_t)((val & 0x0F) + ((val >> 4) * 10));
}

/* There's no way for a freestanding kernel on real x86 hardware to
 * actually detect the user's timezone (no network stack, no GPS, and
 * the CMOS RTC itself has no timezone field - it's just a wall-clock).
 * So "auto-detect" here is deliberately honest about what it can do:
 * the RTC is assumed to already show local time (the same assumption
 * every BIOS/CMOS clock setup screen makes), and this offset starts at
 * 0 - i.e. "trust the RTC as-is" - until the user picks a real UTC
 * offset in Settings, at which point it's applied on top. */
static int timezone_offset_minutes = 0;

void rtc_set_timezone_offset_minutes(int minutes) {
	/* clamp to the real range of UTC offsets in use (UTC-12 to UTC+14) */
	if (minutes < -12 * 60) minutes = -12 * 60;
	if (minutes > 14 * 60) minutes = 14 * 60;
	timezone_offset_minutes = minutes;
}

int rtc_get_timezone_offset_minutes(void) {
	return timezone_offset_minutes;
}

void rtc_get_time(struct rtc_time *out) {
	uint8_t seconds, minutes, hours;
	uint8_t last_seconds, last_minutes, last_hours;

	/* Read twice and compare: the RTC can be mid-update between our
	 * reads (~1 in 244 chance per read at typical tick rates), which
	 * would otherwise occasionally show a garbled/rolled-over value. */
	do {
		while (rtc_update_in_progress()) { }
		seconds = cmos_read(CMOS_REG_SECONDS);
		minutes = cmos_read(CMOS_REG_MINUTES);
		hours = cmos_read(CMOS_REG_HOURS);

		while (rtc_update_in_progress()) { }
		last_seconds = cmos_read(CMOS_REG_SECONDS);
		last_minutes = cmos_read(CMOS_REG_MINUTES);
		last_hours = cmos_read(CMOS_REG_HOURS);
	} while (seconds != last_seconds || minutes != last_minutes || hours != last_hours);

	uint8_t status_b = cmos_read(CMOS_REG_STATUS_B);
	bool is_binary = (status_b & 0x04) != 0;
	bool is_24h = (status_b & 0x02) != 0;

	if (!is_binary) {
		seconds = bcd_to_bin(seconds);
		minutes = bcd_to_bin(minutes);
		/* the top bit of the hours register marks PM in 12-hour BCD mode,
		 * and isn't part of the BCD value itself */
		hours = bcd_to_bin(hours & 0x7F) | (hours & 0x80);
	}

	bool pm = false;
	if (!is_24h) {
		pm = (hours & 0x80) != 0;
		hours &= 0x7F;
		if (pm && hours != 12) hours += 12;
		if (!pm && hours == 12) hours = 0;
	}

	/* Apply the configured timezone offset, wrapping the day around
	 * cleanly (an offset can push the displayed time to the previous or
	 * next day's hours without that meaning anything else changed). */
	int total_minutes = hours * 60 + minutes + timezone_offset_minutes;
	total_minutes %= 24 * 60;
	if (total_minutes < 0) total_minutes += 24 * 60;

	out->hours = (uint8_t)(total_minutes / 60);
	out->minutes = (uint8_t)(total_minutes % 60);
	out->seconds = seconds;
}
