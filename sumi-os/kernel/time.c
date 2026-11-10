/* Sumi OS - PIT system tick and CMOS real-time clock */
#include "kernel.h"

const char *const month_names[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
const char *const day_names[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

static u32 base_secs, base_ticks;

static void timer_irq(void) { ticks++; }

static u8 cmos(u8 reg) { outb(0x70, reg | 0x80); io_wait(); return inb(0x71); }
static int bcd(u8 v) { return (v & 15) + (v >> 4) * 10; }

static u32 days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int era = y / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (u32)(era * 146097 + doe - 719468);
}

void rtc_read(DateTime *dt) {
    u8 s, mi, h, d, mo, y, s2;
    for (int tries = 0; tries < 1000; tries++) {
        while (cmos(0x0A) & 0x80) {}
        s = cmos(0); mi = cmos(2); h = cmos(4); d = cmos(7); mo = cmos(8); y = cmos(9);
        if (cmos(0x0A) & 0x80) continue;
        s2 = cmos(0);
        if (s == s2) break;
    }
    u8 regb = cmos(0x0B);
    bool pm = h & 0x80;
    h &= 0x7F;
    if (!(regb & 4)) { s = bcd(s); mi = bcd(mi); h = bcd(h); d = bcd(d); mo = bcd(mo); y = bcd(y); }
    if (!(regb & 2)) { if (h == 12) h = 0; if (pm) h += 12; }
    dt->sec = s; dt->min = mi; dt->hour = h; dt->day = d; dt->mon = mo; dt->year = 2000 + y;
    dt->wday = (int)((days_from_civil(dt->year, dt->mon, dt->day) + 4) % 7);
}

void time_resync(void) {
    DateTime dt;
    rtc_read(&dt);
    u32 secs = days_from_civil(dt.year, dt.mon, dt.day) * 86400u + dt.hour * 3600 + dt.min * 60 + dt.sec;
    u32 f = irq_save();
    base_secs = secs; base_ticks = ticks;
    irq_restore(f);
}

void time_init(void) {
    u32 div = 1193182 / HZ;
    outb(0x43, 0x36); outb(0x40, div & 0xFF); outb(0x40, div >> 8);
    irq_install(0, timer_irq);
    time_resync();
}

u32 time_now(void) { return base_secs + (ticks - base_ticks) / HZ; }
u32 uptime_ms(void) { return ticks * (1000 / HZ); }

void time_to_dt(u32 t, DateTime *dt) {
    u32 days = t / 86400, rem = t % 86400;
    dt->hour = rem / 3600; dt->min = rem % 3600 / 60; dt->sec = rem % 60;
    dt->wday = (days + 4) % 7;
    int z = (int)days + 719468;
    int era = z / 146097;
    int doe = z - era * 146097;
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = yoe + era * 400;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    dt->day = doy - (153 * mp + 2) / 5 + 1;
    dt->mon = mp < 10 ? mp + 3 : mp - 9;
    dt->year = y + (dt->mon <= 2);
}
