#include "PulpTheme.h"

#include <FreeInkUI.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int kCoverInset = 4;  // frame to cover: 2px outline + 2px white margin
constexpr int kCoverRadius = 2;
// The cover sits on its book: page edges show below and to the right, and the
// book casts a soft shadow.
constexpr int kBookDepth = 6;
constexpr int kBookShadow = 4;
constexpr int kBookExtra = kBookDepth + kBookShadow;
constexpr int kMenuMaxWidth = 360;  // until a cover has set the width
constexpr int kMenuPaddingY = 8;
constexpr int kHomeMenuRows = 2;    // Books, Settings (homeMinimalMenu)
constexpr int kHintClearance = 22;  // air between the home menu and the button hints
constexpr int kHintGroupWidth = 176;
constexpr int kHintGap = 12;
constexpr int kHintRadius = 4;
constexpr int kHintPaddingX = 10;
constexpr int kGuideFontId = HINT_PIXEL_FONT_ID;
// Baseline height above the panel edge; the X4 bezel hides the bottom rows.
constexpr int kHintBaselineInset = 9;

// Cover width from the last parsed thumbnail; kept so a home redraw from the
// stored cover buffer can place the frame without reopening the BMP.
int coverWidth = 0;

std::string upperAscii(const char* text) {
  std::string out = text ? text : "";
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }
  return out;
}
// Shadow, back board and page edges behind a cover at (x, y, w, h), painted
// back to front so each nearer layer hides the outline of the one behind.
// The cover then goes on top.
void drawBookBody(const GfxRenderer& renderer, const int x, const int y, const int w, const int h) {
  renderer.fillRectDither(x + kBookDepth + kBookShadow, y + kBookDepth + kBookShadow, w, h, Color::LightGray);
  for (int k = kBookDepth; k > 0; k -= 2) {
    renderer.fillRect(x + k, y + k, w, h, false);
    renderer.drawRoundedRect(x + k, y + k, w, h, 1, kCoverRadius, true);
  }
}
}  // namespace

// The fui list Highlighter selection's stroke, behind one line of text
// whose line box starts at lineTop. Drawn before the text.
static void drawHighlight(const GfxRenderer& renderer, const int fontId, const int textX, const int lineTop,
                   const std::string& text) {
  const int lineHeight = renderer.getLineHeight(fontId);
  const int overhang = lineHeight / 4;
  const freeink::ui::Rect area{static_cast<int16_t>(textX - overhang), static_cast<int16_t>(lineTop + lineHeight * 2 / 5),
                               static_cast<int16_t>(renderer.getTextWidth(fontId, text.c_str()) + 2 * overhang),
                               static_cast<int16_t>(lineHeight / 2)};
  uint32_t seed = 2166136261u;
  for (const char c : text) seed = (seed ^ static_cast<uint8_t>(c)) * 16777619u;
  freeink::ui::highlighterStroke(area, seed, [&renderer](const freeink::ui::Rect column) {
    renderer.fillRectDither(column.x, column.y, column.width, column.height, Color::LightGray);
  });
}

void PulpTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                    const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                    bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  (void)bufferRestored;
  const int fontId = SETTINGS.getReaderFontId();
  const int coverHeight = homeCoverThumbHeight(renderer);
  if (coverWidth == 0) coverWidth = coverHeight * 5 / 8;

  if (recentBooks.empty()) {
    const int lineHeight = renderer.getLineHeight(fontId);
    const int y = rect.y + rect.height / 2 - lineHeight;
    renderer.drawCenteredText(fontId, y, tr(STR_NO_OPEN_BOOK), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(fontId, y + lineHeight, tr(STR_START_READING), true, EpdFontFamily::ITALIC);
    return;
  }

  const int cardWidth = std::min(rect.width, coverWidth + 2 * kCoverInset);
  const int cardX = rect.x + (rect.width - cardWidth) / 2;
  const int coverX = cardX + kCoverInset;
  const int coverY = rect.y + kCoverInset;

  // Only load from SD on first render, then reuse the stored buffer.
  if (!coverRendered) {
    const RecentBook& book = recentBooks[0];
    bool hasCover = false;
    if (!book.coverBmpPath.empty()) {
      const std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
      HalFile file;
      if (Storage.openFileForRead("HOME", thumbPath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          coverWidth = std::min(bitmap.getWidth(), rect.width - 2 * kCoverInset - kBookExtra);
          const int x = rect.x + (rect.width - coverWidth - kBookExtra) / 2;
          drawBookBody(renderer, x, coverY, coverWidth, coverHeight);
          drawCoverThumbFill(renderer, bitmap, Rect{x, coverY, coverWidth, coverHeight});
          renderer.maskRoundedRectOutsideCorners(x, coverY, coverWidth, coverHeight, kCoverRadius, Color::White);
          renderer.drawRoundedRect(x, coverY, coverWidth, coverHeight, 1, kCoverRadius, true);
          hasCover = true;
        }
        file.close();
      }
    }

    if (!hasCover) {
      // Plain bound book: gray board with the title and author set on it.
      const int x = rect.x + (rect.width - coverWidth - kBookExtra) / 2;
      drawBookBody(renderer, x, coverY, coverWidth, coverHeight);
      renderer.fillRectDither(x, coverY, coverWidth, coverHeight, Color::LightGray);
      renderer.drawRoundedRect(x, coverY, coverWidth, coverHeight, 1, kCoverRadius, true);
      const int textX = x + 16;
      const int textWidth = coverWidth - 32;
      const int lineHeight = renderer.getLineHeight(fontId);
      UITheme::drawCenteredWrappedText(renderer, Rect{textX, coverY, textWidth, coverHeight / 2 + lineHeight}, fontId,
                                       book.title.c_str(), 4, true, EpdFontFamily::BOLD,
                                       UITheme::TextVerticalAlignment::BOTTOM);
      UITheme::drawCenteredWrappedText(
          renderer, Rect{textX, coverY + coverHeight / 2 + lineHeight, textWidth, 2 * lineHeight}, fontId,
          book.author.c_str(), 2, true, EpdFontFamily::ITALIC, UITheme::TextVerticalAlignment::TOP);
    }

    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer
  }

  // Drawn after the snapshot so a restored buffer starts without the frame.
  const int labelTop = coverY + coverHeight + kBookExtra;
  const int labelHeight = rect.y + rect.height - labelTop;
  const std::string label =
      renderer.truncatedText(fontId, tr(STR_CONTINUE_READING), cardWidth - 2 * kHintPaddingX, EpdFontFamily::REGULAR);
  if (selectorIndex == 0) {
    const int labelX = (renderer.getScreenWidth() - renderer.getTextWidth(fontId, label.c_str())) / 2;
    drawHighlight(renderer, fontId, labelX, labelTop + (labelHeight - renderer.getLineHeight(fontId)) / 2, label);
  }
  renderer.drawCenteredText(fontId, labelTop + (labelHeight - renderer.getCenteringHeight(fontId)) / 2, label.c_str());
}

int PulpTheme::getMenuRowHeight(const GfxRenderer& renderer) const {
  return renderer.getLineHeight(SETTINGS.getReaderFontId()) + 2 * kMenuPaddingY;
}

// The cover tile fills the screen down to the menu, which sits just above
// the button hints. Both follow the reader font through the row height.
int PulpTheme::homeCoverTileHeight(const GfxRenderer& renderer) const {
  const auto& metrics = PulpMetrics::values;
  const int menuHeight = kHomeMenuRows * (getMenuRowHeight(renderer) + metrics.menuSpacing) - metrics.menuSpacing;
  return renderer.getScreenHeight() - metrics.homeTopPadding - metrics.homeMenuTopOffset - menuHeight -
         metrics.buttonHintsHeight - kHintClearance;
}

// The cover takes the tile minus its inset and a menu-row-high label band.
int PulpTheme::homeCoverThumbHeight(const GfxRenderer& renderer) const {
  return homeCoverTileHeight(renderer) - kCoverInset - kBookExtra - getMenuRowHeight(renderer);
}

void PulpTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                               const std::function<std::string(int index)>& buttonLabel,
                               const std::function<UIIcon(int index)>& rowIcon) const {
  (void)rowIcon;
  const int fontId = SETTINGS.getReaderFontId();
  const int rowHeight = getMenuRowHeight(renderer);  // shared with HomeActivity's touch grid
  const int rowStep = rowHeight + PulpMetrics::values.menuSpacing;
  const int pageItems = std::max(1, rect.height / rowStep);
  const int pageStartIndex = (std::max(0, selectedIndex) / pageItems) * pageItems;
  // Rows match the home cover card's width.
  const int rowWidth = std::min(coverWidth > 0 ? coverWidth + 2 * kCoverInset : kMenuMaxWidth, rect.width);
  const int rowX = rect.x + (rect.width - rowWidth) / 2;
  const int textOffsetY = (rowHeight - renderer.getCenteringHeight(fontId)) / 2;

  for (int i = pageStartIndex; i < buttonCount && i < pageStartIndex + pageItems; ++i) {
    const int rowY = rect.y + (i - pageStartIndex) * rowStep;
    const std::string label = buttonLabel(i);
    const std::string text =
        renderer.truncatedText(fontId, label.c_str(), rowWidth - 2 * PulpMetrics::values.listSidePadding);
    const int textWidth = renderer.getTextWidth(fontId, text.c_str());
    const int textX = rowX + (rowWidth - textWidth) / 2;
    if (i == selectedIndex) {
      drawHighlight(renderer, fontId, textX, rowY + (rowHeight - renderer.getLineHeight(fontId)) / 2, text);
    }
    renderer.drawText(fontId, textX, rowY + textOffsetY, text.c_str());
  }
}

void PulpTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4) const {
  if (gpio.hasTouch()) {
    return;
  }

  const GfxRenderer::Orientation origOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int hintHeight = PulpMetrics::values.buttonHintsHeight;
  const int groupWidth = std::min(kHintGroupWidth, (pageWidth - kHintGap) / 2);
  const int leftX = (pageWidth - 2 * groupWidth - kHintGap) / 2;
  const int rightX = leftX + groupWidth + kHintGap;
  const int hintY = pageHeight - hintHeight;

  if (renderer.getRenderMode() != GfxRenderer::BW && !renderer.grayPlanesAreAbsolute()) {
    renderer.fillRect(leftX, hintY, groupWidth, hintHeight, true);
    renderer.fillRect(rightX, hintY, groupWidth, hintHeight, true);
    renderer.setOrientation(origOrientation);
    return;
  }

  const int textY = pageHeight - kHintBaselineInset - renderer.getFontAscenderSize(kGuideFontId);
  // Pairs are the physical buttons under each group: back/select, then up/down.
  const char* labels[4] = {btn1, btn2, btn3, btn4};
  for (int group = 0; group < 2; ++group) {
    const int x = group == 0 ? leftX : rightX;
    // Hints always win over anything drawn into this band.
    renderer.fillRect(x, hintY, groupWidth, hintHeight, false);
    // A tab open at the bottom: round the top corners and erase the bottom edge.
    renderer.drawRoundedRect(x, hintY, groupWidth, hintHeight, 1, kHintRadius, true, true, false, false, true);
    renderer.fillRect(x + 1, pageHeight - 1, groupWidth - 2, 1, false);

    const std::string leftText = upperAscii(labels[group * 2]);
    const std::string rightText = upperAscii(labels[group * 2 + 1]);
    if (!leftText.empty()) {
      renderer.drawText(kGuideFontId, x + kHintPaddingX, textY, leftText.c_str());
    }
    if (!rightText.empty()) {
      const int width = renderer.getTextWidth(kGuideFontId, rightText.c_str());
      renderer.drawText(kGuideFontId, x + groupWidth - kHintPaddingX - width, textY, rightText.c_str());
    }
  }

  renderer.setOrientation(origOrientation);
}

// One tab open at the right edge spanning the X4's two stacked side buttons,
// set like the bottom hints: Up reads from the top end, Down from the bottom.
// Edge-button boards keep the base layout.
void PulpTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const {
  if (gpio.hasTouch()) return;
  if (gpio.hasEdgeSideButtons()) {
    BaseTheme::drawSideButtonHints(renderer, topBtn, bottomBtn);
    return;
  }
  // Physical button span, as in BaseTheme.
  constexpr int kTabTop = 345;
  constexpr int kTabHeight = 160;
  constexpr int kTextInset = 8;
  const int tabWidth = PulpMetrics::values.buttonHintsHeight;
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int x = screenW - tabWidth;
  renderer.fillRect(x, kTabTop, tabWidth, kTabHeight, false);
  renderer.drawRoundedRect(x, kTabTop, tabWidth, kTabHeight, 1, kHintRadius, true, false, true, false, true);
  renderer.fillRect(screenW - 1, kTabTop + 1, 1, kTabHeight - 2, false);

  // The renderer only rotates text clockwise (reading upward); flipping the
  // orientation turns that into text reading downward, glyph tops to the edge.
  auto& flipped = const_cast<GfxRenderer&>(renderer);
  const GfxRenderer::Orientation origOrientation = renderer.getOrientation();
  const int textH = renderer.getTextHeight(kGuideFontId);
  const int columnX = x + (tabWidth - textH) / 2;
  const std::string labels[2] = {upperAscii(topBtn), upperAscii(bottomBtn)};
  flipped.setOrientation(GfxRenderer::Orientation::PortraitInverted);
  for (int i = 0; i < 2; ++i) {
    if (labels[i].empty()) continue;
    const int textW = renderer.getTextWidth(kGuideFontId, labels[i].c_str());
    const int top = i == 0 ? kTabTop + kTextInset : kTabTop + kTabHeight - kTextInset - textW;
    renderer.drawTextRotated90CW(kGuideFontId, screenW - columnX - textH, screenH - 1 - top, labels[i].c_str());
  }
  flipped.setOrientation(origOrientation);
}
