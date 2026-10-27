#ifndef FZ_TIMEGM_COMPAT_H
#define FZ_TIMEGM_COMPAT_H

/* ESP-IDF newlib: include sys/types.h first and drop its `quad` macro before
 * any MuPDF header, or fz_stext_char's `quad` member is rewritten to quad_t. */
#include <sys/types.h>
#ifdef quad
#undef quad
#endif
#include <time.h>

/* newlib has no timegm().  Howard Hinnant's civil-date algorithm. */
static inline time_t crosspoint_timegm(struct tm *tm) {
  int y = tm->tm_year + 1900;
  unsigned m = (unsigned)tm->tm_mon + 1;
  unsigned d = (unsigned)tm->tm_mday;
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long long days = (long long)era * 146097 + (long long)doe - 719468;
  return (time_t)(days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec);
}

#define timegm crosspoint_timegm

#endif
