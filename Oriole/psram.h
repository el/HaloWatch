// psram.h — big UI state in PSRAM, made before setup() (the PSRAM heap is up by then): internal RAM is for what cannot live
// anywhere else — Wi-Fi and lwIP buffers, the I2S (mic, speaker) and display DMA. Without them the mic does not start
// and requests fail (it ran down to 720 bytes free once). A reference keeps the name, so nothing else changes
#include <new>

template <class T> static T &psram_new() {
  void *p = heap_caps_calloc(1, sizeof(T), MALLOC_CAP_SPIRAM);
  if (!p) abort();
  return *new (p) T();
}
