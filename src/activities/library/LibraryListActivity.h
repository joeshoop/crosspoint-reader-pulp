#pragma once

#include <LibraryIndexFile.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/UiTabListActivity.h"
#include "components/OptionPopup.h"

// One Library screen: every indexed book on the card shown by recency, title,
// or author. The Recent shelf orders by file modification time (when a book
// landed on the card) and pins the recently OPENED books from RecentBooksStore
// on top, so active reads and fresh arrivals share one list.
//
// The two-slot row is the whole point rather than a styling choice: the problem
// being solved is "I cannot find my books because I do not know the authors",
// and that is answered by a column the eye can sweep, not by a tidier filename.
//
// Rows render through fui::list on the UiTabListActivity ring (0 = the sort
// strip, 1..N = the books), which is what brings touch to rows and tabs. Titles
// are truncated to one line by the widget — more books on the screen, even if
// half a name is hidden.
//
// Only the visible window of rows is materialized per render (strings and
// ListItems for at most one page). The ordinary shelf therefore keeps one page
// of strings; an active search additionally uses one fallible uint16_t slot per
// indexed book so an allocation failure remains recoverable on the C3.
class LibraryListActivity final : public UiTabListActivity {
 public:
  LibraryListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;

 protected:
  // --- UiListActivity / UiTabListActivity contract ---------------------------
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  int tabCount() const override;
  int activeTab() const override;
  const char* tabLabel(int index) const override;
  freeink::ui::TabIndicator tabIndicator(int index) const override;
  void onTabAction(int index) override;
  void stepTab(int direction) override;
  bool handleCustomInput() override;
  bool handleButtons() override;
  void navigateButtons() override;
  // Shelf sections: File Transfer, Recent, On This Device.
  bool rowStartsSection(int row) const override {
    return row == 0 || (bookShelf() && (row == shelfCoverCount() || row == shelfRows()));
  }
  // The FreeInkUI header owns both the title and search touch target.
  void drawChrome() override {}
  void drawFooter() override;
  // OptionPopup is a self-contained modal: it owns rendering (and the button
  // hints) whenever it is up.
  void render(RenderLock&& lock) override;

 private:
  // The screen's own actions, after the base's ACTION_ROW / ACTION_TAB.
  static constexpr freeink::ui::ActionId ACTION_SEARCH = ACTION_TAB_USER;
  static constexpr freeink::ui::ActionId ACTION_REBUILD = ACTION_SEARCH + 1;
  static constexpr freeink::ui::ActionId ACTION_BACK = ACTION_REBUILD + 1;

  // Walk the card and write a fresh index. Blocking, with a popup: at ~70 books
  // it is well under a second, and it only runs when the index is missing or the
  // user asks. `changed` reports whether any book was added, removed, or edited.
  bool rebuildIndex(bool* changed = nullptr);
  // The once-per-boot reconcile, run after the first paint; repaints on change.
  void reconcileAfterPaint();
  // Makes missing shelf cover thumbs after the first paint; repaints on change.
  void makeShelfThumbsAfterPaint();

  // Input
  void openSelectedBook();
  void openSearch();
  // Shared tail of row activation and the options menu's Open entry.
  void openBookByPath(const std::string& path);
  void promptRebuildIndex();
  void resetAfterRebuild();
  // Recent-row long-press menu: open / remove from recents / delete / rebuild.
  void showRecentBookOptions(int entry);
  void promptRemoveRecentBook(const std::string& path, const std::string& title);
  // Long-press delete owns the gesture where grouping does not apply: the
  // Recent sort, degraded lists, and any active search result.
  bool deleteEligible() const;
  // Resolves the row's path and title, then confirms via promptDeleteBookByPath.
  void promptDeleteBook(int entry);
  void promptDeleteBookByPath(const std::string& path, const std::string& title);
  bool collapseGroups(int bookEntry);
  void expandGroup(int groupEntry);
  void restoreExpandedList();
  void selectTab(int index, bool toggleIfActive);
  void toggleSortDirection();
  // Sub-screens act on button press, so a button still held when we resume must
  // not also act here. Records what to swallow on the next release.
  void swallowHeldReleases();
  // Staged back-out shared by Button::Back and the header back arrow.
  void handleBackAction();
  static void searchActionTrampoline(const freeink::ui::ActionEvent& event, void* user);
  static void rebuildActionTrampoline(const freeink::ui::ActionEvent& event, void* user);
  static void backActionTrampoline(const freeink::ui::ActionEvent& event, void* user);

  // Data
  void applyFilter();
  int bookRowCount() const;
  int rowFor(int entry) const;
  // fileName, when asked for, is the on-card name the row's icon derives from
  // (the display title may come from metadata and carry no extension).
  bool rowTextFor(int entry, std::string& title, std::string& author, std::string* fileName = nullptr);
  uint32_t titleInitialFor(int entry);
  bool buildGroupStarts();
  int groupForBook(int bookEntry) const;
  bool groupable() const;

  // Screen building
  void buildHeader(UiScreen& screen);
  // Materializes ListItems and their strings for the visible window only.
  void buildRows(UiScreen& screen);
  static void formatInitialHeading(uint32_t initial, std::string& out);
  void formatAuthorHeading(const std::string& author, std::string& out) const;
  void drawPositionReadout() const;
  void drawHoldHelp() const;
  const char* headerTitle() const override;

  // Ring 0 is the strip; the selected BOOK is ring - 1, with the strip keeping
  // row 0 as the working selection exactly as the pre-ring code did.
  int selectedEntry() const;
  bool tabsFocused() const { return ringPos() == 0; }

  // --- Books shelf (themes with libraryBookShelf) -----------------------------
  // No strip, always sorted by title. The leading rows are the recent-book
  // covers (themes with shelfCoverHeight, else none) and File Transfer; the
  // card's books follow. shelfRows() is that leading row count.
  static bool bookShelf();
  int shelfRows() const { return bookShelf() ? shelfCoverCount() + 1 : 0; }
  bool onFileTransferRow() const { return bookShelf() && ringPos() - 1 == shelfCoverCount(); }
  // The cover at the selection, or -1.
  int selectedCover() const { return ringPos() - 1 < shelfCoverCount() ? ringPos() - 1 : -1; }
  void buildShelfRows(UiScreen& screen);

  // --- shelf cover grid ------------------------------------------------------
  // Up to kShelfCovers recent books as covers, then File Transfer and the "On
  // This Device" heading on the same page; the selection moving on to the
  // books hands over to the full list.
  static constexpr int kShelfCovers = 4;
  int shelfCoverCount() const { return static_cast<int>(shelfBooks.size()); }
  bool shelfCoverView() const { return shelfCoverCount() > 0 && ringPos() - 1 <= shelfCoverCount(); }
  // Copies the leading store books, keeping known cover paths.
  void syncShelfBooks();
  // Side buttons in the grid move a row (two covers); true when handled.
  bool stepShelfGridRow();
  void drawShelfHeading(UiScreen& screen, int16_t rowHeight) const;
  void buildShelfCovers(UiScreen& screen);
  void fillShelfWindow(freeink::ui::ListProps& props, int visibleRows);
  void showBookOptions(const std::string& path, const std::string& title, bool isStoreRow);

  // --- pinned recently-opened overlay ---------------------------------------
  // On the unfiltered Recent shelf the RecentBooksStore entries sit on top, in
  // read order; the modification-time list follows with those books skipped.
  // Entries below pinnedCount() are store rows; the rest go through rowFor().
  int pinnedCount() const;
  // Re-match the store against the index (chunked scan). Call whenever the
  // index or the store changes.
  void resolvePinned();
  // Direction-space translation of the matched rows. Call on sort toggles.
  void refreshOverlap();

  library::LibraryIndexFile index;
  int activeTabIndex = 0;
  library::SortOrder sortOrder = library::SortOrder::RecentDesc;
  // One bit per tab; Recent starts descending (newest first).
  uint8_t descendingTabs = 1u << 0;
  // Set when the walk finished but the sort did not, so the screen can say the
  // order is discovery order rather than silently showing a wrong one.
  bool degraded = false;

  // Rows surviving the current query, as positions in the active sort order.
  // Empty query means no filtering and this owns no allocation, so the ordinary
  // shelf pays nothing proportional to the library for the feature.
  std::string query;
  // The active query, pre-quoted for the header: headerTitle() returns a
  // stable c_str the render task can hold across a build.
  std::string headerSearchTitle;
  std::unique_ptr<uint16_t[]> filtered;
  uint16_t filteredCount = 0;
  bool filterFailed = false;

  // One start row per group. Grouping is only offered for the sorted <=512-book
  // index, so this fallible allocation is at most 1 KiB and is reused after its
  // first successful allocation.
  std::unique_ptr<uint16_t[]> groupStarts;
  uint16_t groupCapacity = 0;
  uint16_t groupCount = 0;
  bool groupsCollapsed = false;
  freeink::ui::ListNav expandedNav;

  // Visible-window row storage, reused across renders (buildRows). Bounded by
  // the densest page, never by the library. Headings get their own storage:
  // the surname-first inversion must not overwrite the author slot, whose raw
  // value the next row's group comparison reads.
  std::vector<freeink::ui::ListItem> winItems;
  std::vector<std::string> winTitles;
  std::vector<std::string> winAuthors;
  std::vector<std::string> winHeaders;

  // Pinned overlay state: per store entry its RecentAsc row (0xFFFF when the
  // book is not in the index), and the current-direction rows to skip, sorted
  // ascending, so unpinned entries map to sort rows with a <=10-step walk.
  uint16_t pinnedAscRows[RecentBooksStore::MAX_RECENT_BOOKS] = {};
  uint16_t overlapRows[RecentBooksStore::MAX_RECENT_BOOKS] = {};
  uint8_t pinnedTotal = 0;
  uint8_t overlapCount = 0;

  // Leading RecentBooksStore entries for the cover grid (at most kShelfCovers),
  // and their thumb height; 0 when the theme has no grid.
  std::vector<RecentBook> shelfBooks;
  int shelfCoverH = 0;
  // Last row position the cover view shows in full, measured as it draws, and
  // the viewport it lays its rows out with (apart from the full list's).

  bool lockNextConfirmRelease = false;
  bool lockNextBackRelease = false;
  bool reconcilePending = false;
  bool thumbsPending = false;
  // Set by the render task once the list is on screen.
  std::atomic<bool> painted{false};

  // Row options modal (Recent long-press menu); owned here so it outlives the
  // touch event that opened it.
  OptionPopup optionPopup;
};
