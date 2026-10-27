#pragma once

#include <atomic>
#include <cstdint>

#include "activities/Activity.h"

// Diagnostic: cycles a large seven-segment character (0-9, A-F) at a chosen
// rate and reports what the panel actually achieved. Skipped characters are
// dropped frames; the gap between target and actual rate is the panel's limit.
// On touch boards a tap in the test area is marked and timed from the moment
// the firmware sees the contact to the end of the refresh that shows it.
class RefreshTestActivity final : public Activity {
 public:
  explicit RefreshTestActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("RefreshTest", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return true; }
  bool preventAutoSleep() override { return true; }

  static constexpr int kMaxRate = 20;

 private:
  struct Layout {
    int top;
    int blinkerBottom;
    int statsTop;
    int sliderY;
    int chipsTop;
    int chipHeight;
    int left;
    int right;
  };
  Layout layout() const;

  void setRate(int rate);
  void resetStats();
  void drawGlyph(int x, int y, int w, int h, int thickness, uint8_t segments, bool black) const;
  void drawChip(int x, int y, int w, int h, const char* label, bool filled) const;

  // Written by the input loop, read by the render task.
  std::atomic<int> rate{2};  // frames per second; 0 = paused
  std::atomic<bool> cleanMode{false};
  std::atomic<bool> fullSize{false};
  std::atomic<uint32_t> frame{0};
  std::atomic<uint32_t> statsEpoch{0};
  std::atomic<bool> tapPending{false};
  std::atomic<int> tapX{0};
  std::atomic<int> tapY{0};
  std::atomic<uint32_t> tapAtMs{0};

  // Input loop only.
  uint32_t nextDueMs = 0;
  int dragRate = -1;

  // Render task only.
  uint32_t shownEpoch = UINT32_MAX;
  uint32_t lastShownFrame = 0;
  uint32_t dropped = 0;
  uint32_t lastRefreshMs = 0;
  uint32_t tapLatencyMs = 0;
  static constexpr int kHistory = 32;
  uint32_t shownAt[kHistory]{};
  int shownCount = 0;
  int shownHead = 0;
};
