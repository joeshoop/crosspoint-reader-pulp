#include "PulpTheme.h"

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
constexpr int kOutline = 2;  // selection outline and home card frame
constexpr int kOutlineRadius = 4;
constexpr int kCoverInset = 4;  // frame to cover: 2px outline + 2px white margin
constexpr int kCoverRadius = 2;
constexpr int kMenuMaxWidth = 360;  // until a cover has set the width
constexpr int kMenuPaddingY = 8;
constexpr int kHomeMenuRows = 2;    // Books, Settings (homeMinimalMenu)
constexpr int kHintClearance = 12;  // air between the home menu and the button hints
constexpr int kHintGroupWidth = 176;
constexpr int kHintGap = 12;
constexpr int kHintRadius = 8;
constexpr int kHintPaddingX = 10;
constexpr int kGuideFontId = SMALL_FONT_ID;

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
}  // namespace

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
          coverWidth = std::min(bitmap.getWidth(), rect.width - 2 * kCoverInset);
          drawCoverThumbFill(renderer, bitmap,
                             Rect{rect.x + (rect.width - coverWidth) / 2, coverY, coverWidth, coverHeight});
          renderer.maskRoundedRectOutsideCorners(rect.x + (rect.width - coverWidth) / 2, coverY, coverWidth,
                                                 coverHeight, kCoverRadius, Color::White);
          hasCover = true;
        }
        file.close();
      }
    }

    if (!hasCover) {
      // Plain bound book: gray board with the title and author set on it.
      const int x = rect.x + (rect.width - coverWidth) / 2;
      renderer.fillRectDither(x, coverY, coverWidth, coverHeight, Color::LightGray);
      renderer.maskRoundedRectOutsideCorners(x, coverY, coverWidth, coverHeight, kCoverRadius, Color::White);
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
  const int labelTop = coverY + coverHeight;
  const int labelHeight = rect.y + rect.height - labelTop;
  const std::string label =
      renderer.truncatedText(fontId, tr(STR_CONTINUE_READING), cardWidth - 2 * kHintPaddingX, EpdFontFamily::REGULAR);
  renderer.drawCenteredText(fontId, labelTop + (labelHeight - renderer.getCenteringHeight(fontId)) / 2, label.c_str());

  if (selectorIndex == 0) {
    renderer.drawRoundedRect(cardX, rect.y, cardWidth, rect.height, kOutline, kOutlineRadius, true);
  }
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
  return homeCoverTileHeight(renderer) - kCoverInset - getMenuRowHeight(renderer);
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
    renderer.drawText(fontId, rowX + (rowWidth - textWidth) / 2, rowY + textOffsetY, text.c_str());
    if (i == selectedIndex) {
      renderer.drawRoundedRect(rowX, rowY, rowWidth, rowHeight, kOutline, kOutlineRadius, true);
    }
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

  const int textY = hintY + (hintHeight - renderer.getLineHeight(kGuideFontId)) / 2;
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
