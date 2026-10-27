#pragma once

#include "components/themes/BaseTheme.h"

class GfxRenderer;

// Pulp: menus set like the pages of a book. Every UI text slot uses the
// current reader font; the selection is a thin outline rather than a fill.
namespace PulpMetrics {
constexpr ThemeMetrics values = {.batteryWidth = 15,
                                 .batteryHeight = 12,
                                 .topPadding = 13,
                                 // Title row only: a 20px top margin, then a 41px row like the list rows.
                                 .batteryBarHeight = 20,
                                 .headerHeight = 61,
                                 .verticalSpacing = 10,
                                 .previewPadding = 12,
                                 .previewHeightPercent = 30,
                                 .contentSidePadding = 24,
                                 .listRowHeight = 41,
                                 .listWithSubtitleRowHeight = 64,
                                 .listRowGap = 0,
                                 .listRowRadius = 4,
                                 .listInset = 8,
                                 .listSidePadding = 16,
                                 .listSelectionStyle = 5,  // fui::SelectionStyle::Pencil
                                 .listScrollWidth = 5,
                                 .listScrollSide = 0,
                                 .listTitleBold = false,
                                 .listSelectionBorder = 0,
                                 .bookStyleLists = true,
                                 .listScrollInset = 4,
                                 .listScrollMargin = 3,
                                 .sheetPencilRule = true,
                                 .popupPencilFrame = true,
                                 .uiUsesReaderFont = true,
                                 .settingsSingleList = true,
                                 .settingsCycleValues = true,
                                 .readerMenuSheet = true,
                                 .sliderDialogSheet = true,
                                 .statusBarBookStyle = true,
                                 .statusBarTextAtTop = true,
                                 .progressRibbon = true,
                                 .homeSideButtonHints = true,
                                 .headerSidePadding = 16,
                                 .headerUnderlineSize = 0,
                                 .headerTitleAlign = 1,  // centered
                                 .headerBatterySide = 0,
                                 .headerTitleBold = true,
                                 // Corner clock: a centered clock would collide with the centered title.
                                 .headerClockCentered = false,
                                 .headerShowsBattery = false,
                                 .menuRowHeight = 44,  // not authoritative: getMenuRowHeight() derives the drawn height
                                 .menuSpacing = 4,
                                 .tabSpacing = 10,
                                 .tabBarHeight = 50,
                                 .scrollBarWidth = 2,
                                 .scrollBarRightOffset = 5,
                                 .homeTopPadding = 40,
                                 // Cover plus the "Continue Reading" band inside the card frame.
                                 .homeCoverHeight = 420,
                                 .homeCoverTileHeight = 470,
                                 .homeRecentBooksCount = 4,
                                 .homeContinueReadingInMenu = false,
                                 .homeMinimalMenu = true,
                                 .homeShelfGrid = true,
                                 .libraryBookShelf = true,
                                 .sleepCoverBookmark = true,
                                 .homeMenuTopOffset = 4,  // = menuSpacing: Continue Reading spaced like a menu row
                                 .buttonHintsHeight = 20,
                                 .sideButtonHintsWidth = 30,
                                 .progressBarHeight = 16,
                                 .progressBarMarginTop = 1,
                                 .statusBarHorizontalMargin = 5,
                                 .statusBarVerticalMargin = 19,
                                 .keyboardKeyHeight = 36,
                                 .keyboardKeySpacing = 6,
                                 .keyboardCenteredText = true,
                                 .keyboardVerticalOffset = 0,
                                 .keyboardTextFieldWidthPercent = 85,
                                 .keyboardWidthPercent = 94,
                                 .popupTopOffsetRatio = 0.12f,
                                 .popupMarginX = 20,
                                 .popupMarginY = 14,
                                 .popupFrameThickness = 2,
                                 .popupCornerRadius = 4,
                                 .popupTextBold = false,
                                 .popupTextInverted = false,
                                 .popupTextBaselineOffsetY = -2,
                                 .popupProgressBarHeight = 4,
                                 .popupProgressDrawOutline = true,
                                 .popupProgressClampPercent = true,
                                 .popupProgressFillInverted = false,
                                 .popupProgressOutlineInverted = false,
                                 .optionPopupItemSpacing = 0,
                                 .optionPopupInnerPadding = 16,
                                 .optionPopupSelectionVPadding = 8,
                                 .optionPopupDialogSideMargin = 20,
                                 .textFieldHorizontalPadding = 8,
                                 .textFieldNormalThickness = 1,
                                 .textFieldCursorThickness = 2,
                                 .textFieldLineEndOffset = 0,
                                 .controlRadius = 4,
                                 .sheetRadius = 4,
                                 .capsuleRadius = 4};
}

class PulpTheme : public BaseTheme {
 public:
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer) const override;
  int getMenuRowHeight(const GfxRenderer& renderer) const override;
  int popupFontId() const override;
  Rect drawPopup(const GfxRenderer& renderer, const char* message) const override;
  int homeCoverTileHeight(const GfxRenderer& renderer) const override;
  int homeCoverThumbHeight(const GfxRenderer& renderer) const override;
  int shelfCoverHeight(const GfxRenderer& renderer) const override;
  int shelfCellHeight(const GfxRenderer& renderer) const override;
  int shelfBookWidth(int coverHeight) const override;
  int shelfSideZone() const override;
  void drawShelfBook(GfxRenderer& renderer, Rect cell, const RecentBook& book, int coverHeight,
                     bool selected) const override;
  freeink::ui::BitmapRef shelfBookIcon() const override;
  bool drawTextHighlightBand(const GfxRenderer& renderer, int x, int lineTop, int width, int lineHeight,
                             bool muted) const override;
  bool drawTextHighlighter(const GfxRenderer& renderer, int textX, int lineTop, int textWidth, int lineHeight,
                           uint32_t seed, bool muted) const override;
  bool drawTextCursor(const GfxRenderer& renderer, int x, int y, int width, int height, bool anchored) const override;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;
  void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                       const char* btn4) const override;
  void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const override;
  void drawPowerButtonHint(const GfxRenderer& renderer, const char* label) const override;
  void drawProgressRibbon(const GfxRenderer& renderer, int left, int right, int top, int height, int notch,
                          bool paperState) const override;
};
