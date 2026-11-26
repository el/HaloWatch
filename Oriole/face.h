// face.h — the watch face (design/face_mockup.webp): the date in capitals, a big clock and a weather pill. It is the
// home of Oriole: swipe up anywhere and the gallery slides up over it; swipe down from the gallery's top edge to
// come back (halo_nav_face in lib/Halo).
//
// Every change moves, at 60 fps: changed digits and letters roll (HaloRoll: the old one goes up and fades, the new one
// comes up from below, several in a cascade from the right), letters of another width push their neighbours aside,
// the pill grows or shrinks with the temperature, and a new weather icon pops in while the old one shrinks away. Only
// what moves is redrawn. The time comes from the internet or the web controller (net.h): until then "--:--"; the
// weather is the first city's, from Open-Meteo (weather.h). 12 hours, or 24 with the clock24 setting.
//
// A tap on the pill opens the Weather screen right over the face (halo_nav_face_open: back — swipe up from the bottom
// edge). The transition is Fade (fixed: docs/halo.md, section 10; it used to be chosen in Settings and by a long press
// here). Holding BOOT opens the Assistant over the face the same way (assistant.h).
//
// Positions are measured from the mockup (screen circle 1215 px -> 466 px, scale 0.3837): text by the middle of its
// letters, the icon by its center. Font sizes come from the digit heights, weights from the amount of "ink"
// (assets/halo.conf), the icons from shapes fitted to the mockup (assets/gen_icons.py). The whole face is then drawn
// FACE_SCALE times larger around the screen center (asked for: closer to the edges); fonts and icons are made at that
// size, not stretched at runtime.
// Commands: fx N (another transition 0..2 until a reboot, to compare), tick [N] (the clock N minutes ahead), day (a day
// ahead). Included from Oriole.ino after weather.h.

#define FACE_DATE 0xC2C3CB  // the date: light gray with a cool tint
#define FACE_WHITE 0xF8F7F9
#define FACE_PILL 0x2D2F2F
#define FACE_SCALE 1.2f  // the face is 1.2x the mockup (keep in sync with halo.conf and FACE_SCALE in gen_icons.py)
// Mockup coordinates -> screen, scaled around the screen center; sizes and offsets — just times FACE_SCALE
static inline float fsc(float v) {
  return CX + (v - CX) * FACE_SCALE;
}
// The whole face up by FACE_LIFT: from the top of the date to the bottom of the pill it was 6.5 px below the circle's
// center (measured on the board); lifted to the exact center it looked a little high — on the board 4 px lower was
// picked, then 6 px more: the block's middle is 10 px below the circle's center (horizontally it is centered: the date
// and the pill by their middles, the clock by its colon)
#define FACE_LIFT -3.5f
// Mockup measurements, board px
#define FACE_DATE_Y (fsc(116.0f) - FACE_LIFT)   // middle of the date's capitals
#define FACE_TIME_Y (fsc(219.2f) - FACE_LIFT)   // middle of the clock digits
#define PILL_Y (fsc(336.1f) - FACE_LIFT)        // pill center
#define PILL_H (75.6f * FACE_SCALE)
#define PILL_W (186.1f * FACE_SCALE)          // pill width with "19°"
#define PILL_ICON (53.5f * FACE_SCALE)        // icon center from the pill's left edge
#define PILL_ICON_Y (fsc(337.0f) - FACE_LIFT)
#define PILL_INK (95.2f * FACE_SCALE)         // left ink edge of "19°" from the pill's left edge
#define PILL_TEMP_Y (fsc(338.0f) - FACE_LIFT)   // middle of the temperature digits
#define FACE_ICON_RISE (10 * FACE_SCALE)      // a changing icon moves up by this much

// Styles (Settings -> Watch face: the face_style setting) and the clock's color (face_color). The first three are the
// mockup's: Classic (date, clock, pill), Simple (date and clock), Time only; the block glides to the middle the classic
// one sits at, and the date and the pill fade out or in. The others are Wear OS-like:
//   Analog  — 12 hour ticks, hour and minute hands in the clock's color, a coral second hand (it ticks), the date below
//   Stacked — the hours over the minutes, big (the hours in the clock's color), the date on top
//   Arcs    — date and clock as Simple, an outer ring filling with the seconds, an inner one with the battery
// Always On (power.h) shows each one dim, without seconds and rings.
enum : uint8_t { FS_CLASSIC, FS_SIMPLE, FS_TIME, FS_ANALOG, FS_STACKED, FS_ARCS, FS_N };
static const char *const FACE_STYLE[FS_N] = {"Classic", "Simple", "Time only", "Analog", "Stacked", "Arcs"};
static const char *const FACE_COLOR_NAME[5] = {"White", "Blue", "Amber", "Mint", "Rose"};
static const uint32_t FACE_COLOR[5] = {FACE_WHITE, 0xA8C7FA, 0xFFC56B, 0x8BE3B9, 0xF7A8C4};
#define FACE_SECOND 0xFF8A65  // the second hand and the cap
#define FACE_TRACK 0x2A2C30   // the rings' empty part
// From apps.h (the timer, the steps): shown on the face; the timer also wakes the board (power.h)
#define STEPS_GOAL 8000
static volatile uint32_t steps_today;  // apps.h counts them
static float timer_face_fraction();    // what is left of the timer, 0..1 (−1 — none running)
static int32_t timer_ms_left();        // until it rings, ms (−1 — not running)
static void steps_poll(bool now);      // from loop(): the pedometer, saved to flash
// From apps.h (the iPhone, over Bluetooth): what plays — "Title - Artist" fitted to w (nullptr: nothing plays),
// how many notifications came since the Notifications screen was opened, and the Music screen over the face
static const char *face_now_playing(float w);
static int face_unread();
static void face_open_music(float y);
static HaloItem fc_song, fc_unread;
static float fc_song_y = -1;  // where the song line is now (−1 — none)
static HaloItem fc_timer, fc_ring_st, fc_steps;
static char fc_steps_s[24];
static HaloSpring fc_dy, fc_date_a, fc_pill_a;  // the block's shift; the date's and the pill's opacity
static float fc_style_dy[FS_N];
static float fc_digit_h, fc_st_date_y, fc_st_hh_y, fc_st_mm_y, fc_mid_bat;  // Stacked: rows; Arcs: the battery text
static HaloRoll &r_hh = psram_new<HaloRoll>(), &r_mm = psram_new<HaloRoll>();  // Stacked (in PSRAM)
struct FcTicks {
  HaloItem it[12];
};
static HaloItem (&fc_tick)[12] = psram_new<FcTicks>().it;   // Analog: hour ticks (in PSRAM)
static HaloItem fc_hand[3], fc_cap[2];                       // Analog: hour, minute, second hands
static HaloItem fc_ring_s, fc_ring_b, fc_bat;                // Arcs
static HaloItem fc_day;                                      // Analog: "THU 26" at 3 o'clock
static char fc_bat_s[20], fc_day_s[12];
static float fc_mid_day;
static int fc_sec;

static const HaloImage *const FACE_ICON[4] = {&img_partly, &img_sun, &img_cloud, &img_rain};  // by WX_PARTLY ...
static const char *const WEEKDAY[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
static const char *const MONTH[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                      "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

static HaloRoll &r_time = psram_new<HaloRoll>(), &r_date = psram_new<HaloRoll>(), &r_temp = psram_new<HaloRoll>();
static HaloItem fc_pill, fc_icon_in, fc_icon_out;
static HaloSpring fc_icon_t;  // icon change: 0 -> 1
static HaloSpringK K_ICON;
static const HaloImage *fc_icon, *fc_icon_old;
static char fc_temp[8];
static int fc_min = -1, fc_hour = -1, fc_yday = -1;
static float fc_pen, fc_pad_r;  // the temperature's pen from the pill's left edge, and the pill after the text
static int32_t fc_offset;       // shift of the clock, s (tick, day)
static uint8_t fc_fx = HALO_FX_FADE;         // the transition to the Weather screen: Fade (fx N — another, to compare)
static bool fc_check;
static bool fc_still;  // Always On (power.h): digits and date change without rolling — asleep there is no time to play it
static volatile int fc_cmd_fx = -1;
static volatile int32_t fc_cmd_shift;

static int face_style() {
  return constrain(cfg.face_style, 0, FS_N - 1);
}

static void face_style_targets() {
  const int st = face_style();
  fc_dy.target = fc_style_dy[st], fc_date_a.target = st != FS_TIME, fc_pill_a.target = st == FS_CLASSIC;
}

// The hands at (cx, cy): hour and minute in `col`, the second hand only with `seconds`; a = 0 hides them all
static void face_hands(float cx, float cy, uint32_t col, bool seconds, float a, const HaloView &v) {
  const bool ok = fc_min >= 0;
  const float sec = seconds ? fc_sec : 0;
  const float ang[3] = {((fc_hour % 12) + fc_min / 60.0f) / 12 * 2 * HALO_PI, (fc_min + sec / 60) / 60 * 2 * HALO_PI,
                        sec / 60 * 2 * HALO_PI};
  static const float LEN[3] = {112, 172, 194}, TAIL[3] = {14, 14, 34}, HW[3] = {7, 5, 2};
  for (int i = 0; i < 3; i++) {
    HaloItem &h = fc_hand[i];
    const float sx = sinf(ang[i]), cy_ = -cosf(ang[i]);
    h.kind = HI_LINE, h.x = cx - sx * TAIL[i], h.y = cy - cy_ * TAIL[i], h.w = cx + sx * LEN[i], h.h = cy + cy_ * LEN[i];
    h.r = HW[i], h.scale = 1, h.color = i == 2 ? FACE_SECOND : col, h.on_black = false;
    h.alpha = ok && (i < 2 || seconds) ? a : 0;
    halo_view_commit(h, v);
  }
  for (int i = 0; i < 2; i++) {  // the cap: a coral dot with a black middle
    HaloItem &c = fc_cap[i];
    c.kind = HI_CIRCLE, c.x = cx, c.y = cy, c.r = i ? 3 : 9, c.scale = 1, c.on_black = false;
    c.color = i ? 0 : seconds ? FACE_SECOND : col, c.alpha = ok ? a : 0;
    halo_view_commit(c, v);
  }
}

static float pill_width() {
  return fc_pen + halo_roll_width(r_temp) + fc_pad_r;
}

// The weather of the first city: the pill on the face (page 0 of the Weather screen shows the same)
static void face_weather(bool animate = true) {
  const WxDay &d = wx_days[0];
  const HaloImage *icon = FACE_ICON[d.kind];
  if (icon != fc_icon && animate) {
    fc_icon_old = fc_icon;
    halo_spring_set(fc_icon_t, 0);
    fc_icon_t.target = 1;
  }
  fc_icon = icon;
  if (wx_ok[0]) snprintf(fc_temp, sizeof(fc_temp), "%d°", d.now);
  else strcpy(fc_temp, "--°");
  halo_roll_set(r_temp, fc_temp, animate);
}

static void face_build() {
  K_ICON = halo_spring(0.5f, 0.7f);  // the icon pops with a visible overshoot
  r_time.tabular = true;  // a cell per digit: nothing shifts when digits change, the colon is right at the center
  halo_roll_init(r_time, f_time, "0123456789", 1, -1);  // letter spacing -1, as on the mockup
  halo_roll_init(r_date, f_date, "HOT0123");
  r_temp.rise = 15 * FACE_SCALE;  // the temperature rolls inside the pill (its letters are drawn with the pill's colors)
  halo_roll_init(r_temp, f_temp, "0123456789", 0);
  halo_roll_set(r_time, "--:--", false);
  r_hh.tabular = r_mm.tabular = true;
  halo_roll_init(r_hh, f_time, "0123456789", 1, -1), halo_roll_init(r_mm, f_time, "0123456789", 1, -1);
  halo_roll_set(r_hh, "--", false), halo_roll_set(r_mm, "--", false);
  face_weather(false);
  halo_spring_set(fc_icon_t, 1);
  // The pill is laid out by the pen of the text (the mockup gives the ink edge of "1")
  const HaloMask *one = halo_glyph(f_temp, '1');
  fc_pen = PILL_INK - (one ? one->ox : 0);
  fc_pad_r = PILL_W - fc_pen - halo_text_width(f_temp, "19°");
  // Each style's block, centered where the classic one is (its middle was picked on the board): by the ink heights
  const HaloMask *d = halo_glyph(f_date, 'H'), *t = halo_glyph(f_time, '0');
  const float hd = d ? d->h : 0, ht = t ? t->h : 0, mid = (FACE_DATE_Y - hd / 2 + PILL_Y + PILL_H / 2) / 2;
  fc_style_dy[0] = 0, fc_style_dy[1] = mid - (FACE_DATE_Y - hd / 2 + FACE_TIME_Y + ht / 2) / 2;
  fc_style_dy[2] = mid - FACE_TIME_Y;
  fc_style_dy[FS_ANALOG] = fc_style_dy[FS_STACKED] = 0, fc_style_dy[FS_ARCS] = fc_style_dy[FS_SIMPLE];
  // Stacked: the date, the hours, the minutes — the block centered as the others
  fc_digit_h = ht;
  const float top = mid - (hd + 24 + ht + 18 + ht) / 2;
  fc_st_date_y = top + hd / 2, fc_st_hh_y = top + hd + 24 + ht / 2, fc_st_mm_y = fc_st_hh_y + ht + 18;
  fc_mid_bat = halo_text_mid(t20, "Battery 0123456789%");
  fc_mid_day = halo_text_mid(t24, "THU 0123456789");
  face_style_targets();
  halo_spring_set(fc_dy, fc_dy.target), halo_spring_set(fc_date_a, fc_date_a.target);
  halo_spring_set(fc_pill_a, fc_pill_a.target);
  halo_command("fx", [](const char *a) { fc_cmd_fx = atoi(a); });
  halo_command("tick", [](const char *a) { fc_cmd_shift = 60 * (*a ? atoi(a) : 1); });
  halo_command("day", [](const char *) { fc_cmd_shift = 86400; });
}

// Every frame, whatever is on screen: the clock and the date, the rolls and the icon spring
static void face_tick(float dt) {
  static uint32_t checked;
  if (wx_apply()) face_weather();  // new weather from the internet
  if (fc_cmd_fx >= 0) fc_fx = fc_cmd_fx % HALO_FX_COUNT, fc_cmd_fx = -1;
  static bool c24 = cfg.clock24;
  if (c24 != cfg.clock24) c24 = cfg.clock24, fc_min = -1, fc_check = true;  // the clock24 setting changed
  bool now_check = halo_ms() - checked >= 250 || fc_check;
  fc_check = false;
  if (fc_cmd_shift) fc_offset += fc_cmd_shift, fc_cmd_shift = 0, now_check = true;
  if (now_check && halo_clock_valid()) {
    checked = halo_ms();
    const time_t now = time(nullptr) + fc_offset;
    struct tm t;
    localtime_r(&now, &t);
    char s[20];
    if (t.tm_min != fc_min || t.tm_hour != fc_hour) {
      fc_min = t.tm_min, fc_hour = t.tm_hour;
      snprintf(s, sizeof(s), "%02d:%02d", c24 ? fc_hour : fc_hour % 12 ? fc_hour % 12 : 12, fc_min);  // 12 or 24 hours
      halo_roll_set(r_time, s, !fc_still);
      s[2] = 0;  // Stacked: "hh" and "mm" of the same string
      halo_roll_set(r_hh, s, !fc_still), halo_roll_set(r_mm, s + 3, !fc_still);
    }
    fc_sec = t.tm_sec;
    if (t.tm_yday != fc_yday) {  // "THU, 29 OCT" — the mockup's format
      fc_yday = t.tm_yday;
      snprintf(s, sizeof(s), "%s, %d %s", WEEKDAY[t.tm_wday], t.tm_mday, MONTH[t.tm_mon]);
      halo_roll_set(r_date, s, !fc_still);
      snprintf(fc_day_s, sizeof(fc_day_s), "%s %d", WEEKDAY[t.tm_wday], t.tm_mday);
    }
  }
  halo_roll_update(r_time, dt), halo_roll_update(r_date, dt), halo_roll_update(r_temp, dt);
  halo_roll_update(r_hh, dt), halo_roll_update(r_mm, dt);
  halo_spring_run(fc_icon_t, K_ICON, dt);
  face_style_targets();
  halo_spring_run(fc_dy, K_M3_DEFAULT, dt), halo_spring_run(fc_date_a, K_M3_DEFAULT, dt);
  halo_spring_run(fc_pill_a, K_M3_DEFAULT, dt);
  if (fc_icon_t.x == 1 && fc_icon_t.v == 0) fc_icon_old = nullptr;
}

// Always On (power.h): on — the clock and the date as they are now, at once (a roll still playing would freeze in
// sleep half-way) and from then on without rolling; off — rolling again
static void face_still(bool on) {
  fc_still = on;
  if (on) fc_min = fc_hour = fc_yday = -1, fc_check = true;
}

// The Weather screen right over the face, with the Fade transition
static void face_open_weather() {
  halo_nav_face_open(WEATHER, fc_fx, CX, PILL_Y + fc_dy.x, pill_width(), PILL_H);
}

// Only while the face is on screen and still: the pill opens the Weather screen
static void face_update(float) {
  const HaloFinger &f = halo_finger;
  if (f.tap && !f.owner && fc_song_y > 0 && fabsf(f.y - fc_song_y) < 24 && fabsf(f.x - CX) < 150)
    return face_open_music(fc_song_y);  // the song line opens Music
  if (!f.tap || f.owner || fc_pill_a.target < 1) return;  // no pill in this style
  if (fabsf(f.x - CX) < pill_width() / 2 + 10 && fabsf(f.y - PILL_Y - fc_dy.x) < PILL_H / 2 + 10) face_open_weather();
}

static void face_layout(const HaloView &v) {
  const int st = face_style();
  const uint32_t col = FACE_COLOR[constrain(cfg.face_color, 0, 4)];
  const float dy = fc_dy.x, da = halo_clamp01(fc_date_a.x), pa = halo_clamp01(fc_pill_a.x);
  if (fabsf(fc_dy.v) > 0.01f) halo_subpixel = true;  // gliding: letters at fractions of a pixel, or they shiver
  // Arcs: the date, the clock and the battery a little smaller, inside the rings
  HaloView vd = v;
  if (st == FS_ARCS) vd.s *= 0.84f;
  const float date_y = st == FS_STACKED ? fc_st_date_y : FACE_DATE_Y + dy;
  halo_roll_layout(r_date, CX, date_y, FACE_DATE, st != FS_ANALOG && da > 0.01f ? da : 0, &halo_black, vd);
  const bool digital = st <= FS_TIME || st == FS_ARCS;
  halo_roll_layout(r_time, CX, FACE_TIME_Y + dy, col, digital ? 1 : 0, &halo_black, vd);
  // Stacked: the hours in the clock's color, the minutes white (gray under white hours)
  const float sa = st == FS_STACKED ? 1 : 0;
  halo_roll_layout(r_hh, CX, fc_st_hh_y, col, sa, &halo_black, v);
  halo_roll_layout(r_mm, CX, fc_st_mm_y, col == FACE_WHITE ? 0x9AA0A6 : FACE_WHITE, sa, &halo_black, v);
  // Analog: hour ticks (the quarters longer and brighter), then the hands over them
  const float aa = st == FS_ANALOG ? 1 : 0;
  for (int i = 0; i < 12; i++) {
    HaloItem &t = fc_tick[i];
    const bool q = i % 3 == 0;
    const float an = i * HALO_PI / 6, sx = sinf(an), sy = -cosf(an), r0 = q ? 192 : 204, r1 = 220;
    t.kind = HI_LINE, t.x = CX + sx * r0, t.y = CX + sy * r0, t.w = CX + sx * r1, t.h = CX + sy * r1;
    t.r = q ? 3.5f : 2, t.scale = 1, t.color = q ? 0xE3E3E3 : 0x8E9099, t.on_black = true, t.alpha = aa;
    halo_view_commit(t, v);
  }
  text_mid(fc_day, t24, fc_day_s, fc_mid_day, CX + 118, CX, 1, FACE_DATE, aa, &halo_black);  // under the hands
  fc_day.on_black = true;
  halo_view_commit(fc_day, v);
  face_hands(CX, CX, col, true, aa, v);
  // Arcs: the seconds outside, the battery inside, its level under the clock
  const float ra = st == FS_ARCS ? 1 : 0;
  const HaloBattery b = halo_battery();
  HaloItem &rs = fc_ring_s, &rb = fc_ring_b;
  rs.kind = rb.kind = HI_RING, rs.x = rb.x = CX, rs.y = rb.y = CX, rs.scale = rb.scale = 1;
  rs.r = 222, rs.w = 8, rs.v = fc_sec / 60.0f, rs.color = col, rs.color2 = FACE_TRACK, rs.alpha = ra;
  rb.r = 206, rb.w = 6, rb.v = b.ok && b.percent >= 0 ? b.percent / 100.0f : 0, rb.color = 0x5CF08A;
  rb.color2 = FACE_TRACK, rb.alpha = ra, rs.on_black = rb.on_black = true;
  halo_view_commit(rs, v), halo_view_commit(rb, v);
  if (b.ok && b.percent >= 0) snprintf(fc_bat_s, sizeof(fc_bat_s), "Battery %d%%", b.percent);
  else strcpy(fc_bat_s, "Battery --");
  text_mid(fc_bat, t20, fc_bat_s, fc_mid_bat, CX, FACE_TIME_Y + dy + fc_digit_h / 2 + 40, 1, DIM, ra, &halo_black);
  fc_bat.on_black = true;
  halo_view_commit(fc_bat, vd);
  // Arcs: the steps — a third ring inside, the count over the date
  HaloItem &rst = fc_ring_st;
  rst.kind = HI_RING, rst.x = rst.y = CX, rst.scale = 1, rst.r = 192, rst.w = 6, rst.color = 0xF7A8C4;
  rst.v = fminf(1, steps_today / (float)STEPS_GOAL), rst.color2 = FACE_TRACK, rst.alpha = ra, rst.on_black = true;
  halo_view_commit(rst, v);
  snprintf(fc_steps_s, sizeof(fc_steps_s), "%lu steps", (unsigned long)steps_today);
  text_mid(fc_steps, t20, fc_steps_s, fc_mid_bat, CX, FACE_DATE_Y + dy - 40, 1, DIM, ra, &halo_black);
  fc_steps.on_black = true;
  halo_view_commit(fc_steps, vd);
  // Any style: what plays on the iPhone, one dim line where the style leaves room (a tap opens Music)
  static const float SONG_Y[FS_N] = {428, 402, 402, 342, 66, 0};
  const float sy = st == FS_ARCS ? FACE_TIME_Y + dy + fc_digit_h / 2 + 68 : SONG_Y[st];
  const char *song = face_now_playing(st == FS_CLASSIC ? 250 : st == FS_ARCS ? 230 : 290);
  fc_song_y = song ? (st == FS_ARCS ? halo_view_y(vd, sy) : sy) : -1;
  text_mid(fc_song, t20, song ? song : "", fc_mid_bat, CX, sy, 1, 0xA8C7FA, song ? 1 : 0, &halo_black);
  fc_song.on_black = true;
  halo_view_commit(fc_song, st == FS_ARCS ? vd : v);
  // New notifications: a blue dot at the bottom edge, until the Notifications screen is opened
  fc_unread.kind = HI_CIRCLE, fc_unread.x = CX, fc_unread.y = 457, fc_unread.r = 6, fc_unread.scale = 1;
  fc_unread.color = 0x8AB4F8, fc_unread.alpha = face_unread() > 0 ? 1 : 0, fc_unread.on_black = true;
  halo_view_commit(fc_unread, v);
  // Any style: a running timer as a thin amber ring at the very edge (in 1/360 steps: not redrawn every frame)
  const float tf = timer_face_fraction();
  fc_timer.kind = HI_RING, fc_timer.x = fc_timer.y = CX, fc_timer.scale = 1, fc_timer.r = 229, fc_timer.w = 4;
  fc_timer.v = floorf(fmaxf(tf, 0) * 360) / 360, fc_timer.color = 0xFFC56B, fc_timer.color2 = FACE_TRACK;
  fc_timer.alpha = tf >= 0 ? 1 : 0, fc_timer.on_black = true;
  halo_view_commit(fc_timer, v);
  // The pill: as wide as its text right now (it follows the temperature's width spring)
  const float w = pill_width(), left = CX - w / 2;
  fc_pill.kind = HI_RRECT, fc_pill.x = CX, fc_pill.y = PILL_Y + dy, fc_pill.w = w, fc_pill.h = PILL_H;
  fc_pill.r = PILL_H / 2, fc_pill.color = FACE_PILL, fc_pill.on_black = true, fc_pill.scale = 1;
  fc_pill.alpha = pa > 0.01f ? pa : 0;
  halo_view_commit(fc_pill, v);
  // The icon: the new one grows from 0.5 with an overshoot and rises a little, the old one shrinks, rises and fades
  // out quickly (gone by a third of the way: two half-transparent icons on top of each other look muddy)
  const float t = fc_icon_t.x, ix = left + PILL_ICON;
  HaloItem &in = fc_icon_in, &out = fc_icon_out;
  in.kind = out.kind = HI_IMAGE, in.on_black = out.on_black = false;
  in.image = fc_icon, in.x = ix, in.y = PILL_ICON_Y + dy + (1 - t) * FACE_ICON_RISE, in.scale = 0.5f + 0.5f * t;
  in.alpha = halo_clamp01((t - 0.12f) * 1.8f) * fc_pill.alpha;
  out.image = fc_icon_old, out.x = ix, out.y = PILL_ICON_Y + dy - t * FACE_ICON_RISE, out.scale = 1 - 0.5f * t;
  out.alpha = fc_icon_old ? halo_clamp01(1 - t * 3) * fc_pill.alpha : 0;
  halo_view_commit(out, v), halo_view_commit(in, v);
  halo_roll_layout(r_temp, left + fc_pen, PILL_TEMP_Y + dy, FACE_WHITE, fc_pill.alpha, &fc_pill, v);
}

static void face_draw() {
  halo_item_draw(fc_ring_s), halo_item_draw(fc_ring_b), halo_item_draw(fc_bat);
  halo_item_draw(fc_ring_st), halo_item_draw(fc_steps), halo_item_draw(fc_timer);
  halo_item_draw(fc_song), halo_item_draw(fc_unread);
  for (auto &t : fc_tick) halo_item_draw(t);
  halo_item_draw(fc_day);
  halo_roll_draw(r_date), halo_roll_draw(r_time), halo_roll_draw(r_hh), halo_roll_draw(r_mm);
  for (auto &h : fc_hand) halo_item_draw(h);
  halo_item_draw(fc_cap[0]), halo_item_draw(fc_cap[1]);
  halo_item_draw(fc_pill), halo_item_draw(fc_icon_out), halo_item_draw(fc_icon_in);
  halo_roll_draw(r_temp);
}
