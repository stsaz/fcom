/**
Copyright (c) 2013 Simon Zolin
*/

#include "time.h"
#include "string.h"
#include <FFOS/error.h>


/** Get year days passed before this month (1: March). */
#define mon_ydays(mon)  (367 * (mon) / 12 - 30)

/** Get month by year day (1: March). */
#define yday_mon(yday)  (((yday) + 31) * 10 / 306)

static const char week_days[][4] = {
	"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static const char month_names[][4] = {
	"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

void fftime_totm(struct tm *tm, const ffdtm *dt)
{
	tm->tm_year = ffmax(dt->year - 1900, 0);
	tm->tm_mon = dt->month - 1;
	tm->tm_mday = dt->day;
	tm->tm_hour = dt->hour;
	tm->tm_min = dt->min;
	tm->tm_sec = dt->sec;
}

void fftime_fromtm(ffdtm *dt, const struct tm *tm)
{
	dt->year = tm->tm_year + 1900;
	dt->month = tm->tm_mon + 1;
	dt->day = tm->tm_mday;
	dt->weekday = tm->tm_wday;
	dt->yday = tm->tm_yday + 1;
	dt->hour = tm->tm_hour;
	dt->min = tm->tm_min;
	dt->sec = tm->tm_sec;
	dt->nsec = 0;
}

static int _fftzone_off;
static uint _fftzone_fast;

void fftime_storelocal(const fftime_zone *tz)
{
	_fftzone_fast = 0;
	if (!tz->have_dst) {
		_fftzone_off = tz->off;
		_fftzone_fast = 1;
	}
}

static void fftime_splitlocal(struct tm *tm, time_t t)
{
#if defined FF_MSVC || defined FF_MINGW
	localtime_s(tm, &t);
#else
	localtime_r(&t, tm);
#endif
}

static time_t fftime_joinlocal(struct tm *tm)
{
	time_t t;
	tm->tm_isdst = -1;
	t = mktime(tm);
	if (t == (time_t)-1)
		t = 0;
	return t;
}

/*
Split timestamp algorithm:
. Get day of week (1/1/1 was Monday).
. Get year and the days passed since its Mar 1:
 . Get days passed since Mar 1, 1 AD
 . Get approximate year (days / ~365.25).
 . Get days passed during the year.
. Get month and its day:
 . Get month by year day
 . Get year days passed before this month
 . Get day of month
. Shift New Year from Mar 1 to Jan 1
 . If year day is within Mar..Dec, add 2 months
 . If year day is within Jan..Feb, also increment year
*/
void fftime_split2(ffdtm *dt, const fftime *t, uint flags)
{
	uint year, mon, days, daysec, yday, mday;
	uint64 sec;

	if (flags == FFTIME_TZLOCAL && !_fftzone_fast)
		return;

	sec = t->sec;
	if (flags == FFTIME_TZLOCAL)
		sec += _fftzone_off;

	days = sec / FFTIME_DAY_SECS;
	daysec = sec % FFTIME_DAY_SECS;
	dt->weekday = (1 + days) % 7;

	days += 306; //306: days from Mar before Jan
	year = 1 + days * 400 / fftime_absdays(400);
	yday = days - fftime_absdays(year);
	if ((int)yday < 0) {
		yday += 365 + fftime_leapyear(year);
		year--;
	}

	mon = yday_mon(yday);
	mday = yday - mon_ydays(mon);

	if (yday >= 306) {
		year++;
		mon -= 10;
		yday -= 306;
	} else {
		mon += 2;
		yday += 31 + 28 + fftime_leapyear(year);
	}

	dt->year = year;
	dt->month = mon;
	dt->yday = yday + 1;
	dt->day = mday + 1;

	dt->hour = daysec / (60*60);
	dt->min = (daysec % (60*60)) / 60;
	dt->sec = daysec % 60;
	dt->nsec = fftime_nsec(t);
}

static uint mon_norm(uint mon, uint *year)
{
	if (mon > 12) {
		mon--;
		*year += mon / 12;
		mon = (mon % 12) + 1;
	}
	return mon;
}

fftime* fftime_join2(fftime *t, const ffdtm *dt, uint flags)
{
	uint year, mon, days;

	if (flags == FFTIME_TZNODATE) {
		days = 0;
		goto set;
	}

	if (flags == FFTIME_TZLOCAL && !_fftzone_fast) {
		fftime_null(t);
		return t;
	}

	if (dt->year <= 0) {
		fftime_null(t);
		return t;
	}

	year = dt->year;
	mon = mon_norm(dt->month, &year) - 2; //Jan -> Mar
	if ((int)mon <= 0) {
		mon += 12;
		year--;
	}

	days = fftime_absdays(year) - fftime_absdays(1)
		+ mon_ydays(mon) + (31 + 28)
		+ dt->day - 1;

set:
	t->sec = (int64)days * FFTIME_DAY_SECS + dt->hour * 60*60 + dt->min * 60 + dt->sec;
	fftime_setnsec(t, dt->nsec);

	if (flags == FFTIME_TZLOCAL) {
		t->sec -= _fftzone_off;
	}

	return t;
}

void fftime_split(ffdtm *dt, const fftime *t, enum FF_TIMEZONE tz)
{
	if (tz == FFTIME_TZLOCAL && !_fftzone_fast) {
		struct tm tm;
		fftime_splitlocal(&tm, t->sec);
		fftime_fromtm(dt, &tm);
		dt->nsec = fftime_nsec(t);
		return;
	}

	fftime tt = *t;
	tt.sec += (int64)fftime_absdays(1970 - 1) * FFTIME_DAY_SECS;
	fftime_split2(dt, &tt, tz);
}

fftime* fftime_join(fftime *t, const ffdtm *dt, enum FF_TIMEZONE tz)
{
	if (tz == FFTIME_TZLOCAL && !_fftzone_fast) {
		struct tm tm;
		fftime_totm(&tm, dt);
		t->sec = fftime_joinlocal(&tm);
		fftime_setnsec(t, dt->nsec);
		return t;
	}

	fftime_join2(t, dt, tz);
	if (tz == FFTIME_TZNODATE)
		return t;
	t->sec -= (int64)fftime_absdays(1970 - 1) * FFTIME_DAY_SECS;
	if (t->sec < 0)
		fftime_null(t);
	return t;
}


size_t fftime_tostr(const ffdtm *dt, char *dst, size_t cap, uint flags)
{
	const char *dst_o = dst;
	const char *end = dst + cap;

	// add date
	switch (flags & 0x0f) {
	case FFTIME_DATE_YMD:
		dst += ffs_fmt(dst, end, "%04u-%02u-%02u"
			, dt->year, dt->month, dt->day);
		break;

	case FFTIME_DATE_MDY0:
		dst += ffs_fmt(dst, end, "%02u/%02u/%04u"
			, dt->month, dt->day, dt->year);
		break;

	case FFTIME_DATE_MDY:
		dst += ffs_fmt(dst, end, "%u/%u/%04u"
			, dt->month, dt->day, dt->year);
		break;

	case FFTIME_DATE_DMY:
		dst += ffs_fmt(dst, end, "%02u.%02u.%04u"
			, dt->day, dt->month, dt->year);
		break;

	case FFTIME_DATE_WDMY:
		dst += ffs_fmt(dst, end, "%s, %02u %s %04u"
			, week_days[dt->weekday], dt->day, month_names[dt->month - 1], dt->year);
		break;

	case 0:
		break; //no date

	default:
		goto fail;
	}

	if ((flags & 0x0f) && (flags & 0xf0))
		dst = ffs_copyc(dst, end, ' ');

	// add time
	switch (flags & 0xf0) {
	case FFTIME_HMS:
		dst += ffs_fmt(dst, end, "%02u:%02u:%02u"
			, dt->hour, dt->min, dt->sec);
		break;

	case FFTIME_HMS_MSEC:
		dst += ffs_fmt(dst, end, "%02u:%02u:%02u.%03u"
			, dt->hour, dt->min, dt->sec, fftime_msec(dt));
		break;

	case FFTIME_HMS_GMT:
		dst += ffs_fmt(dst, end, "%02u:%02u:%02u GMT"
			, dt->hour, dt->min, dt->sec);
		break;

	case 0:
		break; //no time

	default:
		goto fail;
	}

	if (dst == end) {
		fferr_set(EOVERFLOW);
		return 0;
	}

	return dst - dst_o;

fail:
	fferr_set(EINVAL);
	return 0;
}
