// HaloWatch — a watch with a voice assistant, on its own UI library Halo.
//
// Hold BOOT and talk, let go and the answer is spoken: Gemini Live over Wi-Fi (gemini.h — the engine, assistant.h —
// the screen and push-to-talk). Everything is set up from a web page over USB — Wi-Fi, the API key, the voice and
// the prompt, the weather cities, the clock — with a live log (web/controller.html in Chrome; the serial protocol —
// config.h).
//
// The home is the watch face (face.h): the date, a big clock (from the internet: net.h) and the weather pill (real
// weather from Open-Meteo: weather.h); a tap on the pill — the Weather screen right over the face (Fade; back — swipe
// up from the bottom edge). Swipe up on the face — the gallery (Fade), swipe down from its top edge — back to the face
// (halo_nav_face); from any screen of the gallery — its own gesture to the face. Back on the face — next time the
// gallery starts from the top.
//
// The gallery — the WearKit carousel one to one: a Wear OS card list (TransformingLazyColumn: cards shrink and fade
// near the circle's edges, inertia, rubber past the ends), the time along the top arc (scrolls away), a scroll
// indicator on the right (fades after 2 s), a tap on a card — the Android 16 transition, back — swipe from the left
// edge following the finger. Three cards: "Assistant", "Weather" (weather.h), "Settings" (settings.h: "Display" —
// brightness, display.h; "Battery" — battery.h). PWR locks the touch screen (display.h), a long press unlocks.
// Left alone, the watch dims to Always On and sleeps (power.h).
//
// What comes from where (lib/Halo, docs/halo.md): the list — halo_list_*, transitions — halo_nav_*, arcs — HI_ARC, the
// time along the arc — halo_text_arc, sound and the microphones — halo_audio_*, halo_mic_*. Only what changed goes to
// the display: at rest no frames are drawn at all.
// Commands: face / face off (the face / the gallery), open N (0 — Assistant, 1 — Weather, 2 — Settings), back, tap/press/
// swipe/drag/fling (an artificial finger); the controller's: cfg, set, status, time, wifi, weather, ai, ask, mic test,
// reboot; instruments — trace, rec, shot, prof, sys, log.
#pragma GCC optimize("O3", "fast-math")
#include <Halo.h>
#include "src/halo_assets.h"
#include "config.h"  // settings in flash; the web controller's serial protocol
#include "net.h"     // Wi-Fi on demand, the clock from the internet, HTTPS (before the short macros below: W, CX, DP)

#define W 466
#define CX 233.0f
#define DP 2.0f  // 1 dp Wear OS ≈ 2 пикселя этого экрана
// Цвета — как в AmoledWear
#define TEXT 0xFFF8EE     // тёплый белый
#define DIM 0x9E9E9E      // второстепенное
#define CARD_HI 0x2E3034  // кнопка (surfaceContainerHigh)

static HaloSpringK K_M3_DEFAULT, K_PRESS, K_RELEASE, K_SC_VIS, K_PAGER;
struct Press {  // нажатие кнопки (ниже — press_update)
  HaloSpring s;
  bool going_down;
};

// Текст по середине букв (mid — halo_text_mid) в (x, y): так ставит текст WearKit (text_center). on — на чём лежит
// (сплошная карточка, круг или &halo_black): края букв тогда — готовыми цветами, без смешивания
static void text_mid(HaloItem &it, const HaloFont &f, const char *s, float mid, float x, float y, float sc,
                     uint32_t color, float alpha, const HaloItem *on) {
  it.kind = HI_TEXT, it.font = &f, it.text = s, it.align = 1, it.color = color, it.on_black = false, it.on = on;
  it.x = x, it.y = y + (f.line / 2.0f - mid) * sc, it.scale = sc, it.alpha = alpha;
}
// Текст от левого края x, базовая линия — на y
static void text_base(HaloItem &it, const HaloFont &f, const char *s, float x, float y, float sc, uint32_t color,
                      float alpha, const HaloItem *on) {
  it.kind = HI_TEXT, it.font = &f, it.text = s, it.align = 0, it.color = color, it.on_black = false, it.on = on;
  it.x = x, it.y = y + (f.line / 2.0f - f.ascent) * sc, it.scale = sc, it.alpha = alpha;
}

// ---------- Нажатие (как у кнопок Wear OS: RoundButton.kt) ----------
// 0 — отпущена, 1 — нажата. Нажатие — очень быстрая пружина (fast × 9), отпускание — медленная с отскоком. Даже
// короткий тап доигрывает нажатие хотя бы до 0,75 — и только потом отпускает: нажатие всегда видно
static void press_update(Press &p, bool held, float dt, const HaloSpringK &k_down = K_PRESS) {
  if (held) p.going_down = true;
  else if (p.going_down && p.s.x >= 0.75f) p.going_down = false;
  p.s.target = p.going_down ? 1 : 0;
  halo_spring_run(p.s, p.going_down ? k_down : K_RELEASE, dt);
}

// ---------- Карточки ----------
struct Entry {
  const char *title, *sub, *icon;  // значок — Font Awesome (шрифт ic24)
  uint32_t color;
};
static const Entry ENTRIES[] = {
    {"Assistant", "hold BOOT to talk", "\uf130", 0x8AB4F8},
    {"Weather", "now and hourly", "\uf185", 0xFFB030},
    {"Settings", "display, battery", "\uf013", 0xA8C7FA},
};
#define N_ENTRIES (int)(sizeof(ENTRIES) / sizeof(ENTRIES[0]))

// ---------- Меню: лента карточек ----------
#define M_ITEM_H 104.0f  // кнопка с подписью: 52 dp
#define M_ITEM_W 410.0f
#define M_HEAD_H 64.0f
#define TT_R 219.0f      // радиус середины букв времени сверху (отступ от края 2 dp)
static HaloList menu;
static Press m_press[N_ENTRIES];
static int m_armed = -1, open_req = -1, cur;  // cur — открытый пункт
static volatile int cmd_open = -1;
static float tt_k = 1;             // время сверху: 1 — на месте, 0 — уехало (ScrollAway)
static float ui_lock;              // блокировка (display.h): 0..1 — время сверху гаснет за первую половину, потом замок
static int pw_bright = -1;         // Always On (power.h): яркость панели в нём (−1 — своя у display.h)
static bool pw_keep_lit;           // гаснем в Always On или уже в нём: на нуле яркости панель не выключать
static HaloSpring sc_pos;          // индикатор прокрутки: положение (догоняет за 500 мс)
static HaloSpring sc_vis;          // и видимость: виден, пока листают, гаснет через 2 с покоя (пружина k=400)
static uint32_t sc_idle_ms;
static HaloMask tt_mask;           // время сверху, «запечённое» по дуге в маску (раз в минуту)
static float tt_x, tt_y, mid_head, mid_icon, mid_time;
static char time_str[8] = "--:--";
static int clock_h = -1, clock_m = -1;
static HaloItem m_head, m_time, m_sc[3];
static HaloItem m_bg[N_ENTRIES], m_disc[N_ENTRIES], m_icon[N_ENTRIES], m_title[N_ENTRIES], m_sub[N_ENTRIES];

// Пришли на циферблат (halo_nav_face зовёт enter главного): в следующий раз галерея — с начала, как при запуске
static void menu_enter() {
  halo_list_reset(menu);
  m_armed = -1, tt_k = 1;
  halo_spring_set(sc_pos, 0), halo_spring_set(sc_vis, 0);
  for (auto &p : m_press) halo_spring_set(p.s, 0), p.going_down = false;
}

static void menu_update(float dt) {
  HaloFinger &f = halo_finger;
  halo_list_update(menu, dt);
  // Нажатие на карточку — анимация нажатия, отпустил — открыть
  if (f.pressed) {
    const int i = halo_list_hit(menu, f.y0);
    m_armed = i >= 1 ? i - 1 : -1;
    if (m_armed >= 0 && fabsf(f.x0 - CX) > M_ITEM_W / 2 * menu.fx[i].s) m_armed = -1;
  }
  if (f.dragging) m_armed = -1;
  if (f.tap && m_armed >= 0) open_req = m_armed;
  for (int i = 0; i < N_ENTRIES; i++) press_update(m_press[i], i == m_armed && f.down, dt);
  if (!f.down) m_armed = -1;
  // Время сверху уезжает и уменьшается за первые 36 dp прокрутки (ScrollAway: сдвиг 24 dp, до 0,5)
  tt_k = 1 - halo_clamp01((menu.top - menu.scroll) / (36 * DP));
  sc_pos.target = halo_list_progress(menu);
  halo_spring_run(sc_pos, K_M3_DEFAULT, dt);
  if (menu.moving || f.owner == &menu) sc_idle_ms = halo_ms();
  sc_vis.target = halo_ms() - sc_idle_ms < 2000;
  halo_spring_run(sc_vis, K_SC_VIS, dt);
}

static void menu_layout(const HaloView &v) {
  const HaloListFx &h = menu.fx[0];
  if (menu.moving && v.a > 0 && v.s >= 0.999f) halo_subpixel = true;  // лента едет — буквы с долей пикселя, иначе дрожат
  text_mid(m_head, t_title, "Gallery", mid_head, CX, h.top + M_HEAD_H / 2, 1, TEXT, h.a_bg, &halo_black);
  m_head.on_black = true;
  halo_view_commit(m_head, v);
  for (int k = 0; k < N_ENTRIES; k++) {
    const HaloListFx &f = menu.fx[k + 1];
    const Entry &d = ENTRIES[k];
    const bool vis = halo_list_item_visible(menu, k + 1);
    const float s = f.s, h = M_ITEM_H * s, w = M_ITEM_W * s, x0 = CX - w / 2, y0 = f.top;
    const float p = halo_clamp01(m_press[k].s.x), ac = vis ? f.a_c : 0, ix = x0 + 56 * s, iy = y0 + h / 2;
    // Фон: нажатая кнопка — меньше скругление (форма «сжимается»: 26 dp → 18 dp)
    HaloItem &bg = m_bg[k];
    bg.kind = HI_RRECT, bg.x = CX, bg.y = y0 + h / 2, bg.w = M_ITEM_W, bg.h = M_ITEM_H, bg.scale = s;
    bg.r = 52 + (36 - 52) * p, bg.color = CARD_HI, bg.alpha = vis ? f.a_bg : 0, bg.on_black = true;
    // Круг значка — непрозрачный поверх карточки, значок и текст — с прозрачностью содержимого
    HaloItem &dc = m_disc[k];
    dc.kind = HI_CIRCLE, dc.x = ix, dc.y = iy, dc.r = 32, dc.scale = s, dc.color = d.color, dc.alpha = ac;
    dc.on_black = true;
    text_mid(m_icon[k], ic24, d.icon, mid_icon, ix, iy, s, 0x1B1B1F, ac, &dc);
    text_base(m_title[k], t24, d.title, x0 + 104 * s, y0 + 46 * s, s, TEXT, ac, &bg);
    text_base(m_sub[k], t20, d.sub, x0 + 104 * s, y0 + 80 * s, s, DIM, ac, &bg);
    halo_view_commit(bg, v), halo_view_commit(dc, v), halo_view_commit(m_icon[k], v);
    halo_view_commit(m_title[k], v), halo_view_commit(m_sub[k], v);
  }
  // Время по дуге сверху: при прокрутке уезжает вверх на 24 dp, уменьшается и гаснет до 0,5 — и пропадает
  // (маска, запечённая раз в минуту; масштаб — от верхней середины)
  const float k = 0.5f + 0.5f * tt_k, top = tt_y - 24 * DP * (1 - tt_k), left = CX - (CX - tt_x) * k;
  m_time.kind = HI_MASK, m_time.mask = &tt_mask, m_time.color = 0xC6C6C6, m_time.on_black = true, m_time.on = &halo_black;
  m_time.x = left + tt_mask.w * k / 2, m_time.y = top + tt_mask.h * k / 2, m_time.scale = k;
  m_time.alpha = tt_k > 0.02f && tt_mask.bits ? k * (1 - halo_clamp01(ui_lock * 2)) : 0;
  halo_view_commit(m_time, v);
  // Индикатор прокрутки справа (ScrollIndicator): дорожка 50 dp по дуге, ползунок 0,3..0,7 её длины, зазоры 3 dp,
  // толщина 6 dp, круглые концы
  const float r = 233 - 2 * DP - 3 * DP, len = 50 * DP / r, gap = (6 + 3) * DP / r;
  const float a0 = HALO_PI / 2 - len / 2, size = len * constrain(halo_list_visible(menu), 0.3f, 0.7f);
  const float t0 = a0 + (len - size) * halo_clamp01(sc_pos.x), t1 = t0 + size, a = sc_vis.x > 0.01f ? sc_vis.x : 0;
  const float from[3] = {a0, t1 + gap, t0}, to[3] = {t0 - gap, a0 + len, t1};  // дорожка до, дорожка после, ползунок
  for (int i = 0; i < 3; i++) {
    HaloItem &it = m_sc[i];
    it.kind = HI_ARC, it.x = CX, it.y = CX, it.r = r, it.w = 6 * DP, it.scale = 1, it.a0 = from[i], it.a1 = to[i];
    it.color = i == 2 ? 0xC6C6C6 : 0x333333, it.on_black = true, it.alpha = from[i] < to[i] ? a : 0;
    halo_view_commit(it, v);
  }
}

static void menu_draw() {
  halo_item_draw(m_head);
  for (int k = 0; k < N_ENTRIES; k++) {
    halo_item_draw(m_bg[k]), halo_item_draw(m_disc[k]), halo_item_draw(m_icon[k]);
    halo_item_draw(m_title[k]), halo_item_draw(m_sub[k]);
  }
  halo_item_draw(m_time);
  for (auto &it : m_sc) halo_item_draw(it);
}

// Время сверху: сменилась минута — запечь по дуге заново
static void clock_tick() {
  static uint32_t checked;
  if (halo_ms() - checked < 250 || !halo_clock_valid()) return;
  checked = halo_ms();
  const time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  static bool c24 = cfg.clock24;
  if (t.tm_min == clock_m && t.tm_hour == clock_h && c24 == cfg.clock24) return;
  c24 = cfg.clock24;
  clock_h = t.tm_hour, clock_m = t.tm_min;
  snprintf(time_str, sizeof(time_str), "%d:%02d", cfg.clock24 ? clock_h : clock_h % 12 ? clock_h % 12 : 12, clock_m);
  halo_text_arc(tt_mask, t24, time_str, mid_time, TT_R, 0, false, 1, &tt_x, &tt_y);
}

static void menu_build() {
  float heights[N_ENTRIES + 1];
  heights[0] = M_HEAD_H;
  for (int i = 0; i < N_ENTRIES; i++) heights[i + 1] = M_ITEM_H;
  // Низ последней карточки в конце ленты — 333 (так было и с кнопкой у края под ней)
  halo_list_init(menu, heights, N_ENTRIES + 1, 64, 333, true);
  mid_head = halo_text_mid(t_title, "Gallery");
  mid_icon = halo_text_mid(ic24, ENTRIES[1].icon);  // все значки — по одной середине (как в WearKit)
  mid_time = halo_text_mid(t24, "0123456789");
  halo_text_arc(tt_mask, t24, time_str, mid_time, TT_R, 0, false, 1, &tt_x, &tt_y);
}

#include "weather.h"    // экран «Weather» (погода по макету design/weather_mockup.webp), настоящая — от Open-Meteo
#include "face.h"       // циферблат (по макету design/face_mockup.webp)
#include "display.h"    // экран Display: яркость, PWR — блокировка
#include "battery.h"    // экран Battery: уровень, заряд, график за сутки; история — во флеше
#include "gemini.h"     // the assistant's engine: Gemini Live over a WebSocket
#include "power.h"      // Always On и сон: 15 с без касаний и движения — тусклые часы, плата спит до следующей минуты
#include "assistant.h"  // the Assistant screen, push-to-talk on BOOT
#include "settings.h"   // настройки: разделы (Display, Battery) и хранение во флеше
#include "status.h"     // the controller's `status`; reactions to changed settings

// ---------- Кадр ----------
// Жест на циферблат — под то, как экран движется: листалка с точками (Weather) — вверх от нижнего края, как
// назад с погоды поверх циферблата; Settings — оба, но только от самого края (вниз — от верхних 90 px, вверх — от
// нижних 70), остальное — прокрутка списка; Assistant — оба; сама галерея — только вниз от верхнего края
static const HaloScreen MENU = {menu_enter, menu_update, menu_layout, menu_draw};
static const HaloScreen FACE = {nullptr, face_update, face_layout, face_draw};

static void update(float dt) {
  halo_finger_update();  // палец — раз в кадр, в начале
  power_tick(dt);        // Always On и сон — до циферблата: в Always On цифры сменяются без прокрутки
  assistant_tick(dt);    // BOOT held — talking (after power_tick: BOOT wakes the board there first)
  clock_tick();
  face_tick(dt);  // время, дата и анимации циферблата — в каждом кадре, даже когда он закрыт галереей
  display_tick(dt);     // яркость (плавно) и блокировка — где бы ни были
  if (pw_mode == PW_AOD) return power_layout();  // Always On: только тусклые дата и часы
  if (cmd_open >= 0) open_req = cmd_open, cmd_open = -1;
  if (open_req >= 0 && halo_nav_at_home()) {  // открыть: переход Android 16
    cur = open_req;
    halo_nav_open(cur == 0 ? ASSISTANT : cur == 1 ? WEATHER : SETTINGS);
  }
  open_req = -1;
  halo_nav_update(dt);  // свайпы к циферблату и назад, переходы, update и раскладка экранов
  display_overlay_layout();    // подсказка блокировки — поверх всего
  assistant_overlay_layout();  // the assistant's dot, on the other screens
}

static void draw() {  // оба ядра, каждое — свою часть пачки строк
  if (pw_mode == PW_AOD) return power_draw();
  halo_nav_draw();
  display_overlay_draw();
  assistant_overlay_draw();
}

void setup() {
  setenv("TZ", "UTC0", 1), tzset();  // until the time zone is known (net.h)
  cfg_load();  // the controller's settings — from flash, before anything uses them
  K_M3_DEFAULT = halo_spring(HALO_M3_DEFAULT);
  K_PRESS = halo_spring(0.222f / 3, 0.70f);  // нажатие: в 3 раза быстрее expressive fast (жёсткость × 9)
  K_RELEASE = halo_spring(HALO_M3X_SLOW);    // отпускание — медленно, с лёгким отскоком
  K_SC_VIS = halo_spring(halo_stiff(400), 1);
  K_PAGER = halo_spring(HALO_M3_PAGER);
  menu_build();
  weather_build();
  settings_load();  // яркость — из флеша
  face_build();
  display_build();
  battery_load();   // история батареи — из флеша
  battery_build();
  assistant_build();
  settings_build();
  halo_nav_begin(MENU);  // главный — меню; команда back — назад
  halo_nav_face(FACE);   // под ним — циферблат: с него запуск
  halo_command("open", [](const char *a) { cmd_open = constrain(atoi(a), 0, N_ENTRIES - 1); });
  HaloConfig cfg;
  cfg.name = "HaloWatch";
  cfg.brightness = dp_start_level();  // сохранённая яркость (или самая низкая, если заблокировано) — с первого кадра
  halo_begin(update, draw, cfg);
  power_build();         // датчик движения (будит, когда плату берут в руки) и команды aod, idle, sleep
  config_commands();     // cfg, set, reboot
  net_begin();           // Wi-Fi (on now: the clock and the weather), commands time, wifi
  weather_net_begin();   // Open-Meteo, every wx_every minutes
  ai_begin();            // the assistant: speaker and mic, the session task; commands ai, ask, mic
  status_begin();
  Serial.println("setup done");
}

void loop() {
  halo_loop();
  settings_save_poll();  // яркость — во флеш (не из задачи кадров)
  battery_poll();        // уровень батареи раз в 5 с, история — во флеш
}
