#include "PulpTheme.h"

#include <FreeInkUI.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "PulpBookFrame.h"
#include "PulpHighlight.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookProgress.h"

namespace {
constexpr int kCoverInset = 4;  // frame to cover: 2px outline + 2px white margin
constexpr int kCoverRadius = 2;
// The cover sits on its book: page edges and shadow come from a 9-slice
// image (images/book-frame.png) that stretches to the cover size.
namespace frame = pulp_book_frame;
constexpr int kBookExtraX = frame::kCoverLeft + frame::kCoverRight;
constexpr int kBookExtraY = frame::kCoverTop + frame::kCoverBottom;
// Ribbon tucked into the right page edge, lower as more is read.
constexpr int kRibbonHeight = 14;  // as the reader's progress ribbon
constexpr int kRibbonReach = 10;  // past the frame's right edge
constexpr int kRibbonNotch = 4;
constexpr int kRibbonMargin = 6;  // travel stops short of the cover corners
// Page layers in images/book-frame.png: every 2px out to the back page at 10px.
constexpr int kPageStep = 2;
constexpr int kBackPageDepth = 10;
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

constexpr int kTabTextInset = 8;

// A tab on the right screen edge labelling the buttons beside it: the first
// label at its top, the second at its bottom.
void drawEdgeTab(const GfxRenderer& renderer, const int tabTop, const int tabHeight, const char* first,
                 const char* second) {
  const int tabWidth = PulpMetrics::values.buttonHintsHeight;
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int x = screenW - tabWidth;
  renderer.fillRect(x, tabTop, tabWidth, tabHeight, false);
  renderer.drawRoundedRect(x, tabTop, tabWidth, tabHeight, 1, kHintRadius, true, false, true, false, true);
  renderer.fillRect(screenW - 1, tabTop + 1, 1, tabHeight - 2, false);

  // The renderer only rotates text clockwise (reading upward); flipping the
  // orientation turns that into text reading downward, glyph tops to the edge.
  auto& flipped = const_cast<GfxRenderer&>(renderer);
  const GfxRenderer::Orientation origOrientation = renderer.getOrientation();
  const int textH = renderer.getTextHeight(kGuideFontId);
  const int columnX = x + (tabWidth - textH) / 2;
  const std::string labels[2] = {upperAscii(first), upperAscii(second)};
  flipped.setOrientation(GfxRenderer::Orientation::PortraitInverted);
  for (int i = 0; i < 2; ++i) {
    if (labels[i].empty()) continue;
    const int textW = renderer.getTextWidth(kGuideFontId, labels[i].c_str());
    const int top = i == 0 ? tabTop + kTabTextInset : tabTop + tabHeight - kTabTextInset - textW;
    renderer.drawTextRotated90CW(kGuideFontId, screenW - columnX - textH, screenH - 1 - top, labels[i].c_str());
  }
  flipped.setOrientation(origOrientation);
}
// Maps a destination offset along one axis to the 9-slice source: corners
// copy as-is, the middle span repeats.
int nineSliceSource(const int d, const int destSize, const int srcSize, const int sliceStart, const int sliceEnd) {
  if (d < sliceStart) return d;
  if (d >= destSize - sliceEnd) return srcSize - (destSize - d);
  return sliceStart + (d - sliceStart) % (srcSize - sliceStart - sliceEnd);
}

// Page edges and shadow around a cover at (x, y, w, h). The cover goes on top.
void drawBookBody(const GfxRenderer& renderer, const int x, const int y, const int w, const int h) {
  const int frameX = x - frame::kCoverLeft;
  const int frameY = y - frame::kCoverTop;
  const int frameW = w + kBookExtraX;
  const int frameH = h + kBookExtraY;
  for (int dy = 0; dy < frameH; dy++) {
    const int sy = nineSliceSource(dy, frameH, frame::kHeight, frame::kSliceTop, frame::kSliceBottom);
    const bool coverRow = dy >= frame::kCoverTop && dy < frame::kCoverTop + h;
    for (int dx = 0; dx < frameW; dx++) {
      if (coverRow && dx >= frame::kCoverLeft && dx < frame::kCoverLeft + w) {
        dx = frame::kCoverLeft + w - 1;
        continue;
      }
      const int sx = nineSliceSource(dx, frameW, frame::kWidth, frame::kSliceLeft, frame::kSliceRight);
      renderer.drawPixel(frameX + dx, frameY + dy, frame::isBlack(sx, sy));
    }
  }
}

// Ribbon in the highlighter's gray (images/highlight.png), shared by the cover
// bookmark and the reader's progress ribbon so it looks the same in both.
// A paper state of -1 leaves the unpainted pixels as they are.
void fillGrayRibbon(const GfxRenderer& renderer, const int left, const int right, const int top, const int height,
                    const int notch, const bool notchOnLeft, const int paperState = -1) {
  const int half = std::max(1, height / 2);
  for (int row = 0; row < height; row++) {
    const int depth = notch * std::max(0, half - std::abs(row - half)) / half;
    const int from = notchOnLeft ? left + depth : left;
    const int to = notchOnLeft ? right : right - depth;
    const int ty = row % pulp_highlight::kHeight;
    for (int x = from; x <= to; x++) {
      if (pulp_highlight::isBlack((x - left) % pulp_highlight::kWidth, ty)) {
        renderer.drawPixel(x, top + row);
      } else if (paperState >= 0) {
        renderer.drawPixel(x, top + row, paperState != 0);
      }
    }
  }
  // Pencil outline on the free sides: the long edges and the notch. The end it
  // hangs from (screen edge, or between the pages) stays open.
  const int bottom = top + height - 1;
  const int tail = notchOnLeft ? left : right;
  const int inner = notchOnLeft ? left + notch : right - notch;
  const int head = notchOnLeft ? right : left;
  const auto dot = [&renderer](const int x, const int y) { renderer.drawPixel(x, y); };
  freeink::ui::pencilSegment(head, top, tail, top, 0x3C6EF372u, dot);
  freeink::ui::pencilSegment(tail, top, inner, top + half, 0xA54FF53Au, dot);
  freeink::ui::pencilSegment(inner, top + half, tail, bottom, 0x510E527Fu, dot);
  freeink::ui::pencilSegment(tail, bottom, head, bottom, 0x9B05688Cu, dot);
}

// Bookmark ribbon on the right of a cover at (x, y, w, h). Further in the book
// it sits lower and comes out from between later pages. Drawn before the cover.
void drawBookmark(const GfxRenderer& renderer, const int x, const int y, const int w, const int h,
                  const int percent) {
  const int p = std::clamp(percent, 0, 100);
  const int travel = h - kRibbonHeight - 2 * kRibbonMargin;
  const int top = y + kRibbonMargin + travel * p / 100;
  // Starts just past the edge of the page in front of it.
  constexpr int kGaps = kBackPageDepth / kPageStep;
  const int depth = (p * kGaps + 50) / 100 * kPageStep;
  const int left = x + w + depth;
  const int right = x + w + frame::kCoverRight + kRibbonReach;
  renderer.fillRect(left, top - 1, right - left + 2, kRibbonHeight + 2, false);  // halo off the page lines
  fillGrayRibbon(renderer, left, right, top, kRibbonHeight, kRibbonNotch, false);
}
}  // namespace

// Clippings: a pencil line under the baseline, like an editor's mark; the
// same seed always gives the same line.
static int pencilThickness(const int lineHeight) { return std::clamp(lineHeight / 10 + 1, 3, 5); }
// Air between the baseline and the line.
static int pencilGap(const int lineHeight) { return std::max(5, lineHeight / 6); }
// How much of the pencil's grain lands: muted lines read as gray.
static uint8_t pencilDarkness(const bool muted) { return muted ? 45 : 100; }

static void drawPencilUnderline(const GfxRenderer& renderer, const int fontId, const int textX, const int lineTop,
                                const int textWidth, const uint32_t seed, const bool muted = false) {
  const int lineHeight = renderer.getLineHeight(fontId);
  const int reach = lineHeight / 8;
  const freeink::ui::Rect area{static_cast<int16_t>(textX - reach),
                               static_cast<int16_t>(lineTop + renderer.getFontAscenderSize(fontId) + pencilGap(lineHeight)),
                               static_cast<int16_t>(textWidth + 2 * reach),
                               static_cast<int16_t>(pencilThickness(lineHeight))};
  freeink::ui::pencilStroke(
      area, seed, [&renderer](const int x, const int y) { renderer.drawPixel(x, y); }, true, pencilDarkness(muted));
}

// Menu selection: a pencil loop round one line of text whose line box starts
// at lineTop, as fui lists circle theirs. Drawn before the text.
static void drawHighlight(const GfxRenderer& renderer, const int fontId, const int textX, const int lineTop,
                          const std::string& text) {
  uint32_t seed = 2166136261u;
  for (const char c : text) seed = (seed ^ static_cast<uint8_t>(c)) * 16777619u;
  const int lineHeight = renderer.getLineHeight(fontId);
  const int ascender = renderer.getFontAscenderSize(fontId);
  const int padX = lineHeight / 2;
  const int padY = lineHeight / 4;
  // Cap height to descender, roughly: the ascender's top fifth is air.
  const int top = lineTop + ascender / 5 - padY;
  const int bottom = lineTop + lineHeight + padY;
  const freeink::ui::Rect area{static_cast<int16_t>(textX - padX), static_cast<int16_t>(top),
                               static_cast<int16_t>(renderer.getTextWidth(fontId, text.c_str()) + 2 * padX),
                               static_cast<int16_t>(bottom - top)};
  freeink::ui::pencilLoop(area, seed, [&renderer](const int x, const int y) { renderer.drawPixel(x, y); });
}

// A pencil asterisk left of the selection's loop, on centerY.
static void drawSelectionArrow(const GfxRenderer& renderer, const int fontId, const int textX, const int centerY) {
  const int lineHeight = renderer.getLineHeight(fontId);
  const int size = std::max(14, lineHeight * 3 / 4);
  const int cx = textX - lineHeight * 3 / 4 - lineHeight / 4;
  const freeink::ui::Rect star{static_cast<int16_t>(cx - size / 2), static_cast<int16_t>(centerY - size / 2),
                               static_cast<int16_t>(size), static_cast<int16_t>(size)};
  freeink::ui::pencilAsterisk(star, 0x2545F491u ^ static_cast<uint32_t>(centerY) * 2654435761u,
                             [&renderer](const int x, const int y) { renderer.drawPixel(x, y); });
}

// The pencil line without its shape: the grain follows the screen, so
// neighbouring words' lines join without a seam.
bool PulpTheme::drawTextHighlightBand(const GfxRenderer& renderer, const int x, const int lineTop, const int width,
                                      const int lineHeight, const bool muted) const {
  const int fontId = SETTINGS.getReaderFontId();
  const freeink::ui::Rect area{static_cast<int16_t>(x),
                               static_cast<int16_t>(lineTop + renderer.getFontAscenderSize(fontId) + pencilGap(lineHeight)),
                               static_cast<int16_t>(width), static_cast<int16_t>(pencilThickness(lineHeight))};
  freeink::ui::pencilStroke(
      area, 0, [&renderer](const int px, const int py) { renderer.drawPixel(px, py); }, false, pencilDarkness(muted));
  return true;
}

bool PulpTheme::drawTextHighlighter(const GfxRenderer& renderer, const int textX, const int lineTop,
                                    const int textWidth, const int lineHeight, const uint32_t seed,
                                    const bool muted) const {
  (void)lineHeight;
  drawPencilUnderline(renderer, SETTINGS.getReaderFontId(), textX, lineTop, textWidth, seed, muted);
  return true;
}

// A pencil box round the cursor's word, clipped to the box so erasing it
// leaves nothing behind. A thin line while browsing; once a selection starts
// from it, a heavy one filling the frame's margin.
bool PulpTheme::drawTextCursor(const GfxRenderer& renderer, const int x, const int y, const int width,
                               const int height, const bool anchored) const {
  const int thickness = anchored ? 3 : 1;
  const int left = x;
  const int top = y;
  const int right = x + width - thickness;
  const int bottom = y + height - thickness;
  const auto dot = [&renderer, x, y, width, height](const int px, const int py) {
    if (px >= x && px < x + width && py >= y && py < y + height) renderer.drawPixel(px, py);
  };
  const uint32_t seed = static_cast<uint32_t>(x * 7919 + y * 104729) ^ (anchored ? 0x6A09E667u : 0u);
  freeink::ui::pencilSegment(left, top, right, top, seed ^ 0x3C6EF372u, dot, thickness);
  freeink::ui::pencilSegment(right, top, right, bottom, seed ^ 0xA54FF53Au, dot, thickness);
  freeink::ui::pencilSegment(right, bottom, left, bottom, seed ^ 0x510E527Fu, dot, thickness);
  freeink::ui::pencilSegment(left, bottom, left, top, seed ^ 0x9B05688Cu, dot, thickness);
  return true;
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
  const int coverY = rect.y + kCoverInset + frame::kCoverTop;

  // Only load from SD on first render, then reuse the stored buffer.
  if (!coverRendered) {
    const RecentBook& book = recentBooks[0];
    const int percent = loadBookProgress(book.path);
    bool hasCover = false;
    if (!book.coverBmpPath.empty()) {
      const std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
      HalFile file;
      if (Storage.openFileForRead("HOME", thumbPath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          coverWidth = std::min(bitmap.getWidth(), rect.width - 2 * kCoverInset - kBookExtraX - 2 * kRibbonReach);
          const int x = rect.x + (rect.width - coverWidth - kBookExtraX) / 2 + frame::kCoverLeft;
          drawBookBody(renderer, x, coverY, coverWidth, coverHeight);
          if (percent >= 0) drawBookmark(renderer, x, coverY, coverWidth, coverHeight, percent);
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
      const int x = rect.x + (rect.width - coverWidth - kBookExtraX) / 2 + frame::kCoverLeft;
      drawBookBody(renderer, x, coverY, coverWidth, coverHeight);
      if (percent >= 0) drawBookmark(renderer, x, coverY, coverWidth, coverHeight, percent);
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
  const int labelTop = coverY + coverHeight + frame::kCoverBottom;
  const int labelHeight = rect.y + rect.height - labelTop;
  const std::string label =
      renderer.truncatedText(fontId, tr(STR_CONTINUE_READING), cardWidth - 2 * kHintPaddingX, EpdFontFamily::REGULAR);
  if (selectorIndex == 0) {
    const int labelX = (renderer.getScreenWidth() - renderer.getTextWidth(fontId, label.c_str())) / 2;
    drawHighlight(renderer, fontId, labelX, labelTop + (labelHeight - renderer.getLineHeight(fontId)) / 2, label);
    drawSelectionArrow(renderer, fontId, labelX, labelTop + labelHeight / 2);
  }
  renderer.drawCenteredText(fontId, labelTop + (labelHeight - renderer.getCenteringHeight(fontId)) / 2, label.c_str());
}

// Toasts set in the reader font, like the rest of the theme's text.
int PulpTheme::popupFontId() const { return SETTINGS.getReaderFontId(); }

// A white card centred on the screen, lifted off the page by a dithered
// shadow rather than a frame; the message is italic like the theme's notes.
Rect PulpTheme::drawPopup(const GfxRenderer& renderer, const char* message) const {
  constexpr int kMargin = 8;  // keeps the page's text off the card
  constexpr int kShadow = 4;
  const auto& metrics = PulpMetrics::values;
  const int fontId = popupFontId();
  const int textWidth = renderer.getTextWidth(fontId, message, EpdFontFamily::ITALIC);
  const int w = textWidth + metrics.popupMarginX * 2;
  const int h = renderer.getLineHeight(fontId) + metrics.popupMarginY * 2;
  const int x = (renderer.getScreenWidth() - w) / 2;
  const int y = (renderer.getScreenHeight() - h) / 2;
  renderer.fillRect(x - kMargin, y - kMargin, w + 2 * kMargin + kShadow, h + 2 * kMargin + kShadow, false);
  renderer.fillRectDither(x + kShadow, y + kShadow, w, h, Color::DarkGray);
  renderer.fillRect(x, y, w, h, false);
  renderer.drawText(fontId, x + metrics.popupMarginX, y + metrics.popupMarginY + metrics.popupTextBaselineOffsetY,
                    message, true, EpdFontFamily::ITALIC);
  renderer.displayBuffer();
  return Rect{x, y, w, h};
}

void PulpTheme::drawProgressRibbon(const GfxRenderer& renderer, const int left, const int right, const int top,
                                   const int height, const int notch, const bool paperState) const {
  // Over an image (the sleep cover) the ribbon brings its own paper.
  fillGrayRibbon(renderer, left, right, top, height, notch, true, paperState ? 1 : 0);
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
  return homeCoverTileHeight(renderer) - kCoverInset - kBookExtraY - getMenuRowHeight(renderer);
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
      drawSelectionArrow(renderer, fontId, textX, rowY + rowHeight / 2);
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
  drawEdgeTab(renderer, kTabTop, kTabHeight, topBtn, bottomBtn);
}

void PulpTheme::drawPowerButtonHint(const GfxRenderer& renderer, const char* label) const {
  if (gpio.hasTouch() || gpio.hasEdgeSideButtons() || label == nullptr || label[0] == '\0') return;
  // X4 power button, near the top of the right edge.
  constexpr int kTabTop = 90;
  const int height = renderer.getTextWidth(kGuideFontId, upperAscii(label).c_str()) + 2 * kTabTextInset;
  drawEdgeTab(renderer, kTabTop, height, label, nullptr);
}
