#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Bitmap;
class GfxRenderer;
struct RecentBook;
namespace freeink {
namespace ui {
struct HeaderProps;
struct BitmapRef;
struct ListItem;
}  // namespace ui
}  // namespace freeink

struct Rect {
  int x;
  int y;
  int width;
  int height;

  explicit Rect(int x = 0, int y = 0, int width = 0, int height = 0) : x(x), y(y), width(width), height(height) {}
};

struct ThemeMetrics {
  int batteryWidth;
  int batteryHeight;

  int topPadding;
  int batteryBarHeight;
  int headerHeight;
  int verticalSpacing;

  int previewPadding;
  int previewHeightPercent;

  int contentSidePadding;
  int listRowHeight;
  int listWithSubtitleRowHeight;
  // FreeInkUI list shape, consumed by uiThemeTokens() for screens rendered
  // through FreeInkApp: the theme supplies geometry and selection style, the
  // uiScale fonts supply the sizes. Plain data by design — the eventual
  // SD-card theme files will provide exactly these values.
  int listRowGap;          // vertical gap between rows
  int listRowRadius;       // row corner radius (RoundedRaff cards, Lyra pill)
  int listInset;           // horizontal inset of the whole list band
  int listSidePadding;     // text inset within a row
  int listSelectionStyle;  // 0=invert fill, 1=light pill, 2=underline, 3=triangle, 4=highlighter, 5=pencil
                           // (fui::SelectionStyle order)
  int listScrollWidth;     // scroll indicator thickness
  int listScrollSide;      // 0 = right edge, 1 = left edge
  bool listTitleBold;      // bold row titles (RoundedRaff)
  // Outline drawn around the selected row instead of the selection style's
  // fill (Pulp); 0 = none.
  int listSelectionBorder = 0;
  // Lists set like a book (Pulp): centered italic section headings with no
  // rule, and the book's own title over its table of contents.
  bool bookStyleLists = false;
  // Lists show "N more" / "N above" in a row slot instead of a scrollbar (Pulp).
  bool listOverflowCounts = false;
  // Gap between the scroll indicator and the screen edge.
  int listScrollInset = 0;
  // Rows a list keeps visible past the selection as it scrolls (Pulp).
  int listScrollMargin = 0;
  // Sheets are edged with a pencil line instead of a rule or shadow (Pulp).
  bool sheetPencilRule = false;
  // Popup dialogs are outlined with a pencil line instead of a border (Pulp).
  bool popupPencilFrame = false;
  // UI text uses the current reader font instead of the built-in UI font (Pulp).
  bool uiUsesReaderFont = false;
  // Settings is one list with a heading per category instead of tabs (Pulp).
  bool settingsSingleList = false;
  // Settings with several values step to the next one in place instead of opening a picker (Pulp).
  bool settingsCycleValues = false;
  // Select in a book opens a short menu sheet over the page instead of the
  // reader menu style's toolbar or full-screen list (Pulp).
  bool readerMenuSheet = false;
  // Slider dialogs (go to %, intervals) open as a bottom sheet, not a centered popup (Pulp).
  bool sliderDialogSheet = false;
  // Status bar like a book's running head: pages left ("6", not "25/31") and the
  // chapter title in the reader font's italic (Pulp).
  bool statusBarBookStyle = false;
  // In the reader, the status bar's text line runs along the top of the page; a
  // progress bar stays at the bottom (Pulp).
  bool statusBarTextAtTop = false;
  // The reader's progress bar is a ribbon on the right page edge, lower as more
  // is read, like the cover bookmark (Pulp).
  bool progressRibbon = false;
  // Home labels the side buttons as well as the front ones (Pulp).
  bool homeSideButtonHints = false;
  // FreeInkUI header shape, same contract as the list fields above.
  int headerSidePadding;    // title text inset
  int headerUnderlineSize;  // bottom rule thickness (Lyra), 0 = none
  int headerTitleAlign;     // 0 = left, 1 = center, 2 = right (fui::TextAlign order)
  int headerBatterySide;    // 0 = right edge, 1 = left edge
  bool headerTitleBold = false;
  // Header clock opt-out for themes whose title layout can't spare the left
  // reserve (RoundedRaff); the user setting still governs the themes that can.
  bool headerShowsClock = true;
  // Clock slot: centered on the band, or on the left after the back arrow.
  bool headerClockCentered = true;
  // Battery indicator in the header's status strip (off for Pulp).
  bool headerShowsBattery = true;
  int menuRowHeight;
  int menuSpacing;

  int tabSpacing;
  int tabBarHeight;
  int coverGridTabBarHeight = 72;
  // Selected-tab pill fills its equal-width slot (legacy RoundedRaff tabs)
  // instead of shrinking to hug the label (legacy Lyra tabs).
  bool tabPillFullSlot = false;

  int scrollBarWidth;
  int scrollBarRightOffset;

  int homeTopPadding;
  int homeCoverHeight;
  int homeCoverTileHeight;
  int homeRecentBooksCount;
  bool homeContinueReadingInMenu;
  // Home menu is only Books (the Library) and Settings (Pulp).
  bool homeMinimalMenu = false;
  // Library is one Books list: File Transfer, then Recent and On This Device
  // sections, with no sort tabs or search (Pulp).
  bool libraryBookShelf = false;
  // The cover sleep screen marks reading progress with a ribbon on the right edge (Pulp).
  bool sleepCoverBookmark = false;
  int homeMenuTopOffset;

  int buttonHintsHeight;
  int sideButtonHintsWidth;

  int progressBarHeight;
  int progressBarMarginTop;
  int statusBarHorizontalMargin;
  int statusBarVerticalMargin;
  int keyboardKeyHeight;
  int keyboardKeySpacing;
  bool keyboardCenteredText;
  int keyboardVerticalOffset;
  int keyboardTextFieldWidthPercent;
  int keyboardWidthPercent;

  float popupTopOffsetRatio;
  int popupMarginX;
  int popupMarginY;
  int popupFrameThickness;
  int popupCornerRadius;
  bool popupTextBold;
  bool popupTextInverted;
  int popupTextBaselineOffsetY;
  int popupProgressBarHeight;
  bool popupProgressDrawOutline;
  bool popupProgressClampPercent;
  bool popupProgressFillInverted;
  bool popupProgressOutlineInverted;

  int optionPopupItemSpacing;
  int optionPopupInnerPadding;
  int optionPopupSelectionVPadding;
  int optionPopupDialogSideMargin;

  int textFieldHorizontalPadding;
  int textFieldNormalThickness;
  int textFieldCursorThickness;
  int textFieldLineEndOffset;

  // FreeInkUI control shape (the control center panel), same contract as the
  // list fields above: quick-setting tiles and slider step buttons, the
  // sheet's free-edge corners, and the capsule slider's corners (255 = full
  // stadium, i.e. radius = half the control height).
  int controlRadius;
  int sheetRadius;
  int capsuleRadius;
};

enum UIIcon {
  None = 0,
  Folder,
  Text,
  Image,
  Book,
  File,
  Recent,
  Settings,
  Transfer,
  Library,
  Plugins,
  Wifi,
  Hotspot,
  Bookmark,
  Usb,
  Blocks
};

// Default theme implementation (Classic Theme)
// Additional themes can inherit from this and override methods as needed

namespace BaseMetrics {
constexpr ThemeMetrics values = {.batteryWidth = 15,
                                 .batteryHeight = 12,
                                 .topPadding = 5,
                                 .batteryBarHeight = 20,
                                 .headerHeight = 84,
                                 .verticalSpacing = 10,
                                 .previewPadding = 12,
                                 .previewHeightPercent = 30,
                                 .contentSidePadding = 20,
                                 .listRowHeight = 30,
                                 .listWithSubtitleRowHeight = 50,
                                 .listRowGap = 0,
                                 .listRowRadius = 0,
                                 .listInset = 0,
                                 .listSidePadding = 20,
                                 .listSelectionStyle = 0,  // invert fill
                                 .listScrollWidth = 4,
                                 .listScrollSide = 0,
                                 .listTitleBold = false,
                                 .headerSidePadding = 18,
                                 .headerUnderlineSize = 0,
                                 .headerTitleAlign = 1,  // centered
                                 .headerBatterySide = 0,
                                 // Corner clock: a centered clock would collide with the centered title.
                                 .headerClockCentered = false,
                                 .menuRowHeight = 45,
                                 .menuSpacing = 8,
                                 .tabSpacing = 10,
                                 .tabBarHeight = 50,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 .homeTopPadding = 40,
                                 .homeCoverHeight = 400,
                                 .homeCoverTileHeight = 400,
                                 .homeRecentBooksCount = 1,
                                 .homeContinueReadingInMenu = false,
                                 .homeMenuTopOffset = 10,
                                 .buttonHintsHeight = 40,
                                 .sideButtonHintsWidth = 30,
                                 .progressBarHeight = 16,
                                 .progressBarMarginTop = 1,
                                 .statusBarHorizontalMargin = 5,
                                 .statusBarVerticalMargin = 19,
                                 .keyboardKeyHeight = 56,
                                 .keyboardKeySpacing = 0,
                                 .keyboardCenteredText = false,
                                 .keyboardVerticalOffset = -13,
                                 .keyboardTextFieldWidthPercent = 85,
                                 .keyboardWidthPercent = 94,
                                 .popupTopOffsetRatio = 0.075f,
                                 .popupMarginX = 15,
                                 .popupMarginY = 15,
                                 .popupFrameThickness = 2,
                                 .popupCornerRadius = 0,
                                 .popupTextBold = true,
                                 .popupTextInverted = true,
                                 .popupTextBaselineOffsetY = -2,
                                 .popupProgressBarHeight = 4,
                                 .popupProgressDrawOutline = false,
                                 .popupProgressClampPercent = false,
                                 .popupProgressFillInverted = true,
                                 .popupProgressOutlineInverted = true,
                                 .optionPopupItemSpacing = 6,
                                 .optionPopupInnerPadding = 16,
                                 .optionPopupSelectionVPadding = 4,
                                 .optionPopupDialogSideMargin = 20,
                                 .textFieldHorizontalPadding = 6,
                                 .textFieldNormalThickness = 1,
                                 .textFieldCursorThickness = 3,
                                 .textFieldLineEndOffset = 0,
                                 .controlRadius = 0,
                                 .sheetRadius = 0,
                                 .capsuleRadius = 0};
}

// Running-head form of a chapter title: "CHAPTER 3: THE RING GOES SOUTH" ->
// "The Ring Goes South".
void shortenChapterTitle(std::string& title);

class BaseTheme {
 public:
  virtual ~BaseTheme() = default;

  // Component drawing methods
  static freeink::ui::BitmapRef checkboxIcon(bool checked);
  static void setCheckboxRow(freeink::ui::ListItem& item, bool checked);
  static void drawCoverPlaceholder(const GfxRenderer& renderer, Rect rect);
  // Draws a pre-dithered cover thumb 1:1, centered and clipped to fill the
  // slot. Rescaling a dithered bitmap aliases badly, so overflow is cropped.
  static bool drawCoverThumbFill(const GfxRenderer& renderer, const Bitmap& bitmap, Rect slot, int xOffset = 0);
  static void drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total);
  // Bookmark ribbon from left to right with a notched tail on one end.
  static void fillRibbon(const GfxRenderer& renderer, int left, int right, int top, int height, int notch,
                         bool notchOnLeft);
  // The reader's progress ribbon (metrics.progressRibbon), notched on the left.
  // paperState fills the ribbon's unpainted pixels, for drawing over images.
  virtual void drawProgressRibbon(const GfxRenderer& renderer, int left, int right, int top, int height, int notch,
                                  bool paperState = false) const {
    fillRibbon(renderer, left, right, top, height, notch, true);
  }
  // Places the progress ribbon where the reader page shows it; the sleep cover reuses it.
  static void drawReaderProgressRibbon(const GfxRenderer& renderer, int percent, bool textAtTop, int paddingBottom = 0,
                                       bool paperState = false);
  // Blanks the margin column the reader progress ribbon travels in.
  static void clearReaderProgressRibbon(const GfxRenderer& renderer);
  void drawBatteryLeft(const GfxRenderer& renderer, Rect rect,
                       bool showPercentage = true) const;  // Left aligned (reader mode)
  virtual void fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const;
  virtual void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                               const char* btn4) const;
  // Shared by every theme's drawButtonHints(): centres a hint label in its box,
  // wrapping to two lines rather than overflowing when it's too wide to fit.
  static void drawHintLabel(const GfxRenderer& renderer, int fontId, const char* label, int x, int boxWidth, int boxTop,
                            int boxHeight);
  virtual void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const;
  // Labels the power button where the theme supports it.
  virtual void drawPowerButtonHint(const GfxRenderer& renderer, const char* label) const {}
  // Menu row height as DRAWN by drawButtonMenu. HomeActivity builds its touch
  // grid from this, so hit bands always match the visuals (RoundedRaff derives
  // its row height from the font, not the metrics table).
  virtual int getMenuRowHeight(const GfxRenderer& renderer) const;
  // Also draws the wall clock opposite the battery when the user enabled
  // SETTINGS.clockShowInHeader and an RTC is present. On touch boards a
  // tappable back button leads the band (see HeaderBackTapTarget); root
  // screens that own their stack bottom pass backButton = false.
  // titleMaxLines > 1 wraps the title; the caller sizes rect for the lines.
  virtual void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle = nullptr,
                          bool backButton = true, uint8_t titleMaxLines = 1) const;
  // Fill the battery/clock status chrome (settings + theme metrics) into
  // header props, so FUI-native screens drawing their own interactive header
  // carry the same band as drawHeader. Status text is styled with the
  // FONT_LABEL slot (bound to the fixed small font by makeUiTarget and
  // drawHeader). The label strings point at internal static buffers refreshed
  // per call (headers draw on the single render task).
  static void applyHeaderStatus(const GfxRenderer& renderer, freeink::ui::HeaderProps& props);
  // Edge inset drawHeader uses for the clock/battery status line (detached
  // layouts hug the corner with a legacy 12px inset instead of the padding).
  static int headerStatusInset();
  virtual void drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                             const char* rightLabel = nullptr) const;
  virtual void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                   const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                   bool& bufferRestored, std::function<bool()> storeCoverBuffer) const;
  virtual void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                              const std::function<std::string(int index)>& buttonLabel,
                              const std::function<UIIcon(int index)>& rowIcon) const;
  static void drawSelectionHandle(const GfxRenderer& renderer, Rect rect, bool left);
  // editing: a saved clipping is selected; adds Delete and saves with Done.
  static void drawSelectionActions(const GfxRenderer& renderer, Rect rect, bool editing = false);
  virtual Rect drawPopup(const GfxRenderer& renderer, const char* message) const;
  // Font for drawPopup's message.
  virtual int popupFontId() const;
  virtual void fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const;
  static void drawStatusBar(GfxRenderer& renderer, const float bookProgress, const int currentPage, const int pageCount,
                            std::string title, const int paddingBottom = 0, const int textYOffset = 0,
                            const bool fillMargin = true, const bool isPageBookmarked = false,
                            const bool pageCountEstimated = false, const bool textAtTop = false);
  static void drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label);
  virtual void drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode = false,
                             int contentStartX = 0, int contentWidth = 0) const;
  virtual bool showsFileIcons() const { return false; }
  // Thumb generation height for home covers; 0 means use metrics.homeCoverHeight.
  // Themes with slots wider than 0.6 aspect override this so covers still fill.
  virtual int homeCoverThumbHeight(const GfxRenderer&) const { return 0; }
  // Home cover tile height; 0 means use metrics.homeCoverTileHeight.
  virtual int homeCoverTileHeight(const GfxRenderer&) const { return 0; }
  // Books shelf cover grid: thumb height for its covers, 0 for no grid.
  virtual int shelfCoverHeight(const GfxRenderer&) const { return 0; }
  // Height of one grid row, selection marks included.
  virtual int shelfCellHeight(const GfxRenderer&) const { return 0; }
  // One recent book in its grid cell.
  virtual void drawShelfBook(GfxRenderer&, Rect, const RecentBook&, int, bool) const {}
  // Highlighter-pen stroke behind a run of text, before the text is drawn.
  // Returns false when the theme has none (callers draw their own fill).
  // Straight highlighter band behind text in a line box at lineTop, for
  // selections built up word by word. False: callers draw their own fill.
  // Muted: lighter, for saved highlights behind a selection being made.
  virtual bool drawTextHighlightBand(const GfxRenderer&, int /*x*/, int /*lineTop*/, int /*width*/, int /*lineHeight*/,
                                     bool /*muted*/ = false) const {
    return false;
  }
  virtual bool drawTextHighlighter(const GfxRenderer&, int /*textX*/, int /*lineTop*/, int /*textWidth*/,
                                   int /*lineHeight*/, uint32_t /*seed*/, bool /*muted*/ = false) const {
    return false;
  }
  // Frame round the word under a text cursor, kept inside the given box;
  // anchored once a selection has started from it. False: callers draw their own.
  virtual bool drawTextCursor(const GfxRenderer&, int /*x*/, int /*y*/, int /*width*/, int /*height*/,
                              bool /*anchored*/) const {
    return false;
  }

  // Shared constants and helpers for battery drawing (used by all themes)
  static constexpr int batteryPercentSpacing = 4;
  static void drawBatteryOutline(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight);
  static void drawBatteryLightningBolt(const GfxRenderer& renderer, int boltX, int boltY);
};
