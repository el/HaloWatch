// Oriole UI layer: text whose changed letters roll (Oriole.h, OrioleRoll).
//
// Every letter position is a cell holding the letter now and the one leaving. A change runs one spring t from 0 to 1
// with a slight overshoot: the old letter moves up by `rise` and fades out during the first half of it, the new one
// comes up from `rise` below and fades in over the first 60 %; the overshoot lifts it a pixel above its place
// before it settles. When several letters change, they start one after another from the right (an odometer). Cells
// are matched by position; a letter of another width moves its neighbours to their new places with a second spring,
// and the line width follows the same spring (a pill under the text grows with it).
//
// Each letter is its own OrioleItem (a glyph mask), so only the letters that move are redrawn and sent to the display.
#include "oriole_internal.h"

#pragma GCC optimize("O3", "fast-math")

static OrioleSpringK K_T, K_X;
static bool k_ready;

static void make_springs() {
  if (k_ready) return;
  K_T = oriole_spring(0.55f, 0.78f);       // the change itself: settles in about 0.3 s, ~2 % overshoot
  K_X = oriole_spring(ORIOLE_M3X_DEFAULT);   // neighbours moving aside: Material 3 expressive
  k_ready = true;
}

// The width a letter takes: its advance, or the digit cell for digits of a tabular line
static float advance(const OrioleRoll &r, uint32_t code) {
  const OrioleMask *g = code ? oriole_glyph(*r.font, code) : nullptr;
  if (!g) return 0;
  return r.tabular && code >= '0' && code <= '9' ? r.cell : g->adv;
}

// Where the letter's mask starts from its pen: by its own offset, or in the middle of its cell (tabular)
static float ink_from_pen(const OrioleRoll &r, uint32_t code, const OrioleMask &g) {
  return r.tabular ? (advance(r, code) - g.w) / 2.0f : g.ox;
}

// Two colors mixed: k = 0 -> a, 1 -> b
static uint32_t mix_rgb(uint32_t a, uint32_t b, float k) {
  const int K = (int)(oriole_clamp01(k) * 256 + 0.5f);
  uint32_t out = 0;
  for (int sh = 0; sh <= 16; sh += 8) {
    const int ca = a >> sh & 255, cb = b >> sh & 255;
    out |= (uint32_t)(ca + (((cb - ca) * K) >> 8)) << sh;
  }
  return out;
}

void oriole_roll_init(OrioleRoll &r, const OrioleFont &f, const char *mid_ref, int align, int spacing) {
  make_springs();
  r.font = &f, r.align = align, r.spacing = spacing, r.n = 0;
  r.mid = oriole_text_mid(f, mid_ref);
  r.cell = 0;
  for (uint32_t d = '0'; d <= '9'; d++) {
    const OrioleMask *g = oriole_glyph(f, d);
    if (g) r.cell = fmaxf(r.cell, g->adv);
  }
  if (r.rise <= 0) {
    const OrioleMask *g = oriole_glyph(f, '0');
    r.rise = 0.4f * (g ? g->h : f.ascent);
  }
  oriole_spring_set(r.w, 0);
  for (auto &c : r.c) {
    c.code = c.old = 0, c.wait = 0;
    oriole_spring_set(c.x, 0), oriole_spring_set(c.t, 1);
  }
}

void oriole_roll_set(OrioleRoll &r, const char *s, bool animate) {
  if (!r.font) return;
  uint32_t code[ORIOLE_ROLL_MAX];
  int n = 0;
  while (*s && n < ORIOLE_ROLL_MAX) code[n++] = oriole_utf8_next(s);
  // Where each letter of the new text starts (from the anchor, by align) and how wide the text is
  float pen[ORIOLE_ROLL_MAX], w = 0;
  for (int i = 0; i < n; i++) pen[i] = w, w += advance(r, code[i]) + r.spacing;
  if (n) w -= r.spacing;
  const float left = -w * r.align / 2;
  const int m = max(n, r.n);
  // The cascade lasts at most 0.3 s: in a long line (a date) its neighbours already move to their new places, and the
  // letters still waiting their turn would drift apart for too long ("T HU")
  int changed = 0;
  for (int i = 0; i < m; i++) changed += (i < n ? code[i] : 0) != r.c[i].code;
  const float stagger = changed > 1 ? fminf(r.stagger, 0.3f / (changed - 1)) : 0;
  int order = 0;
  for (int i = m - 1; i >= 0; i--) {  // from the right: the cascade starts at the last letter
    OrioleRollCell &c = r.c[i];
    const uint32_t now = i < n ? code[i] : 0;
    if (i < n) {
      c.x.target = left + pen[i];
      const bool fresh = c.code == 0 && (c.old == 0 || c.t.x >= 1);  // an empty cell: appears in place, no slide
      if (fresh || !animate) oriole_spring_set(c.x, c.x.target);
    }
    if (now == c.code) continue;
    if (!animate) {
      c.code = now, c.old = 0, c.wait = 0;
      oriole_spring_set(c.t, 1);
      continue;
    }
    c.old = c.code, c.code = now, c.wait = order++ * stagger;
    oriole_spring_set(c.t, 0);
  }
  r.n = m;  // letters that went away stay until they have rolled out
  r.w.target = w;
  if (!animate) oriole_spring_set(r.w, w);
}

void oriole_roll_update(OrioleRoll &r, float dt) {
  oriole_spring_run(r.w, K_X, dt);
  int last = 0;
  for (int i = 0; i < r.n; i++) {
    OrioleRollCell &c = r.c[i];
    oriole_spring_run(c.x, K_X, dt);
    if (c.wait > 0) c.wait -= dt;  // still waiting for its turn in the cascade
    else c.t.target = 1;
    oriole_spring_run(c.t, K_T, dt);
    if (c.t.x == c.t.target && c.t.target == 1 && c.t.v == 0) c.old = 0;  // the old letter is gone
    if (c.code || c.old) last = i + 1;
  }
  r.n = last;
}

bool oriole_roll_ink(const OrioleRoll &r, float &x0, float &x1, int from, int to) {
  x0 = 1e9f, x1 = -1e9f;
  if (!r.font) return false;
  for (int i = max(from, 0); i < min(to, r.n); i++) {
    const OrioleRollCell &c = r.c[i];
    const OrioleMask *g = c.code ? oriole_glyph(*r.font, c.code) : nullptr;
    if (!g) continue;
    const float a = c.x.target + ink_from_pen(r, c.code, *g);
    x0 = fminf(x0, a), x1 = fmaxf(x1, a + g->w);
  }
  return x0 <= x1;
}

bool oriole_roll_busy(const OrioleRoll &r) {
  if (fabsf(r.w.x - r.w.target) > 0.05f) return true;
  for (int i = 0; i < r.n; i++)
    if (r.c[i].old || r.c[i].t.x != 1 || fabsf(r.c[i].x.x - r.c[i].x.target) > 0.05f) return true;
  return false;
}

// One letter as a glyph mask: pen_x — its pen position, top — the line top; k — how much of it is shown (0..1).
// blend — the new letter lies over the old one right now: it is blended over what is drawn (slower, but only while they
// change). The quick ways (ready edge colors over the background, a dimmed color on black) paint each pixel as if only
// the background were under it — the new letter's edges would cut a dark notch into the old one. The old letter is
// drawn first, straight onto the background, so it keeps the quick way
static void put(OrioleItem &it, const OrioleRoll &r, uint32_t code, float pen_x, float top, float k, uint32_t color,
                float alpha, const OrioleItem *on, bool blend, const OrioleView &v) {
  const OrioleMask *g = code ? oriole_glyph(*r.font, code) : nullptr;
  it.kind = HI_MASK, it.mask = g, it.scale = 1, it.on = blend ? nullptr : on, it.on_black = !blend;
  if (!g || k <= 0.004f || alpha <= 0) {
    it.alpha = 0;
    oriole_item_commit(it);
    return;
  }
  it.x = pen_x + ink_from_pen(r, code, *g) + g->w / 2.0f, it.y = top + g->oy + g->h / 2.0f;
  // On a solid pill a letter fades to the pill's color (not to black): its color is mixed instead of its alpha, so
  // the cheap "on black" drawing still holds when the whole screen fades
  if (on && on != &oriole_black && !blend) it.color = mix_rgb(on->color, color, k), it.alpha = alpha;
  else it.color = color, it.alpha = alpha * k;
  oriole_view_commit(it, v);
}

void oriole_roll_layout(OrioleRoll &r, float x, float y, uint32_t color, float alpha, const OrioleItem *on, const OrioleView &v) {
  if (!r.font) return;
  const float top = y - r.mid;
  const bool busy = oriole_roll_busy(r);
  if (busy && v.s >= 0.999f) oriole_subpixel = true;  // letters move by fractions of a pixel: no 1 px steps
  for (int i = 0; i < ORIOLE_ROLL_MAX; i++) {
    OrioleRollCell &c = r.c[i];
    const bool live = i < r.n;
    const float t = c.t.x, px = x + c.x.x;
    const float k_in = oriole_clamp01(t * 1.6f), k_out = oriole_clamp01(1 - t * 2);  // the old one is gone halfway
    const bool blend = live && c.code && c.old && k_in > 0.004f && k_out > 0.004f;
    put(c.in, r, live ? c.code : 0, px, top + (1 - t) * r.rise, k_in, color, alpha, on, blend, v);
    put(c.out, r, live ? c.old : 0, px, top - t * r.rise, k_out, color, alpha, on, false, v);
  }
}

void oriole_roll_draw(const OrioleRoll &r) {
  for (int i = 0; i < ORIOLE_ROLL_MAX; i++) oriole_item_draw(r.c[i].out), oriole_item_draw(r.c[i].in);
}
