#include "BookProgress.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Xtc.h>

#include <algorithm>
#include <string>

namespace {
uint32_t readLe32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// Recent EPUB answers, keyed by path and the saved position they came from.
struct Remembered {
  std::string path;
  int size = 0;
  uint8_t data[10]{};
  int percent = -1;
};
constexpr int kRecent = 4;
Remembered recent[kRecent];
int nextSlot = 0;
}  // namespace

int loadBookProgress(const std::string& path) {
  uint8_t data[10]{};
  if (FsHelpers::hasReflowableBookExtension(path)) {
    // Metadata objects exceed the stack budget; only the featured book is loaded, once per entry.
    auto epub = makeUniqueNoThrow<Epub>(path, "/.crosspoint");
    if (!epub) {
      LOG_ERR("HOME", "OOM: progress metadata");
      return -1;
    }
    // The cache path needs no parse, so the saved position is read first and
    // the metadata load (tens of ms) is skipped while it is unchanged: a
    // shelf repaints each cover many times per frame.
    HalFile file;
    if (!Storage.openFileForRead("HOME", epub->getCachePath() + "/progress.bin", file)) return -1;
    const int size = file.read(data, sizeof(data));
    file.close();
    if (size != 4 && size != 6 && size != 10) return -1;
    for (const auto& hit : recent) {
      if (hit.size == size && hit.path == path && std::equal(data, data + size, hit.data)) return hit.percent;
    }
    if (!epub->load(false, true)) return -1;
    const int spine = data[0] | (data[1] << 8);
    const int page = data[2] | (data[3] << 8);
    const int total = size >= 6 ? data[4] | (data[5] << 8) : 0;
    if (epub->getSpineItemsCount() <= 0 || epub->getBookSize() == 0) return -1;
    int percent = -1;
    if (spine == epub->getSpineItemsCount()) {
      percent = 100;
    } else if (spine < epub->getSpineItemsCount()) {
      const float fraction = total > 0 && page != UINT16_MAX ? std::clamp(float(page) / total, 0.0f, 1.0f) : 0;
      percent = std::clamp(static_cast<int>(epub->calculateProgress(spine, fraction) * 100 + 0.5f), 0, 100);
    }
    auto& slot = recent[nextSlot];
    nextSlot = (nextSlot + 1) % kRecent;
    slot.path = path;
    slot.size = size;
    std::copy(data, data + size, slot.data);
    slot.percent = percent;
    return percent;
  }
  if (FsHelpers::hasXtcExtension(path)) {
    auto xtc = makeUniqueNoThrow<Xtc>(path, "/.crosspoint");
    if (!xtc) {
      LOG_ERR("HOME", "OOM: XTC progress metadata");
      return -1;
    }
    if (!xtc->load()) return -1;
    HalFile file;
    if (!Storage.openFileForRead("HOME", xtc->getCachePath() + "/progress.bin", file) || file.read(data, 4) != 4)
      return -1;
    const uint32_t page = readLe32(data);
    if (xtc->getPageCount() == 0) return -1;
    if (page >= xtc->getPageCount()) return 100;
    return xtc->calculateProgress(page);
  }
  return -1;
}
