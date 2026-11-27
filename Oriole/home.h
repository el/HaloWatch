// home.h — the Home app: Home Assistant entities to see and switch. Set in the web controller: the address
// (ha_url, e.g. http://192.168.1.20:8123 — an IP address: the watch does not resolve .local names), a long-lived
// access token (ha_token: Home Assistant -> your profile -> Security) and the entities (ha_items: "light.kitchen;
// switch.fan;scene.movie"). A card each: its name and state; a tap toggles it (light, switch, fan, input_boolean,
// automation, cover, media_player), runs it (scene, script) or presses it (button). The requests run in a task of
// their own (Wi-Fi comes on for them, Bluetooth pauses meanwhile): on opening the screen, every 30 s while it is up,
// and after each tap. Included from Oriole.ino after apps.h.

#define HA_N 12
struct HaItem {
  char id[64], name[48], state[24];
  bool busy;  // a tap on its way
};
struct HaStore {
  int n;
  HaItem it[HA_N];
  char error[48];  // the last failure ("" — none)
};
static HaStore &ha = psram_new<HaStore>();
static portMUX_TYPE ha_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t ha_ver;
static volatile int ha_tap = -1;         // the item to act on
static volatile bool ha_want_refresh;
static TaskHandle_t ha_task_h;
static volatile uint32_t ha_seen_ms;  // the screen was last up then (its 30 s refreshes)

static void home_refresh() {  // any task: read the states again (cfg_changed, the screen)
  ha_want_refresh = true;
  if (ha_task_h) xTaskNotifyGive(ha_task_h);
}

static void ha_parse_items() {  // the ha_items setting into the list (names and states kept if the id is the same)
  char list[300];
  {
    CfgLock l;
    strlcpy(list, cfg.ha_items, sizeof(list));
  }
  static HaStore &old = psram_new<HaStore>();
  portENTER_CRITICAL(&ha_mux);
  old = ha;
  portEXIT_CRITICAL(&ha_mux);
  int n = 0;
  HaItem it[HA_N];
  for (char *save = nullptr, *tok = strtok_r(list, ";, \n", &save); tok && n < HA_N; tok = strtok_r(nullptr, ";, \n", &save)) {
    if (!strchr(tok, '.')) continue;
    HaItem &h = it[n++];
    memset(&h, 0, sizeof(h));
    strlcpy(h.id, tok, sizeof(h.id));
    for (int k = 0; k < old.n; k++)
      if (!strcmp(old.it[k].id, tok)) h = old.it[k], h.busy = false;
    if (!h.name[0]) strlcpy(h.name, strchr(tok, '.') + 1, sizeof(h.name));
  }
  portENTER_CRITICAL(&ha_mux);
  ha.n = n;
  for (int i = 0; i < n; i++) ha.it[i] = it[i];
  ha_ver++;
  portEXIT_CRITICAL(&ha_mux);
}

static void ha_error(const char *fmt, int v) {
  portENTER_CRITICAL(&ha_mux);
  snprintf(ha.error, sizeof(ha.error), fmt, v), ha_ver++;
  portEXIT_CRITICAL(&ha_mux);
}

static bool ha_read(int i, const char *base, const char *token, char *buf, int cap) {  // one entity's state and name
  char url[200], id[64];
  portENTER_CRITICAL(&ha_mux);
  strlcpy(id, ha.it[i].id, sizeof(id));
  portEXIT_CRITICAL(&ha_mux);
  snprintf(url, sizeof(url), "%s/api/states/%s", base, id);
  const int st = net_request(false, url, token, nullptr, buf, cap);
  if (st != 200) return ha_error(st < 0 ? "Home Assistant did not answer" : "Home Assistant: error %d", st), false;
  char state[24] = "", name[48] = "";
  js_str(js_find(buf, "state"), state, sizeof(state));
  js_str(js_find(buf, "friendly_name"), name, sizeof(name));
  portENTER_CRITICAL(&ha_mux);
  if (i < ha.n && !strcmp(ha.it[i].id, id)) {
    strlcpy(ha.it[i].state, state, sizeof(ha.it[i].state));
    if (name[0]) strlcpy(ha.it[i].name, name, sizeof(ha.it[i].name));
    ha.it[i].busy = false, ha_ver++;
  }
  portEXIT_CRITICAL(&ha_mux);
  return true;
}

static void ha_act(int i, const char *base, const char *token) {  // the tap: the service that fits the domain
  char id[64], state[24];
  portENTER_CRITICAL(&ha_mux);
  strlcpy(id, ha.it[i].id, sizeof(id)), strlcpy(state, ha.it[i].state, sizeof(state));
  portEXIT_CRITICAL(&ha_mux);
  char domain[32];
  strlcpy(domain, id, min(sizeof(domain), (size_t)(strchr(id, '.') - id + 1)));
  const char *svc = !strcmp(domain, "scene") || !strcmp(domain, "script") ? "turn_on"
                  : !strcmp(domain, "button") || !strcmp(domain, "input_button") ? "press"
                  : !strcmp(domain, "lock") ? (!strcmp(state, "locked") ? "unlock" : "lock")
                  : "toggle";
  char url[200], body[100];
  snprintf(url, sizeof(url), "%s/api/services/%s/%s", base, domain, svc);
  snprintf(body, sizeof(body), "{\"entity_id\":\"%s\"}", id);
  const int st = net_request(true, url, token, body, nullptr, 0);
  if (st != 200) ha_error(st < 0 ? "Home Assistant did not answer" : "Home Assistant: error %d", st);
  LOG("home: %s %s -> %d\n", svc, id, st);
}

static void ha_task(void *) {
  static char *buf = (char *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);  // a state's JSON (the start is enough)
  uint32_t last = 0;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    const int tap = ha_tap;
    const bool up = millis() - ha_seen_ms < 2000;
    const bool due = ha_want_refresh || tap >= 0 || (up && millis() - last > 30000);
    if (!due) continue;
    ha_want_refresh = false, ha_tap = -1, last = millis();
    ha_parse_items();
    char base[96], token[256];
    {
      CfgLock l;
      strlcpy(base, cfg.ha_url, sizeof(base)), strlcpy(token, cfg.ha_token, sizeof(token));
    }
    while (base[0] && base[strlen(base) - 1] == '/') base[strlen(base) - 1] = 0;
    if (!base[0] || !token[0] || !ha.n || !buf) continue;
    if (!net_acquire(20000)) {
      ha_error("No Wi-Fi", 0);
      continue;
    }
    ha_error("", 0);
    if (tap >= 0 && tap < ha.n) ha_act(tap, base, token), vTaskDelay(pdMS_TO_TICKS(400)), ha_read(tap, base, token, buf, 4096);
    else
      for (int i = 0; i < ha.n; i++)
        if (!ha_read(i, base, token, buf, 4096)) break;
    net_release();
  }
}

// ---------- The screen ----------
struct HaUi {
  HaStore copy;
  char sub[HA_N][32], name[HA_N][48];
};
static HaUi &hu = psram_new<HaUi>();
static uint32_t hu_seen = ~0u;
static CardRow ha_rows[HA_N];
static CardList &ha_list = cl_new("Home", ha_rows, HA_N);

static const char *ha_state_text(const HaItem &h) {
  const char *d = h.id;
  if (h.busy) return "...";
  if (!strncmp(d, "scene.", 6)) return "Tap to activate";
  if (!strncmp(d, "script.", 7)) return "Tap to run";
  if (!strncmp(d, "button.", 7) || !strncmp(d, "input_button.", 13)) return "Tap to press";
  if (!h.state[0]) return "Loading...";
  if (!strcmp(h.state, "on")) return "On";
  if (!strcmp(h.state, "off")) return "Off";
  if (!strcmp(h.state, "unavailable")) return "Unavailable";
  return h.state;
}

static void hu_snapshot() {
  hu_seen = ha_ver;
  portENTER_CRITICAL(&ha_mux);
  hu.copy = ha;
  portEXIT_CRITICAL(&ha_mux);
  const HaStore &c = hu.copy;
  for (int i = 0; i < c.n; i++) {
    fit_text(hu.name[i], 48, t24, c.it[i].name, 290);
    strlcpy(hu.sub[i], ha_state_text(c.it[i]), sizeof(hu.sub[i]));
  }
  if (!c.n || c.error[0]) {  // the first card says what is wrong (or how to set it up)
    bool set;
    {
      CfgLock l;
      set = cfg.ha_url[0] && cfg.ha_token[0] && cfg.ha_items[0];
    }
    if (!c.n) strlcpy(hu.name[0], set ? "No entities" : "Not set up", 48);
    fit_text(hu.sub[0], 32, t20, c.error[0] ? c.error : set ? "Check the list in the controller" : "In the web controller", 290);
  }
  ha_list.n = max(1, c.n);
}

static void ha_tap_row(int k) {
  portENTER_CRITICAL(&ha_mux);
  const bool ok = k < ha.n;
  if (ok) ha.it[k].busy = true, ha_ver++;
  portEXIT_CRITICAL(&ha_mux);
  if (!ok) return;
  ha_tap = k;
  if (ha_task_h) xTaskNotifyGive(ha_task_h);
}

static const OrioleScreen HOME_SCREEN = {[] { ha_seen_ms = millis(), home_refresh(), hu_snapshot(), cl_enter(ha_list); },
                                       [](float dt) {
                                         ha_seen_ms = millis();
                                         if (ha_ver != hu_seen) hu_snapshot();
                                         cl_update(ha_list, dt);
                                       },
                                       [](const OrioleView &v) { cl_layout(ha_list, v); }, [] { cl_draw(ha_list); },
                                       false, ORIOLE_HOME_BOTH};

static void home_begin() {
  for (int i = 0; i < HA_N; i++)
    ha_rows[i] = {nullptr, nullptr, SE_VALUE, nullptr, nullptr, [](int k) { return (const char *)hu.name[k]; },
                  [](int k) { return (const char *)hu.sub[k]; }, ha_tap_row};
  cl_build(ha_list);
  ha_parse_items();
  xTaskCreatePinnedToCoreWithCaps(ha_task, "home", 6144, nullptr, 2, &ha_task_h, 0, MALLOC_CAP_SPIRAM);
}
