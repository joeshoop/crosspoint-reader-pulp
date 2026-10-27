#pragma once

#include <cstdint>

#include "activities/Activity.h"

// Diagnostic (touch boards): finds the smallest reliable tap target. Each round
// packs a grid of equal targets with thin gaps, like a row of real buttons, and
// lights one at random. Ten taps per size; the results list hit rate and the
// average offset between the finger and the target centre for each size.
class TapTargetTestActivity final : public Activity {
 public:
  explicit TapTargetTestActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("TapTargetTest", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

  static constexpr int kSizes[] = {32, 44, 56, 72, 88};
  static constexpr int kSizeCount = static_cast<int>(sizeof(kSizes) / sizeof(kSizes[0]));
  static constexpr int kTapsPerSize = 10;

 private:
  struct Grid {
    int x;
    int y;
    int cols;
    int rows;
    int step;
  };
  Grid grid() const;
  void restart();
  void pickTarget();

  int sizeIndex = 0;
  int tapIndex = 0;
  int target = 0;
  int hits[kSizeCount]{};
  int offsetSum[kSizeCount]{};
  // Signed: positive = right of / below the target centre.
  int dxSum[kSizeCount]{};
  int dySum[kSizeCount]{};
  void drawShift(int x, int y, int dx, int dy) const;
  void drawRing(int cx, int cy, int radius, int thickness, bool black) const;
  void drawTapMark(int tapX, int tapY, int aimX, int aimY) const;
  void drawScatter(int top, int bottom) const;

  // Last tap and the centre it aimed at, marked on the next screen.
  bool hasLast = false;
  int lastTapX = 0;
  int lastTapY = 0;
  int lastAimX = 0;
  int lastAimY = 0;
  // Every tap's offset from its target centre, for the results plot.
  static constexpr int kTotalTaps = kSizeCount * kTapsPerSize;
  int16_t tapDx[kTotalTaps]{};
  int16_t tapDy[kTotalTaps]{};
  int tapCount = 0;
};
