#pragma once
#include "CrossPointSettings.h"
#include "ReaderFontSizes.h"

#include <iterator>
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

// Fixed UI font ids drawn directly (status screens, captions): book-styled
// themes set them in the reader font too; other themes keep the id as given.
inline int themedFontId(const int fontId) {
  return UITheme::getInstance().getMetrics().uiUsesReaderFont ? SETTINGS.getReaderFontId() : fontId;
}

// Sans for the reader's overlay chrome (float menu pills, Themes & Settings sheet): built-in
// Noto Sans at the size nearest the reader font's, whatever the book's face.
inline int readerChromeSansFontId() {
  const uint8_t pt = snapToNearestPointSize(BUILTIN_READER_POINT_SIZES, std::size(BUILTIN_READER_POINT_SIZES),
                                            SETTINGS.fontPointSize);
  switch (pt) {
    case 12:
      return NOTOSANS_12_FONT_ID;
    case 16:
      return NOTOSANS_16_FONT_ID;
    case 18:
      return NOTOSANS_18_FONT_ID;
    case 14:
    default:
      return NOTOSANS_14_FONT_ID;
  }
}
