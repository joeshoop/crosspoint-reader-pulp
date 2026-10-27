#include "RefreshTestActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdlib>
#include <cstdio>

#include "MappedInputManager.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Seven-segment bit masks (bit 0 = top, clockwise, bit 6 = middle) for 0-9, A-F.
constexpr uint8_t kSegments[] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07,
                                 0x7F, 0x6F, 0x77, 0x7C, 0x39, 0x5E, 0x79, 0x71};
constexpr int kGlyphCount = static_cast<int>(sizeof(kSegments));
constexpr int kHistoryWindowMs = 3000;
constexpr int kSliderBand = 28;  // touch slop above and below the track
}  // namespace

void RefreshTestActivity::onEnter() {
  Activity::onEnter();
  nextDueMs = 0;
  resetStats();
  requestUpdate();
}

RefreshTestActivity::Layout RefreshTestActivity::layout() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const int lineHeight = renderer.getLineHeight(themedFontId(UI_10_FONT_ID));
  const int bottom = height - (mappedInput.hasTouch() ? 16 : metrics.buttonHintsHeight + 8);

  Layout l{};
  l.left = metrics.contentSidePadding;
  l.right = width - metrics.contentSidePadding;
  l.top = metrics.topPadding + metrics.headerHeight + 8;
  l.chipHeight = 48;
  l.chipsTop = bottom - l.chipHeight;
  l.sliderY = l.chipsTop - 40;
  l.statsTop = l.sliderY - 32 - 3 * lineHeight;
  l.blinkerBottom = l.statsTop - 12;
  return l;
}

void RefreshTestActivity::resetStats() { statsEpoch.fetch_add(1); }

void RefreshTestActivity::setRate(const int value) {
  const int clamped = std::clamp(value, 0, kMaxRate);
  if (clamped == rate.load()) return;
  rate.store(clamped);
  nextDueMs = 0;
  resetStats();
  requestUpdate();
}

void RefreshTestActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) setRate(rate.load() - 1);
  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) setRate(rate.load() + 1);
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    cleanMode.store(!cleanMode.load());
    resetStats();
    requestUpdate();
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    fullSize.store(!fullSize.load());
    resetStats();
    requestUpdate();
  }

  if (mappedInput.hasTouch()) {
    const Layout l = layout();
    int x = 0;
    int y = 0;
    // Slider: follow the finger while it is held on the track band.
    if (mappedInput.isScreenTouchHeld(x, y) && (dragRate >= 0 || std::abs(y - l.sliderY) <= kSliderBand)) {
      const int span = std::max(1, l.right - l.left);
      dragRate = std::clamp(((x - l.left) * kMaxRate + span / 2) / span, 0, kMaxRate);
      setRate(dragRate);
    } else {
      dragRate = -1;
    }
    // Press edge in the test area: time it to the refresh that shows the mark.
    if (dragRate < 0 && mappedInput.wasScreenTouchPressed(x, y) && y >= l.top && y < l.blinkerBottom) {
      tapX.store(x);
      tapY.store(y);
      tapAtMs.store(millis());
      tapPending.store(true);
      requestUpdate();
    }
    if (mappedInput.wasScreenTapped(x, y) && y >= l.chipsTop && y < l.chipsTop + l.chipHeight) {
      if (x < (l.left + l.right) / 2) {
        cleanMode.store(!cleanMode.load());
      } else {
        fullSize.store(!fullSize.load());
      }
      resetStats();
      requestUpdate();
    }
  }

  // Frame clock: every tick advances the character. Ticks that pass while the
  // panel is still busy coalesce into one render; the render counts the skip.
  const int hz = rate.load();
  if (hz <= 0) {
    nextDueMs = 0;
    return;
  }
  const uint32_t period = 1000 / hz;
  const uint32_t now = millis();
  if (nextDueMs == 0) nextDueMs = now;
  if (static_cast<int32_t>(now - nextDueMs) < 0) return;
  const uint32_t ticks = 1 + (now - nextDueMs) / period;
  nextDueMs += ticks * period;
  frame.fetch_add(ticks);
  requestUpdate();
}

void RefreshTestActivity::drawGlyph(const int x, const int y, const int w, const int h, const int t,
                                    const uint8_t segments, const bool black) const {
  const int mid = y + h / 2;
  const int gap = std::max(2, t / 5);
  const int upperH = mid - t / 2 - (y + t) - 2 * gap;
  const int lowerTop = mid + t / 2 + gap;
  const int lowerH = (y + h - t) - lowerTop - gap;
  const int barW = w - 2 * t - 2 * gap;
  if (segments & 0x01) renderer.fillRect(x + t + gap, y, barW, t, black);
  if (segments & 0x02) renderer.fillRect(x + w - t, y + t + gap, t, upperH, black);
  if (segments & 0x04) renderer.fillRect(x + w - t, lowerTop, t, lowerH, black);
  if (segments & 0x08) renderer.fillRect(x + t + gap, y + h - t, barW, t, black);
  if (segments & 0x10) renderer.fillRect(x, lowerTop, t, lowerH, black);
  if (segments & 0x20) renderer.fillRect(x, y + t + gap, t, upperH, black);
  if (segments & 0x40) renderer.fillRect(x + t + gap, mid - t / 2, barW, t, black);
}

void RefreshTestActivity::drawChip(const int x, const int y, const int w, const int h, const char* label,
                                   const bool filled) const {
  const int font = themedFontId(UI_10_FONT_ID);
  if (filled) {
    renderer.fillRect(x, y, w, h, true);
  } else {
    renderer.drawRect(x, y, w, h, 2, true);
  }
  const int textW = renderer.getTextWidth(font, label);
  const int textY = y + (h - renderer.getLineHeight(font)) / 2;
  renderer.drawText(font, x + (w - textW) / 2, textY, label, !filled);
}

void RefreshTestActivity::render(RenderLock&&) {
  const uint32_t epoch = statsEpoch.load();
  const uint32_t current = frame.load();
  if (epoch != shownEpoch) {
    shownEpoch = epoch;
    dropped = 0;
    shownCount = 0;
    shownHead = 0;
    lastRefreshMs = 0;
    lastShownFrame = current;
  }
  const bool advanced = current != lastShownFrame;
  if (advanced) dropped += current - lastShownFrame - 1;
  lastShownFrame = current;

  const int hz = rate.load();
  const bool clean = cleanMode.load();
  const bool full = fullSize.load();
  const Layout l = layout();
  const int width = renderer.getScreenWidth();
  const int font = themedFontId(UI_10_FONT_ID);
  const int lineHeight = renderer.getLineHeight(font);

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, UITheme::getInstance().getMetrics().topPadding, width,
                                UITheme::getInstance().getMetrics().headerHeight},
                 tr(STR_REFRESH_TEST));

  // Test area. Full size also inverts the whole area on odd frames so every
  // pixel changes, not only the segments.
  const int areaH = l.blinkerBottom - l.top;
  const bool inverted = full && (current & 1);
  if (inverted) renderer.fillRect(0, l.top, width, areaH, true);
  const int glyphH = full ? areaH - 16 : std::min(160, areaH - 16);
  const int glyphW = std::min(glyphH * 3 / 5, l.right - l.left);
  drawGlyph((width - glyphW) / 2, l.top + (areaH - glyphH) / 2, glyphW, glyphH, std::max(6, glyphH / 9),
            kSegments[current % kGlyphCount], !inverted);

  const bool tapped = tapPending.exchange(false);
  // Read now: a second tap during this refresh must not shorten this reading.
  const uint32_t tapAt = tapAtMs.load();
  if (tapped) {
    const int mx = std::clamp(tapX.load(), l.left + 22, l.right - 22);
    const int my = std::clamp(tapY.load(), l.top + 22, l.blinkerBottom - 22);
    renderer.fillRect(mx - 12, my - 12, 24, 24, !inverted);
    renderer.drawRect(mx - 22, my - 22, 44, 44, 3, !inverted);
  }

  // Stats (from the previous refresh; this one is still to be measured).
  char line[96];
  char actual[24];
  if (hz == 0) {
    snprintf(actual, sizeof(actual), "%s", tr(STR_REFRESH_PAUSED));
  } else if (shownCount >= 2) {
    const int newest = (shownHead + kHistory - 1) % kHistory;
    const int oldest = (shownHead + kHistory - shownCount) % kHistory;
    const uint32_t span = shownAt[newest] - shownAt[oldest];
    const uint32_t perTen = span ? (shownCount - 1) * 10000 / span : 0;
    snprintf(actual, sizeof(actual), "%lu.%lu/s", static_cast<unsigned long>(perTen / 10),
             static_cast<unsigned long>(perTen % 10));
  } else {
    snprintf(actual, sizeof(actual), "-");
  }
  int y = l.statsTop;
  snprintf(line, sizeof(line), "%s %d/s   %s %s", tr(STR_REFRESH_TARGET), hz, tr(STR_REFRESH_ACTUAL), actual);
  renderer.drawText(font, l.left, y, line);
  y += lineHeight;
  snprintf(line, sizeof(line), "%s %lu ms   %s %lu", tr(STR_REFRESH_LAST), static_cast<unsigned long>(lastRefreshMs),
           tr(STR_REFRESH_DROPPED), static_cast<unsigned long>(dropped));
  renderer.drawText(font, l.left, y, line);
  y += lineHeight;
  if (mappedInput.hasTouch()) {
    if (tapLatencyMs > 0) {
      snprintf(line, sizeof(line), "%s %lu ms", tr(STR_REFRESH_TAP_LATENCY), static_cast<unsigned long>(tapLatencyMs));
    } else {
      snprintf(line, sizeof(line), "%s", tr(STR_REFRESH_TAP_HINT));
    }
    renderer.drawText(font, l.left, y, line);
  }

  // Rate slider.
  const int span = l.right - l.left;
  renderer.fillRect(l.left, l.sliderY - 1, span, 3, true);
  for (int i = 0; i <= kMaxRate; i += 5) {
    renderer.fillRect(l.left + i * span / kMaxRate - 1, l.sliderY - 8, 3, 16, true);
  }
  const int thumbX = l.left + hz * span / kMaxRate;
  renderer.fillRect(thumbX - 8, l.sliderY - 16, 16, 32, true);

  // Mode and size chips.
  const int chipGap = 12;
  const int chipW = (span - chipGap) / 2;
  snprintf(line, sizeof(line), "%s", clean ? tr(STR_REFRESH_MODE_CLEAN) : tr(STR_REFRESH_MODE_FAST));
  drawChip(l.left, l.chipsTop, chipW, l.chipHeight, line, clean);
  snprintf(line, sizeof(line), "%s", full ? tr(STR_REFRESH_SIZE_FULL) : tr(STR_REFRESH_SIZE_SMALL));
  drawChip(l.left + chipW + chipGap, l.chipsTop, chipW, l.chipHeight, line, full);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_REFRESH_MODE), "-", "+");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  const uint32_t start = millis();
  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  const uint32_t done = millis();
  lastRefreshMs = done - start;
  if (advanced) {
    shownAt[shownHead] = done;
    shownHead = (shownHead + 1) % kHistory;
    shownCount = std::min(shownCount + 1, kHistory);
    // Keep only the trailing window so the rate follows slider changes.
    while (shownCount > 2 && done - shownAt[(shownHead + kHistory - shownCount) % kHistory] > kHistoryWindowMs) {
      shownCount--;
    }
  }
  if (tapped) {
    tapLatencyMs = done - tapAt;
    // Paused, nothing else would repaint the stats with the new reading.
    requestUpdate();
  }
}
