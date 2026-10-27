#pragma once

#include <Icon.h>

#include <cstdint>

class GfxRenderer;

// Battery level for Pulp's touch chrome, which has no status bar.
namespace battery_status {

constexpr int kLowPercent = 15;
constexpr int kBadgeHeight = 28;

// 0 while the gauge has not been read yet.
int percent();
bool charging();
// At or below kLowPercent and not on USB power.
bool isLow();
// True once per low spell; it re-arms after charging or rising above 20%.
bool takeLowNotice();
// The battery icon for the current level and charging state.
const freeink::Icon& icon();
// Icon and "12%", centred on centerX, kBadgeHeight tall from y.
void drawBadge(const GfxRenderer& renderer, int centerX, int y);

}  // namespace battery_status
