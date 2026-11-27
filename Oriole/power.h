// power.h — Always On and sleep (lib/Oriole: oriole_sleep; docs/oriole.md, section 12; tried first in experiments/SleepLab).
//
// 15 s without touches and motion: the screen fades out (0.25 s, display.h's brightness spring) and, at zero, only
// the face's date and clock come back, dim on black — Always On. The board sleeps (light sleep: memory and program
// stay) and wakes once a minute for one frame: new digits, set without rolling (asleep there is no time to play a
// roll), and the pair a few pixels elsewhere (AMOLED burn-in: 8 places on a 7 px circle). The panel keeps the picture
// by itself. (BOOT is the assistant's push-to-talk now, assistant.h: it no longer turns the screen off.)
// A touch, BOOT or picking the board up (after it lay still) — the screen where it was, at full brightness, at once;
// the waking touch presses nothing, the waking BOOT is already talking. A touch while the screen is fading out — it
// stays. While the assistant listens, thinks or speaks the screen stays on and the board does not really sleep (the
// sound needs the processor): oriole_stay_awake; while Wi-Fi is on — neither (net.h). On USB power it does not either
// (Oriole: the USB port would go quiet and the board could not be flashed) — the same screens and wake-ups, the frames
// just go on.
// Commands: aod on|off (Always On, or dark when idle), idle N (s; 0 — never), sleep (Always On now), wake (as a
// touch — only while the board stays up: on USB), usb on|off (really sleep on USB too — the port then goes quiet:
// wake the board by hand before flashing it).
// Included from Oriole.ino after gemini.h, before assistant.h.
#include <sys/time.h>

#define PW_IDLE_S 15
#define PW_AOD_BRIGHT 40   // the panel in Always On (of 255); the text is dimmed too
#define PW_TIME 0xB8B2A8   // Always On: the clock — a dim warm white
#define PW_DATE 0x6A6B70   // the date — a dim cool gray, as the face's

enum { PW_ACTIVE, PW_FADING, PW_AOD, PW_DARK };
static int pw_mode = PW_ACTIVE;
static bool pw_to_aod;        // where the fade goes: Always On or dark
static float pw_k;            // Always On brightness ramp, 0..1
static int64_t pw_active_us;  // the last touch or motion
static uint32_t pw_motion;
static bool pw_held;          // oriole_stay_awake held for the assistant
static volatile bool pw_cmd_sleep, pw_cmd_wake, pw_aod_on = true;
static volatile int pw_idle_s = PW_IDLE_S;

static uint32_t pw_ms_to_minute() {  // until the next minute, +30 ms to be sure it has turned
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return 60000 - (uint32_t)((tv.tv_sec % 60) * 1000 + tv.tv_usec / 1000) + 30;
}

static void pw_fade(bool to_aod) {
  pw_mode = PW_FADING, pw_to_aod = pw_keep_lit = to_aod, dp_power.target = 0;
}

static void pw_wake(uint8_t w) {  // the screen where it was, at full brightness, at once
  static const char *const NAMES[] = {"", "touch", "BOOT", "motion", "timer"};
  LOG("power: woke by %s\n", NAMES[w]);
  pw_mode = PW_ACTIVE, pw_bright = -1, pw_keep_lit = false, pw_active_us = esp_timer_get_time();
  face_still(false);
  if (!oriole_display_is_on()) oriole_display_on(true);
  oriole_spring_set(dp_power, 1);
  oriole_damage_all();
}

// Every frame, first (before the face: Always On wants its digits without rolling)
static void power_tick(float dt) {
  const OrioleFinger &f = oriole_finger;
  const int64_t now = esp_timer_get_time();
  const bool moved = oriole_motion_count() != pw_motion;
  const bool busy = ai_busy();
  if (f.down || moved || busy) pw_active_us = now;  // touched, moved or talking — not idle
  pw_motion = oriole_motion_count();
  if (busy != pw_held) pw_held = busy, oriole_stay_awake(pw_held);
  uint8_t w = oriole_woke();
  if (pw_cmd_wake) pw_cmd_wake = false, w = w ? w : (pw_mode == PW_AOD || pw_mode == PW_DARK) ? ORIOLE_WAKE_TOUCH : 0;
  const bool boot = oriole_boot_click() || (digitalRead(0) == LOW && pw_mode == PW_FADING);  // talking cancels a fade
  switch (pw_mode) {
    case PW_ACTIVE:
      if (pw_cmd_sleep || (pw_idle_s && now - pw_active_us > pw_idle_s * 1000000ll)) pw_fade(pw_aod_on);
      break;
    case PW_FADING:
      // Touched, BOOT or picked up while fading out — the screen stays
      if (f.down || boot || busy || moved) {
        pw_mode = PW_ACTIVE, pw_keep_lit = false, dp_power.target = 1, pw_active_us = now;
      } else if (dp_power.x < 0.01f) {
        if (pw_to_aod) {
          pw_mode = PW_AOD, pw_k = 0, pw_bright = 0;
          face_still(true);
          oriole_damage_all();
        } else {
          pw_mode = PW_DARK;  // display.h has turned the panel off at zero
          oriole_sleep(timer_ms_left() >= 0 ? timer_ms_left() + 20 : 0);  // asleep for good, unless a timer runs
        }
      }
      break;
    case PW_AOD:
      if (w && w != ORIOLE_WAKE_TIMER) {
        pw_wake(w);
      } else {
        if (w == ORIOLE_WAKE_TIMER) fc_check = true;  // a new minute: face_tick sets the digits
        if (pw_k < 1) {  // Always On comes up in 0.4 s, then sleep until the next minute
          pw_k = fminf(1, pw_k + dt / 0.4f), pw_bright = (int)lroundf((fc_night_on ? 18 : PW_AOD_BRIGHT) * pw_k);
        } else if (!oriole_asleep()) {
          const int32_t t = timer_ms_left();  // a timer rings before the minute turns: wake for it
          oriole_sleep(t >= 0 && (uint32_t)t + 20 < pw_ms_to_minute() ? t + 20 : pw_ms_to_minute());
        }
      }
      break;
    case PW_DARK:
      if (w) pw_wake(w);
      break;
  }
  pw_cmd_sleep = false;  // sent while already asleep — it must not put the board back to sleep right after it wakes
}

// Always On: the face's date and clock, dim, a few pixels elsewhere every minute
static void power_layout() {
  static const int8_t SX[8] = {0, 5, 7, 5, 0, -5, -7, -5}, SY[8] = {-7, -5, 0, 5, 7, 5, 0, -5};
  const int i = fc_min >= 0 ? fc_min & 7 : 0;
  const OrioleView v;
  const int st = face_style();  // the face's style (face.h): dim, no seconds, no rings
  const float dy = fc_dy.target, x = CX + SX[i];
  const bool an = face_analog(st);
  const float date_y = an ? 0 : st == FS_STACKED ? fc_st_date_y : FACE_DATE_Y + dy;
  oriole_roll_layout(r_date, x, date_y + SY[i], fnc(PW_DATE), an ? 0 : fc_date_a.target, &oriole_black, v);
  oriole_roll_layout(r_time, x, FACE_TIME_Y + dy + SY[i], fnc(PW_TIME), an || st == FS_STACKED ? 0 : 1,
                   &oriole_black, v);
  const float sa = st == FS_STACKED ? 1 : 0;
  oriole_roll_layout(r_hh, x, fc_st_hh_y + SY[i], fnc(PW_TIME), sa, &oriole_black, v);
  oriole_roll_layout(r_mm, x, fc_st_mm_y + SY[i], fnc(PW_DATE), sa, &oriole_black, v);
  face_hands(x, CX + SY[i], PW_TIME, false, an ? 1 : 0, v);
}

static void power_draw() {
  oriole_roll_draw(r_date), oriole_roll_draw(r_time), oriole_roll_draw(r_hh), oriole_roll_draw(r_mm);
  for (auto &h : fc_hand) oriole_item_draw(h);
  oriole_item_draw(fc_cap[0]), oriole_item_draw(fc_cap[1]);
}

static void power_build() {  // after oriole_begin: the motion sensor is on its I2C bus
  oriole_motion_begin(150);
  pw_active_us = esp_timer_get_time();
  oriole_command("aod", [](const char *a) { pw_aod_on = strcmp(a, "off") != 0, LOG("aod: %s\n", pw_aod_on ? "on" : "off"); });
  oriole_command("idle", [](const char *a) { pw_idle_s = max(0, atoi(a)), LOG("idle: %d s\n", pw_idle_s); });
  oriole_command("sleep", [](const char *) { pw_cmd_sleep = true; });
  oriole_command("wake", [](const char *) { pw_cmd_wake = true; });
  oriole_command("usb", [](const char *a) { oriole_sleep_on_usb(strcmp(a, "off") != 0); });
}
