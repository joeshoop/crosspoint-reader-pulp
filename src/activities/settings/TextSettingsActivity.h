#pragma once

#include <SdCardFontRegistry.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "TextSettingsPreview.h"
#include "activities/UiTabListActivity.h"
#include "components/OptionPopup.h"
#include "components/themes/BaseTheme.h"

// Reader text settings with a shared live preview pane: tab bar
// (Font | Size | Layout | Style) is position 0 of the Up/Down nav ring, same
// idiom as SettingsActivity. Family/Size rows apply on Confirm; Layout/Style
// rows toggle or open an OptionPopup picker. (Tab::Family/Style are the enum
// names for the Font/Style tabs.)
class TextSettingsActivity final : public UiTabListActivity {
 public:
  enum class Tab : uint8_t { Family, Size, Layout, Style, Count };

  // sheetTop >= 0 (single-list themes, opened from the reader's menu sheet):
  // the rows sit in a bottom sheet with its top edge at that screen y, over the
  // page the reader left in the framebuffer. drawRunningHead redraws the
  // reader's running head when an edit changes the page's text.
  TextSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const SdCardFontRegistry* registry,
                       Tab initialTab = Tab::Family, int sheetTop = -1,
                       std::function<void()> drawRunningHead = nullptr);

  void onEnter() override;
  void render(RenderLock&&) override;

 private:
  // Row indices per tab. enum class (not plain enum) so a LayoutRow can't be
  // silently confused with a StyleRow of equal value.
  enum class LayoutRow {
    LineSpacing,
    WordSpacing,
    CharacterSpacing,
    ParaSpacing,
    ParaIndentation,
    Alignment,
    ScreenMargin,
    Count
  };
  enum class StyleRow { FocusReading, Hyphenation, EmbeddedStyle, AntiAliasing, Count };

  // --- UiTabListActivity contract ---
  int listCount() const override;
  int tabCount() const override { return static_cast<int>(Tab::Count); }
  int activeTab() const override { return static_cast<int>(tab_); }
  const char* tabLabel(int index) const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onTabAction(int index) override;
  void stepTab(int direction) override { switchTab(direction); }
  void drawChrome() override;
  void drawFooter() override;
  bool handleButtons() override;
  bool handleCustomInput() override;
  void navigateButtons() override;
  bool rowStartsSection(int row) const override { return row == 0 || row == kLayoutStart || row == kStyleStart; }

  // Single-list themes (settingsSingleList): no tab bar; one list of Font,
  // Size, then the Layout and Style rows, each stepping its value in place.
  static bool usesSingleList();
  static constexpr int kFontRow = 0;
  static constexpr int kSizeRow = 1;
  static constexpr int kLayoutStart = 2;
  static constexpr int kStyleStart = kLayoutStart + static_cast<int>(LayoutRow::Count);
  static constexpr int kSingleListCount = kStyleStart + static_cast<int>(StyleRow::Count);
  void buildSingleList(UiScreen& screen);
  void activateSingleListRow(int row);

  // Sheet mode: the page above the sheet stays as the reader drew it until a
  // setting changes its text, then the preview redraws it in the reader's text box.
  const int sheetTop_;
  const std::function<void()> drawRunningHead_;
  bool inSheet() const { return sheetTop_ >= 0 && usesSingleList(); }
  void renderSheet();
  // Screen rows the page preview owns: below the running head, above the sheet's shadow.
  int pageBandTop() const;
  int pageBandBottom() const;
  textsettings::PreviewKey entryKey_;
  bool pageEdited_ = false;
  void cycleLayoutRow(int row);
  std::string singleListValueText(int row) const;

  void applyFamily(int listIndex);
  void applySize(int listIndex);
  // Repopulates sizes_ (and currentSizeIndex_) from the active family's
  // installed point sizes. Call after any family change.
  void rebuildSizeList();
  void confirmLayoutRow(int row);
  void confirmStyleRow(int row);
  // Applies the row at the given list index for the active tab (Confirm and tap share this).
  void activateRow(int row);

  std::string layoutValueText(int row) const;
  bool styleRowChecked(int row) const;
  // Button-hint label for Confirm at the current ring position.
  const char* confirmLabelText() const;
  // True when the focused list row is a setting the preview cannot reflect.
  bool focusedRowHasNoPreview() const;
  void present() override;
  void switchTab(int direction = 1);

  // Row storage for the active tab: rowItems_ (label/actionValue) is
  // rebuilt only when the tab or its backing data changes (rebuildRowItems(),
  // called from onEnter()/onTabAction()/switchTab()); rowValues_ holds the
  // live per-row value text, refreshed every buildScreen() call by assigning
  // into the existing strings (no vector growth), so steady-state rendering
  // never allocates/frees row storage.
  std::vector<std::string> rowValues_;
  std::vector<freeink::ui::ListItem> rowItems_;
  void rebuildRowItems();

  struct FontEntry {
    std::string name;
    bool isBuiltin;
    uint8_t settingIndex;
  };

  struct SizeEntry {
    std::string name;  // the point size, rendered for display ("14 pt")
    uint8_t pointSize;
  };

  const SdCardFontRegistry* registry_;
  OptionPopup optionPopup_;
  std::vector<FontEntry> fonts_;
  std::vector<SizeEntry> sizes_;
  textsettings::PreviewLayout previewLayout_;  // cached preview line layout; relaid only on setting/geometry change

  Tab tab_;
  int currentFamilyIndex_ = 0;
  int currentSizeIndex_ = 0;

  ThemeMetrics metrics_ = {};
  int afterHeader = 0;
  int bottomReserved = 0;
  int usableHeight = 0;
  int previewHeight = 0;
  // Hash of the preview region whose grays are on the glass; 0 when none are.
  uint32_t grayPreviewHash_ = 0;
};
