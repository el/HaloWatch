// ancs.h — iPhone notifications over Bluetooth LE: Apple's Notification Center Service (ANCS), no app on the phone.
//
// The watch advertises as "Oriole" and asks for ANCS (a service solicitation in the advertisement), so it shows up in
// the iPhone's Settings -> Bluetooth. Pair once there (Just Works; the bond is kept in flash by NimBLE) and the iPhone
// reconnects by itself. On every connection: encryption, then the ANCS service and its three characteristics —
// Notification Source (an 8-byte event per notification: added / modified / removed, its category and UID), Control
// Point (we ask for a notification's attributes) and Data Source (the answer, possibly split over several packets) —
// both sources subscribed. For each new notification: the app, the title and the message, one request at a time.
// Kept: the last NT_N, newest first (the Notifications screen, apps.h); removed on the phone — removed here. One that
// arrives while connected (not one already on the phone) dings, wakes the screen and slides down as a banner.
//
// NimBLE runs in its own task (core 0); everything it touches here is copied under nt_mux.
// Memory: Bluetooth takes 72 KB of internal RAM and Wi-Fi about as much — together they do not fit (the watch ran
// out). So they take turns, in the net task (bt_radio): Wi-Fi comes on only for a while (the clock, the weather, the
// assistant), and before it does Bluetooth stops and gives its memory back; once Wi-Fi is off, Bluetooth starts again
// and the iPhone reconnects by itself. On every connection the iPhone sends all its notifications again: the list is
// rebuilt from them, and one dated after Bluetooth stopped still gets its banner. This core's Bluetooth controller
// cannot sleep: while Bluetooth is on the board stays up (halo_stay_awake), as on USB. Settings -> Bluetooth turns it
// off (the bt_on setting) and forgets the iPhone.
// Included from Oriole.ino after power.h.
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

extern "C" void ble_store_config_init(void);  // NimBLE's bond store in NVS (no header declares it)

extern "C" bool btInUse() {  // keep the core from freeing the Bluetooth controller's memory at boot
  return true;
}

#define NT_N 20
#define NT_TITLE 64
#define NT_MSG 256
struct Notif {
  uint32_t uid, epoch;
  uint8_t cat;  // ANCS category: 1 incoming call, 4 social, 6 email, ...
  char app[24], title[NT_TITLE], msg[NT_MSG];
};
struct NotifStore {
  int count, head;  // a ring, newest at head − 1
  Notif e[NT_N];
};
static NotifStore &nt = psram_new<NotifStore>();
static portMUX_TYPE nt_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t nt_ver;       // bumped on every change: the screen takes a new copy
static volatile uint32_t nt_new_ver;   // bumped by a notification worth a banner
static volatile int nt_unread;         // such ones since the Notifications screen was opened (the face's dot)
static Notif &nt_banner = psram_new<Notif>();  // that notification (under nt_mux)

enum : uint8_t { BT_OFF, BT_ADV, BT_CONN, BT_SECURE, BT_READY, BT_NO_ANCS, BT_PAUSED };
static const char *const BT_NAME[] = {"off", "advertising", "connected", "encrypted", "ready", "no ANCS", "paused"};
static char bt_down[16];          // when Bluetooth last stopped, as ANCS dates are ("20261126T213245", local time)
static volatile bool bt_forget_req;
static volatile uint8_t bt_state = BT_OFF;
static volatile bool bt_inited;
static bool bt_held;
static uint16_t bt_conn = BLE_HS_CONN_HANDLE_NONE;
static uint8_t bt_addr_type;

static const ble_uuid128_t ANCS_SVC =
    BLE_UUID128_INIT(0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F, 0xA4, 0x99, 0x4E, 0xCE, 0xB5, 0x31, 0xF4, 0x05, 0x79);
static const ble_uuid128_t ANCS_NS =
    BLE_UUID128_INIT(0xBD, 0x1D, 0xA2, 0x99, 0xE6, 0x25, 0x58, 0x8C, 0xD9, 0x42, 0x01, 0x63, 0x0D, 0x12, 0xBF, 0x9F);
static const ble_uuid128_t ANCS_CP =
    BLE_UUID128_INIT(0xD9, 0xD9, 0xAA, 0xFD, 0xBD, 0x9B, 0x21, 0x98, 0xA8, 0x49, 0xE1, 0x45, 0xF3, 0xD8, 0xD1, 0x69);
static const ble_uuid128_t ANCS_DS =
    BLE_UUID128_INIT(0xFB, 0x7B, 0x7C, 0xCE, 0x6A, 0xB3, 0x44, 0xBE, 0xB5, 0x4B, 0xD6, 0x24, 0xE9, 0xC6, 0xEA, 0x22);
static uint16_t an_start, an_end, an_ns, an_cp, an_ds, an_ns_cccd, an_ds_cccd;

// ---------- Fetching attributes: one request at a time, from a small queue ----------
struct NtReq {
  uint32_t uid;
  uint8_t cat;
  bool pre;    // already on the phone when we connected (a banner only if it came after Bluetooth stopped)
  bool quiet;  // modified: no banner
};
static NtReq an_q[24];
static volatile int an_qh, an_qt;          // ring
static volatile bool an_busy;              // a request is out
static volatile uint32_t an_busy_at;
static NtReq an_cur;
static uint8_t an_buf[NT_TITLE + NT_MSG + 64];
static int an_len;

static void an_next();
static void am_begin();  // AMS (below), once ANCS is set up
static void cts_begin();  // CTS (below), once AMS is

static const char *nt_app_name(const char *id, char *out, size_t n) {  // a bundle ID -> a short name
  static const char *const MAP[][2] = {
      {"com.apple.MobileSMS", "Messages"}, {"com.apple.mobilephone", "Phone"},  {"com.apple.mobilemail", "Mail"},
      {"com.apple.mobilecal", "Calendar"}, {"com.apple.reminders", "Reminders"}, {"com.apple.facetime", "FaceTime"},
      {"net.whatsapp.WhatsApp", "WhatsApp"}, {"ph.telegra.Telegraph", "Telegram"}, {"com.google.Gmail", "Gmail"},
      {"com.facebook.Messenger", "Messenger"}, {"com.burbn.instagram", "Instagram"}, {"com.tinyspeck.chatlyio", "Slack"},
  };
  for (auto &m : MAP)
    if (!strcmp(id, m[0])) return strlcpy(out, m[1], n), out;
  const char *dot = strrchr(id, '.');
  strlcpy(out, dot && dot[1] ? dot + 1 : id, n);  // the last part of the ID: "com.foo.Bar" -> "Bar"
  if (*out >= 'a' && *out <= 'z') *out -= 32;
  return out;
}

static void an_store(const char *app_id, const char *title, const char *msg, const char *date) {
  const bool quiet = an_cur.quiet || (an_cur.pre && (!bt_down[0] || strcmp(date, bt_down) <= 0));
  char app[24];
  nt_app_name(app_id, app, sizeof(app));
  portENTER_CRITICAL(&nt_mux);
  for (int k = 0; k < nt.count; k++) {  // modified: replace it in place
    Notif &e = nt.e[(nt.head + NT_N - 1 - k) % NT_N];
    if (e.uid == an_cur.uid) {
      strlcpy(e.title, title, NT_TITLE), strlcpy(e.msg, msg, NT_MSG);
      nt_ver++;
      portEXIT_CRITICAL(&nt_mux);
      return;
    }
  }
  Notif &e = nt.e[nt.head];
  e.uid = an_cur.uid, e.cat = an_cur.cat, e.epoch = halo_clock_valid() ? (uint32_t)time(nullptr) : 0;
  strlcpy(e.app, app, sizeof(e.app)), strlcpy(e.title, title, NT_TITLE), strlcpy(e.msg, msg, NT_MSG);
  nt.head = (nt.head + 1) % NT_N;
  if (nt.count < NT_N) nt.count++;
  nt_ver++;
  if (!quiet) nt_banner = e, nt_new_ver++, nt_unread++;
  portEXIT_CRITICAL(&nt_mux);
}

static void nt_remove(uint32_t uid) {
  portENTER_CRITICAL(&nt_mux);
  int w = 0;  // keep the order: copy the others down, oldest first
  static NotifStore &tmp = psram_new<NotifStore>();
  Notif *keep = tmp.e;
  for (int k = nt.count - 1; k >= 0; k--) {
    const Notif &e = nt.e[(nt.head + NT_N - 1 - k) % NT_N];
    if (e.uid != uid) keep[w++] = e;
  }
  if (w != nt.count) {
    for (int i = 0; i < w; i++) nt.e[i] = keep[i];
    nt.count = w, nt.head = w % NT_N, nt_ver++;
  }
  portEXIT_CRITICAL(&nt_mux);
}

// The Data Source answer: [0][UID 4] then (ID, length 2, bytes) for App, Title, Message, Date; may come in pieces
static void an_data(const uint8_t *d, int n) {
  if (!an_busy) return;
  if (an_len + n > (int)sizeof(an_buf)) an_len = 0;  // nonsense: start over
  memcpy(an_buf + an_len, d, n), an_len += n;
  if (an_len < 5) return;
  static char f[4][NT_MSG];  // the NimBLE task only
  int pos = 5, got = 0;
  while (got < 4 && pos + 3 <= an_len) {
    const int len = an_buf[pos + 1] | an_buf[pos + 2] << 8;
    if (pos + 3 + len > an_len) return;  // the rest comes in the next packet
    const int k = min(len, NT_MSG - 1);
    memcpy(f[got], an_buf + pos + 3, k), f[got][k] = 0;
    pos += 3 + len, got++;
  }
  if (got < 4) return;
  an_store(f[0], f[1], f[2], f[3]);
  an_busy = false, an_len = 0;
  an_next();
}

static void an_next() {  // the next queued notification's attributes
  if (an_busy || an_qh == an_qt || !an_cp || bt_conn == BLE_HS_CONN_HANDLE_NONE) return;
  an_cur = an_q[an_qh], an_qh = (an_qh + 1) % 24;
  const uint32_t u = an_cur.uid;
  const uint8_t cmd[] = {0,    (uint8_t)u, (uint8_t)(u >> 8), (uint8_t)(u >> 16), (uint8_t)(u >> 24),
                         0,                                         // App identifier
                         1,    NT_TITLE - 4, 0,                     // Title, at most
                         3,    (uint8_t)(NT_MSG - 6), 0,            // Message, at most
                         5};                                        // Date
  an_busy = true, an_busy_at = millis(), an_len = 0;
  if (ble_gattc_write_flat(bt_conn, an_cp, cmd, sizeof(cmd), nullptr, nullptr)) an_busy = false;
}

// Notification Source: [event][flags][category][count][UID 4]
static void an_source(const uint8_t *d, int n) {
  if (n < 8) return;
  const uint32_t uid = d[4] | d[5] << 8 | d[6] << 16 | (uint32_t)d[7] << 24;
  if (d[0] == 2) return nt_remove(uid);
  const int nx = (an_qt + 1) % 24;
  if (nx == an_qh) an_qh = (an_qh + 1) % 24;  // full: the oldest request goes
  an_q[an_qt] = {uid, d[2], (d[1] & 0x04) != 0, d[0] == 1}, an_qt = nx;
  an_next();
}

// ---------- Discovery: the service, its characteristics, their CCCDs; then subscribe (Data Source first) ----------
static int an_subscribed(uint16_t, const ble_gatt_error *e, ble_gatt_attr *, void *arg) {
  if (e->status) return LOG("bluetooth: subscribe failed (%d)\n", e->status), 0;
  if (arg) {  // Data Source done: now the Notification Source (the iPhone then sends all it has: the list anew)
    portENTER_CRITICAL(&nt_mux);
    nt.count = nt.head = 0, nt_ver++;
    portEXIT_CRITICAL(&nt_mux);
    static const uint8_t on[2] = {1, 0};
    ble_gattc_write_flat(bt_conn, an_ns_cccd, on, 2, an_subscribed, nullptr);
  } else {
    bt_state = BT_READY;
    LOG("bluetooth: ANCS ready\n");
    am_begin();  // then the music
  }
  return 0;
}

static int an_dsc(uint16_t, const ble_gatt_error *e, uint16_t, const ble_gatt_dsc *d, void *) {
  if (e->status == 0 && ble_uuid_u16(&d->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
    uint16_t owner = 0;  // the characteristic value just before this descriptor
    for (uint16_t h : {an_ns, an_cp, an_ds})
      if (h && h < d->handle && h > owner) owner = h;
    if (owner == an_ns) an_ns_cccd = d->handle;
    if (owner == an_ds) an_ds_cccd = d->handle;
  } else if (e->status == BLE_HS_EDONE) {
    if (!an_ns_cccd || !an_ds_cccd) return LOG("bluetooth: ANCS without notifications\n"), bt_state = BT_NO_ANCS, 0;
    static const uint8_t on[2] = {1, 0};
    ble_gattc_write_flat(bt_conn, an_ds_cccd, on, 2, an_subscribed, (void *)1);
  }
  return 0;
}

static int an_chr(uint16_t, const ble_gatt_error *e, const ble_gatt_chr *c, void *) {
  if (e->status == 0) {
    if (!ble_uuid_cmp(&c->uuid.u, &ANCS_NS.u)) an_ns = c->val_handle;
    if (!ble_uuid_cmp(&c->uuid.u, &ANCS_CP.u)) an_cp = c->val_handle;
    if (!ble_uuid_cmp(&c->uuid.u, &ANCS_DS.u)) an_ds = c->val_handle;
  } else if (e->status == BLE_HS_EDONE) {
    if (!an_ns || !an_cp || !an_ds) return bt_state = BT_NO_ANCS, 0;
    ble_gattc_disc_all_dscs(bt_conn, an_start, an_end, an_dsc, nullptr);  // the CCCDs, whatever the order
  }
  return 0;
}

static int an_svc(uint16_t, const ble_gatt_error *e, const ble_gatt_svc *s, void *) {
  static bool found;
  if (e->status == 0) {
    found = true, an_start = s->start_handle, an_end = s->end_handle;
    ble_gattc_disc_all_chrs(bt_conn, s->start_handle, s->end_handle, an_chr, nullptr);
  } else if (e->status == BLE_HS_EDONE && !found) {
    bt_state = BT_NO_ANCS;
    LOG("bluetooth: the phone has no ANCS (not an iPhone, or not paired)\n");
  }
  if (e->status) found = false;
  return 0;
}

// ---------- Apple Media Service (AMS): what plays on the iPhone, and its remote (the Music screen, apps.h) ----------
// The same pairing as ANCS. After ANCS is set up: the service, its Remote Command and Entity Update characteristics,
// the latter subscribed, then two registrations — the Track (artist, album, title, duration) and the Player (its
// app's name, "state,rate,elapsed", volume); the iPhone sends their values now and on every change: [entity][attr]
// [flags][value]. No artwork: AMS carries none.
static const ble_uuid128_t AMS_SVC =
    BLE_UUID128_INIT(0xDC, 0xF8, 0x55, 0xAD, 0x02, 0xC5, 0xF4, 0x8E, 0x3A, 0x43, 0x36, 0x0F, 0x2B, 0x50, 0xD3, 0x89);
static const ble_uuid128_t AMS_RC =
    BLE_UUID128_INIT(0xC2, 0x51, 0xCA, 0xF7, 0x56, 0x0E, 0xDF, 0xB8, 0x8A, 0x4A, 0xB1, 0x57, 0xD8, 0x81, 0x3C, 0x9B);
static const ble_uuid128_t AMS_EU =
    BLE_UUID128_INIT(0x02, 0xC1, 0x96, 0xBA, 0x92, 0xBB, 0x0C, 0x9A, 0x1F, 0x41, 0x8D, 0x80, 0xCE, 0xAB, 0x7C, 0x2F);
static uint16_t am_start, am_end, am_rc, am_eu, am_eu_cccd;
enum : uint8_t { AMS_PLAY = 0, AMS_PAUSE = 1, AMS_TOGGLE = 2, AMS_NEXT = 3, AMS_PREV = 4, AMS_VOL_UP = 5, AMS_VOL_DOWN = 6 };

struct Media {
  bool valid;              // AMS is set up (an iPhone that has it, connected)
  char player[24], title[96], artist[64], album[64];
  float duration, elapsed, rate, volume;
  uint8_t state;           // 0 paused, 1 playing, 2 rewinding, 3 fast-forwarding
  uint32_t at_ms;          // when elapsed was true
};
static Media &md = psram_new<Media>();
static portMUX_TYPE md_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t md_ver;

static void am_update(const uint8_t *d, int n) {  // an Entity Update notification
  if (n < 3) return;
  char v[96];
  const int k = min(n - 3, (int)sizeof(v) - 1);
  memcpy(v, d + 3, k), v[k] = 0;
  portENTER_CRITICAL(&md_mux);
  if (d[0] == 0 && d[1] == 0) strlcpy(md.player, v, sizeof(md.player));
  if (d[0] == 0 && d[1] == 1) {
    unsigned st = 0;
    float r = 0, e = 0;
    sscanf(v, "%u,%f,%f", &st, &r, &e);
    md.state = st, md.rate = r, md.elapsed = e, md.at_ms = millis();
  }
  if (d[0] == 0 && d[1] == 2) md.volume = strtof(v, nullptr);
  if (d[0] == 2 && d[1] == 0) strlcpy(md.artist, v, sizeof(md.artist));
  if (d[0] == 2 && d[1] == 1) strlcpy(md.album, v, sizeof(md.album));
  if (d[0] == 2 && d[1] == 2) strlcpy(md.title, v, sizeof(md.title));
  if (d[0] == 2 && d[1] == 3) md.duration = strtof(v, nullptr);
  md_ver++;
  portEXIT_CRITICAL(&md_mux);
}

static int am_registered(uint16_t, const ble_gatt_error *e, ble_gatt_attr *, void *arg) {
  if (e->status) return LOG("bluetooth: AMS registration failed (%d)\n", e->status), 0;
  if (arg) {  // the Track is registered: now the Player
    static const uint8_t player[] = {0, 0, 1, 2};
    ble_gattc_write_flat(bt_conn, am_eu, player, sizeof(player), am_registered, nullptr);
  } else {
    portENTER_CRITICAL(&md_mux);
    md.valid = true, md_ver++;
    portEXIT_CRITICAL(&md_mux);
    LOG("bluetooth: AMS ready\n");
    cts_begin();  // then the time
  }
  return 0;
}

static int am_subscribed(uint16_t, const ble_gatt_error *e, ble_gatt_attr *, void *) {
  if (e->status) return LOG("bluetooth: AMS subscribe failed (%d)\n", e->status), 0;
  static const uint8_t track[] = {2, 0, 1, 2, 3};
  ble_gattc_write_flat(bt_conn, am_eu, track, sizeof(track), am_registered, (void *)1);
  return 0;
}

static int am_dsc(uint16_t, const ble_gatt_error *e, uint16_t, const ble_gatt_dsc *d, void *) {
  if (e->status == 0 && ble_uuid_u16(&d->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16 && am_eu && d->handle > am_eu &&
      (am_rc < am_eu || d->handle < am_rc))  // the CCCD after Entity Update (not Remote Command's)
    if (!am_eu_cccd) am_eu_cccd = d->handle;
  if (e->status == BLE_HS_EDONE && am_eu_cccd) {
    static const uint8_t on[2] = {1, 0};
    ble_gattc_write_flat(bt_conn, am_eu_cccd, on, 2, am_subscribed, nullptr);
  }
  return 0;
}

static int am_chr(uint16_t, const ble_gatt_error *e, const ble_gatt_chr *c, void *) {
  if (e->status == 0) {
    if (!ble_uuid_cmp(&c->uuid.u, &AMS_RC.u)) am_rc = c->val_handle;
    if (!ble_uuid_cmp(&c->uuid.u, &AMS_EU.u)) am_eu = c->val_handle;
  } else if (e->status == BLE_HS_EDONE && am_rc && am_eu) {
    ble_gattc_disc_all_dscs(bt_conn, am_start, am_end, am_dsc, nullptr);
  }
  return 0;
}

static int am_svc(uint16_t, const ble_gatt_error *e, const ble_gatt_svc *s, void *) {
  static bool found;
  if (e->status == 0) {
    found = true, am_start = s->start_handle, am_end = s->end_handle;
    ble_gattc_disc_all_chrs(bt_conn, am_start, am_end, am_chr, nullptr);
  } else if (e->status == BLE_HS_EDONE && !found) {
    cts_begin();  // no AMS: the time anyway
  }
  if (e->status) found = false;
  return 0;
}

static void am_begin() {  // after ANCS is set up (one discovery at a time)
  am_rc = am_eu = am_eu_cccd = 0;
  ble_gattc_disc_svc_by_uuid(bt_conn, &AMS_SVC.u, am_svc, nullptr);
}

static void am_lost() {  // disconnected or stopped: nothing is known to play
  portENTER_CRITICAL(&md_mux);
  md.valid = false, md_ver++;
  portEXIT_CRITICAL(&md_mux);
}

static bool ams_cmd(uint8_t c) {  // the Music screen's buttons (any task: NimBLE locks itself)
  if (!bt_inited || bt_conn == BLE_HS_CONN_HANDLE_NONE || !am_rc) return false;
  return ble_gattc_write_flat(bt_conn, am_rc, &c, 1, nullptr, nullptr) == 0;
}

// ---------- Current Time Service (CTS): the clock and the time zone from the iPhone (no Wi-Fi needed) ----------
// Local Time Information (0x2A0F): the zone in 15-minute steps and the DST offset; Current Time (0x2A2B): the
// iPhone's local date and time. Together: UTC exactly, and the "auto" time zone where the phone is (net.h)
static uint16_t ct_time, ct_info;
static int8_t ct_zone = -128;  // 15-minute steps; −128 — not known
static uint8_t ct_dst = 255;   // 15-minute steps; 255 — not known

static int64_t ct_days(int y, int m, int d) {  // days since 1970-01-01 (Howard Hinnant's days_from_civil)
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
  const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + doe - 719468;
}

static int ct_got_time(uint16_t, const ble_gatt_error *e, ble_gatt_attr *a, void *) {
  uint8_t b[10];
  if (e->status || OS_MBUF_PKTLEN(a->om) < 7) return 0;
  os_mbuf_copydata(a->om, 0, 7, b);
  const int y = b[0] | b[1] << 8;
  if (y < 2020 || ct_zone == -128 || ct_dst == 255) return LOG("bluetooth: the iPhone's time has no zone\n"), 0;
  const int32_t off = (ct_zone + ct_dst) * 15 * 60;  // seconds east of UTC, DST included
  const int64_t local = ct_days(y, b[2], b[3]) * 86400 + b[4] * 3600 + b[5] * 60 + b[6];
  net_set_auto_offset(off, true);
  net_time_set((time_t)(local - off), "iPhone");
  LOG("bluetooth: time from the iPhone, UTC%+.2g h\n", off / 3600.0f);
  return 0;
}

static int ct_got_info(uint16_t, const ble_gatt_error *e, ble_gatt_attr *a, void *) {
  uint8_t b[2];
  if (!e->status && OS_MBUF_PKTLEN(a->om) >= 2) os_mbuf_copydata(a->om, 0, 2, b), ct_zone = (int8_t)b[0], ct_dst = b[1];
  if (ct_time) ble_gattc_read(bt_conn, ct_time, ct_got_time, nullptr);
  return 0;
}

static int ct_chr(uint16_t, const ble_gatt_error *e, const ble_gatt_chr *c, void *) {
  if (e->status == 0) {
    const uint16_t u = ble_uuid_u16(&c->uuid.u);
    if (u == 0x2A2B) ct_time = c->val_handle;
    if (u == 0x2A0F) ct_info = c->val_handle;
  } else if (e->status == BLE_HS_EDONE) {
    if (ct_info) ble_gattc_read(bt_conn, ct_info, ct_got_info, nullptr);
    else if (ct_time) ble_gattc_read(bt_conn, ct_time, ct_got_time, nullptr);
  }
  return 0;
}

static int ct_svc(uint16_t, const ble_gatt_error *e, const ble_gatt_svc *s, void *) {
  if (e->status == 0) ble_gattc_disc_all_chrs(bt_conn, s->start_handle, s->end_handle, ct_chr, nullptr);
  return 0;
}

static void cts_begin() {
  static const ble_uuid16_t CTS = BLE_UUID16_INIT(0x1805);
  ct_time = ct_info = 0, ct_zone = -128, ct_dst = 255;
  ble_gattc_disc_svc_by_uuid(bt_conn, &CTS.u, ct_svc, nullptr);
}

// ---------- Connection ----------
static void bt_advertise();

static int bt_gap(ble_gap_event *ev, void *) {
  switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT: {
      // A bonded iPhone may encrypt before this event is handled (ENC_CHANGE first): then all is under way already
      if (ev->connect.status) return bt_advertise(), 0;
      bt_conn = ev->connect.conn_handle;
      LOG("bluetooth: connected\n");
      ble_gap_conn_desc d;
      if (!ble_gap_conn_find(bt_conn, &d) && d.sec_state.encrypted) break;
      if (bt_state < BT_CONN || bt_state == BT_PAUSED) bt_state = BT_CONN;
      ble_gap_security_initiate(bt_conn);  // ANCS needs an encrypted link: pairs the first time
      break;
    }
    case BLE_GAP_EVENT_DISCONNECT:
      LOG("bluetooth: disconnected (%d)\n", ev->disconnect.reason);
      bt_conn = BLE_HS_CONN_HANDLE_NONE, an_busy = false, am_lost();
      if (bt_state != BT_OFF && bt_state != BT_PAUSED) bt_advertise();  // not while it is being stopped
      break;
    case BLE_GAP_EVENT_ENC_CHANGE:
      if (ev->enc_change.status) return LOG("bluetooth: pairing failed (%d)\n", ev->enc_change.status), 0;
      bt_conn = ev->enc_change.conn_handle, bt_state = BT_SECURE;  // the handle from this event: it may come first
      an_ns = an_cp = an_ds = an_ns_cccd = an_ds_cccd = 0, an_busy = false, an_qh = an_qt = 0;
      LOG("bluetooth: encrypted\n");
      ble_gattc_disc_svc_by_uuid(bt_conn, &ANCS_SVC.u, an_svc, nullptr);
      break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {  // the iPhone forgot us and pairs again: drop the old bond
      ble_gap_conn_desc d;
      if (!ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d)) ble_store_util_delete_peer(&d.peer_id_addr);
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_NOTIFY_RX: {
      uint8_t b[256];
      const int n = min<int>(OS_MBUF_PKTLEN(ev->notify_rx.om), sizeof(b));
      os_mbuf_copydata(ev->notify_rx.om, 0, n, b);
      if (ev->notify_rx.attr_handle == an_ns) an_source(b, n);
      else if (ev->notify_rx.attr_handle == an_ds) an_data(b, n);
      else if (ev->notify_rx.attr_handle == am_eu) am_update(b, n);
      break;
    }
    case BLE_GAP_EVENT_ADV_COMPLETE:
      if (bt_state == BT_ADV) bt_advertise();
      break;
  }
  return 0;
}

static void bt_advertise() {
  if (bt_conn != BLE_HS_CONN_HANDLE_NONE) return;
  ble_hs_adv_fields f = {};
  f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  f.sol_uuids128 = &ANCS_SVC, f.sol_num_uuids128 = 1;  // "I want ANCS": the iPhone lists us in Settings
  f.appearance = 0x00C0, f.appearance_is_present = 1;   // a watch
  ble_gap_adv_set_fields(&f);
  ble_hs_adv_fields r = {};
  r.name = (const uint8_t *)"Oriole", r.name_len = 6, r.name_is_complete = 1;
  ble_gap_adv_rsp_set_fields(&r);
  ble_gap_adv_params p = {};
  p.conn_mode = BLE_GAP_CONN_MODE_UND, p.disc_mode = BLE_GAP_DISC_MODE_GEN;
  p.itvl_min = BLE_GAP_ADV_ITVL_MS(300), p.itvl_max = BLE_GAP_ADV_ITVL_MS(400);
  if (!ble_gap_adv_start(bt_addr_type, nullptr, BLE_HS_FOREVER, &p, bt_gap, nullptr)) bt_state = BT_ADV;
}

static void bt_on_sync() {
  ble_hs_id_infer_auto(0, &bt_addr_type);
  if (cfg.bt_on) bt_advertise();
}

static void bt_host_task(void *) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}

static void bt_start() {  // the net task
  const size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  if (nimble_port_init() != ESP_OK) return LOG("bluetooth: could not start\n");
  ble_hs_cfg.sync_cb = bt_on_sync, ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
  ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT, ble_hs_cfg.sm_bonding = 1, ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_our_key_dist = ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_svc_gap_init(), ble_svc_gatt_init();
  ble_svc_gap_device_name_set("Oriole");
  ble_svc_gap_device_appearance_set(0x00C0);
  ble_store_config_init();
  if (bt_forget_req) bt_forget_req = false, ble_store_clear(), LOG("bluetooth: the iPhone is forgotten\n");
  nimble_port_freertos_init(bt_host_task);
  bt_inited = true;
  LOG("bluetooth: on (%u KB of internal memory)\n",
      (unsigned)((before - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)) / 1024));
}

static void bt_stop(uint8_t why) {  // the net task: everything freed (BT_PAUSED — for Wi-Fi, BT_OFF — in Settings)
  bt_state = why;
  if (bt_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(bt_conn, BLE_ERR_REM_USER_CONN_TERM), delay(150);
  ble_gap_adv_stop();
  nimble_port_stop();
  nimble_port_deinit();
  bt_inited = false, bt_conn = BLE_HS_CONN_HANDLE_NONE, an_busy = false, bt_state = why;
  am_lost();
  if (halo_clock_valid()) {
    const time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(bt_down, sizeof(bt_down), "%Y%m%dT%H%M%S", &tm);
  }
  LOG("bluetooth: %s\n", why == BT_PAUSED ? "paused for Wi-Fi" : "off");
}

// From the net task, every round: Bluetooth runs when it is on in Settings and Wi-Fi is off
static void bt_radio(bool wifi) {
  const bool want = cfg.bt_on && !wifi;
  if (cfg.bt_on != bt_held) bt_held = cfg.bt_on, halo_stay_awake(bt_held);  // its controller cannot sleep
  if (want && !bt_inited) bt_start();
  else if (!want && bt_inited) bt_stop(wifi ? BT_PAUSED : BT_OFF);
  else if (!want && bt_state != (wifi && cfg.bt_on ? BT_PAUSED : BT_OFF)) bt_state = wifi && cfg.bt_on ? BT_PAUSED : BT_OFF;
  if (want && bt_forget_req) {  // forget, while it runs: drop the link and the bond, start again clean
    bt_stop(BT_OFF);
    bt_start();
  }
}

static void bt_apply() {  // the bt_on setting changed (cfg_changed, loop): the net task applies it
  if (net_task_h) xTaskNotifyGive(net_task_h);
}

static void bt_forget() {  // the bond goes; the iPhone must forget "Oriole" too, then pair again
  bt_forget_req = true;
  if (net_task_h) xTaskNotifyGive(net_task_h);
}

static void bt_poll() {  // from loop(): a request the phone never answered — give up on it, ask for the next
  if (an_busy && millis() - an_busy_at > 4000) an_busy = false, an_len = 0, an_next();
}
