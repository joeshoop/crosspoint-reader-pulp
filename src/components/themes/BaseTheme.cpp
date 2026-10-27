#include "BaseTheme.h"

#include <FreeInkUIGfxRenderer.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>

#include "I18n.h"
#include "RecentBooksStore.h"
#include "clippings/SelectionGeometry.h"
#include "components/HeaderBackTapTarget.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "components/icons/bookmark.h"
#include "components/icons/cover.h"
#include "components/icons/headerIcons.h"
#include "fontIds.h"

freeink::ui::BitmapRef BaseTheme::checkboxIcon(const bool checked) {
  return freeink::ui::bitmapFromIcon(checked ? icon_checkbox_on_32 : icon_checkbox_off_32);
}

freeink::ui::BitmapRef BaseTheme::shelfBookIcon() const { return {}; }

void BaseTheme::setCheckboxRow(freeink::ui::ListItem& item, const bool checked) {
  item.value = nullptr;
  item.toggle = true;
  item.toggleChecked = checked;
}

// Internal constants
namespace {
constexpr int homeMenuMargin = 20;
constexpr int homeMarginTop = 30;
constexpr int subtitleY = 738;
constexpr int bookmarkStatusIconWidth = 16;
constexpr int bookmarkStatusIconHeight = 14;
constexpr int bookmarkStatusIconGap = 4;
constexpr int bookmarkStatusIconTopCrop = 2;

void drawBookmarkStatusIcon(const GfxRenderer& renderer, const int x, const int y) {
  constexpr int bytesPerRow = bookmarkStatusIconWidth / 8;
  for (int row = 0; row < bookmarkStatusIconHeight; ++row) {
    for (int col = 0; col < bookmarkStatusIconWidth; ++col) {
      const uint8_t byte = BookmarkStatusIcon[(row + bookmarkStatusIconTopCrop) * bytesPerRow + col / 8];
      const uint8_t mask = 1U << (7 - (col % 8));
      renderer.drawPixel(x + col, y + row, (byte & mask) != 0);
    }
  }
}

// "Chapter 3: The Ring Goes South" -> "The Ring Goes South". Only trims when
// a name follows a separator (: . - en/em dash), so "Chapter 3" stays. The
// number may be digits, Roman numerals or words ("Twenty-One").
void trimChapterPrefix(std::string& text) {
  const auto lowerAt = [&text](const size_t i) {
    const char c = text[i];
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
  };
  const auto startsWith = [&](const size_t i, const char* word) {
    const size_t n = strlen(word);
    if (i + n > text.size()) return false;
    for (size_t k = 0; k < n; k++) {
      if (lowerAt(i + k) != word[k]) return false;
    }
    return true;
  };
  const auto skipSpaces = [&text](size_t i) {
    while (i < text.size() && text[i] == ' ') i++;
    return i;
  };

  size_t i = skipSpaces(0);
  if (startsWith(i, "chapter")) {
    i += 7;
  } else if (startsWith(i, "ch.")) {
    i += 3;
  } else {
    return;
  }
  if (i >= text.size() || text[i] != ' ') return;
  i = skipSpaces(i);

  // The number: one token of letters, digits and hyphens.
  const size_t numberStart = i;
  while (i < text.size() && (isalnum(static_cast<unsigned char>(text[i])) || text[i] == '-')) i++;
  if (i == numberStart) return;
  std::string number;
  for (size_t k = numberStart; k < i; k++) number += lowerAt(k);
  static constexpr const char* kNumberWords[] = {
      "one",    "two",    "three",    "four",     "five",    "six",     "seven",     "eight",    "nine",     "ten",
      "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen", "twenty",
      "thirty", "forty",  "fifty",    "sixty",    "seventy", "eighty",  "ninety",    "hundred"};
  const auto isNumberWord = [](const std::string& part) {
    return std::any_of(std::begin(kNumberWords), std::end(kNumberWords), [&part](const char* w) { return part == w; });
  };
  bool isNumber = number.find_first_not_of("0123456789") == std::string::npos ||
                  number.find_first_not_of("ivxlc") == std::string::npos;
  if (!isNumber) {
    isNumber = true;
    size_t from = 0;
    while (from <= number.size()) {
      const size_t dash = number.find('-', from);
      const size_t to = dash == std::string::npos ? number.size() : dash;
      if (!isNumberWord(number.substr(from, to - from))) isNumber = false;
      from = to + 1;
    }
  }
  if (!isNumber) return;

  i = skipSpaces(i);
  if (i < text.size() && (text[i] == ':' || text[i] == '.' || text[i] == '-')) {
    i++;
  } else if (text.compare(i, 3, "\xE2\x80\x93") == 0 || text.compare(i, 3, "\xE2\x80\x94") == 0) {
    i += 3;  // en or em dash
  } else {
    return;
  }
  i = skipSpaces(i);
  if (i >= text.size()) return;
  text.erase(0, i);
}

// "THE RING GOES SOUTH" -> "The Ring Goes South". Titles with any lowercase
// letter are left alone. Handles ASCII and Latin-1 accents; Roman numerals
// (II, XIV) stay capital, and short joining words go lowercase except at the
// start or after a colon.
void titleCaseIfAllCaps(std::string& text) {
  const auto at = [&text](const size_t i) { return static_cast<uint8_t>(text[i]); };
  // UTF-8 À-Þ is C3 80-9E (except × at 97); à-þ is C3 A0-BE.
  const auto isAccentUpper = [&](const size_t i) {
    return at(i) == 0xC3 && i + 1 < text.size() && at(i + 1) >= 0x80 && at(i + 1) <= 0x9E && at(i + 1) != 0x97;
  };
  for (size_t i = 0; i < text.size(); i++) {
    if (at(i) >= 'a' && at(i) <= 'z') return;
    if (at(i) == 0xC3 && i + 1 < text.size() && at(i + 1) >= 0xA0 && at(i + 1) <= 0xBE) return;
  }
  static constexpr const char* kSmallWords[] = {"a",    "an", "and", "as", "at", "but", "by", "for",
                                                "from", "in", "of",  "on", "or", "the", "to", "with"};
  const auto isUpper = [&](const size_t i) { return (at(i) >= 'A' && at(i) <= 'Z') || isAccentUpper(i); };
  bool capitalizeNext = true;
  size_t i = 0;
  while (i < text.size()) {
    if (!isUpper(i)) {
      if (text[i] == ':') capitalizeNext = true;
      i++;
      continue;
    }
    size_t end = i;
    while (end < text.size() && (isUpper(end) || text[end] == '\'' || at(end) >= 0x80))
      end += isAccentUpper(end) ? 2 : 1;
    std::string word = text.substr(i, end - i);
    const bool roman = word.find_first_not_of("IVXLC") == std::string::npos;
    std::string lower = word;
    for (size_t k = 0; k < lower.size(); k++) {
      const auto c = static_cast<uint8_t>(lower[k]);
      if (c >= 'A' && c <= 'Z') {
        lower[k] = static_cast<char>(c - 'A' + 'a');
      } else if (c == 0xC3 && k + 1 < lower.size()) {
        const auto next = static_cast<uint8_t>(lower[k + 1]);
        if (next >= 0x80 && next <= 0x9E && next != 0x97) lower[k + 1] = static_cast<char>(next + 0x20);
        k++;
      }
    }
    const bool small = !capitalizeNext && std::any_of(std::begin(kSmallWords), std::end(kSmallWords),
                                                      [&lower](const char* w) { return lower == w; });
    if (!roman) {
      // Keep the first letter (one or two bytes) as it was.
      const size_t firstLen = isAccentUpper(i) ? 2 : 1;
      if (!small) lower.replace(0, firstLen, word, 0, firstLen);
      text.replace(i, end - i, lower);
    }
    capitalizeNext = false;
    i = end;
  }
}

}  // namespace

void shortenChapterTitle(std::string& title) {
  trimChapterPrefix(title);
  titleCaseIfAllCaps(title);
}

void BaseTheme::drawBatteryOutline(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight) {
  // Top line
  renderer.drawLine(x + 1, y, x + battWidth - 3, y);
  // Bottom line
  renderer.drawLine(x + 1, y + rectHeight - 1, x + battWidth - 3, y + rectHeight - 1);
  // Left line
  renderer.drawLine(x, y + 1, x, y + rectHeight - 2);
  // Battery end
  renderer.drawLine(x + battWidth - 2, y + 1, x + battWidth - 2, y + rectHeight - 2);
  renderer.drawPixel(x + battWidth - 1, y + 3);
  renderer.drawPixel(x + battWidth - 1, y + rectHeight - 4);
  renderer.drawLine(x + battWidth - 0, y + 4, x + battWidth - 0, y + rectHeight - 5);
}

void BaseTheme::drawBatteryLightningBolt(const GfxRenderer& renderer, int boltX, int boltY) {
  // Draw lightning bolt (white/inverted on black fill for visibility)
  renderer.drawLine(boltX + 4, boltY + 0, boltX + 5, boltY + 0, false);
  renderer.drawLine(boltX + 3, boltY + 1, boltX + 4, boltY + 1, false);
  renderer.drawLine(boltX + 2, boltY + 2, boltX + 5, boltY + 2, false);
  renderer.drawLine(boltX + 3, boltY + 3, boltX + 4, boltY + 3, false);
  renderer.drawLine(boltX + 2, boltY + 4, boltX + 3, boltY + 4, false);
  renderer.drawLine(boltX + 1, boltY + 5, boltX + 4, boltY + 5, false);
  renderer.drawLine(boltX + 2, boltY + 6, boltX + 3, boltY + 6, false);
  renderer.drawLine(boltX + 1, boltY + 7, boltX + 2, boltY + 7, false);
}

void BaseTheme::fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const {
  const bool charging = gpio.isUsbConnected();

  const int maxFillWidth = rect.width - 5;
  const int fillHeight = rect.height - 4;
  if (maxFillWidth <= 0 || fillHeight <= 0) {
    return;
  }
  // +1 to round up so we always fill at least one pixel
  int filledWidth = percentage * maxFillWidth / 100 + 1;
  if (filledWidth > maxFillWidth) {
    filledWidth = maxFillWidth;
  }

  // When charging, ensure minimum fill so lightning bolt is fully visible
  constexpr int minFillForBolt = 8;
  if (charging && filledWidth < minFillForBolt) {
    filledWidth = std::min(minFillForBolt, maxFillWidth);
  }

  renderer.fillRect(rect.x + 2, rect.y + 2, filledWidth, fillHeight);

  if (charging) {
    drawBatteryLightningBolt(renderer, rect.x + 4, rect.y + 2);
  }
}

void BaseTheme::drawBatteryLeft(const GfxRenderer& renderer, Rect rect, const bool showPercentage) const {
  // Left aligned: icon on left, percentage on right (reader mode)
  const uint16_t percentage = powerManager.getBatteryPercentage();
  const int y = rect.y + 6;

  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    renderer.drawText(SMALL_FONT_ID, rect.x + batteryPercentSpacing + rect.width, rect.y, percentageText.c_str());
  }

  const Rect iconRect{rect.x, y, rect.width, rect.height};
  drawBatteryOutline(renderer, rect.x, y, rect.width, rect.height);
  fillBatteryIcon(renderer, iconRect, percentage);
}

void BaseTheme::drawCoverPlaceholder(const GfxRenderer& renderer, Rect rect) {
  if (rect.width <= 0 || rect.height <= 0) return;
  const int topHeight = rect.height / 3;
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  renderer.fillRect(rect.x, rect.y + topHeight, rect.width, rect.height - topHeight, true);
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height, true);
  constexpr int ICON_SIZE = 32;
  if (rect.width >= ICON_SIZE + 4 && topHeight >= ICON_SIZE + 4) {
    const int insetX = std::min(24, (rect.width - ICON_SIZE) / 2);
    const int insetY = std::min(24, (topHeight - ICON_SIZE) / 2);
    renderer.drawIcon(CoverIcon, rect.x + insetX, rect.y + insetY, ICON_SIZE);
  }
}

bool BaseTheme::drawCoverThumbFill(const GfxRenderer& renderer, const Bitmap& bitmap, Rect slot, const int xOffset) {
  if (slot.width <= 0 || slot.height <= 0) return false;
  // xOffset nudges the centered art sideways; the clip stays on the slot.
  const int x = slot.x + (slot.width - bitmap.getWidth()) / 2 + xOffset;
  const int y = slot.y + (slot.height - bitmap.getHeight()) / 2;
  const auto clip = renderer.getClipRect();
  const int left = std::max(slot.x, clip[0]);
  const int top = std::max(slot.y, clip[1]);
  renderer.setClipRect(left, top, std::max(0, std::min(slot.x + slot.width, clip[0] + clip[2]) - left),
                       std::max(0, std::min(slot.y + slot.height, clip[1] + clip[3]) - top));
  const bool drawn = renderer.drawBitmap(bitmap, x, y, bitmap.getWidth(), bitmap.getHeight());
  renderer.setClipRect(clip[0], clip[1], clip[2], clip[3]);
  return drawn;
}

void BaseTheme::drawProgressBar(const GfxRenderer& renderer, Rect rect, const size_t current, const size_t total) {
  if (total == 0) {
    return;
  }

  // Use 64-bit arithmetic to avoid overflow for large files
  const int percent = static_cast<int>((static_cast<uint64_t>(current) * 100) / total);

  LOG_DBG("UI", "Drawing progress bar: current=%u, total=%u, percent=%d", current, total, percent);
  // Draw outline
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);

  // Draw filled portion
  const int fillWidth = (rect.width - 4) * percent / 100;
  if (fillWidth > 0) {
    renderer.fillRect(rect.x + 2, rect.y + 2, fillWidth, rect.height - 4);
  }

  // Draw percentage text centered below bar
  const std::string percentText = std::to_string(percent) + "%";
  renderer.drawCenteredText(UI_10_FONT_ID, rect.y + rect.height + 15, percentText.c_str());
}

// Centre hint labels by text height, wrapping to at most two lines.
void BaseTheme::drawHintLabel(const GfxRenderer& renderer, const int fontId, const char* label, const int x,
                              const int boxWidth, const int boxTop, const int boxHeight) {
  constexpr int textPadding = 4;  // keeps a wrapped label off the button's border
  const int maxTextWidth = boxWidth - (textPadding * 2);

  const int textHeight = renderer.getTextHeight(fontId);
  const int textWidth = renderer.getTextWidth(fontId, label);
  if (textWidth <= maxTextWidth) {
    const int textY = boxTop + std::max(1, (boxHeight - textHeight) / 2);
    renderer.drawText(fontId, x + (boxWidth - 1 - textWidth) / 2, textY, label);
    return;
  }

  // Spaced by the glyph height, not getLineHeight() — that returns the font's
  // full advanceY (leading included), which stacks two lines taller than the
  // button and clips the second one.
  constexpr int lineGap = 2;
  const int step = textHeight + lineGap;
  const auto lines = renderer.wrappedText(fontId, label, maxTextWidth, 2);
  const int block = static_cast<int>(lines.size()) * step - lineGap;
  int lineY = boxTop + std::max(1, (boxHeight - block) / 2);
  for (const auto& line : lines) {
    const int lineWidth = renderer.getTextWidth(fontId, line.c_str());
    renderer.drawText(fontId, x + (boxWidth - 1 - lineWidth) / 2, lineY, line.c_str());
    lineY += step;
  }
}

bool BaseTheme::buttonHintsHidden() {
  return gpio.hasTouch() || SETTINGS.buttonLabels == CrossPointSettings::LABELS_HIDE;
}

void BaseTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4) const {
  if (buttonHintsHidden()) {
    return;
  }

  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageHeight = renderer.getScreenHeight();
  constexpr int buttonWidth = 106;
  constexpr int buttonHeight = BaseMetrics::values.buttonHintsHeight;
  constexpr int buttonY = BaseMetrics::values.buttonHintsHeight;  // Distance from bottom
  // Keyed to the portrait panel width: the 528-wide X3 gets more spacing than
  // the 480-wide boards (X4, X4 Pro, and the other 800x480 panels).
  constexpr int narrowButtonPositions[] = {25, 130, 245, 350};
  constexpr int wideButtonPositions[] = {38, 154, 268, 384};
  const int* buttonPositions = renderer.getScreenWidth() >= 528 ? wideButtonPositions : narrowButtonPositions;
  const char* labels[] = {btn1, btn2, btn3, btn4};
  const bool grayscale = renderer.getRenderMode() != GfxRenderer::BW && !renderer.grayPlanesAreAbsolute();

  for (int i = 0; i < 4; i++) {
    // Only draw if the label is non-empty
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int x = buttonPositions[i];
      // Zero gray-plane bits leave the monochrome hint from the base pass intact.
      renderer.fillRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, grayscale);
      if (grayscale) continue;
      renderer.drawRect(x, pageHeight - buttonY, buttonWidth, buttonHeight);
      drawHintLabel(renderer, UI_10_FONT_ID, labels[i], x, buttonWidth, pageHeight - buttonY, buttonHeight);
    }
  }

  renderer.setOrientation(orig_orientation);
}

void BaseTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const {
  if (buttonHintsHidden()) {
    return;
  }

  const int screenWidth = renderer.getScreenWidth();
  constexpr int buttonWidth = BaseMetrics::values.sideButtonHintsWidth;  // Width on screen (height when rotated)
  constexpr int buttonHeight = 80;                                       // Height on screen (width when rotated)
  constexpr int buttonMargin = 4;

  if (gpio.hasEdgeSideButtons()) {
    // Edge-button layout (X3, X4 Pro): Up on left side, Down on right side, positioned higher
    constexpr int x3ButtonY = 155;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      const int leftX = buttonMargin;
      renderer.drawRect(leftX, x3ButtonY, buttonWidth, buttonHeight);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, topBtn);
      const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
      const int textX = leftX + (buttonWidth - textHeight) / 2;
      const int textY = x3ButtonY + (buttonHeight + textWidth) / 2;
      renderer.drawTextRotated90CW(SMALL_FONT_ID, textX, textY, topBtn);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      const int rightX = screenWidth - buttonMargin - buttonWidth;
      renderer.drawRect(rightX, x3ButtonY, buttonWidth, buttonHeight);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, bottomBtn);
      const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
      const int textX = rightX + (buttonWidth - textHeight) / 2;
      const int textY = x3ButtonY + (buttonHeight + textWidth) / 2;
      renderer.drawTextRotated90CW(SMALL_FONT_ID, textX, textY, bottomBtn);
    }
  } else {
    // X4 layout: Both buttons stacked on right side
    constexpr int topButtonY = 345;
    const char* labels[] = {topBtn, bottomBtn};
    const int x = screenWidth - buttonMargin - buttonWidth;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      renderer.drawLine(x, topButtonY, x + buttonWidth - 1, topButtonY);
      renderer.drawLine(x, topButtonY, x, topButtonY + buttonHeight - 1);
      renderer.drawLine(x + buttonWidth - 1, topButtonY, x + buttonWidth - 1, topButtonY + buttonHeight - 1);
    }

    if ((topBtn != nullptr && topBtn[0] != '\0') || (bottomBtn != nullptr && bottomBtn[0] != '\0')) {
      renderer.drawLine(x, topButtonY + buttonHeight, x + buttonWidth - 1, topButtonY + buttonHeight);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      renderer.drawLine(x, topButtonY + buttonHeight, x, topButtonY + 2 * buttonHeight - 1);
      renderer.drawLine(x + buttonWidth - 1, topButtonY + buttonHeight, x + buttonWidth - 1,
                        topButtonY + 2 * buttonHeight - 1);
      renderer.drawLine(x, topButtonY + 2 * buttonHeight - 1, x + buttonWidth - 1, topButtonY + 2 * buttonHeight - 1);
    }

    for (int i = 0; i < 2; i++) {
      if (labels[i] != nullptr && labels[i][0] != '\0') {
        const int y = topButtonY + i * buttonHeight;
        const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
        const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
        const int textX = x + (buttonWidth - textHeight) / 2;
        const int textY = y + (buttonHeight + textWidth) / 2;
        renderer.drawTextRotated90CW(SMALL_FONT_ID, textX, textY, labels[i]);
      }
    }
  }
}

// Slightly inside the side padding: the battery's boxed glyph and the clock
// digits read wider than text/cover ink on the same line, so flush placement
// looks like it overhangs the content columns.
int BaseTheme::headerStatusInset() { return UITheme::getInstance().getMetrics().headerSidePadding + 4; }

void BaseTheme::applyHeaderStatus(const GfxRenderer& renderer, freeink::ui::HeaderProps& props) {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  auto& status = props.status;

  // Status text stays at the fixed small font on every screen: FONT_LABEL is
  // bound to SMALL_FONT_ID by makeUiTarget() (FUI screens) and drawHeader()
  // (passive frames), while the uiScale FONT_SMALL is for list subtitles.
  status.battery.text.font = freeink::ui::GfxRendererTarget::FONT_LABEL;

  status.showBattery = metrics.headerShowsBattery;
  const uint16_t percentage = powerManager.getBatteryPercentage();
  status.battery.percent = static_cast<uint8_t>(percentage > 100 ? 100 : percentage);
  status.battery.charging = gpio.isUsbConnected();
  // Static label buffers: headers draw on the single render task, and the
  // strings only need to outlive the fui::header() call that consumes them.
  static char percentText[8];
  if (SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS) {
    snprintf(percentText, sizeof(percentText), "%u%%", static_cast<unsigned>(percentage));
    status.battery.label = percentText;
  }
  status.battery.glyphWidth = static_cast<int16_t>(metrics.batteryWidth);
  status.battery.glyphHeight = static_cast<int16_t>(metrics.batteryHeight);
  status.battery.gap = batteryPercentSpacing;
  status.batteryLeft = metrics.headerBatterySide == 1;
  status.edgeInset = static_cast<int16_t>(headerStatusInset());

  // Header chrome geometry. Status lives on the theme's thin top strip in
  // fixed corners — battery top-right, a corner clock (headerClockCentered =
  // false) top-left, a centered clock top-center — and never repositions.
  // The content row (title, back arrow, trailing action buttons) centers on
  // the region between the strip and the band bottom, so its padding reads
  // as balanced under the status line rather than against the full band.
  // Text hangs low in its line cell by the font's internal leading; the icon
  // buttons drop by that amount to align with the glyphs the user sees.
  // Buttons keep a standard square (a band-8 button dwarfs its 24px icon);
  // the boxes are invisible and minTouchSize pads the tap target.
  constexpr int16_t headerButtonSize = 48;
  const int16_t bandHeight = static_cast<int16_t>(metrics.headerHeight);
  const int16_t strip = static_cast<int16_t>(metrics.batteryBarHeight);
  const int titleFontId = uiScaleSpec().titleFontId;
  const int16_t opticalDrop =
      static_cast<int16_t>((renderer.getLineHeight(titleFontId) - renderer.getTextHeight(titleFontId)) / 2);
  props.leadingSize = headerButtonSize;
  props.trailingSize = headerButtonSize;
  props.actionOffsetY = static_cast<int16_t>(strip + (bandHeight - strip - headerButtonSize) / 2 - 4 + opticalDrop);
  // Shift the title's band-centered box down by half the strip: its center
  // lands on the below-strip region's midline with the buttons.
  props.titleOffsetY = static_cast<int16_t>(strip / 2);
  status.stripHeight = strip;
  status.clockCentered = metrics.headerClockCentered;

  // Header clock, opposite the battery, on every screen that draws this band
  // (SETTINGS.clockShowInHeader). Themes whose title layout has no room for
  // the clock's left reserve opt out via headerShowsClock.
  static char clockText[10];
  if (metrics.headerShowsClock && SETTINGS.clockShowInHeader && halClock.isAvailable() &&
      halClock.formatTime(clockText, sizeof(clockText), SETTINGS.clockFormat == 1)) {
    status.clockText = clockText;
  }
}

void BaseTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle,
                           const bool backButton, const uint8_t titleMaxLines) const {
  // Every activity header renders through the FreeInkUI header + battery
  // indicator components, styled by the active theme's tokens (padding,
  // centering, underline). Non-interactive frame: no hit rects registered.
  namespace fui = freeink::ui;
  const auto spec = uiScaleSpec();
  fui::GfxRendererFrame<1> ui(renderer, spec.smallFontId, spec.bodyFontId, spec.titleFontId);
  // Refresh the app-wide shared tokens instead of copying ~1.5KB of
  // ThemeTokens onto this render-path stack frame; the values derived here
  // are identical to what every FreeInkApp screen derives. Goes through the
  // same publish-a-fresh-slot path applySharedUiTheme() uses (see
  // UiAppHelpers.h) rather than overwriting the previously-published
  // instance in place, since some other FreeInkApp could be mid-read of it.
  const fui::ThemeTokens& tokens = refreshSharedUiThemeTokens(ui.target);
  // Header status text (battery percent, right label) stays at the fixed
  // small font like the legacy headers; the uiScale small font is for list
  // subtitles.
  ui.target.setFont(fui::GfxRendererTarget::FONT_SMALL, SMALL_FONT_ID);
  ui.target.setFont(fui::GfxRendererTarget::FONT_LABEL, SMALL_FONT_ID);
  // Callers pass a full-width header rect, so the battery/title would sit at the
  // true screen edges — clipped on rounded-corner panels (EEGO A4). Inset the band
  // horizontally by the board's viewable insets so header chrome clears the bezel.
  int hMarginTop, hMarginRight, hMarginBottom, hMarginLeft;
  renderer.getOrientedViewableTRBL(&hMarginTop, &hMarginRight, &hMarginBottom, &hMarginLeft);
  const fui::Rect band{static_cast<int16_t>(rect.x + hMarginLeft), static_cast<int16_t>(rect.y),
                       static_cast<int16_t>(rect.width - hMarginLeft - hMarginRight),
                       static_cast<int16_t>(rect.height)};

  fui::HeaderProps props;
  props.title = title;
  props.rightLabel = subtitle;  // firmware headers right-align the secondary text
  // Battery + clock chrome and their title reserves live in the FreeInkUI
  // header component; this only fills the values from settings and metrics.
  applyHeaderStatus(renderer, props);
  if (rect.height < UITheme::getInstance().getMetrics().headerHeight) {
    // Short bands (home) are not split into strip + content row: the title
    // centers on the band, clear of the band's bottom edge.
    props.titleOffsetY = 0;
  }
  // Tappable back button leading the band on touch boards, so every pushed
  // screen offers a visible way out beside the edge-swipe gesture. This frame
  // registers no hit rects, so the rect is recorded in HeaderBackTapTarget and
  // MappedInputManager folds taps on it into Button::Back. Same geometry as
  // the FUI header's leading slot (applyHeaderStatus set the size/offset) so
  // the recorded rect matches the drawn button.
  const int16_t backBtnSize = props.leadingSize;
  const bool showBackButton = backButton && title != nullptr && gpio.hasTouch();
  if (showBackButton) {
    props.leadingIcon = fui::bitmapFromIcon(icon_header_back_32);
    props.leadingAction = 1;  // any non-NO_ACTION id: paints the button, routing is via HeaderBackTapTarget
    HeaderBackTapTarget::set(band.x + 4, band.y + 4 + props.actionOffsetY, backBtnSize, backBtnSize);
  } else {
    HeaderBackTapTarget::clear();
  }
  props.borderEdges = fui::EdgeBottom;
  props.titleText = tokens.titleText;
  props.titleText.align = tokens.headerTitleAlign;
  props.titleText.maxLines = titleMaxLines;
  props.subtitleText = tokens.smallText;
  props.styles = tokens.popup;
  props.sidePadding = tokens.headerSidePadding;
  // Underline only under a titled header: an untitled band (Lyra home screen)
  // historically drew no rule, and the old themes keyed the line on the title.
  if (title != nullptr && props.styles.normal.border.kind == fui::PaintKind::None && tokens.headerUnderline > 0) {
    props.styles.normal.border = fui::Paint::solid(fui::Color::Black);
    props.styles.normal.borderWidth = tokens.headerUnderline;
  }
  UITheme::noteHeaderBand(band.y, band.height);
  fui::header(ui.frame, band, props);
}

void BaseTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label, const char* rightLabel) const {
  constexpr int labelGap = 10;
  const int smallFontId = themedFontId(SMALL_FONT_ID);
  const int labelFontId = themedFontId(UI_12_FONT_ID);
  const int contentWidth = std::max(0, rect.width - BaseMetrics::values.contentSidePadding * 2);

  int labelWidth = contentWidth;
  if (rightLabel) {
    auto truncatedRightLabel = renderer.truncatedText(smallFontId, rightLabel, contentWidth, EpdFontFamily::REGULAR);
    const int rightLabelWidth = renderer.getTextWidth(smallFontId, truncatedRightLabel.c_str());
    renderer.drawText(smallFontId, rect.x + rect.width - BaseMetrics::values.contentSidePadding - rightLabelWidth,
                      rect.y + 7, truncatedRightLabel.c_str());
    labelWidth = std::max(0, contentWidth - rightLabelWidth - labelGap);
  }

  if (labelWidth > 0) {
    auto truncatedLabel = renderer.truncatedText(labelFontId, label, labelWidth, EpdFontFamily::REGULAR);
    renderer.drawText(labelFontId, rect.x + BaseMetrics::values.contentSidePadding, rect.y, truncatedLabel.c_str(),
                      true, EpdFontFamily::REGULAR);
  }
}

// Draw the "Recent Book" cover card on the home screen
// TODO: Refactor method to make it cleaner, split into smaller methods
void BaseTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                    const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                    bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  const bool hasContinueReading = !recentBooks.empty();
  const bool bookSelected = hasContinueReading && selectorIndex == 0;

  // --- Top "book" card for the current title (selectorIndex == 0) ---
  // When there's no cover image, use fixed size (half screen)
  // When there's cover image, adapt width to image aspect ratio, keep height fixed at 400px
  const int baseHeight = rect.height;  // Fixed height (400px)

  int bookWidth, bookX;
  bool hasCoverImage = false;

  if (hasContinueReading && !recentBooks[0].coverBmpPath.empty()) {
    // Try to get actual image dimensions from BMP header
    const std::string coverBmpPath =
        UITheme::getCoverThumbPath(recentBooks[0].coverBmpPath, BaseMetrics::values.homeCoverHeight);

    HalFile file;
    if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        hasCoverImage = true;
        const int imgWidth = bitmap.getWidth();
        const int imgHeight = bitmap.getHeight();

        // Calculate width based on aspect ratio, maintaining baseHeight
        if (imgWidth > 0 && imgHeight > 0) {
          const float aspectRatio = static_cast<float>(imgWidth) / static_cast<float>(imgHeight);
          bookWidth = static_cast<int>(baseHeight * aspectRatio);

          // Ensure width doesn't exceed reasonable limits (max 90% of screen width)
          const int maxWidth = static_cast<int>(rect.width * 0.9f);
          if (bookWidth > maxWidth) {
            bookWidth = maxWidth;
          }
        } else {
          bookWidth = rect.width / 2;  // Fallback
        }
      }
    }
  }

  if (!hasCoverImage) {
    // No cover: use half screen size
    bookWidth = rect.width / 2;
  }

  bookX = rect.x + (rect.width - bookWidth) / 2;
  const int bookY = rect.y;
  const int bookHeight = baseHeight;

  // Bookmark dimensions (used in multiple places)
  const int bookmarkWidth = bookWidth / 8;
  const int bookmarkHeight = bookHeight / 5;
  const int bookmarkX = bookX + bookWidth - bookmarkWidth - 10;
  const int bookmarkY = bookY + 5;

  // Draw book card regardless, fill with message based on `hasContinueReading`
  {
    // Draw cover image as background if available (inside the box)
    // Only load from SD on first render, then use stored buffer

    if (hasContinueReading && !recentBooks[0].coverBmpPath.empty() && !coverRendered) {
      const std::string coverBmpPath =
          UITheme::getCoverThumbPath(recentBooks[0].coverBmpPath, BaseMetrics::values.homeCoverHeight);

      // First time: load cover from SD and render
      HalFile file;
      if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          LOG_DBG("THEME", "Rendering bmp");

          // The card matches the cover aspect except when width-capped; fill
          // the card 1:1 and crop the overflow rather than rescale the dither.
          drawCoverThumbFill(renderer, bitmap, Rect{bookX, bookY, bookWidth, bookHeight});

          // Draw border around the card
          renderer.drawRect(bookX, bookY, bookWidth, bookHeight);

          // No bookmark ribbon when cover is shown - it would just cover the art

          // Store the buffer with cover image for fast navigation
          coverBufferStored = storeCoverBuffer();
          coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer

          // First render: if selected, draw selection indicators now
          if (bookSelected) {
            LOG_DBG("THEME", "Drawing selection");
            renderer.drawRect(bookX + 1, bookY + 1, bookWidth - 2, bookHeight - 2);
            renderer.drawRect(bookX + 2, bookY + 2, bookWidth - 4, bookHeight - 4);
          }
        }
      }
    }

    if (!bufferRestored && !coverRendered) {
      // No cover image: draw border or fill, plus bookmark as visual flair
      if (bookSelected) {
        renderer.fillRect(bookX, bookY, bookWidth, bookHeight);
      } else {
        renderer.drawRect(bookX, bookY, bookWidth, bookHeight);
      }

      // Draw bookmark ribbon when no cover image (visual decoration)
      if (hasContinueReading) {
        const int notchDepth = bookmarkHeight / 3;
        const int centerX = bookmarkX + bookmarkWidth / 2;

        const int xPoints[5] = {
            bookmarkX,                  // top-left
            bookmarkX + bookmarkWidth,  // top-right
            bookmarkX + bookmarkWidth,  // bottom-right
            centerX,                    // center notch point
            bookmarkX                   // bottom-left
        };
        const int yPoints[5] = {
            bookmarkY,                                // top-left
            bookmarkY,                                // top-right
            bookmarkY + bookmarkHeight,               // bottom-right
            bookmarkY + bookmarkHeight - notchDepth,  // center notch point
            bookmarkY + bookmarkHeight                // bottom-left
        };

        // Draw bookmark ribbon (inverted if selected)
        renderer.fillPolygon(xPoints, yPoints, 5, !bookSelected);
      }
    }

    // If buffer was restored, draw selection indicators if needed
    if (bufferRestored && bookSelected && coverRendered) {
      // Draw selection border (no bookmark inversion needed since cover has no bookmark)
      renderer.drawRect(bookX + 1, bookY + 1, bookWidth - 2, bookHeight - 2);
      renderer.drawRect(bookX + 2, bookY + 2, bookWidth - 4, bookHeight - 4);
    } else if (!coverRendered && !bufferRestored) {
      // Selection border already handled above in the no-cover case
    }
  }

  if (hasContinueReading) {
    const std::string& lastBookTitle = recentBooks[0].title;
    const std::string& lastBookAuthor = recentBooks[0].author;

    // Invert text colors based on selection state:
    // - With cover: selected = white text on black box, unselected = black text on white box
    // - Without cover: selected = white text on black card, unselected = black text on white card

    auto lines = renderer.wrappedText(UI_12_FONT_ID, lastBookTitle.c_str(), bookWidth - 40, 3);

    // Book title text
    int totalTextHeight = renderer.getLineHeight(UI_12_FONT_ID) * static_cast<int>(lines.size());
    if (!lastBookAuthor.empty()) {
      totalTextHeight += renderer.getLineHeight(UI_10_FONT_ID) * 3 / 2;
    }

    // Vertically center the title block within the card
    int titleYStart = bookY + (bookHeight - totalTextHeight) / 2;

    const auto truncatedAuthor = lastBookAuthor.empty()
                                     ? std::string{}
                                     : renderer.truncatedText(UI_10_FONT_ID, lastBookAuthor.c_str(), bookWidth - 40);

    // If cover image was rendered, draw box behind title and author
    if (coverRendered) {
      constexpr int boxPadding = 8;
      // Calculate the max text width for the box
      int maxTextWidth = 0;
      for (const auto& line : lines) {
        const int lineWidth = renderer.getTextWidth(UI_12_FONT_ID, line.c_str());
        if (lineWidth > maxTextWidth) {
          maxTextWidth = lineWidth;
        }
      }
      if (!truncatedAuthor.empty()) {
        const int authorWidth = renderer.getTextWidth(UI_10_FONT_ID, truncatedAuthor.c_str());
        if (authorWidth > maxTextWidth) {
          maxTextWidth = authorWidth;
        }
      }

      const int boxWidth = maxTextWidth + boxPadding * 2;
      const int boxHeight = totalTextHeight + boxPadding * 2;
      const int boxX = rect.x + (rect.width - boxWidth) / 2;
      const int boxY = titleYStart - boxPadding;

      // Draw box (inverted when selected: black box instead of white)
      renderer.fillRect(boxX, boxY, boxWidth, boxHeight, bookSelected);
      // Draw border around the box (inverted when selected: white border instead of black)
      renderer.drawRect(boxX, boxY, boxWidth, boxHeight, !bookSelected);
    }

    for (const auto& line : lines) {
      renderer.drawCenteredText(UI_12_FONT_ID, titleYStart, line.c_str(), !bookSelected);
      titleYStart += renderer.getLineHeight(UI_12_FONT_ID);
    }

    if (!truncatedAuthor.empty()) {
      titleYStart += renderer.getLineHeight(UI_10_FONT_ID) / 2;
      renderer.drawCenteredText(UI_10_FONT_ID, titleYStart, truncatedAuthor.c_str(), !bookSelected);
    }

    // "Continue Reading" label at the bottom
    const int continueY = bookY + bookHeight - renderer.getLineHeight(UI_10_FONT_ID) * 3 / 2;
    if (coverRendered) {
      // Draw box behind "Continue Reading" text (inverted when selected: black box instead of white)
      const char* continueText = tr(STR_CONTINUE_READING);
      const int continueTextWidth = renderer.getTextWidth(UI_10_FONT_ID, continueText);
      constexpr int continuePadding = 6;
      const int continueBoxWidth = continueTextWidth + continuePadding * 2;
      const int continueBoxHeight = renderer.getLineHeight(UI_10_FONT_ID) + continuePadding;
      const int continueBoxX = rect.x + (rect.width - continueBoxWidth) / 2;
      const int continueBoxY = continueY - continuePadding / 2;
      renderer.fillRect(continueBoxX, continueBoxY, continueBoxWidth, continueBoxHeight, bookSelected);
      renderer.drawRect(continueBoxX, continueBoxY, continueBoxWidth, continueBoxHeight, !bookSelected);
      renderer.drawCenteredText(UI_10_FONT_ID, continueY, continueText, !bookSelected);
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, continueY, tr(STR_CONTINUE_READING), !bookSelected);
    }
  } else {
    // No book to continue reading
    const int y =
        bookY + (bookHeight - renderer.getLineHeight(UI_12_FONT_ID) - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_NO_OPEN_BOOK));
    renderer.drawCenteredText(UI_10_FONT_ID, y + renderer.getLineHeight(UI_12_FONT_ID), tr(STR_START_READING));
  }
}

int BaseTheme::getMenuRowHeight(const GfxRenderer&) const { return UITheme::getInstance().getMetrics().menuRowHeight; }

void BaseTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                               const std::function<std::string(int index)>& buttonLabel,
                               const std::function<UIIcon(int index)>& rowIcon) const {
  for (int i = 0; i < buttonCount; ++i) {
    const int tileY = BaseMetrics::values.verticalSpacing + rect.y +
                      static_cast<int>(i) * (BaseMetrics::values.menuRowHeight + BaseMetrics::values.menuSpacing);

    const bool selected = selectedIndex == i;

    if (selected) {
      renderer.fillRect(rect.x + BaseMetrics::values.contentSidePadding, tileY,
                        rect.width - BaseMetrics::values.contentSidePadding * 2, BaseMetrics::values.menuRowHeight);
    } else {
      renderer.drawRect(rect.x + BaseMetrics::values.contentSidePadding, tileY,
                        rect.width - BaseMetrics::values.contentSidePadding * 2, BaseMetrics::values.menuRowHeight);
    }

    std::string labelStr = buttonLabel(i);
    const char* label = labelStr.c_str();
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label);
    const int textX = rect.x + (rect.width - textWidth) / 2;
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int textY =
        tileY + (BaseMetrics::values.menuRowHeight - lineHeight) / 2;  // vertically centered assuming y is top of text
    // Invert text when the tile is selected, to contrast with the filled background
    renderer.drawText(UI_10_FONT_ID, textX, textY, label, selectedIndex != i);
  }
}

int BaseTheme::popupFontId() const { return UI_12_FONT_ID; }

Rect BaseTheme::drawPopup(const GfxRenderer& renderer, const char* message) const {
  const int fontId = popupFontId();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int marginX = metrics.popupMarginX;
  const int marginY = metrics.popupMarginY;
  const int frameThickness = metrics.popupFrameThickness;
  const EpdFontFamily::Style popupFontFamily = metrics.popupTextBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  // Scale y position proportionally to screen height
  const int y = static_cast<int>(renderer.getScreenHeight() * metrics.popupTopOffsetRatio);
  const int textWidth = renderer.getTextWidth(fontId, message, popupFontFamily);
  const int textHeight = renderer.getLineHeight(fontId);
  const int w = textWidth + marginX * 2;
  const int h = textHeight + marginY * 2;
  const int x = (renderer.getScreenWidth() - w) / 2;

  const bool useRoundedPopup = metrics.popupCornerRadius > 0;
  if (useRoundedPopup) {
    renderer.fillRoundedRect(x - frameThickness, y - frameThickness, w + frameThickness * 2, h + frameThickness * 2,
                             metrics.popupCornerRadius + frameThickness, Color::White);
    renderer.fillRoundedRect(x, y, w, h, metrics.popupCornerRadius, Color::Black);
  } else {
    renderer.fillRect(x - frameThickness, y - frameThickness, w + frameThickness * 2, h + frameThickness * 2, true);
    renderer.fillRect(x, y, w, h, false);
  }

  const int textX = x + (w - textWidth) / 2;
  const int textY = y + marginY + metrics.popupTextBaselineOffsetY;
  renderer.drawText(fontId, textX, textY, message, metrics.popupTextInverted, popupFontFamily);
  renderer.displayBuffer();
  return Rect{x, y, w, h};
}

void BaseTheme::fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int barHeight = metrics.popupProgressBarHeight;
  const int barWidth =
      std::max(0, layout.width - metrics.popupMarginX * 2);  // twice the margin in drawPopup to match text width
  const int barX = layout.x + (layout.width - barWidth) / 2;
  const int barY = layout.y + layout.height - metrics.popupMarginY / 2 - barHeight / 2 - 1;
  if (barWidth <= 0 || barHeight <= 0) {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  const int scaledProgress = metrics.popupProgressClampPercent ? std::clamp(progress, 0, 100) : progress;
  const int fillWidth = barWidth * scaledProgress / 100;

  if (metrics.popupProgressDrawOutline) {
    renderer.drawRect(barX, barY, barWidth, barHeight, 1, metrics.popupProgressOutlineInverted);
  }
  if (fillWidth > 0) {
    renderer.fillRect(barX, barY, fillWidth, barHeight, metrics.popupProgressFillInverted);
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void BaseTheme::fillRibbon(const GfxRenderer& renderer, const int left, const int right, const int top,
                           const int height, const int notch, const bool notchOnLeft) {
  const int bottom = top + height - 1;
  const int tail = notchOnLeft ? left : right;
  const int inner = notchOnLeft ? left + notch : right - notch;
  const int head = notchOnLeft ? right : left;
  const int xs[] = {head, tail, inner, tail, head};
  const int ys[] = {top, top, top + height / 2, bottom, bottom};
  renderer.fillPolygon(xs, ys, 5, true);
}

namespace {
int readerProgressRibbonWidth(const GfxRenderer& renderer) {
  int marginTop, marginRight, marginBottom, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  return std::clamp(marginRight + SETTINGS.screenMargin - 2, 8, 16);
}
}  // namespace

void BaseTheme::clearReaderProgressRibbon(const GfxRenderer& renderer) {
  const int width = readerProgressRibbonWidth(renderer);
  renderer.fillRect(renderer.getScreenWidth() - width, 0, width, renderer.getScreenHeight(), false);
}

// Hangs in the right margin from the screen edge, between the running head and the bottom margin.
void BaseTheme::drawReaderProgressRibbon(const GfxRenderer& renderer, const int percent, const bool textAtTop,
                                         const int paddingBottom, const bool paperState) {
  constexpr int kRibbonHeight = 14;
  constexpr int kRibbonNotch = 4;
  int marginTop, marginRight, marginBottom, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  const int right = renderer.getScreenWidth() - 1;
  const int width = readerProgressRibbonWidth(renderer);
  const int travelTop = marginTop + (textAtTop ? UITheme::getStatusBarTopHeight(renderer) : 0);
  const int travelBottom = renderer.getScreenHeight() - marginBottom - paddingBottom - SETTINGS.screenMargin;
  const int travel = std::max(0, travelBottom - travelTop - kRibbonHeight);
  const int top = travelTop + travel * std::clamp(percent, 0, 100) / 100;
  GUI.drawProgressRibbon(renderer, right - width + 1, right, top, kRibbonHeight, kRibbonNotch, paperState);
}

void BaseTheme::drawStatusBar(GfxRenderer& renderer, const float bookProgress, const int currentPage,
                              const int pageCount, std::string title, const int paddingBottom, const int textYOffset,
                              const bool fillMargin, const bool isPageBookmarked, const bool pageCountEstimated,
                              const bool textAtTop, const int bookPage, const bool bookPercent) {
  auto metrics = UITheme::getInstance().getMetrics();
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  const auto sb = SETTINGS.statusBarSpec();
  const bool showStatusBarTextLane = sb.textLaneVisible(halClock.isAvailable());

  // Draw Progress Text
  const auto screenHeight = renderer.getScreenHeight();
  // At the top, place the reader-font line (book style) below the padding.
  auto textY =
      textAtTop
          ? orientedMarginTop + UITheme::kStatusBarTopPadding +
                renderer.getFontAscenderSize(SETTINGS.getReaderFontId()) - renderer.getFontAscenderSize(SMALL_FONT_ID)
          : screenHeight - UITheme::getInstance().getStatusBarHeight() - orientedMarginBottom - paddingBottom - 4;

  const int leftClusterX = metrics.statusBarHorizontalMargin + orientedMarginLeft + 1;
  const int rightClusterX = renderer.getScreenWidth() - metrics.statusBarHorizontalMargin - orientedMarginRight;
  int leftClusterWidth = 0;
  int rightClusterWidth = 0;

  if (sb.showBookProgressPercent || sb.showChapterPageCount) {
    // Right aligned text for progress counter
    char progressStr[32];

    // Book style: pages left in the chapter and/or the book percent, set like the running title.
    const bool bookStyle = metrics.statusBarBookStyle;
    const int progressFontId = bookStyle ? SETTINGS.getReaderFontId() : SMALL_FONT_ID;
    const EpdFontFamily::Style progressStyle = EpdFontFamily::REGULAR;
    const int progressY =
        bookStyle ? textY + renderer.getFontAscenderSize(SMALL_FONT_ID) - renderer.getFontAscenderSize(progressFontId)
                  : textY;

    // Draw the estimate marker separately so it can use the next UI font size.
    const bool showEstimate = pageCountEstimated && sb.showChapterPageCount && !bookStyle;

    // On a chapter's last page the book percent stands in for the 0.
    const int pagesLeft = std::max(0, pageCount - currentPage);
    if (bookStyle && bookPercent) {
      snprintf(progressStr, sizeof(progressStr), "%.0f%%", bookProgress);
    } else if (bookStyle && sb.showChapterPageCount && bookPage > 0) {
      // Book-wide page, counting up (estimated from the chapters read so far).
      snprintf(progressStr, sizeof(progressStr), "%d", bookPage);
    } else if (bookStyle && sb.showChapterPageCount && (sb.showBookProgressPercent || pagesLeft == 0)) {
      if (pagesLeft == 0) {
        snprintf(progressStr, sizeof(progressStr), "%.0f%%", bookProgress);
      } else {
        snprintf(progressStr, sizeof(progressStr), "%d  %.0f%%", pagesLeft, bookProgress);
      }
    } else if (bookStyle && sb.showChapterPageCount) {
      snprintf(progressStr, sizeof(progressStr), "%d", pagesLeft);
    } else if (sb.showBookProgressPercent && sb.showChapterPageCount) {
      snprintf(progressStr, sizeof(progressStr), "%d/%d  %.0f%%", currentPage, pageCount, bookProgress);
    } else if (sb.showBookProgressPercent) {
      snprintf(progressStr, sizeof(progressStr), "%.0f%%", bookProgress);
    } else {
      snprintf(progressStr, sizeof(progressStr), "%d/%d", currentPage, pageCount);
    }

    int progressTextWidth = renderer.getTextWidth(progressFontId, progressStr, progressStyle);
    const int estimateWidth = showEstimate ? renderer.getTextWidth(UI_10_FONT_ID, "~") : 0;
    constexpr int estimateGap = 2;
    const int estimateSpacing = showEstimate ? estimateGap : 0;
    const int progressX = rightClusterX - estimateWidth - estimateSpacing - progressTextWidth;
    if (showEstimate) {
      const int estimateY = textY + (renderer.getLineHeight(SMALL_FONT_ID) - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
      renderer.drawText(UI_10_FONT_ID, progressX, estimateY, "~");
    }
    renderer.drawText(progressFontId, progressX + estimateWidth + estimateSpacing, progressY, progressStr, true,
                      progressStyle);

    rightClusterWidth += estimateWidth + estimateSpacing + progressTextWidth;
  }

  // Draw Progress Bar
  if (sb.showsProgressBar() && metrics.progressRibbon) {
    const int progress = sb.progressBarMode == CrossPointSettings::STATUS_BAR_PROGRESS_BAR::BOOK_PROGRESS
                             ? static_cast<int>(bookProgress)
                             : (pageCount > 0 ? currentPage * 100 / pageCount : 0);
    drawReaderProgressRibbon(renderer, progress, textAtTop, paddingBottom);
  } else if (sb.showsProgressBar()) {
    const int barMarginLeft = fillMargin ? 0 : orientedMarginLeft;
    const int barMarginRight = fillMargin ? 0 : orientedMarginRight;
    const int progressBarMaxWidth = renderer.getScreenWidth() - barMarginLeft - barMarginRight;
    const int progressBarY = renderer.getScreenHeight() - orientedMarginBottom - sb.progressBarHeightPx -
                             paddingBottom + (fillMargin ? 1 : 0);
    size_t progress;
    if (sb.progressBarMode == CrossPointSettings::STATUS_BAR_PROGRESS_BAR::BOOK_PROGRESS) {
      progress = static_cast<size_t>(bookProgress);
    } else {
      // Chapter progress
      progress = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) * 100 : 0;
    }
    const int barWidth = progressBarMaxWidth * progress / 100;
    const int barHeight = sb.progressBarHeightPx + (fillMargin ? orientedMarginBottom - 1 : 0);
    renderer.fillRect(barMarginLeft, progressBarY, barWidth, barHeight, true);
  }

  // Draw Battery
  const bool showBatteryPercentage = sb.showBatteryPercent;

  if (sb.showBattery) {
    GUI.drawBatteryLeft(renderer,
                        Rect{leftClusterX + leftClusterWidth, textY, metrics.batteryWidth, metrics.batteryHeight},
                        showBatteryPercentage);
    int batteryWidth = metrics.batteryWidth;

    if (showBatteryPercentage) {
      const uint16_t percentage = powerManager.getBatteryPercentage();
      // width of icon + spacing + text for layout purposes
      batteryWidth +=
          batteryPercentSpacing + renderer.getTextWidth(SMALL_FONT_ID, (std::to_string(percentage) + "%").c_str());
    }

    leftClusterWidth += batteryWidth;
  }

  // Draw Clock (any board whose RTC probe succeeded)
  if (sb.showsClock() && halClock.isAvailable()) {
    char timeBuf[9];
    if (halClock.formatTime(timeBuf, sizeof(timeBuf), sb.clock12h)) {
      int clockTextWidth = renderer.getTextWidth(SMALL_FONT_ID, timeBuf);
      int clockX = 0;
      // Position to the left or right of the progress text (with a small gap)
      if (sb.clockMode == CrossPointSettings::STATUS_BAR_CLOCK_LEFT) {
        clockX = leftClusterX + leftClusterWidth + (leftClusterWidth > 0 ? 10 : 0);
        leftClusterWidth += clockTextWidth + 10;
      } else if (sb.clockMode == CrossPointSettings::STATUS_BAR_CLOCK_RIGHT) {
        clockX = rightClusterX - rightClusterWidth - (rightClusterWidth > 0 ? 10 : 0) - clockTextWidth;
        rightClusterWidth += clockTextWidth + 10;
      }
      renderer.drawText(SMALL_FONT_ID, clockX, textY, timeBuf);
    }
  }

  // Draw Bookmark
  if (showStatusBarTextLane && isPageBookmarked) {
    const int bookmarkGap = leftClusterWidth > 0 ? bookmarkStatusIconGap : 0;
    const int bookmarkX = leftClusterX + leftClusterWidth + bookmarkGap;
    const int bookmarkY = textY + 5;
    drawBookmarkStatusIcon(renderer, bookmarkX, bookmarkY);
    leftClusterWidth += bookmarkStatusIconWidth + bookmarkGap;
  }

  // Draw Title
  if (!title.empty()) {
    textY -= textYOffset;
    // Centered chapter title text
    // Page width minus existing content with 30px padding on each side
    const int rendererableScreenWidth =
        renderer.getScreenWidth() - (metrics.statusBarHorizontalMargin * 2) - orientedMarginLeft - orientedMarginRight;

    const int titleMarginLeft = leftClusterWidth + 30;
    const int titleMarginRight = rightClusterWidth + 30;

    // Attempt to center title on the screen, but if title is too wide then later we will center it within the
    // available space. Book style keeps it centred on the page and truncates instead.
    int titleMarginLeftAdjusted = std::max(titleMarginLeft, titleMarginRight);
    int availableTitleSpace = rendererableScreenWidth - 2 * titleMarginLeftAdjusted;

    int titleFontId = SMALL_FONT_ID;
    EpdFontFamily::Style titleStyle = EpdFontFamily::REGULAR;
    int titleY = textY;
    if (metrics.statusBarBookStyle) {
      // Share the progress text's baseline.
      titleFontId = SETTINGS.getReaderFontId();
      titleStyle = EpdFontFamily::ITALIC;
      shortenChapterTitle(title);
      titleY = textY + renderer.getFontAscenderSize(SMALL_FONT_ID) - renderer.getFontAscenderSize(titleFontId);
    }

    int titleWidth;
    titleWidth = renderer.getTextWidth(titleFontId, title.c_str(), titleStyle);
    if (titleWidth > availableTitleSpace && !metrics.statusBarBookStyle) {
      // Not enough space to center on the screen, center it within the remaining space instead
      availableTitleSpace = rendererableScreenWidth - titleMarginLeft - titleMarginRight;
      titleMarginLeftAdjusted = titleMarginLeft;
    }
    if (titleWidth > availableTitleSpace) {
      title = renderer.truncatedText(titleFontId, title.c_str(), availableTitleSpace, titleStyle);
      titleWidth = renderer.getTextWidth(titleFontId, title.c_str(), titleStyle);
    }

    renderer.drawText(titleFontId,
                      titleMarginLeftAdjusted + metrics.statusBarHorizontalMargin + orientedMarginLeft +
                          (availableTitleSpace - titleWidth) / 2,
                      titleY, title.c_str(), true, titleStyle);
  }
}

void BaseTheme::drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Book-styled themes set help lines as an italic aside in the reader font.
  const int fontId = themedFontId(SMALL_FONT_ID);
  const auto style = metrics.uiUsesReaderFont ? EpdFontFamily::ITALIC : EpdFontFamily::REGULAR;
  auto truncatedLabel = renderer.truncatedText(fontId, label, rect.width - metrics.contentSidePadding * 2, style);
  renderer.drawCenteredText(fontId, rect.y, truncatedLabel.c_str(), true, style);
}

void BaseTheme::drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode,
                              int contentStartX, int contentWidth) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int lineY = rect.y + rect.height + lineHeight + metrics.verticalSpacing;
  const int thickness = cursorMode ? metrics.textFieldCursorThickness : metrics.textFieldNormalThickness;
  if (contentWidth > 0) {
    renderer.drawLine(rect.x + contentStartX, lineY,
                      rect.x + contentStartX + contentWidth + metrics.textFieldLineEndOffset, lineY, thickness, true);
  } else {
    const int lineW = textWidth + metrics.textFieldHorizontalPadding * 2;
    const int lineStart = rect.x + (rect.width - lineW) / 2;
    renderer.drawLine(lineStart, lineY, lineStart + lineW + metrics.textFieldLineEndOffset, lineY, thickness, true);
  }
}

void BaseTheme::drawSelectionHandle(const GfxRenderer& renderer, const Rect rect, const bool left) {
  const int radius = rect.width / 2;
  renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, radius, Color::Black);
  renderer.fillRect(left ? rect.x + radius : rect.x, rect.y, radius, radius);
}

void BaseTheme::drawSelectionActions(const GfxRenderer& renderer, const Rect rect, const bool editing) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const char* labels[] = {tr(STR_LOOKUP), editing ? tr(STR_DONE) : tr(STR_CLIP), tr(STR_BOOKMARK_OPTION),
                          tr(STR_DELETE)};
  const int count = editing ? 4 : 3;
  for (int i = 0; i < count; ++i) {
    const Rect button = selectionGeometry::button(rect, i, metrics.menuSpacing, count);
    renderer.fillRoundedRect(button.x, button.y, button.width, button.height, metrics.controlRadius, Color::White);
    renderer.drawRoundedRect(button.x, button.y, button.width, button.height, 1, metrics.controlRadius, true);
    UITheme::drawCenteredWrappedText(renderer, button, uiScaleSpec().smallFontId, labels[i], 2, true,
                                     EpdFontFamily::REGULAR, UITheme::TextVerticalAlignment::CENTER);
  }
}
