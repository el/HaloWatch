// apps.h — the Oriole menu's apps besides Assistant, Weather and Settings:
//   History     — the last questions and answers (history.h keeps them); a tap shows the whole answer, scrolling
//   World clock — the time in the weather cities (their UTC offsets come with the weather) and how far from here
//   Timer       — 1 to 60 minutes on a slider; a ring empties as it runs (on the face too), it beeps when done and
//                 wakes the board for it; a touch or BOOT stops the beeping
//   Stopwatch   — minutes, seconds and hundredths; it goes on while you are elsewhere
//   Steps       — today's steps from the motion sensor's pedometer (it counts while the board sleeps), kept in flash,
//                 back to zero at midnight: shown on the Arcs face (the app itself is gone)
// Included from Oriole.ino after settings.h.

// ---------- Text that fits: one line with "…", or wrapped into lines ----------
static void fit_text(char *dst, size_t n, const HaloFont &f, const char *src, float w) {
  strlcpy(dst, src, n);
  for (char *p = dst; *p; p++)
    if (*p == '\n') *p = ' ';
  if (halo_text_width(f, dst) <= w) return;
  for (size_t k = strlen(dst); k > 0; k--) {
    dst[k] = 0;
    if (k + 4 < n) strcpy(dst + k, "…");
    if (halo_text_width(f, dst) <= w) return;
  }
}

static int wrap_text(const HaloFont &f, const char *s, float w, char (*out)[96], int max) {  // the number of lines
  int n = 0;
  out[0][0] = 0;
  char word[96], test[192];
  while (*s && n < max) {
    while (*s == ' ' || *s == '\n') s++;
    int k = 0;
    while (*s && *s != ' ' && *s != '\n' && k < 95) word[k++] = *s++;
    word[k] = 0;
    if (!k) break;
    snprintf(test, sizeof(test), "%s%s%s", out[n], out[n][0] ? " " : "", word);
    if (out[n][0] && halo_text_width(f, test) > w) {
      if (++n >= max) break;
      strlcpy(out[n], word, 96);
    } else {
      strlcpy(out[n], test, 96);
    }
  }
  return out[0][0] ? min(n + 1, max) : 0;
}

static long local_offset_s(time_t now) {  // here's UTC offset now (newlib's struct tm has no tm_gmtoff)
  struct tm l, g;
  localtime_r(&now, &l), gmtime_r(&now, &g);
  int days = l.tm_yday - g.tm_yday;
  if (l.tm_year != g.tm_year) days = l.tm_year > g.tm_year ? 1 : -1;  // across new year
  return days * 86400L + (l.tm_hour - g.tm_hour) * 3600L + (l.tm_min - g.tm_min) * 60L;
}

static void clock_str(char *s, size_t n, const struct tm &t) {  // "7:45 PM" or "19:45"
  if (cfg.clock24) snprintf(s, n, "%d:%02d", t.tm_hour, t.tm_min);
  else snprintf(s, n, "%d:%02d %s", t.tm_hour % 12 ? t.tm_hour % 12 : 12, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
}

// ---------- A pill button: tapped -> true ----------
struct Button {
  float x, y, w, h;
  bool primary;  // filled with the accent, or a gray card
  Press p;
  bool armed;
  HaloItem bg, label;
};
static float btn_mid;

static bool btn_update(Button &b, float dt, bool on) {
  const HaloFinger &f = halo_finger;
  if (f.pressed) b.armed = on && !f.owner && fabsf(f.x0 - b.x) < b.w / 2 && fabsf(f.y0 - b.y) < b.h / 2;
  if (f.dragging) b.armed = false;
  const bool tap = f.tap && b.armed;
  press_update(b.p, b.armed && f.down, dt);
  if (!f.down) b.armed = false;
  return tap;
}

static void btn_layout(Button &b, const char *text, const HaloView &v, float a) {
  const float p = halo_clamp01(b.p.s.x);
  b.bg.kind = HI_RRECT, b.bg.x = b.x, b.bg.y = b.y, b.bg.w = b.w, b.bg.h = b.h, b.bg.r = b.h / 2 * (1 - 0.3f * p);
  b.bg.scale = 1, b.bg.color = b.primary ? DP_PRIMARY : CARD_HI, b.bg.alpha = a, b.bg.on_black = true;
  halo_view_commit(b.bg, v);
  text_mid(b.label, t24, text, btn_mid, b.x, b.y, 1, b.primary ? DP_ON_PRIMARY : TEXT, a, &b.bg);
  halo_view_commit(b.label, v);
}

static void btn_draw(const Button &b) {
  halo_item_draw(b.bg), halo_item_draw(b.label);
}

// Layout shared by Timer and Stopwatch: a header, big digits, a line under them, one or two buttons at the bottom
#define AP_HEAD_Y 66.0f
#define AP_BIG_Y 176.0f
#define AP_BIG_S 0.62f
#define AP_LINE_Y 282.0f
#define AP_BTN_Y 382.0f
static float ap_mid_head, ap_mid_big, ap_mid_t20, ap_mid_t24;

static void two_buttons(Button &a, Button &b, bool two) {  // places them: two side by side, or the first alone
  a.y = b.y = AP_BTN_Y, a.h = b.h = 72;
  if (two) a.x = CX - 82, b.x = CX + 82, a.w = b.w = 150;
  else a.x = CX, a.w = 210;
}

// ---------- Timer ----------
static const int TM_MIN[] = {1, 2, 3, 5, 10, 15, 20, 25, 30, 40, 45, 50, 60};  // the slider's steps
#define TM_STEPS (int)(sizeof(TM_MIN) / sizeof(TM_MIN[0]) - 1)
enum : uint8_t { TM_IDLE, TM_RUN, TM_PAUSE, TM_DONE };
static volatile uint8_t tm_state = TM_IDLE;
static int64_t tm_end_us, tm_left_us, tm_total_us;
static Slider &tm_sl = psram_new<Slider>();
static Button tm_b1 = {0, 0, 0, 0, true}, tm_b2;
static HaloItem tm_head, tm_big, tm_line, tm_ring;
static char tm_big_s[12], tm_line_s[40];
static int tm_pick = 3;  // 5 minutes

static int64_t tm_left() {  // µs
  return tm_state == TM_RUN ? max<int64_t>(0, tm_end_us - esp_timer_get_time()) : tm_left_us;
}
static int32_t timer_ms_left() {
  return tm_state == TM_RUN ? (int32_t)(tm_left() / 1000) : -1;
}
static float timer_face_fraction() {
  return (tm_state == TM_RUN || tm_state == TM_PAUSE) && tm_total_us ? (float)tm_left() / tm_total_us : -1;
}
static bool tm_chiming() {
  return (int32_t)(millis() - ai_chime_until) < 0;
}

// Every frame, before power_tick (the board may have woken just for this): rings, or stops ringing
static void timer_tick() {
  if (tm_state == TM_RUN && tm_left() == 0) {
    tm_state = TM_DONE, tm_left_us = 0;
    ai_chime_mode = 0, ai_chime_until = millis() + 30000;  // gemini.h: beeps for 30 s
    pw_cmd_wake = true;                 // power.h: the screen on
    LOG("timer: done\n");
  }
  if (tm_chiming() && (halo_finger.down || digitalRead(0) == LOW)) ai_chime_until = millis();
}

static void timer_enter() {
  slider_init(tm_sl, AP_LINE_Y, TM_STEPS, tm_pick), slider_enter(tm_sl);
  tm_b1.armed = tm_b2.armed = false;
}

static void timer_update(float dt) {
  const uint8_t st = tm_state;
  two_buttons(tm_b1, tm_b2, st == TM_RUN || st == TM_PAUSE);
  if (st == TM_IDLE) slider_update(tm_sl, dt), slider_tick(tm_sl, dt), tm_pick = tm_sl.value;
  const bool t1 = btn_update(tm_b1, dt, true), t2 = btn_update(tm_b2, dt, st == TM_RUN || st == TM_PAUSE);
  if (t1 && st == TM_IDLE) {
    tm_total_us = tm_left_us = TM_MIN[tm_pick] * 60000000ll;
    tm_end_us = esp_timer_get_time() + tm_total_us, tm_state = TM_RUN;
    LOG("timer: %d min\n", TM_MIN[tm_pick]);
  } else if (t1 && st == TM_RUN) {
    tm_left_us = tm_left(), tm_state = TM_PAUSE;
  } else if (t1 && st == TM_PAUSE) {
    tm_end_us = esp_timer_get_time() + tm_left_us, tm_state = TM_RUN;
  } else if (t1 && st == TM_DONE) {
    ai_chime_until = millis(), tm_state = TM_IDLE;
  }
  if (t2) tm_state = TM_IDLE, tm_left_us = 0;
}

static void timer_layout(const HaloView &v) {
  const uint8_t st = tm_state;
  const int64_t left = st == TM_IDLE ? TM_MIN[tm_pick] * 60000000ll : tm_left();
  const int sec = (int)((left + 999999) / 1000000);  // 4:59.2 shows 5:00: the second ends at zero
  snprintf(tm_big_s, sizeof(tm_big_s), "%02d:%02d", sec / 60, sec % 60);
  if (st == TM_RUN) {
    const time_t end = time(nullptr) + sec;
    struct tm t;
    localtime_r(&end, &t);
    char c[16];
    clock_str(c, sizeof(c), t);
    snprintf(tm_line_s, sizeof(tm_line_s), halo_clock_valid() ? "Rings at %s" : "Running", c);
  } else {
    strlcpy(tm_line_s, st == TM_PAUSE ? "Paused" : st == TM_DONE ? "Time's up" : "", sizeof(tm_line_s));
  }
  text_mid(tm_head, t_title, "Timer", ap_mid_head, CX, AP_HEAD_Y, 1, TEXT, 1, &halo_black);
  const bool blink = st == TM_DONE && (millis() / 500) % 2;
  text_mid(tm_big, f_time, tm_big_s, ap_mid_big, CX, AP_BIG_Y, AP_BIG_S, st == TM_DONE ? 0xFFC56B : TEXT,
           blink ? 0.35f : 1, &halo_black);
  text_mid(tm_line, st == TM_DONE ? t24 : t20, tm_line_s, st == TM_DONE ? ap_mid_t24 : ap_mid_t20, CX, AP_LINE_Y, 1,
           st == TM_DONE ? 0xFFC56B : DIM, tm_line_s[0] ? 1 : 0, &halo_black);
  tm_head.on_black = tm_big.on_black = tm_line.on_black = true;
  halo_view_commit(tm_head, v), halo_view_commit(tm_big, v), halo_view_commit(tm_line, v);
  const float tf = timer_face_fraction();
  tm_ring.kind = HI_RING, tm_ring.x = tm_ring.y = CX, tm_ring.r = 222, tm_ring.w = 8, tm_ring.scale = 1;
  tm_ring.v = floorf(fmaxf(tf, 0) * 360) / 360, tm_ring.color = 0xFFC56B, tm_ring.color2 = FACE_TRACK;
  tm_ring.alpha = tf >= 0 ? 1 : 0, tm_ring.on_black = true;
  halo_view_commit(tm_ring, v);
  if (st == TM_IDLE) slider_layout(tm_sl, v);
  else for (auto *it : {&tm_sl.pill, &tm_sl.track, &tm_sl.fill, &tm_sl.circle[0], &tm_sl.circle[1], &tm_sl.icon[0],
                        &tm_sl.icon[1]}) it->alpha = 0, halo_view_commit(*it, v);
  if (st != TM_IDLE) for (int i = 0; i < SL_MAX_DOTS; i++) tm_sl.dot[i].alpha = 0, halo_view_commit(tm_sl.dot[i], v);
  btn_layout(tm_b1, st == TM_IDLE ? "Start" : st == TM_RUN ? "Pause" : st == TM_PAUSE ? "Resume" : "Stop", v, 1);
  btn_layout(tm_b2, "Reset", v, st == TM_RUN || st == TM_PAUSE ? 1 : 0);
}

static void timer_draw() {
  halo_item_draw(tm_ring), halo_item_draw(tm_head), halo_item_draw(tm_big), halo_item_draw(tm_line);
  slider_draw(tm_sl);
  btn_draw(tm_b1), btn_draw(tm_b2);
}
static const HaloScreen TIMER_SCREEN = {timer_enter, timer_update, timer_layout, timer_draw, false, HALO_HOME_BOTH};

// ---------- Stopwatch ----------
static int64_t sw_start_us, sw_acc_us;
static bool sw_run;
static Button sw_b1 = {0, 0, 0, 0, true}, sw_b2;
static HaloItem sw_head, sw_big, sw_small;
static char sw_big_s[12], sw_small_s[8];

static int64_t sw_elapsed() {
  return sw_acc_us + (sw_run ? esp_timer_get_time() - sw_start_us : 0);
}

static void stopwatch_update(float dt) {
  const bool paused = !sw_run && sw_acc_us > 0;
  two_buttons(sw_b1, sw_b2, paused);
  const bool t1 = btn_update(sw_b1, dt, true), t2 = btn_update(sw_b2, dt, paused);
  if (t1 && sw_run) sw_acc_us = sw_elapsed(), sw_run = false;
  else if (t1) sw_start_us = esp_timer_get_time(), sw_run = true;
  if (t2) sw_acc_us = 0;
}

static void stopwatch_layout(const HaloView &v) {
  const int64_t e = sw_elapsed();
  const int cs = (int)(e / 10000 % 100), s = (int)(e / 1000000);
  if (s >= 3600) snprintf(sw_big_s, sizeof(sw_big_s), "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
  else snprintf(sw_big_s, sizeof(sw_big_s), "%02d:%02d", s / 60, s % 60);
  snprintf(sw_small_s, sizeof(sw_small_s), ".%02d", cs);
  text_mid(sw_head, t_title, "Stopwatch", ap_mid_head, CX, AP_HEAD_Y, 1, TEXT, 1, &halo_black);
  text_mid(sw_big, f_time, sw_big_s, ap_mid_big, CX, AP_BIG_Y, s >= 3600 ? 0.44f : AP_BIG_S, TEXT, 1, &halo_black);
  text_mid(sw_small, t_title, sw_small_s, ap_mid_head, CX, AP_LINE_Y - 20, 1, sw_run ? DP_PRIMARY : DIM, 1, &halo_black);
  sw_head.on_black = sw_big.on_black = sw_small.on_black = true;
  halo_view_commit(sw_head, v), halo_view_commit(sw_big, v), halo_view_commit(sw_small, v);
  const bool paused = !sw_run && sw_acc_us > 0;
  btn_layout(sw_b1, sw_run ? "Pause" : paused ? "Resume" : "Start", v, 1);
  btn_layout(sw_b2, "Reset", v, paused ? 1 : 0);
}

static void stopwatch_draw() {
  halo_item_draw(sw_head), halo_item_draw(sw_big), halo_item_draw(sw_small);
  btn_draw(sw_b1), btn_draw(sw_b2);
}
static const HaloScreen STOPWATCH_SCREEN = {[] { sw_b1.armed = sw_b2.armed = false; }, stopwatch_update,
                                            stopwatch_layout, stopwatch_draw, false, HALO_HOME_BOTH};

// ---------- World clock: a card per weather city ----------
static char wc_sub[WX_PAGES][48];
static const char *wc_title(int i) {
  return wx_city_name[i];
}
static const char *wc_subtitle(int i) {
  const WxDay &d = wx_days[i];
  if (!halo_clock_valid() || !d.off_ok) return strcpy(wc_sub[i], "Waiting for the weather"), wc_sub[i];
  const time_t now = time(nullptr), there = now + d.utc_off;
  struct tm t;
  gmtime_r(&there, &t);
  char c[16], diff[16];
  clock_str(c, sizeof(c), t);
  const long dm = (d.utc_off - local_offset_s(now)) / 60;  // minutes ahead of here
  if (!dm) strcpy(diff, "same as here");
  else if (dm % 60) snprintf(diff, sizeof(diff), "%+ld:%02ld h", dm / 60, labs(dm) % 60);
  else snprintf(diff, sizeof(diff), "%+ld h", dm / 60);
  snprintf(wc_sub[i], sizeof(wc_sub[i]), "%s %s, %s", c, WEEKDAY[t.tm_wday], diff);
  return wc_sub[i];
}
static CardRow wc_rows[WX_PAGES];
static CardList &wc_list = cl_new("World clock", wc_rows, WX_PAGES);
static const HaloScreen WORLD_SCREEN = {[] { wc_list.n = max(1, wx_n), cl_enter(wc_list); },
                                        [](float dt) { cl_update(wc_list, dt); },
                                        [](const HaloView &v) { cl_layout(wc_list, v); }, [] { cl_draw(wc_list); },
                                        false, HALO_HOME_BOTH};

// ---------- History: a card per answer (newest first); a tap — the whole answer ----------
struct HiText {
  char t[HIST_N][64], s[HIST_N][64];   // the cards' titles and subtitles, fitted
  char q[HIST_N][64], a[HIST_N][96];   // their raw starts, copied under the lock
  HistEntry open;                      // the one shown whole
  char l[HALO_LIST_MAX][96];           // its lines
  HaloItem it[HALO_LIST_MAX];          // and their items
};
static HiText &hi = psram_new<HiText>();
static int hi_n;
static uint32_t hi_seen = ~0u;
static CardRow hi_rows[HIST_N];
static CardList &hi_list = cl_new("History", hi_rows, HIST_N);

static void hi_snapshot() {  // a copy of the titles and first lines (the ring may change while the screen is up)
  hi_seen = hist_ver;
  portENTER_CRITICAL(&hist_mux);
  hi_n = hist.count;
  for (int k = 0; k < hi_n; k++) strlcpy(hi.q[k], hist_at(k).q, 64), strlcpy(hi.a[k], hist_at(k).a, 96);
  portEXIT_CRITICAL(&hist_mux);
  for (int k = 0; k < hi_n; k++) fit_text(hi.t[k], 64, t24, hi.q[k], 290), fit_text(hi.s[k], 64, t20, hi.a[k], 290);
  if (!hi_n) strcpy(hi.t[0], "No questions yet"), strcpy(hi.s[0], "Hold BOOT and ask something");
  hi_list.n = max(1, hi_n);
}

// The whole answer: its time, the question (dim), the answer — lines in a scrolling list
#define HD_MAX (HALO_LIST_MAX - 1)
#define hd_l hi.l
static uint8_t hd_kind[HD_MAX];  // 0 — the question, 1 — the answer, 2 — a gap
static int hd_n;
static char hd_when[32];
static HaloList hd_list;
static HaloItem hd_head;
#define hd_it hi.it

static void hd_fill(const char *when, const char *q, const char *a);

static void hd_open(int k) {
  if (k >= hi_n) return;
  HistEntry &e = hi.open;
  portENTER_CRITICAL(&hist_mux);
  e = hist_at(k);
  portEXIT_CRITICAL(&hist_mux);
  char when[32] = "";
  if (e.epoch) {
    const time_t t = e.epoch;
    struct tm tm;
    localtime_r(&t, &tm);
    char c[16];
    clock_str(c, sizeof(c), tm);
    snprintf(when, sizeof(when), "%s, %s", WEEKDAY[tm.tm_wday], c);
  }
  hd_fill(when, e.q, e.a);
}

// The detail screen's lines: a header, a dim part (the question, a notification's title), the body
static void hd_fill(const char *when, const char *q, const char *a) {
  strlcpy(hd_when, when, sizeof(hd_when));
  const int nq = wrap_text(t20, q, 300, hd_l, 4);
  for (int i = 0; i < nq; i++) hd_kind[i] = 0;
  hd_n = nq;
  if (hd_n < HD_MAX) hd_kind[hd_n] = 2, hd_l[hd_n][0] = 0, hd_n++;
  const int na = wrap_text(t24, a, 300, hd_l + hd_n, HD_MAX - hd_n);
  for (int i = 0; i < na; i++) hd_kind[hd_n + i] = 1;
  hd_n += na;
  float h[HALO_LIST_MAX];
  h[0] = M_HEAD_H;
  for (int i = 0; i < hd_n; i++) h[i + 1] = hd_kind[i] == 0 ? 30 : hd_kind[i] == 1 ? 36 : 16;
  halo_list_init(hd_list, h, hd_n + 1, 64, 390, true);
}

static void hd_layout(const HaloView &v) {
  if (hd_list.moving && v.a > 0 && v.s >= 0.999f) halo_subpixel = true;
  const HaloListFx &h = hd_list.fx[0];
  text_mid(hd_head, t24, hd_when[0] ? hd_when : "History", ap_mid_t24, CX, h.top + M_HEAD_H / 2, 1, DIM, h.a_bg,
           &halo_black);
  hd_head.on_black = true;
  halo_view_commit(hd_head, v);
  for (int i = 0; i < HD_MAX; i++) {
    HaloItem &it = hd_it[i];
    if (i >= hd_n || hd_kind[i] == 2) {
      it.alpha = 0, halo_view_commit(it, v);
      continue;
    }
    const HaloListFx &f = hd_list.fx[i + 1];
    const bool q = hd_kind[i] == 0;
    const float a = halo_list_item_visible(hd_list, i + 1) ? f.a_c : 0;
    text_mid(it, q ? t20 : t24, hd_l[i], q ? ap_mid_t20 : ap_mid_t24, CX, f.top + (q ? 15 : 18) * f.s, f.s,
             q ? 0x8E9099 : TEXT, a, &halo_black);
    it.on_black = true;
    halo_view_commit(it, v);
  }
}

static void hd_draw() {
  halo_item_draw(hd_head);
  for (int i = 0; i < hd_n; i++) halo_item_draw(hd_it[i]);
}
static const HaloScreen HIST_DETAIL_SCREEN = {[] { halo_list_reset(hd_list); },
                                              [](float dt) { halo_list_update(hd_list, dt); }, hd_layout, hd_draw,
                                              false, HALO_HOME_BOTH};

static void hd_open_tap(int k) {
  if (k < hi_n) hd_open(k), halo_nav_open(HIST_DETAIL_SCREEN);
}

static const HaloScreen HISTORY_SCREEN = {[] { hi_snapshot(), cl_enter(hi_list); },
                                          [](float dt) {
                                            if (hist_ver != hi_seen) hi_snapshot();  // a new answer came in
                                            cl_update(hi_list, dt);
                                          },
                                          [](const HaloView &v) { cl_layout(hi_list, v); }, [] { cl_draw(hi_list); },
                                          false, HALO_HOME_BOTH};

// ---------- Notifications (ancs.h): a card per notification, newest first; a tap — the whole message ----------
struct NtText {
  char t[NT_N][64], s[NT_N][64];
  NotifStore copy;
  Notif open, banner;
};
static NtText &ntx = psram_new<NtText>();
static int ntx_n;
static uint32_t ntx_seen = ~0u;
static uint8_t ntx_bt = 0xFF;
static CardRow nt_rows[NT_N];
static CardList &nt_list = cl_new("Notifications", nt_rows, NT_N);

static void nt_title(char *out, size_t n, const Notif &e, const HaloFont &f, float w) {  // "Messages: Mom"
  char t[96];
  snprintf(t, sizeof(t), "%s%s%s", e.app, e.title[0] ? ": " : "", e.title);
  fit_text(out, n, f, t, w);
}

static void nt_snapshot() {
  ntx_seen = nt_ver, ntx_bt = bt_state;
  portENTER_CRITICAL(&nt_mux);
  ntx.copy = nt;
  portEXIT_CRITICAL(&nt_mux);
  const NotifStore &c = ntx.copy;
  ntx_n = c.count;
  for (int k = 0; k < ntx_n; k++) {
    const Notif &e = c.e[(c.head + NT_N - 1 - k) % NT_N];
    nt_title(ntx.t[k], 64, e, t24, 290), fit_text(ntx.s[k], 64, t20, e.msg, 290);
  }
  if (!ntx_n) {
    const char *t = !cfg.bt_on ? "Bluetooth is off" : bt_state == BT_READY ? "No notifications" : "No iPhone yet";
    const char *s = !cfg.bt_on ? "Turn it on in Settings" : bt_state == BT_READY ? "New ones show up here" : BT_HOW;
    fit_text(ntx.t[0], 64, t24, t, 290), fit_text(ntx.s[0], 64, t20, s, 290);
  }
  nt_list.n = max(1, ntx_n);
}

static void nt_show(const Notif &e) {  // into the detail screen: "Messages, 21:32", the title, the message
  char when[48], c[16] = "";
  if (e.epoch) {
    const time_t t = e.epoch;
    struct tm tm;
    localtime_r(&t, &tm);
    clock_str(c, sizeof(c), tm);
  }
  snprintf(when, sizeof(when), "%s%s%s", e.app, c[0] ? ", " : "", c);
  hd_fill(when, e.title, e.msg);
}

static void nt_open_tap(int k) {
  if (k >= ntx_n) return;
  ntx.open = ntx.copy.e[(ntx.copy.head + NT_N - 1 - k) % NT_N];
  nt_show(ntx.open), halo_nav_open(HIST_DETAIL_SCREEN);
}

static const HaloScreen NOTIF_SCREEN = {[] { nt_unread = 0, nt_snapshot(), cl_enter(nt_list); },  // read: no dot
                                        [](float dt) {
                                          if (nt_ver != ntx_seen || bt_state != ntx_bt) nt_snapshot();
                                          cl_update(nt_list, dt);
                                        },
                                        [](const HaloView &v) { cl_layout(nt_list, v); }, [] { cl_draw(nt_list); },
                                        false, HALO_HOME_BOTH};

// The banner: a new notification slides down from the top for 5 s, over any screen; a tap opens it
static HaloSpring nb_k;  // 0 — hidden above the edge, 1 — shown
static uint32_t nb_seen, nb_until;
static bool nb_pending;
static char nb_t[64], nb_s[64];
static HaloItem nb_bg, nb_title, nb_msg;

static float nb_y() {
  return -70 + 170 * nb_k.x;
}

static void notif_tick() {  // every frame, before power_tick (Always On too): a new one wakes the screen and dings
  if (nt_new_ver == nb_seen) return;
  nb_seen = nt_new_ver, nb_pending = true;
  pw_cmd_wake = true;
  if ((int32_t)(millis() - ai_chime_until) >= 0) ai_chime_mode = 1, ai_chime_until = millis() + 300;  // not over an alarm
}

static void banner_update(float dt) {  // before halo_nav_update: a touch on it is not the screen's
  if (nb_pending) {
    nb_pending = false;
    portENTER_CRITICAL(&nt_mux);
    ntx.banner = nt_banner;
    portEXIT_CRITICAL(&nt_mux);
    nt_title(nb_t, sizeof(nb_t), ntx.banner, t24, 300), fit_text(nb_s, sizeof(nb_s), t20, ntx.banner.msg, 300);
    nb_until = millis() + 5000, nb_k.target = 1;
  }
  if ((int32_t)(millis() - nb_until) > 0) nb_k.target = 0;
  halo_spring_run(nb_k, K_M3_DEFAULT, dt);
  HaloFinger &f = halo_finger;
  const float y = nb_y();
  if (f.pressed && nb_k.target > 0.5f && fabsf(f.x0 - CX) < 180 && fabsf(f.y0 - y) < 52) halo_finger_claim(&nb_k);
  if (f.tap && f.owner == &nb_k) {
    nb_k.target = 0, nb_until = millis();
    nt_show(ntx.banner);
    if (halo_nav_on_face()) halo_nav_face_open(HIST_DETAIL_SCREEN, HALO_FX_FADE, CX, y, 360, 104);
    else halo_nav_open(HIST_DETAIL_SCREEN);
  }
}

static void banner_layout() {
  const float y = nb_y(), a = nb_k.x > 0.01f ? 1 : 0;
  if (fabsf(nb_k.v) > 0.01f) halo_subpixel = true;
  nb_bg.kind = HI_RRECT, nb_bg.x = CX, nb_bg.y = y, nb_bg.w = 360, nb_bg.h = 104, nb_bg.r = 52, nb_bg.scale = 1;
  nb_bg.color = 0x3A3D44, nb_bg.alpha = a, nb_bg.on_black = true;
  halo_item_commit(nb_bg);
  text_mid(nb_title, t24, nb_t, ap_mid_t24, CX, y - 16, 1, TEXT, a, &nb_bg);
  text_mid(nb_msg, t20, nb_s, ap_mid_t20, CX, y + 20, 1, 0xC6C6C6, a, &nb_bg);
  halo_item_commit(nb_title), halo_item_commit(nb_msg);
}

static void banner_draw() {
  halo_item_draw(nb_bg), halo_item_draw(nb_title), halo_item_draw(nb_msg);
}

// ---------- Music (ancs.h: AMS): what plays on the iPhone, its progress, and the remote ----------
// No artwork: Apple's media service sends only text. Icons are drawn with lines (the font has no play/skip shapes)
struct MusicUi {
  Media m;                                  // a copy, taken when md_ver changes
  char t[2][96], artist[64], player[40], left[12], right[12], vol[16];
  int nt;                                   // title lines
  HaloItem head, title[2], art, bar, fill, tl, tr, vol_t;
  HaloItem disc[3], ic[3][7];               // prev, play/pause, next: a circle and up to 7 strokes
  HaloItem vminus, vplus;
  Press p[5];                               // prev, play, next, vol −, vol +
  int armed;
};
static MusicUi &mu = psram_new<MusicUi>();
static uint32_t mu_seen = ~0u;
#define MU_CTRL_Y 334.0f
static const float MU_X[3] = {CX - 112, CX, CX + 112}, MU_R[3] = {34, 46, 34};

static void mu_snapshot() {
  mu_seen = md_ver;
  portENTER_CRITICAL(&md_mux);
  mu.m = md;
  portEXIT_CRITICAL(&md_mux);
  mu.nt = wrap_text(t24, mu.m.title[0] ? mu.m.title : "", 330, mu.t, 2);
  fit_text(mu.t[1], 96, t24, mu.t[1], 330);  // a longer title: the second line ends in "…"
  fit_text(mu.artist, 64, t20, mu.m.artist, 320);
  strlcpy(mu.player, mu.m.player[0] ? mu.m.player : "Music", sizeof(mu.player));
}

static float mu_elapsed() {  // now, from the last update and its rate
  const Media &m = mu.m;
  float e = m.elapsed + (m.state == 1 ? m.rate * (millis() - m.at_ms) / 1000.0f : 0);
  return m.duration > 0 ? fminf(e, m.duration) : e;
}

static void mu_time(char *s, size_t n, float sec, bool minus) {
  const int t = (int)fmaxf(0, sec);
  snprintf(s, n, "%s%d:%02d", minus ? "-" : "", t / 60, t % 60);
}

// A filled triangle of strokes, pointing right (dir 1) or left (−1), centered at (x, y), h tall
static int mu_tri(HaloItem *it, float x, float y, float h, int dir, uint32_t c, float a, const HaloView &v) {
  const float bx = x - dir * h * 0.42f, tx = x + dir * h * 0.5f, hh = h / 2;
  const float pts[6][4] = {{bx, y - hh, bx, y + hh}, {bx, y - hh, tx, y}, {bx, y + hh, tx, y},
                           {bx, y - hh / 2, tx, y}, {bx, y + hh / 2, tx, y}, {bx, y, tx, y}};
  for (int i = 0; i < 6; i++) {
    HaloItem &l = it[i];
    l.kind = HI_LINE, l.x = pts[i][0], l.y = pts[i][1], l.w = pts[i][2], l.h = pts[i][3], l.r = 3, l.scale = 1;
    l.color = c, l.alpha = a, l.on_black = false;
    halo_view_commit(l, v);
  }
  return 6;
}

static void mu_hide(HaloItem *it, int from, int to, const HaloView &v) {
  for (int i = from; i < to; i++) it[i].alpha = 0, halo_view_commit(it[i], v);
}

static void music_update(float dt) {
  if (md_ver != mu_seen) mu_snapshot();
  const HaloFinger &f = halo_finger;
  if (f.pressed) {
    mu.armed = -1;
    for (int i = 0; i < 3; i++)
      if (hypotf(f.x0 - MU_X[i], f.y0 - MU_CTRL_Y) < MU_R[i] + 10) mu.armed = i;
    if (fabsf(f.y0 - 414) < 30 && fabsf(f.x0 - (CX - 64)) < 34) mu.armed = 3;
    if (fabsf(f.y0 - 414) < 30 && fabsf(f.x0 - (CX + 64)) < 34) mu.armed = 4;
    if (f.owner || !mu.m.valid) mu.armed = -1;
  }
  if (f.dragging) mu.armed = -1;
  if (f.tap && mu.armed >= 0) {
    static const uint8_t CMD[5] = {AMS_PREV, AMS_TOGGLE, AMS_NEXT, AMS_VOL_DOWN, AMS_VOL_UP};
    ams_cmd(CMD[mu.armed]);
  }
  for (int i = 0; i < 5; i++) press_update(mu.p[i], i == mu.armed && f.down, dt);
  if (!f.down) mu.armed = -1;
}

static void music_layout(const HaloView &v) {
  const Media &m = mu.m;
  const bool ok = m.valid && m.title[0];
  // Header: the app; then the title (two lines at most) and the artist — or why there is nothing
  const char *head = !m.valid ? (bt_state == BT_PAUSED ? "Paused while Wi-Fi is on" : "No iPhone connected")
                              : ok ? mu.player : "Nothing playing";
  text_mid(mu.head, t20, head, ap_mid_t20, CX, 74, 1, DIM, 1, &halo_black);
  mu.head.on_black = true;
  halo_view_commit(mu.head, v);
  const char *t0 = ok ? mu.t[0] : m.valid ? "Play something on your iPhone" : BT_HOW;
  const float ty = ok && mu.nt > 1 ? 122 : 140;
  text_mid(mu.title[0], ok ? t24 : t20, t0, ok ? ap_mid_t24 : ap_mid_t20, CX, ty, 1, ok ? TEXT : DIM, 1, &halo_black);
  text_mid(mu.title[1], t24, mu.t[1], ap_mid_t24, CX, ty + 34, 1, TEXT, ok && mu.nt > 1 ? 1 : 0, &halo_black);
  text_mid(mu.art, t20, mu.artist, ap_mid_t20, CX, ty + (mu.nt > 1 ? 70 : 38), 1, 0xA8C7FA, ok ? 1 : 0, &halo_black);
  mu.title[0].on_black = mu.title[1].on_black = mu.art.on_black = true;
  halo_view_commit(mu.title[0], v), halo_view_commit(mu.title[1], v), halo_view_commit(mu.art, v);
  // Progress: a bar and the times (redrawn once a second: the times change, the bar in whole pixels)
  const float e = mu_elapsed(), d = m.duration, r = d > 0 ? halo_clamp01(e / d) : 0, bw = 300;
  mu_time(mu.left, sizeof(mu.left), e, false), mu_time(mu.right, sizeof(mu.right), d - e, true);
  const float pa = ok && d > 0 ? 1 : 0;
  mu.bar.kind = mu.fill.kind = HI_RRECT, mu.bar.y = mu.fill.y = 240, mu.bar.h = mu.fill.h = 8, mu.bar.r = mu.fill.r = 4;
  mu.bar.x = CX, mu.bar.w = bw, mu.bar.color = DP_TRACK, mu.bar.alpha = pa, mu.bar.scale = mu.fill.scale = 1;
  const float fw = floorf(bw * r);
  mu.fill.x = CX - bw / 2 + fw / 2, mu.fill.w = fw, mu.fill.color = DP_PRIMARY, mu.fill.alpha = fw > 2 ? pa : 0;
  mu.bar.on_black = true, mu.fill.on_black = false;
  halo_view_commit(mu.bar, v), halo_view_commit(mu.fill, v);
  text_base(mu.tl, t20, mu.left, CX - bw / 2, 274, 1, DIM, pa, &halo_black);
  mu.tr.kind = HI_TEXT, text_base(mu.tr, t20, mu.right, CX + bw / 2, 274, 1, DIM, pa, &halo_black);
  mu.tr.align = 2, mu.tl.on_black = mu.tr.on_black = true;
  halo_view_commit(mu.tl, v), halo_view_commit(mu.tr, v);
  // The remote: previous, play/pause (the big one), next
  const float ca = m.valid ? 1 : 0.35f;
  for (int i = 0; i < 3; i++) {
    HaloItem &c = mu.disc[i];
    const float p = halo_clamp01(mu.p[i].s.x);
    c.kind = HI_CIRCLE, c.x = MU_X[i], c.y = MU_CTRL_Y, c.r = MU_R[i] * (1 - 0.08f * p), c.scale = 1;
    c.color = i == 1 ? DP_PRIMARY : CARD_HI, c.alpha = ca, c.on_black = true;
    halo_view_commit(c, v);
  }
  const uint32_t dark = DP_ON_PRIMARY;
  int n = mu_tri(mu.ic[0], MU_X[0] - 4, MU_CTRL_Y, 22, -1, TEXT, ca, v);  // previous: ◀|
  HaloItem &pb = mu.ic[0][n];
  pb.kind = HI_LINE, pb.x = pb.w = MU_X[0] - 15, pb.y = MU_CTRL_Y - 11, pb.h = MU_CTRL_Y + 11, pb.r = 3, pb.scale = 1;
  pb.color = TEXT, pb.alpha = ca, halo_view_commit(pb, v);
  if (m.state == 1) {  // playing: pause bars
    for (int i = 0; i < 2; i++) {
      HaloItem &l = mu.ic[1][i];
      const float x = MU_X[1] + (i ? 9 : -9);
      l.kind = HI_LINE, l.x = l.w = x, l.y = MU_CTRL_Y - 14, l.h = MU_CTRL_Y + 14, l.r = 4.5f, l.scale = 1;
      l.color = dark, l.alpha = ca, l.on_black = false, halo_view_commit(l, v);
    }
    mu_hide(mu.ic[1], 2, 7, v);
  } else {
    mu_tri(mu.ic[1], MU_X[1] + 3, MU_CTRL_Y, 30, 1, dark, ca, v);
    mu_hide(mu.ic[1], 6, 7, v);
  }
  n = mu_tri(mu.ic[2], MU_X[2] + 4, MU_CTRL_Y, 22, 1, TEXT, ca, v);  // next: ▶|
  HaloItem &nb = mu.ic[2][n];
  nb.kind = HI_LINE, nb.x = nb.w = MU_X[2] + 15, nb.y = MU_CTRL_Y - 11, nb.h = MU_CTRL_Y + 11, nb.r = 3, nb.scale = 1;
  nb.color = TEXT, nb.alpha = ca, halo_view_commit(nb, v);
  // The iPhone's volume: − and + with the level between them
  snprintf(mu.vol, sizeof(mu.vol), "%d%%", (int)lroundf(m.volume * 100));
  const float va = m.valid ? 1 : 0;
  text_mid(mu.vminus, ic32, "", sl_mid_icon, CX - 64, 414, 1, halo_clamp01(mu.p[3].s.x) > 0.3f ? DP_PRIMARY : TEXT, va, &halo_black);
  text_mid(mu.vplus, ic32, "", sl_mid_icon, CX + 64, 414, 1, halo_clamp01(mu.p[4].s.x) > 0.3f ? DP_PRIMARY : TEXT, va, &halo_black);
  text_mid(mu.vol_t, t20, mu.vol, ap_mid_t20, CX, 414, 1, DIM, va, &halo_black);
  mu.vminus.on_black = mu.vplus.on_black = mu.vol_t.on_black = true;
  halo_view_commit(mu.vminus, v), halo_view_commit(mu.vplus, v), halo_view_commit(mu.vol_t, v);
}

static void music_draw() {
  halo_item_draw(mu.head), halo_item_draw(mu.title[0]), halo_item_draw(mu.title[1]), halo_item_draw(mu.art);
  halo_item_draw(mu.bar), halo_item_draw(mu.fill), halo_item_draw(mu.tl), halo_item_draw(mu.tr);
  for (int i = 0; i < 3; i++) {
    halo_item_draw(mu.disc[i]);
    for (auto &l : mu.ic[i]) halo_item_draw(l);
  }
  halo_item_draw(mu.vminus), halo_item_draw(mu.vplus), halo_item_draw(mu.vol_t);
}
static const HaloScreen MUSIC_SCREEN = {[] { mu_snapshot(), mu.armed = -1; }, music_update, music_layout, music_draw,
                                        false, HALO_HOME_BOTH};

static void open_history() {  // the Gemini screen's History button (assistant.h)
  halo_nav_open(HISTORY_SCREEN);
}

// ---------- The face's bits from the iPhone (face.h): the song line, the unread dot, Music over the face ----------
static const char *face_now_playing(float w) {
  static uint32_t seen = ~0u;
  static float seen_w;
  static char line[96];
  static bool playing;
  if (md_ver != seen || w != seen_w) {
    seen = md_ver, seen_w = w;
    char t[96], a[64];
    portENTER_CRITICAL(&md_mux);
    playing = md.valid && md.state == 1 && md.title[0];
    strlcpy(t, md.title, sizeof(t)), strlcpy(a, md.artist, sizeof(a));
    portEXIT_CRITICAL(&md_mux);
    char both[170];
    snprintf(both, sizeof(both), "%s%s%s", t, a[0] ? " - " : "", a);
    fit_text(line, sizeof(line), t20, both, w);
  }
  return playing ? line : nullptr;
}

static int face_unread() {
  return nt_unread;
}

static void face_open_music(float y) {
  halo_nav_face_open(MUSIC_SCREEN, HALO_FX_FADE, CX, y, 300, 40);
}

// ---------- Steps ----------
static int st_day = -1, st_saved_day = -1;  // year * 400 + day of the year
static uint32_t st_base, st_saved_n, st_written = ~0u;

static void steps_begin() {  // after power_build (the motion sensor), after settings_load (se_prefs)
  st_saved_day = se_prefs.getInt("st_day", -1), st_saved_n = se_prefs.getUInt("st_n", 0);
  steps_today = st_saved_n;
  halo_steps_begin();
}

static void steps_poll(bool now) {  // from loop(): every 2 s; into flash every 10 min (and before a restart)
  static uint32_t at, saved_at;
  if (!now && millis() - at < 2000) return;
  at = millis();
  uint32_t c = halo_steps();
  if (halo_clock_valid()) {
    const time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    const int d = tm.tm_year * 400 + tm.tm_yday;
    if (st_day < 0) st_day = d, st_base = st_saved_day == d ? st_saved_n : 0;  // the clock is known: which day
    else if (d != st_day) st_day = d, st_base = 0, halo_steps_reset(), c = 0;  // midnight
  } else {
    st_base = st_saved_n;  // until the clock is known: the last saved count, assumed today
  }
  steps_today = st_base + c;
  if (st_day >= 0 && steps_today != st_written && (now || millis() - saved_at > 600000)) {
    se_prefs.putInt("st_day", st_day), se_prefs.putUInt("st_n", steps_today);
    st_written = steps_today, saved_at = millis();
  }
}

// ---------- Build ----------
static void apps_build() {
  ap_mid_head = halo_text_mid(t_title, "Timer");
  ap_mid_big = halo_text_mid(f_time, "0123456789");
  ap_mid_t20 = halo_text_mid(t20, "Rings at 0123456789");
  ap_mid_t24 = halo_text_mid(t24, "Hello");
  btn_mid = ap_mid_t24;
  for (int i = 0; i < WX_PAGES; i++) wc_rows[i] = {nullptr, nullptr, SE_VALUE, nullptr, nullptr, wc_title, wc_subtitle};
  for (int i = 0; i < HIST_N; i++)
    hi_rows[i] = {nullptr, nullptr, SE_VALUE, nullptr, nullptr, [](int k) { return (const char *)hi.t[k]; },
                  [](int k) { return (const char *)hi.s[k]; }, hd_open_tap};
  for (int i = 0; i < NT_N; i++)
    nt_rows[i] = {nullptr, nullptr, SE_VALUE, nullptr, nullptr, [](int k) { return (const char *)ntx.t[k]; },
                  [](int k) { return (const char *)ntx.s[k]; }, nt_open_tap};
  cl_build(wc_list), cl_build(hi_list), cl_build(nt_list);
}
