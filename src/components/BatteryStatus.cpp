#include "BatteryStatus.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>

#include <cstdio>

#include "components/FloatButtons.h"
#include "components/UIScale.h"
#include "components/icons/readerFloatIcons.h"

namespace battery_status {

namespace {
constexpr int kRearmPercent = 20;
// The level moves slowly; the charging check can be a bus read.
constexpr unsigned long kNoticeCheckMs = 30000;
bool noticeShown = false;
unsigned long lastNoticeCheck = 0;
}  // namespace

int percent() { return powerManager.getBatteryPercentage(); }

bool charging() { return gpio.isUsbConnected(); }

bool isLow() {
  const int pct = percent();
  return pct > 0 && pct <= kLowPercent && !charging();
}

bool takeLowNotice() {
  const unsigned long now = millis();
  if (lastNoticeCheck != 0 && now - lastNoticeCheck < kNoticeCheckMs) return false;
  lastNoticeCheck = now;
  if (charging() || percent() > kRearmPercent) {
    noticeShown = false;
    return false;
  }
  if (noticeShown || !isLow()) return false;
  noticeShown = true;
  return true;
}

const freeink::Icon& icon() {
  if (charging()) return icon_float_battery_charging_28;
  const int pct = percent();
  if (pct > 60) return icon_float_battery_full_28;
  if (pct > kLowPercent) return icon_float_battery_medium_28;
  return icon_float_battery_low_28;
}

void drawBadge(const GfxRenderer& renderer, const int centerX, const int y) {
  char text[8];
  snprintf(text, sizeof(text), "%d%%", percent());
  const int font = readerChromeSansFontId();
  const freeink::Icon& ic = icon();
  constexpr int kSpace = 4;
  const int w = ic.w + kSpace + renderer.getTextWidth(font, text);
  const int x = centerX - w / 2;
  float_ui::drawIcon(renderer, ic, x, y);
  renderer.drawText(font, x + ic.w + kSpace, y + (kBadgeHeight - renderer.getLineHeight(font)) / 2, text);
}

}  // namespace battery_status
