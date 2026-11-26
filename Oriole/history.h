// history.h — the assistant's last HIST_N questions and answers, kept across restarts (the History screen: apps.h).
//
// In memory (PSRAM): a ring, newest at head − 1. On flash: one file, /history.bin, in the "ffat" partition with
// LittleFS (it formats in a moment, where FAT with wear levelling erases the whole 23 MB first, and a power cut never
// leaves it half-written). A finished answer (gemini.h, turnComplete) is copied in at once; the file is written by
// its own task once the assistant is quiet: a flash write stops code running from flash for a moment, and the speaker
// would skip. Included from Oriole.ino before gemini.h.
#include <LittleFS.h>

#define HIST_N 20
#define HIST_Q 200   // the question (what you said, as transcribed, or typed)
#define HIST_A 1000  // the answer's transcript
#define HIST_MAGIC 0x48495331u  // "HIS1": the file's layout

struct HistEntry {
  uint32_t epoch;  // when (0 — the clock was not set)
  char q[HIST_Q], a[HIST_A];
};
struct HistStore {
  uint32_t magic, count, head;
  HistEntry e[HIST_N];
};
static HistStore &hist = psram_new<HistStore>();
static portMUX_TYPE hist_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t hist_ver;  // bumped on every change: the screen takes a new copy
static volatile bool hist_unsaved;
static TaskHandle_t hist_task_h;

static bool ai_busy();  // gemini.h

// The k-th newest (0 — the last one); call under hist_mux or from a copy
static const HistEntry &hist_at(int k) {
  return hist.e[(hist.head + HIST_N - 1 - k) % HIST_N];
}

// A finished answer, from the session task (it owns the strings it passes)
static void hist_push(const char *q, const char *a) {
  if (!*a) return;
  portENTER_CRITICAL(&hist_mux);
  HistEntry &e = hist.e[hist.head];
  e.epoch = halo_clock_valid() ? (uint32_t)time(nullptr) : 0;
  strlcpy(e.q, *q ? q : "(no transcript)", HIST_Q), strlcpy(e.a, a, HIST_A);
  hist.head = (hist.head + 1) % HIST_N;
  if (hist.count < HIST_N) hist.count++;
  hist_ver++, hist_unsaved = true;
  portEXIT_CRITICAL(&hist_mux);
  if (hist_task_h) xTaskNotifyGive(hist_task_h);
}

static void hist_task(void *) {
  if (!LittleFS.begin(true, "/fs", 4, "ffat")) {
    LOG("history: no file system (the \"ffat\" partition)\n");
    vTaskDelete(nullptr);
  }
  File f = LittleFS.open("/history.bin", "r");
  if (f && f.size() == sizeof(HistStore)) {
    static HistStore &tmp = psram_new<HistStore>();  // read outside the lock, then swapped in
    if (f.read((uint8_t *)&tmp, sizeof(tmp)) == sizeof(tmp) && tmp.magic == HIST_MAGIC && tmp.count <= HIST_N &&
        tmp.head < HIST_N) {
      portENTER_CRITICAL(&hist_mux);
      if (!hist.count) hist = tmp, hist_ver++;  // nothing new came in meanwhile
      portEXIT_CRITICAL(&hist_mux);
    }
  }
  if (f) f.close();
  LOG("history: %lu answers\n", (unsigned long)hist.count);
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
    if (!hist_unsaved || ai_busy()) continue;  // quiet first: the speaker must not skip
    static HistStore &copy = psram_new<HistStore>();  // the flash driver bounces PSRAM through its own buffer
    portENTER_CRITICAL(&hist_mux);
    hist.magic = HIST_MAGIC, copy = hist, hist_unsaved = false;
    portEXIT_CRITICAL(&hist_mux);
    File w = LittleFS.open("/history.bin", "w");
    const bool ok = w && w.write((const uint8_t *)&copy, sizeof(copy)) == sizeof(copy);
    if (w) w.close();
    if (!ok) hist_unsaved = true, LOG("history: could not save\n");
  }
}

static void hist_begin() {
  xTaskCreatePinnedToCore(hist_task, "history", 6144, nullptr, 1, &hist_task_h, 0);
}
