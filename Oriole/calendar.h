// calendar.h — the next 7 days from a private iCal link (the cal_url setting: Google Calendar's "secret address in
// iCal format", an iCloud public calendar's webcal:// link, any .ics URL). Read line by line with the weather (the
// weather task, Wi-Fi is up then; every wx_every minutes and when the link changes): it can be megabytes, so it is
// never held whole. Each VEVENT: its start, end, all-day or not, its title; a repeating one (RRULE: DAILY, WEEKLY with
// BYDAY, MONTHLY, YEARLY; INTERVAL, COUNT, UNTIL) is expanded into the days it falls on. Times with a TZID are taken
// as local (the watch's zone), with Z as UTC. Not read: EXDATE (a skipped meeting still shows), BYMONTHDAY, BYSETPOS.
// The Calendar screen (apps.h) shows the list. Included from Oriole.ino before weather.h.

#define CAL_N 16
struct CalEvent {
  int64_t start, end;  // epoch seconds
  bool allday;
  char title[64];
};
struct CalStore {
  int n;
  CalEvent e[CAL_N];  // sorted by start
};
static CalStore &cal = psram_new<CalStore>();      // what the screen shows (under cal_mux)
static CalStore &cal_new = psram_new<CalStore>();  // being read
static portMUX_TYPE cal_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t cal_ver;
static volatile int cal_status;  // 0 — never read, 1 — read, −1 — could not

// One event being read
static struct {
  bool in, cancelled, allday;
  int64_t start, end;
  char title[64], rrule[160];
} cv;
static int64_t cal_from, cal_to;  // the window: now (minus an hour, for what is on) to 7 days ahead

// "20261127", "20261127T090000" (local) or "20261127T090000Z" (UTC) -> epoch; all-day for a date alone
static int64_t cal_time(const char *v, bool *allday) {
  struct tm t = {};
  if (sscanf(v, "%4d%2d%2d", &t.tm_year, &t.tm_mon, &t.tm_mday) != 3) return 0;
  t.tm_year -= 1900, t.tm_mon -= 1, t.tm_isdst = -1;
  const char *T = strchr(v, 'T');
  if (allday) *allday = !T;
  if (T) sscanf(T + 1, "%2d%2d%2d", &t.tm_hour, &t.tm_min, &t.tm_sec);
  if (T && strchr(T, 'Z')) {  // UTC: days since 1970 by hand (mktime is local)
    int y = t.tm_year + 1900, m = t.tm_mon + 1;
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + t.tm_mday - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return ((int64_t)era * 146097 + doe - 719468) * 86400 + t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
  }
  return mktime(&t);
}

static void cal_add(int64_t s, int64_t dur) {  // one occurrence, if it is in the window: into the sorted list
  if (s + max<int64_t>(dur, 1) <= cal_from || s >= cal_to) return;
  CalStore &c = cal_new;
  int i = c.n < CAL_N ? c.n : CAL_N - 1;
  if (c.n == CAL_N && s >= c.e[CAL_N - 1].start) return;  // full, and later than all: not kept
  while (i > 0 && c.e[i - 1].start > s) c.e[i] = c.e[i - 1], i--;
  c.e[i] = {s, s + dur, cv.allday, {}};
  strlcpy(c.e[i].title, cv.title[0] ? cv.title : "(no title)", sizeof(c.e[i].title));
  if (c.n < CAL_N) c.n++;
}

static const char *cal_rule(const char *key, char *out, size_t n) {  // a part of the RRULE ("FREQ") or ""
  out[0] = 0;
  const char *p = strstr(cv.rrule, key);
  if (!p || p[strlen(key)] != '=') return out;
  p += strlen(key) + 1;
  size_t k = 0;
  while (*p && *p != ';' && k < n - 1) out[k++] = *p++;
  out[k] = 0;
  return out;
}

static void cal_expand() {  // the event done: its occurrences in the window
  const int64_t dur = cv.end > cv.start ? cv.end - cv.start : cv.allday ? 86400 : 3600;
  if (!cv.rrule[0]) return cal_add(cv.start, dur);
  char f[16], v[48];
  cal_rule("FREQ", f, sizeof(f));
  const int every = max(1, atoi(cal_rule("INTERVAL", v, sizeof(v)))), count = atoi(cal_rule("COUNT", v, sizeof(v)));
  cal_rule("UNTIL", v, sizeof(v));
  const int64_t until = v[0] ? cal_time(v, nullptr) : 0;
  cal_rule("BYDAY", v, sizeof(v));
  int days = 0;  // BYDAY as a mask, Sunday = bit 0
  static const char *const DN[7] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
  for (int i = 0; i < 7; i++)
    if (strstr(v, DN[i])) days |= 1 << i;
  const time_t s0 = (time_t)cv.start;
  struct tm t0;
  localtime_r(&s0, &t0);
  const bool weekly = !strcmp(f, "WEEKLY"), daily = !strcmp(f, "DAILY");
  if (weekly && !days) days = 1 << t0.tm_wday;
  int n = 0;
  int64_t skip = 0;  // long-running daily/weekly ones: jump close to the window (no COUNT to keep)
  if (!count && (daily || weekly) && cal_from - cv.start > 14 * 86400) {
    const int64_t step = (daily ? 1 : 7) * (int64_t)every * 86400;
    skip = (cal_from - cv.start) / step * step / 86400 - (daily ? every : 7 * every);
  }
  for (int64_t k = skip; n < 4000; k++) {  // k: days (daily, weekly), months or years from the start
    struct tm t = t0;
    if (daily || weekly) t.tm_mday += (int)k;
    else if (!strcmp(f, "MONTHLY")) t.tm_mon += (int)k * every;
    else if (!strcmp(f, "YEARLY")) t.tm_year += (int)k * every;
    else return cal_add(cv.start, dur);  // a rule not read here: just the first one
    t.tm_isdst = -1;
    const int64_t s = mktime(&t);
    if (s >= cal_to || (until && s > until) || (count && n >= count)) return;
    if (!(daily || weekly) && t.tm_mday != t0.tm_mday) continue;  // the 31st of a short month: none
    if (daily && k % every) continue;
    if (weekly && (!(days >> t.tm_wday & 1) || (k / 7) % every)) continue;
    n++;
    cal_add(s, dur);
  }
}

static void cal_line(char *l) {  // one unfolded line of the file
  if (!strcmp(l, "BEGIN:VEVENT")) return (void)(memset(&cv, 0, sizeof(cv)), cv.in = true);
  if (!cv.in) return;
  if (!strcmp(l, "END:VEVENT")) {
    cv.in = false;
    if (!cv.cancelled && cv.start) cal_expand();
    return;
  }
  char *colon = strchr(l, ':');
  if (!colon) return;
  *colon = 0;
  const char *val = colon + 1;
  if (!strncmp(l, "DTSTART", 7)) cv.start = cal_time(val, &cv.allday);
  else if (!strncmp(l, "DTEND", 5)) cv.end = cal_time(val, nullptr);
  else if (!strcmp(l, "RRULE")) strlcpy(cv.rrule, val, sizeof(cv.rrule));
  else if (!strcmp(l, "STATUS")) cv.cancelled = !strcmp(val, "CANCELLED");
  else if (!strncmp(l, "SUMMARY", 7)) {  // "\," "\;" "\n" unescaped
    size_t k = 0;
    for (const char *p = val; *p && k < sizeof(cv.title) - 1; p++) {
      if (*p == '\\' && p[1]) p++, cv.title[k++] = *p == 'n' ? ' ' : *p;
      else cv.title[k++] = *p;
    }
    cv.title[k] = 0;
  }
}

// From the weather task, with Wi-Fi up: the link read again (nothing to do without one or without the clock)
static void cal_update() {
  char url[300];
  {
    CfgLock l;
    strlcpy(url, cfg.cal_url, sizeof(url));
  }
  if (!url[0] || !oriole_clock_valid()) return;
  if (!strncmp(url, "webcal://", 9)) memmove(url + 5, url + 6, strlen(url + 6) + 1), memcpy(url, "https", 5);
  const int64_t now = time(nullptr);
  cal_from = now - 3600, cal_to = now + 7 * 86400;
  cal_new.n = 0, cv.in = false;
  if (!net_acquire(25000)) return (void)(cal_status = -1);
  const bool ok = net_lines(url, cal_line);
  net_release_soon();
  if (ok) {
    portENTER_CRITICAL(&cal_mux);
    cal = cal_new, cal_ver++;
    portEXIT_CRITICAL(&cal_mux);
  }
  cal_status = ok ? 1 : -1;
  LOG("calendar: %s (%d events in 7 days)\n", ok ? "updated" : "could not read the link", cal_new.n);
}
