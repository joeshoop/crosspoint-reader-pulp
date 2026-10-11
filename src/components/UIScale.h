#pragma once
#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"

// FreeInkUI font slots. Row heights, header height, and touch sizes are not
// chosen here: FreeInkApp derives all metric tokens from the body font's line
// height (themeTokensForLineHeight). One fixed tier for every board — the
// user-facing UI-scale setting was removed.
struct UIScaleSpec {
  int smallFontId;
  int bodyFontId;
  int titleFontId;
};

inline UIScaleSpec uiScaleSpec() {
  UIScaleSpec spec{};
  if (UITheme::getInstance().getMetrics().uiUsesReaderFont) {
    // Book-styled themes set every slot in the reader font, at one size.
    const int readerFontId = SETTINGS.getReaderFontId();
    spec.smallFontId = readerFontId;
    spec.bodyFontId = readerFontId;
    spec.titleFontId = readerFontId;
    return spec;
  }
  spec.smallFontId = UI_10_FONT_ID;
  spec.bodyFontId = UI_12_FONT_ID;
  // Titles use the UI font, not a reader font: fui headers draw book and
  // directory titles, and the built-in Ubuntu UI fonts cover Hebrew (plus the
  // size-matched SD CJK fallback) where the NotoSans reader subsets do not.
  // Same font develop's drawHeader used, so script coverage matches develop.
  spec.titleFontId = UI_12_FONT_ID;
  return spec;
}
