#include "TapTargetTestActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "MappedInputManager.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int kGap = 4;  // between neighbouring targets, like adjacent buttons
// 4.26" 800x480 (X4 Pro): ~219 PPI, ~8.6 px per mm.
int tenthsOfMm(const int px) { return (px * 100 + 43) / 86; }
}  // namespace

void TapTargetTestActivity::onEnter() {
  Activity::onEnter();
  restart();
}

void TapTargetTestActivity::restart() {
  sizeIndex = 0;
  tapIndex = 0;
  for (int i = 0; i < kSizeCount; i++) {
    hits[i] = 0;
    offsetSum[i] = 0;
    dxSum[i] = 0;
    dySum[i] = 0;
  }
  hasLast = false;
  tapCount = 0;
  pickTarget();
  requestUpdate();
}

TapTargetTestActivity::Grid TapTargetTestActivity::grid() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const int lineHeight = renderer.getLineHeight(themedFontId(UI_10_FONT_ID));
  const int top = metrics.topPadding + metrics.headerHeight + 2 * lineHeight + 16;
  const int bottom = height - 16;
  const int areaW = width - 2 * metrics.contentSidePadding;
  const int step = kSizes[std::min(sizeIndex, kSizeCount - 1)] + kGap;

  Grid g{};
  g.step = step;
  g.cols = std::max(1, (areaW + kGap) / step);
  g.rows = std::max(1, (bottom - top + kGap) / step);
  g.x = (width - (g.cols * step - kGap)) / 2;
  g.y = top + (bottom - top - (g.rows * step - kGap)) / 2;
  return g;
}

void TapTargetTestActivity::pickTarget() {
  const Grid g = grid();
  const int count = g.cols * g.rows;
  int next = static_cast<int>(random(count));
  if (count > 1 && next == target) next = (next + 1) % count;
  target = next;
}

void TapTargetTestActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  int x = 0;
  int y = 0;
  if (!mappedInput.wasScreenTapped(x, y)) return;

  if (sizeIndex >= kSizeCount) {
    restart();
    return;
  }

  const Grid g = grid();
  const int size = kSizes[sizeIndex];
  const int tx = g.x + (target % g.cols) * g.step;
  const int ty = g.y + (target / g.cols) * g.step;
  if (x >= tx && x < tx + size && y >= ty && y < ty + size) hits[sizeIndex]++;
  const int dx = x - (tx + size / 2);
  const int dy = y - (ty + size / 2);
  dxSum[sizeIndex] += dx;
  dySum[sizeIndex] += dy;
  if (tapCount < kTotalTaps) {
    tapDx[tapCount] = static_cast<int16_t>(dx);
    tapDy[tapCount] = static_cast<int16_t>(dy);
    tapCount++;
  }
  hasLast = true;
  lastTapX = x;
  lastTapY = y;
  lastAimX = tx + size / 2;
  lastAimY = ty + size / 2;
  offsetSum[sizeIndex] += static_cast<int>(std::lround(std::sqrt(static_cast<float>(dx * dx + dy * dy))));

  if (++tapIndex >= kTapsPerSize) {
    tapIndex = 0;
    sizeIndex++;
  }
  if (sizeIndex < kSizeCount) pickTarget();
  requestUpdate();
}

void TapTargetTestActivity::drawShift(const int x, const int y, const int dx, const int dy) const {
  char line[64];
  snprintf(line, sizeof(line), "%d px %s, %d px %s", std::abs(dy), dy < 0 ? tr(STR_TAP_DIR_UP) : tr(STR_TAP_DIR_DOWN),
           std::abs(dx), dx < 0 ? tr(STR_TAP_DIR_LEFT) : tr(STR_TAP_DIR_RIGHT));
  renderer.drawText(themedFontId(UI_10_FONT_ID), x, y, line);
}

void TapTargetTestActivity::drawRing(const int cx, const int cy, const int radius, const int thickness,
                                     const bool black) const {
  const int outer = radius * radius;
  const int inner = (radius - thickness) * (radius - thickness);
  for (int dy = -radius; dy <= radius; dy++) {
    for (int dx = -radius; dx <= radius; dx++) {
      const int d = dx * dx + dy * dy;
      if (d <= outer && d > inner) renderer.drawPixel(cx + dx, cy + dy, black);
    }
  }
}

// Cross at the aimed centre, ring where the tap was detected; white halos keep
// both readable over the black target.
void TapTargetTestActivity::drawTapMark(const int tapX, const int tapY, const int aimX, const int aimY) const {
  renderer.fillRect(aimX - 13, aimY - 3, 27, 7, false);
  renderer.fillRect(aimX - 3, aimY - 13, 7, 27, false);
  renderer.fillRect(aimX - 11, aimY - 1, 23, 3, true);
  renderer.fillRect(aimX - 1, aimY - 11, 3, 23, true);
  drawRing(tapX, tapY, 18, 7, false);
  drawRing(tapX, tapY, 16, 3, true);
}

// All taps as dots around one target centre, at 2x scale, with the 44 and 56 px
// target outlines for reference.
void TapTargetTestActivity::drawScatter(const int top, const int bottom) const {
  const int width = renderer.getScreenWidth();
  const int cx = width / 2;
  const int cy = (top + bottom) / 2;
  const int half = std::min((bottom - top) / 2, width / 2 - 8);
  if (half < 40) return;
  constexpr int kScale = 2;
  renderer.drawLine(cx - half, cy, cx + half, cy, true);
  renderer.drawLine(cx, cy - half, cx, cy + half, true);
  for (const int size : {44, 56}) {
    const int h = std::min(half, size * kScale / 2);
    renderer.drawRect(cx - h, cy - h, 2 * h, 2 * h, true);
  }
  for (int i = 0; i < tapCount; i++) {
    const int px = std::clamp(cx + tapDx[i] * kScale, cx - half, cx + half);
    const int py = std::clamp(cy + tapDy[i] * kScale, cy - half, cy + half);
    renderer.fillRect(px - 3, py - 3, 7, 7, true);
  }
}

void TapTargetTestActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int font = themedFontId(UI_10_FONT_ID);
  const int lineHeight = renderer.getLineHeight(font);
  char line[96];

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_TAP_TARGET_TEST));
  const int left = metrics.contentSidePadding;
  int y = metrics.topPadding + metrics.headerHeight + 8;

  if (sizeIndex >= kSizeCount) {
    renderer.drawText(font, left, y, tr(STR_TAP_TARGET_RESULTS), true, EpdFontFamily::BOLD);
    y += lineHeight + 8;
    int dxAll = 0;
    int dyAll = 0;
    for (int i = 0; i < kSizeCount; i++) {
      snprintf(line, sizeof(line), "%d px  %d.%d mm   %d/%d   %s %d px", kSizes[i], tenthsOfMm(kSizes[i]) / 10,
               tenthsOfMm(kSizes[i]) % 10, hits[i], kTapsPerSize, tr(STR_TAP_TARGET_OFFSET),
               offsetSum[i] / kTapsPerSize);
      renderer.drawText(font, left, y, line);
      y += lineHeight;
      drawShift(left + 24, y, dxSum[i] / kTapsPerSize, dySum[i] / kTapsPerSize);
      y += lineHeight + 6;
      dxAll += dxSum[i];
      dyAll += dySum[i];
    }
    renderer.drawText(font, left, y, tr(STR_TAP_TARGET_ALL), true, EpdFontFamily::BOLD);
    y += lineHeight;
    drawShift(left + 24, y, dxAll / (kSizeCount * kTapsPerSize), dyAll / (kSizeCount * kTapsPerSize));
    y += lineHeight;
    const int againY = renderer.getScreenHeight() - 16 - lineHeight;
    drawScatter(y + 8, againY - 8);
    renderer.drawText(font, left, againY, tr(STR_TAP_TARGET_AGAIN));
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  const int size = kSizes[sizeIndex];
  renderer.drawText(font, left, y, tr(STR_TAP_TARGET_PROMPT));
  y += lineHeight;
  snprintf(line, sizeof(line), "%d px  %d.%d mm   %d / %d", size, tenthsOfMm(size) / 10, tenthsOfMm(size) % 10, tapIndex + 1,
           kTapsPerSize);
  renderer.drawText(font, left, y, line);

  const Grid g = grid();
  for (int i = 0; i < g.cols * g.rows; i++) {
    const int cx = g.x + (i % g.cols) * g.step;
    const int cy = g.y + (i / g.cols) * g.step;
    if (i == target) {
      renderer.fillRect(cx, cy, size, size, true);
    } else {
      renderer.drawRect(cx, cy, size, size, true);
    }
  }
  if (hasLast) drawTapMark(lastTapX, lastTapY, lastAimX, lastAimY);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
