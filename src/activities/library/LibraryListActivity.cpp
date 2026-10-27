#include "LibraryListActivity.h"

#include <BoardConfig.h>
#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryText.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/BatteryStatus.h"
#include "components/CoverThumbs.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "components/icons/headerIcons.h"
#include "components/icons/listIcons.h"
#include "components/icons/search32.h"
#include "components/FloatButtons.h"
#include "components/icons/readerFloatIcons.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"

namespace fui = freeink::ui;

namespace {
constexpr int SIDE_PADDING = 12;
constexpr unsigned long LONG_PRESS_MS = 1000;

constexpr int RECENT_TAB = 0;
constexpr int TITLE_TAB = 1;
constexpr int AUTHOR_TAB = 2;
constexpr int TAB_SLOTS = AUTHOR_TAB + 1;

constexpr bool isDescending(const library::SortOrder order) {
  return order == library::SortOrder::RecentDesc || order == library::SortOrder::TitleDesc ||
         order == library::SortOrder::AuthorDesc;
}

constexpr bool isRecentSort(const library::SortOrder order) {
  return order == library::SortOrder::RecentAsc || order == library::SortOrder::RecentDesc;
}

constexpr bool isAuthorSort(const library::SortOrder order) {
  return order == library::SortOrder::AuthorAsc || order == library::SortOrder::AuthorDesc;
}

constexpr bool isSeriesSort(const library::SortOrder order) { return order == library::SortOrder::SeriesAsc; }

constexpr library::SortOrder orderForTab(const int tab, const uint8_t descendingTabs) {
  const bool descending = (descendingTabs & (1u << tab)) != 0;
  if (tab == TITLE_TAB) return descending ? library::SortOrder::TitleDesc : library::SortOrder::TitleAsc;
  if (tab == AUTHOR_TAB) return descending ? library::SortOrder::AuthorDesc : library::SortOrder::AuthorAsc;
  return descending ? library::SortOrder::RecentDesc : library::SortOrder::RecentAsc;
}

const char* tabLabelFor(const int tab) {
  if (tab == TITLE_TAB) return tr(STR_LIBRARY_TAB_TITLE);
  if (tab == AUTHOR_TAB) return tr(STR_LIBRARY_TAB_AUTHOR);
  return tr(STR_LIBRARY_TAB_RECENT);
}

}  // namespace

LibraryListActivity::LibraryListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const bool asHome)
    : UiTabListActivity("Library", renderer, mappedInput, true), asHome(asHome) {
  // Three short tab labels: a full-slot pill would stretch across a third of
  // the screen, so cap it at the label plus padding (slots stay put).
  tabPillMaxPad = 16;
}

void LibraryListActivity::onEnter() {
  // One lock across the base lifecycle AND the data phase: the base onEnter
  // schedules a paint, and the render task must not read the index or the
  // filter before they are in place. The rebuild also needs the lock: the
  // render task's SD-loaded fonts read glyph data at draw time, and the walk
  // needs the card to itself.
  RenderLock lock(*this);
  UiTabListActivity::onEnter();
  app.on(ACTION_SEARCH, &LibraryListActivity::searchActionTrampoline, this);
  app.on(ACTION_REBUILD, &LibraryListActivity::rebuildActionTrampoline, this);
  app.on(ACTION_BACK, &LibraryListActivity::backActionTrampoline, this);
  app.on(ACTION_GRID, &LibraryListActivity::gridActionTrampoline, this);

  // Recent is backed by the resident store. Prune before opening the index so
  // its persistence write never overlaps the long-lived index reader.
  if (RECENT_BOOKS.pruneMissing()) RECENT_BOOKS.saveToFile();
  shelfCoverH = bookShelf() ? GUI.shelfCoverHeight(renderer) : 0;
  if (isHome()) shelfCoverH = homeCoverHeight();
  syncShelfBooks();
  // Missing cover thumbs are made once the page is up: making one parses its
  // book, which would hold the first paint behind a Loading popup.
  thumbsPending = shelfCoverCount() > 0;
  if (bookShelf()) {
    // The tab stays on Title so the Recent tab's pinned rows never apply: the
    // covers already show the recent reads.
    activeTabIndex = TITLE_TAB;
    applyShelfSort();
  }

  // Rebuild when the index is missing, invalid, or was built with the other
  // metadata mode. Also reconcile once per boot: books copied onto the card in
  // another machine leave no dirty marker, and the card can only change while
  // the device is off or asleep (wake is a reboot). That pass waits until the
  // list is on screen, so the page opens at once from the existing index.
  static bool reconciledThisBoot = false;
  const bool readMetadata = SETTINGS.libraryUseMetadata != 0;
  const bool rebuildNeeded = library::isLibraryIndexDirty() || !index.open(library::libraryIndexPath()) ||
                             index.header().metadataEnabled != readMetadata;
  reconcilePending = !rebuildNeeded && !reconciledThisBoot;
  reconciledThisBoot = true;
  painted = false;
  if (rebuildNeeded) {
    index.close();
    GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
    rebuildIndex();
    if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot open library index");
  }
  degraded = index.isOpen() && index.ranksDegraded();
  if (bookShelf() && degraded) sortOrder = library::SortOrder::TitleAsc;
  groupsPending = bookShelf() && groupable();
  if (index.isOpen() && index.dedupDegraded()) {
    LOG_ERR("LIB", "index was built without duplicate detection");
  }
  resolvePinned();

  // Entered while Confirm was still held (typical when launched from the home
  // menu): ignore its release, or we would open whatever sits at row 0.
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  if (bookShelf()) {
    // No strip to rest on: start on the first cover, else the first book, or
    // File Transfer when the card has none.
    activeNav().selected = shelfCoverCount() > 0 || listCount() == shelfRows() ? 1 : shelfRows() + 1;
  }
  requestUpdate(true);
}

// Home's row opens Books; on Books it is File Transfer.
const char* LibraryListActivity::menuRowLabel() const { return isHome() ? tr(STR_BOOKS) : tr(STR_FILE_TRANSFER); }

bool LibraryListActivity::bookShelf() { return UITheme::getInstance().getMetrics().libraryBookShelf; }

bool LibraryListActivity::shelfIsHome() {
  return bookShelf() && UITheme::getInstance().getMetrics().homeShelfGrid && BoardConfig::hasTouch();
}

void LibraryListActivity::onExit() {
  index.close();
  // After the close: one file open at a time on the card.
  if (sortDirty) {
    sortDirty = false;
    SETTINGS.saveToFile();
  }
  Activity::onExit();
}

void LibraryListActivity::applyShelfSort() {
  // Recently read pins the RecentBooksStore on top (pinnedCount() keys on the
  // Recent tab); every other order is the plain index order.
  activeTabIndex = SETTINGS.librarySort == CrossPointSettings::LIBRARY_SORT_RECENT ? RECENT_TAB : TITLE_TAB;
  switch (SETTINGS.librarySort) {
    case CrossPointSettings::LIBRARY_SORT_AUTHOR:
      sortOrder = library::SortOrder::AuthorAsc;
      break;
    case CrossPointSettings::LIBRARY_SORT_SERIES:
      sortOrder = library::SortOrder::SeriesAsc;
      break;
    case CrossPointSettings::LIBRARY_SORT_RECENT:
    case CrossPointSettings::LIBRARY_SORT_ADDED:
      sortOrder = library::SortOrder::RecentDesc;
      break;
    default:
      sortOrder = library::SortOrder::TitleAsc;
      break;
  }
  applyFilter();
  refreshOverlap();
  groupsPending = groupable();
}

void LibraryListActivity::showSortPicker() {
  if (degraded) return;
  // Picker order, as librarySort values.
  static constexpr uint8_t kSorts[] = {CrossPointSettings::LIBRARY_SORT_RECENT, CrossPointSettings::LIBRARY_SORT_TITLE,
                                       CrossPointSettings::LIBRARY_SORT_AUTHOR, CrossPointSettings::LIBRARY_SORT_SERIES,
                                       CrossPointSettings::LIBRARY_SORT_ADDED};
  static constexpr StrId kLabels[] = {StrId::STR_SHELF_RECENT_FIRST, StrId::STR_SHELF_BY_TITLE,
                                      StrId::STR_SHELF_BY_AUTHOR, StrId::STR_SHELF_BY_SERIES,
                                      StrId::STR_SHELF_BY_ADDED};
  static_assert(std::size(kSorts) == CrossPointSettings::LIBRARY_SORT_COUNT, "sort options");
  int current = 0;
  for (int i = 0; i < static_cast<int>(std::size(kSorts)); i++) {
    if (kSorts[i] == SETTINGS.librarySort) current = i;
  }
  optionPopup.show(StrId::STR_SHELF_SORT, kLabels, static_cast<int>(std::size(kLabels)), current, [this](int i) {
    if (kSorts[i] == SETTINGS.librarySort) return;
    SETTINGS.librarySort = kSorts[i];
    sortDirty = true;
    applyShelfSort();
    activeNav().selected = 1;
    activeNav().top = 0;
  });
  requestUpdate();
}

void LibraryListActivity::cycleShelfSort() {
  // Without finished ranks only the discovery order exists.
  if (degraded) return;
  SETTINGS.librarySort = static_cast<uint8_t>((SETTINGS.librarySort + 1) % CrossPointSettings::LIBRARY_SORT_COUNT);
  sortDirty = true;
  applyShelfSort();
  requestUpdate();
}

bool LibraryListActivity::rowStartsSection(const int row) const {
  if (row == 0) return true;
  if (!bookShelf()) return false;
  const int covers = shelfCoverCount();
  if (shelfLeadRows() > 0 && (row == covers || row == covers + 1)) return true;
  if (row == shelfRows()) return true;
  const int entry = row - shelfRows();
  if (entry <= 0 || groupCount == 0 || groupsCollapsed) return false;
  return std::binary_search(groupStarts.get(), groupStarts.get() + groupCount, static_cast<uint16_t>(entry));
}

void LibraryListActivity::jumpShelfSection(const int direction, const int count) {
  const int entry = ringPos() - 1 - shelfRows();
  // Above the books, or back past the first one, a hold steps rows.
  if (entry < 0 || groupCount == 0 || groupsCollapsed || (direction < 0 && entry == 0)) {
    holdStepRows(direction, count);
    return;
  }
  const int group = groupForBook(entry);
  int target = 0;
  if (direction > 0) {
    target = group + 1 < groupCount ? groupStarts[group + 1] : bookRowCount() - 1;
  } else if (groupStarts[group] < entry) {
    target = groupStarts[group];
  } else {
    target = group > 0 ? groupStarts[group - 1] : 0;
  }
  moveRingTo(1 + shelfRows() + target);
}

bool LibraryListActivity::rebuildIndex(bool* changed) {
  library::BuildStats stats;
  const bool ok = library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0);
  if (!ok) {
    LOG_ERR("LIB", "index build failed");
    return false;
  }
  if (changed) *changed = stats.added + stats.renamed + stats.removed + stats.enriched > 0;
  LOG_INF("LIB", "reconciled: %u unchanged, %u added, %u renamed, %u removed, %u enriched (%u dup, %u unreadable)",
          static_cast<unsigned>(stats.unchanged), static_cast<unsigned>(stats.added),
          static_cast<unsigned>(stats.renamed), static_cast<unsigned>(stats.removed),
          static_cast<unsigned>(stats.enriched), static_cast<unsigned>(stats.duplicatesDropped),
          static_cast<unsigned>(stats.unreadableSkipped));
  if (stats.dedupDegraded) LOG_ERR("LIB", "rebuild completed without duplicate detection");
  return true;
}

void LibraryListActivity::swallowHeldReleases() {
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  lockNextBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
}

int LibraryListActivity::selectedEntry() const {
  const int entry = ringPos() - 1 - shelfRows();
  return entry < 0 ? 0 : entry;
}

// The pinned overlay applies only to the shelf that reads as "what am I up
// to": the unfiltered Recent sort, newest first. A search result is a flat
// list the reader narrowed down on purpose, and the ascending toggle asks for
// oldest-first, which pinned fresh reads would contradict.
int LibraryListActivity::pinnedCount() const {
  if (activeTabIndex != RECENT_TAB || !query.empty() || !isDescending(sortOrder)) return 0;
  return pinnedTotal;
}

void LibraryListActivity::resolvePinned() {
  const auto& books = RECENT_BOOKS.getBooks();
  pinnedTotal = static_cast<uint8_t>(std::min<size_t>(books.size(), RecentBooksStore::MAX_RECENT_BOOKS));
  for (int i = 0; i < pinnedTotal; i++) pinnedAscRows[i] = 0xFFFF;
  if (pinnedTotal > 0 && index.isOpen()) {
    library::BookIdentity identities[RecentBooksStore::MAX_RECENT_BOOKS];
    for (int i = 0; i < pinnedTotal; i++) {
      const std::string& path = books[static_cast<size_t>(i)].path;
      identities[i].pathHash = library::clixPathHash(path.data(), path.size());
      // Size is only a lookup prefilter; 0 (stat failed, e.g. the index handle
      // is the card's one open reader) falls back to hash-only matching.
      identities[i].fileSize = 0;
      HalFile file;
      if (Storage.openFileForRead("LIB", path.c_str(), file)) {
        identities[i].fileSize = static_cast<uint32_t>(file.fileSize());
      }
    }
    if (!index.recentRowsFor(identities, pinnedTotal, pinnedAscRows)) {
      // Without the match the overlay would duplicate every pinned book that is
      // also in the index; better to drop the pins than to show doubles.
      LOG_ERR("LIB", "recent-book lookup failed; overlay disabled");
      pinnedTotal = 0;
    }
  }
  refreshOverlap();
  syncShelfBooks();
}

void LibraryListActivity::syncShelfBooks() {
  if (shelfCoverH <= 0) return;
  const auto& books = RECENT_BOOKS.getBooks();
  const size_t count = std::min<size_t>(books.size(), shelfCoverLimit());
  std::vector<RecentBook> previous;
  previous.swap(shelfBooks);
  shelfBooks.assign(books.begin(), books.begin() + static_cast<std::ptrdiff_t>(count));
  for (auto& book : shelfBooks) {
    for (const auto& known : previous) {
      if (known.path == book.path && !known.coverBmpPath.empty()) book.coverBmpPath = known.coverBmpPath;
    }
  }
}

void LibraryListActivity::makeShelfThumbsAfterPaint() {
  // Books without a usable cover, so they are not parsed again on every visit.
  static std::vector<std::string> noCover;
  RenderLock lock(*this);
  bool changed = false;
  bool quiet = true;  // no Loading popup: the page is already up
  Rect unused{};
  for (auto& book : shelfBooks) {
    if (std::find(noCover.begin(), noCover.end(), book.path) != noCover.end()) continue;
    if (ensureCoverThumb(renderer, book, shelfCoverH, quiet, unused)) changed = true;
    if (book.coverBmpPath.empty()) noCover.push_back(book.path);
  }
  if (changed) requestUpdate();
}

void LibraryListActivity::refreshOverlap() {
  overlapCount = 0;
  const int total = static_cast<int>(index.bookCount());
  for (int i = 0; i < pinnedTotal; i++) {
    if (pinnedAscRows[i] == 0xFFFF || pinnedAscRows[i] >= total) continue;
    const uint16_t row =
        isDescending(sortOrder) ? static_cast<uint16_t>(total - 1 - pinnedAscRows[i]) : pinnedAscRows[i];
    overlapRows[overlapCount++] = row;
  }
  std::sort(overlapRows, overlapRows + overlapCount);
}

bool LibraryListActivity::pathForEntry(const int entry, std::string& path) {
  if (entry < pinnedCount()) {
    const auto& books = RECENT_BOOKS.getBooks();
    if (entry < 0 || entry >= static_cast<int>(books.size())) return false;
    path = books[static_cast<size_t>(entry)].path;
    return true;
  }
  if (!index.isOpen()) return false;
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  if (ordinal == 0xFFFF) return false;
  library::ClixRecord record{};
  if (!index.readRecord(ordinal, record) || !index.readPath(record, path)) {
    LOG_ERR("LIB", "cannot resolve path for row %d", entry);
    return false;
  }
  return true;
}

void LibraryListActivity::openSelectedBook() {
  std::string path;
  if (pathForEntry(selectedEntry(), path)) openBookByPath(path);
}

// Shared by row activation and the options menu: the reader screen this opens
// has its own surfaces; a lingering tap flash would gray an unrelated element
// there. The index handle is released first — on hardware only one reader can
// hold a file open at a time, and the reader is about to open files of its own.
void LibraryListActivity::openBookByPath(const std::string& path) {
  app.clearTapFlash();
  index.close();
  onSelectBook(path);
}

void LibraryListActivity::activateIndex(const int index) {
  if (selectedCover() >= 0) {
    openBookByPath(shelfBooks[static_cast<size_t>(selectedCover())].path);
    return;
  }
  if (onFileTransferRow()) {
    // The parameter shadows the index member; release the card before leaving.
    app.clearTapFlash();
    this->index.close();
    if (isHome()) {
      activityManager.goToLibrary();
    } else {
      activityManager.goToFileTransfer();
    }
    return;
  }
  if (onSortRow()) {
    if (shelfIsHome()) {  // touch boards: Home and Books both pick
      showSortPicker();
    } else {
      cycleShelfSort();
    }
    return;
  }
  if (groupsCollapsed) {
    expandGroup(index);
  } else {
    openSelectedBook();
  }
}

// Row long-press prompts delete wherever grouping does not own the gesture:
// an active search is already a flat list the reader narrowed down on purpose
// ("find it, hold it, delete it"). Unfiltered Title/Author lists keep
// collapse-to-groups. The Recent shelf always opens the row options menu.
bool LibraryListActivity::deleteEligible() const { return !groupsCollapsed && (!query.empty() || !groupable()); }

void LibraryListActivity::onRowLongPress(const int index) {
  if (onFileTransferRow() || onSortRow()) return;
  if (selectedCover() >= 0) {
    const auto& book = shelfBooks[static_cast<size_t>(selectedCover())];
    showBookOptions(book.path, book.title, true);
  } else if (bookShelf() || isRecentSort(sortOrder)) {
    showRecentBookOptions(selectedEntry());
  } else if (deleteEligible()) {
    promptDeleteBook(index);
  } else if (!groupsCollapsed && groupable()) {
    collapseGroups(index);
  } else {
    activateIndex(index);
  }
}

// Recent-shelf long-press menu (button hold and touch long-press). The first
// rows may come from RecentBooksStore; the rest are index rows sorted by
// modification time. Only store rows can be removed from recents.
void LibraryListActivity::showRecentBookOptions(const int entry) {
  if (entry < 0 || entry >= listCount()) return;

  std::string path;
  std::string title;
  const bool isStoreRow = entry < pinnedCount();
  if (isStoreRow) {
    const auto& books = RECENT_BOOKS.getBooks();
    if (entry >= static_cast<int>(books.size())) return;
    path = books[static_cast<size_t>(entry)].path;
    title = books[static_cast<size_t>(entry)].title;
  } else {
    if (!index.isOpen()) return;
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
    library::ClixRecord record{};
    std::string author;
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record) || !index.readPath(record, path) ||
        !rowTextFor(entry, title, author)) {
      LOG_ERR("LIB", "cannot resolve Recent row %d", entry);
      return;
    }
  }

  showBookOptions(path, title, isStoreRow);
}

void LibraryListActivity::showBookOptions(const std::string& path, const std::string& title, const bool isStoreRow) {
  const char* STORE_OPTIONS[] = {tr(STR_OPEN), tr(STR_REMOVE_FROM_RECENTS), tr(STR_DELETE), tr(STR_LIBRARY_REBUILD)};
  const char* INDEX_OPTIONS[] = {tr(STR_OPEN), tr(STR_DELETE), tr(STR_LIBRARY_REBUILD)};
  app.clearTapFlash();
  optionPopup.show(tr(STR_LIBRARY), title.c_str(), isStoreRow ? STORE_OPTIONS : INDEX_OPTIONS, isStoreRow ? 4 : 3, 0,
                   [this, path, title, isStoreRow](const int choice) {
                     swallowHeldReleases();
                     switch (choice) {
                       case 0:
                         openBookByPath(path);
                         break;
                       case 1:
                         if (isStoreRow) {
                           promptRemoveRecentBook(path, title);
                         } else {
                           promptDeleteBookByPath(path, title);
                         }
                         break;
                       case 2:
                         if (isStoreRow)
                           promptDeleteBookByPath(path, title);
                         else
                           promptRebuildIndex();
                         break;
                       case 3:
                         if (isStoreRow) promptRebuildIndex();
                         break;
                       default:
                         break;
                     }
                   });
  requestUpdate();
}

// Manual index refresh, same card discipline as the onEnter rebuild: the walk
// wants the card to itself, and the render task must not read the index (or
// the filter) around it.
void LibraryListActivity::promptRebuildIndex() {
  RenderLock lock(*this);
  GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
  index.close();
  rebuildIndex();
  if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot open library index");
  resetAfterRebuild();
  requestUpdate(true);
}

void LibraryListActivity::reconcileAfterPaint() {
  // Same card discipline as promptRebuildIndex, without the popup.
  RenderLock lock(*this);
  index.close();
  bool changed = false;
  rebuildIndex(&changed);
  if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot open library index");
  degraded = index.isOpen() && index.ranksDegraded();
  // Rows are re-read from the reopened index, so an unchanged card needs no
  // repaint.
  if (!changed) return;
  resetAfterRebuild();
  requestUpdate();
}

void LibraryListActivity::resetAfterRebuild() {
  // Sort positions, group starts, and pinned rows all point into the old order.
  applyFilter();
  resolvePinned();
  groupsPending = bookShelf() && groupable();
  auto& nav = activeNav();
  const int count = listCount();
  if (count == 0) {
    nav.selected = 0;
  } else if (nav.selected > count) {
    nav.selected = count;
  }
  nav.followOnBuild = true;
}

void LibraryListActivity::promptRemoveRecentBook(const std::string& path, const std::string& title) {
  const bool reopenIndex = index.isOpen();
  index.close();
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title);
  if (!confirmation) {
    LOG_ERR("LIB", "OOM: recent removal confirmation");
    if (reopenIndex && !index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    return;
  }

  startActivityForResult(std::move(confirmation), [this, path, reopenIndex](const ActivityResult& result) {
    swallowHeldReleases();
    if (reopenIndex && !index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    if (!result.isCancelled && RECENT_BOOKS.removeByPath(path)) {
      resolvePinned();
      closeRouting();
      auto& nav = activeNav();
      const int count = listCount();
      if (count == 0) {
        nav.selected = 0;
      } else if (nav.selected > count) {
        nav.selected = count;
      }
      nav.followOnBuild = true;
    }
  });
}

void LibraryListActivity::promptDeleteBook(const int entry) {
  if (!index.isOpen() || entry < 0 || entry >= bookRowCount()) return;
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  if (ordinal == 0xFFFF) return;

  std::string path;
  library::ClixRecord record{};
  if (!index.readRecord(ordinal, record) || !index.readPath(record, path)) {
    LOG_ERR("LIB", "cannot resolve path for row %d", entry);
    return;
  }
  std::string title;
  std::string author;
  rowTextFor(entry, title, author);
  promptDeleteBookByPath(path, title);
}

void LibraryListActivity::promptDeleteBookByPath(const std::string& path, const std::string& title) {
  // The dialog and the delete both want the card; reopen when we resume.
  index.close();
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE) + std::string("? "), title);
  if (!confirmation) {
    LOG_ERR("LIB", "OOM: delete confirmation");
    if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    return;
  }

  startActivityForResult(std::move(confirmation), [this, path](const ActivityResult& result) {
    swallowHeldReleases();
    {
      // Same lock rationale as onEnter: the walk wants the card to itself, and
      // the render task must not read the index (or the filter) around the
      // rebuild.
      RenderLock lock(*this);
      if (!result.isCancelled) {
        LOG_DBG("LIB", "deleting %s", path.c_str());
        if (!removeBookFile(path)) LOG_ERR("LIB", "cannot delete %s", path.c_str());
        if (RECENT_BOOKS.removeByPath(path)) RECENT_BOOKS.saveToFile();
        GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
        rebuildIndex();
      }
      if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
      if (!result.isCancelled) {
        resetAfterRebuild();
      }
    }
    if (!result.isCancelled) {
      closeRouting();
      requestUpdate(true);
    }
  });
}

void LibraryListActivity::openSearch() {
  app.clearTapFlash();
  // No key filtering here on purpose. Greying out the letters that lead nowhere
  // was built, tested on device and removed: a letter you can see but cannot
  // reach reads as a broken keyboard, and the eye keeps returning to it.
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_LIBRARY_SEARCH), query, 48,
                                                           InputType::Text);
  if (!keyboard) {
    LOG_ERR("LIB", "OOM: search keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    swallowHeldReleases();
    if (result.isCancelled) return;
    query = std::get<KeyboardResult>(result.data).text;
    applyFilter();
    auto& nav = activeNav();
    if (!query.empty() && filteredCount == 0 && !degraded) {
      // Up from the tab bar reopens Search even with no results.
      nav.selected = 0;
    } else {
      // A non-empty result belongs to the list: land on
      // its first surviving row, not on the strip.
      nav.selected = 1;
    }
    nav.top = 0;
    requestUpdate();
  });
}

void LibraryListActivity::stepTab(const int direction) {
  const int next = (activeTab() + (direction > 0 ? 1 : TAB_SLOTS - 1)) % TAB_SLOTS;
  selectTab(next, false);
}

void LibraryListActivity::onTabAction(const int index) {
  app.clearTapFlash();
  selectTab(index, true);
}

void LibraryListActivity::selectTab(const int index, const bool toggleIfActive) {
  if (index < 0 || index >= TAB_SLOTS) return;
  if (toggleIfActive && index == activeTab()) descendingTabs ^= static_cast<uint8_t>(1u << index);
  sortOrder = orderForTab(index, descendingTabs);
  // The filter and the overlap rows hold positions in the old order, so they
  // must be rebuilt.
  applyFilter();
  activeTabIndex = index;
  refreshOverlap();
  // Tab changes happen only while the bar owns focus. A tab's remembered row
  // must not pull focus back into the list after the switch.
  auto& nav = activeNav();
  nav.selected = 0;
  nav.top = 0;
  requestUpdate();
}

void LibraryListActivity::toggleSortDirection() { selectTab(activeTab(), true); }

int LibraryListActivity::tabCount() const { return TAB_SLOTS; }

int LibraryListActivity::activeTab() const { return activeTabIndex; }

const char* LibraryListActivity::tabLabel(const int index) const { return tabLabelFor(index); }

fui::TabIndicator LibraryListActivity::tabIndicator(const int index) const {
  if (index != activeTab()) return fui::TabIndicator::None;
  return isDescending(sortOrder) ? fui::TabIndicator::Down : fui::TabIndicator::Up;
}

int LibraryListActivity::bookRowCount() const {
  if (!query.empty()) return static_cast<int>(filteredCount);
  // Pinned books already in the index are skipped below the pins, not doubled;
  // pinned books the index missed still show, so the difference stays split.
  const int pinned = pinnedCount();
  return static_cast<int>(index.bookCount()) + (pinned > 0 ? pinned - overlapCount : 0);
}

int LibraryListActivity::listCount() const {
  // Home has no list: the book and the Books button are drawn directly.
  if (isHome()) return 0;
  return groupsCollapsed ? static_cast<int>(groupCount) : bookRowCount() + shelfRows();
}

// Entry position on screen to row position in the sort order. Identity while
// unfiltered and unpinned, so the shelf costs nothing when nothing is typed.
// With pins active, entries below pinnedCount() belong to the store and must
// not reach this; the rest walk past the pinned books' own sort rows.
int LibraryListActivity::rowFor(const int entry) const {
  if (!query.empty()) {
    if (entry < 0 || entry >= static_cast<int>(filteredCount) || !filtered) return 0;
    return filtered[entry];
  }
  const int pinned = pinnedCount();
  if (pinned == 0) return entry;
  int row = entry - pinned;
  for (int i = 0; i < overlapCount; i++) {
    if (overlapRows[i] <= row) row++;
  }
  return row;
}

bool LibraryListActivity::groupable() const { return !degraded && !isRecentSort(sortOrder) && bookRowCount() > 0; }

uint32_t LibraryListActivity::titleInitialFor(const int entry) {
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  library::ClixRecord record{};
  if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) return 0;
  return library::foldedGroupInitial(std::string_view(record.fold, record.foldLen));
}

bool LibraryListActivity::buildGroupStarts() {
  const int count = bookRowCount();
  if (count <= 0) return false;
  if (groupCapacity < count) {
    auto starts = makeUniqueNoThrow<uint16_t[]>(static_cast<size_t>(count));
    if (!starts) {
      LOG_ERR("LIB", "cannot allocate %u-byte group map", static_cast<unsigned>(count * sizeof(uint16_t)));
      return false;
    }
    groupStarts = std::move(starts);
    groupCapacity = static_cast<uint16_t>(count);
  }

  groupCount = 0;
  uint32_t previousInitial = 0;
  std::string previousAuthor;
  std::string title;
  std::string author;
  previousAuthor.reserve(128);
  title.reserve(128);
  author.reserve(128);
  std::string position;
  for (int entry = 0; entry < count; entry++) {
    bool startsGroup = entry == 0;
    if (isSeriesSort(sortOrder)) {
      // previousAuthor holds the previous book's series here.
      seriesFor(entry, title, position);
      startsGroup = startsGroup || title != previousAuthor;
      previousAuthor = title;
    } else if (isAuthorSort(sortOrder)) {
      rowTextFor(entry, title, author);
      startsGroup = startsGroup || author != previousAuthor;
      previousAuthor = author;
    } else {
      const uint32_t initial = titleInitialFor(entry);
      startsGroup = startsGroup || initial != previousInitial;
      previousInitial = initial;
    }
    if (startsGroup) groupStarts[groupCount++] = static_cast<uint16_t>(entry);
  }
  LOG_DBG("LIB", "group map: %u groups, %u bytes", static_cast<unsigned>(groupCount),
          static_cast<unsigned>(groupCapacity * sizeof(uint16_t)));
  return groupCount > 0;
}

int LibraryListActivity::groupForBook(const int bookEntry) const {
  int group = 0;
  while (group + 1 < groupCount && groupStarts[group + 1] <= bookEntry) group++;
  return group;
}

bool LibraryListActivity::collapseGroups(const int bookEntry) {
  if (!groupable() || !buildGroupStarts()) return false;
  expandedNav = activeNav();
  groupsCollapsed = true;
  auto& nav = activeNav();
  nav.reset(groupForBook(bookEntry) + 1);
  requestUpdate();
  return true;
}

void LibraryListActivity::expandGroup(const int groupEntry) {
  if (!groupsCollapsed || groupEntry < 0 || groupEntry >= groupCount) return;
  const int bookEntry = groupStarts[groupEntry];
  groupsCollapsed = false;
  activeNav() = expandedNav;
  auto& nav = activeNav();
  nav.selected = bookEntry + 1;
  nav.top = bookEntry;
  nav.followOnBuild = true;
  requestUpdate();
}

void LibraryListActivity::restoreExpandedList() {
  if (!groupsCollapsed) return;
  groupsCollapsed = false;
  activeNav() = expandedNav;
  requestUpdate();
}

// One pass over the sort order, keeping what matches. No index, no cache: at the
// 4096-book format cap this is 4096 comparisons of at most 96 bytes. The result
// array is allocated once with the exact upper bound and fails back to an
// explicit message rather than letting vector growth abort the firmware.
void LibraryListActivity::applyFilter() {
  groupsCollapsed = false;
  groupCount = 0;
  groupsPending = false;
  filtered.reset();
  filteredCount = 0;
  filterFailed = false;
  // The header shows the active query in place of the screen title, so the
  // reader can see what narrowed the list without reopening the keyboard.
  headerSearchTitle = query.empty() ? std::string() : "“" + query + "”";
  if (query.empty()) return;

  const std::string needle = library::fold(query);
  const int total = static_cast<int>(index.bookCount());
  if (total <= 0) return;

  auto matches = makeUniqueNoThrow<uint16_t[]>(static_cast<size_t>(total));
  if (!matches) {
    LOG_ERR("LIB", "cannot allocate %u-byte search result buffer", static_cast<unsigned>(total * sizeof(uint16_t)));
    filterFailed = true;
    return;
  }

  uint16_t matchCount = 0;
  std::string author;
  for (int row = 0; row < total; row++) {
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(row));
    library::ClixRecord record{};
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) continue;
    if (library::matchesQuery(std::string_view(record.fold, record.foldLen), needle)) {
      matches[matchCount++] = static_cast<uint16_t>(row);
      continue;
    }
    // The stored fold covers the title only, so the author has to be read and
    // folded here. That is the search most worth having: the reader who knows
    // the author usually also knows where the book is, while "emily" finding
    // Alice Hunter is the case the shelf exists to answer.
    author.clear();
    if (index.readAuthor(record, author) && library::matchesQuery(library::fold(author), needle)) {
      matches[matchCount++] = static_cast<uint16_t>(row);
    }
  }
  filtered = std::move(matches);
  filteredCount = matchCount;
}

// Staged back-out, shared by the Back button and the header's back arrow:
// clear the search, expand collapsed groups, return focus to the tabs, then
// leave for home.
void LibraryListActivity::handleBackAction() {
  auto& nav = activeNav();
  if (isHome()) {
    return;  // already home
  } else if (bookShelf()) {
    onGoHome();
  } else if (!query.empty()) {
    query.clear();
    applyFilter();
    nav.selected = 0;
    nav.top = 0;
    requestUpdate();
  } else if (groupsCollapsed) {
    restoreExpandedList();
  } else if (!tabsFocused() && !degraded) {
    // Keep the current list and viewport while returning focus to the tabs.
    nav.selected = 0;
    requestUpdate();
  } else {
    onGoHome();
  }
}

void LibraryListActivity::searchActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->openSearch();
}

void LibraryListActivity::backActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->handleBackAction();
}

void LibraryListActivity::rebuildActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->promptRebuildIndex();
}

// Title and author for one entry, read straight from the index. Only ever
// called for rows about to be drawn, so at most a screenful of strings exists
// at once.
bool LibraryListActivity::rowTextFor(const int entry, std::string& title, std::string& author, std::string* fileName) {
  title.clear();
  author.clear();
  if (fileName) fileName->clear();
  if (entry < pinnedCount()) {
    const auto& books = RECENT_BOOKS.getBooks();
    if (entry < 0 || entry >= static_cast<int>(books.size())) return false;
    const auto& book = books[static_cast<size_t>(entry)];
    title = book.title;
    author = book.author;
    if (fileName) *fileName = book.path;
    return true;
  }
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  library::ClixRecord record{};
  if (ordinal != 0xFFFF && index.readRecord(ordinal, record)) {
    // The build already decided both fields — from the book's own metadata when
    // it has any, and with one spelling chosen per author across the library.
    // Re-parsing the name here would throw that away, and only works while the
    // name still looks like "Title - Author".
    if (!index.readAuthor(record, author)) author.clear();
    // The stored title when the book gave one, the filename otherwise.
    if (!index.readTitle(record, title) || title.empty()) index.readName(record, title);
    if (fileName) index.readName(record, *fileName);
  }
  if (title.empty()) title = tr(STR_LIBRARY_UNKNOWN_TITLE);
  return true;
}

void LibraryListActivity::seriesFor(const int entry, std::string& series, std::string& position) {
  series.clear();
  position.clear();
  // Pinned store rows carry no series; the shelf never pins.
  if (entry < pinnedCount()) return;
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  library::ClixRecord record{};
  if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) return;
  if (!index.readSeries(record, series) || series.empty()) {
    series.clear();
    return;
  }
  if (!index.readSeriesPosition(record, position)) position.clear();
}

bool LibraryListActivity::handleCustomInput() {
  if (thumbsPending && painted) {
    thumbsPending = false;
    makeShelfThumbsAfterPaint();
  } else if (reconcilePending && painted) {
    reconcilePending = false;
    reconcileAfterPaint();
  } else if (groupsPending && painted) {
    groupsPending = false;
    RenderLock lock(*this);
    buildGroupStarts();
  }
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;

  if (isHome() && handleHomeInput()) return true;

  if (lockNextConfirmRelease && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    lockNextConfirmRelease = false;
    return true;
  }
  if (lockNextBackRelease && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    lockNextBackRelease = false;
    return true;
  }

  return false;
}

bool LibraryListActivity::handleButtons() {
  const int count = listCount();

  // Every hold action fires at the threshold, mid-hold, including the ones
  // that open a dialog (remove-recent, delete). The release that follows is
  // armed as suppressed by wasLongPressed() and consumed globally by
  // ActivityManager::loop() before any activity runs, so it cannot land in
  // the freshly opened confirmation and select its default.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, LONG_PRESS_MS)) {
    if (onFileTransferRow()) {
      activateIndex(0);
    } else if (bookShelf()) {
      onRowLongPress(ringPos() - 1);
    } else if (tabsFocused()) {
      if (!degraded) toggleSortDirection();
    } else if (isRecentSort(sortOrder)) {
      showRecentBookOptions(selectedEntry());
    } else if (deleteEligible()) {
      if (count > 0) promptDeleteBook(selectedEntry());
    } else if (!groupsCollapsed && groupable()) {
      collapseGroups(selectedEntry());
    } else {
      activateIndex(selectedEntry());
    }
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    handleBackAction();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (tabsFocused()) {
      stepTab(1);
      return true;
    }
    if (count > 0) activateIndex(ringPos() - 1);
    return true;
  }

  return false;
}

void LibraryListActivity::navigateButtons() {
  const int count = listCount();
  if (bookShelf()) {
    if (stepShelfGridRow()) return;
    // Rows only: the ring never visits the absent strip.
    buttonNavigator.onNextPress([this, count] { stepRowsWithSectionJumps(1, count); });
    buttonNavigator.onPreviousPress([this, count] { stepRowsWithSectionJumps(-1, count); });
    buttonNavigator.onNextContinuous([this, count] { jumpShelfSection(1, count); });
    buttonNavigator.onPreviousContinuous([this, count] { jumpShelfSection(-1, count); });
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::NavNext) ||
      mappedInput.wasPressed(MappedInputManager::Button::NavPrevious)) {
    navigationStartedOnTabs = tabsFocused();
  }
  buttonNavigator.onNextPress([this, count] {
    if (count > 0) moveRingTo(ringPos() == count ? 1 : ringPos() + 1);
  });
  buttonNavigator.onPreviousPress([this, count] {
    if ((!navigationStartedOnTabs || degraded) && count > 0) {
      moveRingTo(ringPos() <= 1 ? count : ringPos() - 1);
    }
  });
  // Search is an activation: defer it so holding Previous can still step tabs.
  buttonNavigator.onPreviousRelease([this] {
    if (navigationStartedOnTabs && tabsFocused() && !degraded) openSearch();
  });
  // A held button steps tabs while the strip has focus (the base behaviour
  // Settings keeps) and page-jumps once the selection is down in the rows,
  // where fast travel through a long shelf is what a hold means.
  buttonNavigator.onNextContinuous([this, count] {
    if (navigationStartedOnTabs) {
      activeNav().selected = 0;
      stepTab(1);
    } else if (count > 0) {
      moveRingTo(ButtonNavigator::nextPageIndex(selectedEntry(), count, activeNav().pageRows()) + 1);
    }
  });
  buttonNavigator.onPreviousContinuous([this, count] {
    if (navigationStartedOnTabs) {
      activeNav().selected = 0;
      stepTab(-1);
    } else if (count > 0) {
      moveRingTo(ButtonNavigator::previousPageIndex(selectedEntry(), count, activeNav().pageRows()) + 1);
    }
  });
}

bool LibraryListActivity::stepShelfGridRow() {
  const int covers = shelfCoverCount();
  const int position = ringPos() - 1;
  if (covers == 0 || position > covers) return false;
  const bool swapped = mappedInput.isNavDirectionSwapped();
  const bool down = mappedInput.wasPressed(swapped ? MappedInputManager::Button::Up : MappedInputManager::Button::Down);
  const bool up = mappedInput.wasPressed(swapped ? MappedInputManager::Button::Down : MappedInputManager::Button::Up);
  if (down && position < covers) {
    // The bottom row steps on to File Transfer.
    moveRingTo(1 + std::min(position + 2, covers));
    return true;
  }
  if (up && position >= 2) {
    // From File Transfer, up to the start of the bottom row.
    moveRingTo(1 + (position == covers ? (covers - 1) / 2 * 2 : position - 2));
    return true;
  }
  return false;
}

void LibraryListActivity::buildRows(UiScreen& screen) {
  auto& nav = activeNav();
  const int count = listCount();
  const bool authorGrouped = isAuthorSort(sortOrder);
  const bool grouped = !isRecentSort(sortOrder);

  fui::ListProps props;
  props.count = static_cast<uint16_t>(count);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 1;
  // Breathing room between rows; the dense theme default packs the two-line
  // rows edge-to-edge.
  props.rowGap = std::max<int16_t>(screen.theme().listRowGap, 6);
  props.headerUnderline = false;
  syncTabListViewport(screen, props);

  // Keep one extra entry in the reusable window for a clipped trailing row.
  const size_t cap = static_cast<size_t>(nav.visibleRows > 0 ? nav.visibleRows : 1) + 1;
  if (winTitles.size() < cap) winTitles.resize(cap);
  if (winAuthors.size() < cap) winAuthors.resize(cap);
  if (!groupsCollapsed && winHeaders.size() < cap) winHeaders.resize(cap);
  winItems.clear();
  if (winItems.capacity() < cap) winItems.reserve(cap);

  int rows = 0;
  int headers = 0;
  uint32_t previousInitial = 0;
  std::string rowFile;
  rowFile.reserve(128);
  // Capture this after syncTabListViewport(), which may clamp nav.top.
  const int windowStart = static_cast<int>(props.topIndex);
  for (int entry = windowStart; entry < count && rows < static_cast<int>(cap); entry++) {
    std::string& title = winTitles[static_cast<size_t>(rows)];
    std::string& author = winAuthors[static_cast<size_t>(rows)];
    fui::ListItem item;
    if (groupsCollapsed) {
      const int bookEntry = groupStarts[entry];
      if (authorGrouped) {
        rowTextFor(bookEntry, title, author);
        formatAuthorHeading(author, title);
      } else {
        formatInitialHeading(titleInitialFor(bookEntry), title);
      }
    } else {
      if (!rowTextFor(entry, title, author, &rowFile)) continue;
      uint32_t initial = 0;
      bool startsGroup = false;
      if (authorGrouped) {
        startsGroup = rows == 0 || author != winAuthors[static_cast<size_t>(rows - 1)];
      } else if (grouped) {
        initial = titleInitialFor(entry);
        startsGroup = rows == 0 || initial != previousInitial;
        previousInitial = initial;
      }
      if (startsGroup) {
        std::string& heading = winHeaders[static_cast<size_t>(headers++)];
        if (authorGrouped)
          formatAuthorHeading(author, heading);
        else
          formatInitialHeading(initial, heading);
        item.sectionHeading = heading.c_str();
      }
      if (!authorGrouped && !author.empty()) item.subtitle = author.c_str();
    }

    item.label = title.c_str();
    // Group headings stay bare; every book row gets its file-type icon.
    if (!groupsCollapsed && !rowFile.empty()) item.icon = listIconFor(UITheme::getFileIcon(rowFile), 32);
    item.actionValue = static_cast<int16_t>(entry);
    winItems.push_back(item);
    rows++;
  }

  props.items = winItems.data();
  props.itemsWindowFirst = static_cast<uint16_t>(windowStart);
  props.itemsWindowCount = static_cast<uint16_t>(winItems.size());
  screen.list(props);
  const int next = nav.drawnRows;
  const auto body = screen.body();
  LOG_DBG("LIB", "page tab=%d top=%d full=%d loaded=%d body=%d..%d next=%d title=%s", activeTabIndex, windowStart,
          nav.drawnRows, rows, body.y, body.bottom(),
          next < rows ? winItems[static_cast<size_t>(next)].actionValue : -1,
          next < rows ? winItems[static_cast<size_t>(next)].label : "<none>");
  LOG_DBG("LIB", "page first=%d title=%s", rows > 0 ? winItems[0].actionValue : -1,
          rows > 0 ? winItems[0].label : "<none>");
}

// Books shelf, full list: the card's books in the chosen order under a pinned
// "On This Device" heading, title over an italic author, with letter or author
// headings. Windowed like buildRows(); list positions are shifted by the
// shelf's leading rows.
void LibraryListActivity::buildShelfRows(UiScreen& screen) {
  if (shelfCoverView()) {
    buildShelfCovers(screen);
    return;
  }
  // Without covers the sort row is a list row of its own.
  if (shelfCoverCount() > 0) {
    drawShelfHeading(screen, static_cast<int16_t>(UITheme::getInstance().getMetrics().listRowHeight), false);
  }
  fui::ListProps props;
  props.count = static_cast<uint16_t>(listCount());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  applySectionHeadingStyle(screen.theme(), props);
  props.labelText = screen.theme().bodyText;
  // Long titles wrap, with air between rows as in the file browser.
  props.labelText.maxLines = 2;
  props.subtitleText = screen.theme().bodyText;
  props.subtitleText.italic = true;
  props.subtitleGap = 0;  // the author is simply the next line
  props.rowGap = std::max<int16_t>(screen.theme().listRowGap, 6);
  if (spineShelf()) {
    props.labelText.maxLines = 1;
    props.valueText = screen.theme().bodyText;
    props.valueText.italic = true;
    props.rowGap = 0;
    props.separatorPaint = fui::Paint::solid(fui::Color::LightGray);
    props.balanceWrappedLabelWithValue = false;
  }
  syncTabListViewport(screen, props);
  // Covers, File Transfer and the sort row stay on the cover page; without
  // covers, they lead the list.
  const int firstRow = shelfCoverCount() > 0 ? shelfRows() : 0;
  if (props.topIndex < firstRow) {
    props.topIndex = static_cast<uint16_t>(firstRow);
    activeNav().top = firstRow;
  }
  fillShelfWindow(props, activeNav().visibleRows);
  screen.list(props);
}

const char* LibraryListActivity::shelfOrderText() {
  switch (SETTINGS.librarySort) {
    case CrossPointSettings::LIBRARY_SORT_AUTHOR:
      return tr(STR_SHELF_BY_AUTHOR);
    case CrossPointSettings::LIBRARY_SORT_SERIES:
      return tr(STR_SHELF_BY_SERIES);
    case CrossPointSettings::LIBRARY_SORT_RECENT:
      return tr(STR_SHELF_RECENT_FIRST);
    case CrossPointSettings::LIBRARY_SORT_ADDED:
      return tr(STR_SHELF_BY_ADDED);
    default:
      return tr(STR_SHELF_BY_TITLE);
  }
}

const char* LibraryListActivity::shelfHeadingText() {
  shelfHeading = tr(STR_ON_THIS_DEVICE);
  if (!degraded) shelfHeading.append(" \u00B7 ").append(shelfOrderText());
  return shelfHeading.c_str();
}

// The sort row as a one-row list, so it takes the theme's selection mark and
// taps like any row.
void LibraryListActivity::drawShelfHeading(UiScreen& screen, const int16_t rowHeight, const bool selected) {
  fui::ListProps props;
  applySectionHeadingStyle(screen.theme(), props);
  props.labelText = props.headerText;
  fui::ListItem item;
  item.label = shelfHeadingText();
  item.actionValue = static_cast<int16_t>(shelfCoverCount() + 1);
  props.items = &item;
  props.count = 1;
  props.selectedIndex = selected ? 0 : -1;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  screen.list(props, rowHeight);
}

// Row items for the shelf from props.topIndex on: File Transfer and the sort
// row when they lead the list, then the card's books in the chosen order.
void LibraryListActivity::fillShelfWindow(fui::ListProps& props, const int visibleRows) {
  const int count = listCount();
  const int covers = shelfCoverCount();
  const bool authorGrouped = isAuthorSort(sortOrder);
  const bool seriesGrouped = isSeriesSort(sortOrder);
  const bool grouped = groupable();
  const size_t cap = static_cast<size_t>(visibleRows > 0 ? visibleRows : 1) + 1;
  if (winTitles.size() < cap) winTitles.resize(cap);
  if (winAuthors.size() < cap) winAuthors.resize(cap);
  if (winHeaders.size() < cap) winHeaders.resize(cap);
  if (winSeries.size() < cap) winSeries.resize(cap);
  if (winSubtitles.size() < cap) winSubtitles.resize(cap);
  std::string seriesPosition;
  winItems.clear();
  if (winItems.capacity() < cap) winItems.reserve(cap);
  fui::BitmapRef bookIcon = GUI.shelfBookIcon();
  if (!bookIcon) bookIcon = listIconFor(UIIcon::Book, 32);
  const bool spines = spineShelf();

  const int windowStart = static_cast<int>(props.topIndex);
  int rows = 0;
  int headers = 0;
  // The window's previous book row, for the group comparison; -1 before one.
  int previousBook = -1;
  uint32_t previousInitial = 0;
  for (int position = windowStart; position < count && rows < static_cast<int>(cap); position++) {
    fui::ListItem item;
    item.actionValue = static_cast<int16_t>(position);
    if (shelfLeadRows() > 0 && (position == covers || position == covers + 1)) {
      item.label = position == covers ? menuRowLabel() : shelfHeadingText();
      winItems.push_back(item);
      rows++;
      continue;
    }
    const int entry = position - shelfRows();
    std::string& title = winTitles[static_cast<size_t>(rows)];
    std::string& author = winAuthors[static_cast<size_t>(rows)];
    std::string& series = winSeries[static_cast<size_t>(rows)];
    if (!rowTextFor(entry, title, author)) continue;
    seriesFor(entry, series, seriesPosition);
    // Each group gets its heading, and the window's first book that of its
    // group, so the reader always sees where they are.
    if (grouped) {
      bool startsGroup = previousBook < 0;
      uint32_t initial = 0;
      if (seriesGrouped) {
        startsGroup = startsGroup || series != winSeries[static_cast<size_t>(previousBook)];
      } else if (authorGrouped) {
        startsGroup = startsGroup || author != winAuthors[static_cast<size_t>(previousBook)];
      } else if (spines) {
        startsGroup = false;  // spines run on without letter headings
      } else {
        initial = titleInitialFor(entry);
        startsGroup = startsGroup || initial != previousInitial;
        previousInitial = initial;
      }
      if (startsGroup) {
        std::string& heading = winHeaders[static_cast<size_t>(headers++)];
        if (seriesGrouped)
          heading = series.empty() ? tr(STR_SHELF_STANDALONE) : series;
        else if (authorGrouped)
          formatAuthorHeading(author, heading);
        else
          formatInitialHeading(initial, heading);
        item.sectionHeading = heading.c_str();
      }
    }
    previousBook = rows;
    item.label = title.c_str();
    // "Author · Series #2"; the part the heading already names is left out.
    std::string& subtitle = winSubtitles[static_cast<size_t>(rows)];
    subtitle.clear();
    if (!authorGrouped) subtitle = author;
    if (!series.empty() && (!seriesGrouped || !seriesPosition.empty())) {
      if (!subtitle.empty()) subtitle.append(" \u00B7 ");
      if (!seriesGrouped) subtitle.append(series);
      if (!seriesPosition.empty()) subtitle.append(seriesGrouped ? "#" : " #").append(seriesPosition);
    }
    if (spines) {
      // A spine: the author at the right; the series is the heading's job.
      subtitle = authorGrouped ? std::string() : author;
      if (!subtitle.empty()) item.value = subtitle.c_str();
      winItems.push_back(item);
      rows++;
      continue;
    }
    if (!subtitle.empty()) item.subtitle = subtitle.c_str();
    item.icon = bookIcon;
    winItems.push_back(item);
    rows++;
  }

  props.items = winItems.data();
  props.itemsWindowFirst = static_cast<uint16_t>(windowStart);
  props.itemsWindowCount = static_cast<uint16_t>(winItems.size());
}

// The covers two to a row, each taking taps and holds like the row it stands
// for, then File Transfer centred and the heading the full list opens under.
void LibraryListActivity::buildShelfCovers(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int position = ringPos() - 1;
  const int covers = shelfCoverCount();

  const fui::Rect body = screen.body();
  const int side = metrics.contentSidePadding;
  const int columnWidth = (body.width - 3 * side) / 2;
  const int cellHeight = GUI.shelfCellHeight(renderer);
  for (int i = 0; i < covers; i++) {
    const Rect cell{body.x + side + (i % 2) * (columnWidth + side), body.y + (i / 2) * cellHeight, columnWidth,
                    cellHeight};
    GUI.drawShelfBook(renderer, cell, shelfBooks[static_cast<size_t>(i)], shelfCoverH, position == i);
    screen.frame().hit(fui::Rect{static_cast<int16_t>(cell.x), static_cast<int16_t>(cell.y),
                                 static_cast<int16_t>(cell.width), static_cast<int16_t>(cell.height)},
                       ACTION_ROW, static_cast<int16_t>(i), fui::InputTouch | fui::InputLongPress);
  }
  screen.spacer(static_cast<int16_t>(((covers + 1) / 2) * cellHeight));

  // File Transfer and the heading sit where Home's Books and Settings rows do,
  // File Transfer drawn as Home draws its menu.
  const int menuTop = metrics.homeTopPadding + GUI.homeCoverTileHeight(renderer) + metrics.homeMenuTopOffset;
  const int menuRowHeight = GUI.getMenuRowHeight(renderer);
  const int pageWidth = renderer.getScreenWidth();
  GUI.drawButtonMenu(
      renderer, Rect{0, menuTop, pageWidth, menuRowHeight}, 1, position == covers ? 0 : -1,
      [this](int) { return std::string(menuRowLabel()); },
      [this](int) { return isHome() ? UIIcon::Book : UIIcon::Transfer; });
  screen.frame().hit(
      fui::Rect{0, static_cast<int16_t>(menuTop), static_cast<int16_t>(pageWidth), static_cast<int16_t>(menuRowHeight)},
      ACTION_ROW, static_cast<int16_t>(covers), fui::InputTouch | fui::InputLongPress);
  const int headingTop = menuTop + menuRowHeight + metrics.menuSpacing;
  screen.spacer(static_cast<int16_t>(std::max(0, headingTop - screen.body().y)));
  drawShelfHeading(screen, static_cast<int16_t>(menuRowHeight), position == covers + 1);
}

void LibraryListActivity::formatInitialHeading(uint32_t initial, std::string& out) {
  out.clear();
  if (initial == 0) {
    out.push_back('#');
    return;
  }
  if (initial >= 'a' && initial <= 'z') initial -= 'a' - 'A';
  utf8AppendCodepoint(initial, out);
}

void LibraryListActivity::formatAuthorHeading(const std::string& author, std::string& out) const {
  out = author.empty() ? std::string(tr(STR_LIBRARY_UNKNOWN_AUTHOR)) : author;
  if (author.empty()) return;
  const size_t lastSpace = out.find_last_of(' ');
  if (lastSpace != std::string::npos && lastSpace + 1 < out.size()) {
    out = out.substr(lastSpace + 1) + ", " + out.substr(0, lastSpace);
  }
}

void LibraryListActivity::buildHeader(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  if (isHome()) return;  // no title: the book fills the page
  if (spineShelf()) {
    buildSpineHeader(screen);
    return;
  }
  const auto& theme = screen.theme();
  fui::HeaderProps header;
  header.title = headerTitle();
  header.titleText = theme.titleText;
  header.titleText.align = theme.headerTitleAlign;
  header.sidePadding = theme.headerSidePadding;
  header.minTouchSize = theme.minTouchSize;
  header.styles = theme.popup;
  if (header.styles.normal.border.kind == fui::PaintKind::None && theme.headerUnderline > 0) {
    header.styles.normal.border = fui::Paint::solid(fui::Color::Black);
    header.styles.normal.borderWidth = theme.headerUnderline;
  }
  header.trailingStyles = fui::plainStyles(fui::Paint::solid(fui::Color::Black));
  header.borderEdges = fui::EdgeBottom;
  // Same battery/clock band as every GUI.drawHeader screen; the header
  // heights are unified across themes, so the buttons derive from the band.
  GUI.applyHeaderStatus(renderer, header);
  if (mappedInput.hasTouch()) {
    header.leadingIcon = fui::bitmapFromIcon(icon_header_back_32);
    header.leadingAction = ACTION_BACK;
  }
  if (!degraded && !bookShelf()) {
    // Keep both touch actions together on the right; button boards reach
    // rebuild through the row options menu.
    header.trailingIcon = fui::bitmapFromIcon(icon_search_32);
    header.trailingAction = ACTION_SEARCH;
    if (mappedInput.hasTouch()) {
      header.trailingAdjacentIcon = fui::bitmapFromIcon(icon_refresh_cw_32);
      header.trailingAdjacentAction = ACTION_REBUILD;
    }
    // Vertical placement comes from applyHeaderStatus: buttons center on the
    // unified band.
  }
  const auto frameRect = screen.frame().screen();
  // Header and tabs share a screen-relative boundary, independent of bezel insets.
  UITheme::noteHeaderBand(metrics.topPadding, metrics.headerHeight);
  fui::header(screen.frame(),
              fui::Rect{frameRect.x, static_cast<int16_t>(metrics.topPadding), frameRect.width,
                        static_cast<int16_t>(metrics.headerHeight)},
              header);
}

void LibraryListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // The position readout owns the line above the hints; rows must not overlap
  // it.
  const int16_t readoutReserved =
      bookShelf() ? 0
                  : static_cast<int16_t>(renderer.getLineHeight(themedFontId(SMALL_FONT_ID)) + metrics.verticalSpacing);
  buildHeader(screen);
  // Home has no header: content starts under the float buttons.
  int contentTop = isHome() ? float_ui::kReaderMargin + float_ui::kReaderButton + float_ui::kGap
                                 : metrics.topPadding + metrics.headerHeight;
  if (spineShelf()) contentTop = spineContentTop();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(contentTop), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight + readoutReserved), 0});

  if (isHome()) {
    buildHome(screen);
    return;
  }
  if (bookShelf()) {
    buildShelfRows(screen);
    return;
  }
  if (!degraded) buildTabBar(screen);
  if (bookRowCount() == 0) {
    const char* message = tr(STR_LIBRARY_NO_RESULTS);
    if (filterFailed) {
      message = tr(STR_LIBRARY_SEARCH_UNAVAILABLE);
    } else if (query.empty()) {
      message = tr(STR_LIBRARY_EMPTY);
    }
    screen.centeredText(message);
    return;
  }
  buildRows(screen);
}

// "12/69 books" at the bottom right: which book is selected, out of how many.
//
// NOT a page count. How many rows fit varies with the view (author headings
// consume band height), so a page total grows and shrinks as you scroll. The
// book position is stable by construction, and it answers the question the
// reader actually has: how far in am I, and how much is left.
void LibraryListActivity::drawPositionReadout() const {
  const int count = listCount();
  if (count <= 0) return;

  char buf[32];
  const char* positionFormat = groupsCollapsed ? tr(STR_LIBRARY_GROUP_POSITION) : tr(STR_LIBRARY_POSITION);
  snprintf(buf, sizeof(buf), positionFormat, selectedEntry() + 1, count);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getTextWidth(themedFontId(SMALL_FONT_ID), buf);
  const int x = renderer.getScreenWidth() - width - SIDE_PADDING;
  const int y =
      renderer.getScreenHeight() - metrics.buttonHintsHeight - renderer.getLineHeight(themedFontId(SMALL_FONT_ID));
  renderer.drawText(themedFontId(SMALL_FONT_ID), x, y, buf, true);
}

const char* LibraryListActivity::headerTitle() const {
  if (bookShelf()) return tr(STR_BOOKS);
  if (!headerSearchTitle.empty()) return headerSearchTitle.c_str();
  return degraded ? tr(STR_LIBRARY_TITLE_UNSORTED) : tr(STR_LIBRARY);
}

void LibraryListActivity::drawHoldHelp() const {
  if (mappedInput.hasTouch() || groupsCollapsed) return;
  const char* help = nullptr;
  if (tabsFocused() && !degraded)
    help = tr(STR_LIBRARY_HOLD_SORT);
  else if (!tabsFocused() && isRecentSort(sortOrder) && listCount() > 0)
    help = tr(STR_LIBRARY_HOLD_OPTIONS);  // recent rows: hold opens the row menu
  else if (!tabsFocused() && deleteEligible() && listCount() > 0)
    help = tr(STR_HOLD_OPEN_TO_DELETE);
  else if (!tabsFocused() && groupable())
    help = tr(STR_LIBRARY_HOLD_GROUPS);
  if (!help) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(themedFontId(SMALL_FONT_ID));
  const int y = renderer.getScreenHeight() - metrics.buttonHintsHeight - lineHeight;
  GUI.drawHelpText(renderer, Rect{SIDE_PADDING, y, renderer.getScreenWidth() / 2 - SIDE_PADDING, lineHeight}, help);
}

// OptionPopup is a self-contained modal: it owns the whole frame (hints
// included) whenever it is up, mirroring the FileBrowser pattern.
void LibraryListActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiTabListActivity::render(std::move(lock));
  painted = true;
}

void LibraryListActivity::drawFooter() {
  if (bookShelf()) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }
  drawPositionReadout();
  drawHoldHelp();

  const bool backGoesHome = tabsFocused() && !groupsCollapsed && query.empty();
  const char* backLabel = backGoesHome ? tr(STR_HOME) : tr(STR_BACK);
  const char* confirmLabel = groupsCollapsed ? tr(STR_SELECT) : tr(STR_OPEN);
  const bool canSearch = tabsFocused() && !degraded;
  const auto labels = mappedInput.mapLabels(backLabel, tabsFocused() ? tr(STR_TOGGLE) : confirmLabel,
                                            canSearch ? tr(STR_SEARCH) : tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// --- touch Home: the open book, and the way to the shelf ---------------------

bool LibraryListActivity::handleHomeInput() {
  // Nothing to move between: Confirm opens the book, the side buttons rest.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (lockNextConfirmRelease) {
      lockNextConfirmRelease = false;
    } else if (shelfCoverCount() > 0) {
      openBookByPath(shelfBooks.front().path);
    }
    return true;
  }
  return mappedInput.wasPressed(MappedInputManager::Button::Up) ||
         mappedInput.wasPressed(MappedInputManager::Button::Down) ||
         mappedInput.wasPressed(MappedInputManager::Button::Confirm);
}

int LibraryListActivity::homeButtonsTop() const {
  return renderer.getScreenHeight() - kHomeButtonsBottom - float_ui::kReaderButton;
}

int LibraryListActivity::homeCellHeight() const { return (homeButtonsTop() - kHomeButtonsGap - kHomeTop) / 2; }

// As tall as a row allows, unless two books side by side would leave less
// than kHomeMinGap at the edges and between them.
int LibraryListActivity::homeCoverHeight() const {
  const int extra = GUI.shelfCellHeight(renderer) - GUI.shelfCoverHeight(renderer);
  int height = homeCellHeight() - extra;
  const int width = renderer.getScreenWidth();
  while (height > 0 && 2 * GUI.shelfBookWidth(height) + 3 * kHomeMinGap > width) height--;
  return height;
}

void LibraryListActivity::gridActionTrampoline(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<LibraryListActivity*>(user);
  if (event.value >= kSpineSort) {
    if (!event.longPress) self->onSpineAction(event.value);
    return;
  }
  if (event.value == kGridBooks) {
    if (event.longPress) return;
    self->app.clearTapFlash();
    self->index.close();
    activityManager.goToLibrary();
    return;
  }
  if (event.value < 0 || event.value >= self->shelfCoverCount()) return;
  const auto& book = self->shelfBooks[static_cast<size_t>(event.value)];
  if (event.longPress) {
    self->showBookOptions(book.path, book.title, true);
  } else {
    self->openBookByPath(book.path);
  }
}

void LibraryListActivity::buildHome(UiScreen& screen) {
  const int width = renderer.getScreenWidth();
  const auto hit = [&screen](const int x, const int y, const int w, const int h, const int16_t value,
                             const uint8_t input) {
    screen.frame().hit(fui::Rect{static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(w),
                                 static_cast<int16_t>(h)},
                       ACTION_GRID, value, input);
  };

  // The recent books, two to a row, with the same space at the edges as
  // between them.
  const int covers = shelfCoverCount();
  if (covers > 0) {
    const int bookWidth = GUI.shelfBookWidth(shelfCoverH);
    const int gap = (width - 2 * bookWidth) / 3;
    const int cellWidth = bookWidth + 2 * GUI.shelfSideZone();
    const int cellHeight = homeCellHeight();
    for (int i = 0; i < covers; i++) {
      const int bookX = gap + (i % 2) * (bookWidth + gap);
      const Rect cell{bookX - GUI.shelfSideZone(), kHomeTop + (i / 2) * cellHeight, cellWidth, cellHeight};
      GUI.drawShelfBook(renderer, cell, shelfBooks[static_cast<size_t>(i)], shelfCoverH, false);
      hit(cell.x, cell.y, cell.width, cell.height, static_cast<int16_t>(i), fui::InputTouch | fui::InputLongPress);
    }
  } else {
    screen.centeredText(tr(STR_NO_OPEN_BOOK));
  }

  // The shelf. Settings is the control center, pulled down.
  constexpr int size = float_ui::kReaderButton;
  const int x = (width - size) / 2;
  const int top = homeButtonsTop();
  float_ui::drawRound(renderer, float_ui::Box{x, top, size, size}, icon_float_library_28, float_ui::kReaderFill);
  hit(x, top, size, size, kGridBooks, fui::InputTouch);

  if (battery_status::isLow()) battery_status::drawBadge(renderer, width / 2, kLowBadgeTop);
}

// --- touch Books: book spines -------------------------------------------------

int LibraryListActivity::spineContentTop() const {
  return float_ui::kReaderEdge + float_ui::kReaderButton + float_ui::kReaderEdge;
}

void LibraryListActivity::buildSpineHeader(UiScreen& screen) {
  const int width = renderer.getScreenWidth();
  constexpr int size = float_ui::kReaderButton;
  const int top = float_ui::kReaderEdge;
  const int closeX = width - float_ui::kReaderMargin - size;

  const auto hit = [&screen](const int x, const int y, const int w, const int h, const int16_t value) {
    screen.frame().hit(fui::Rect{static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(w),
                                 static_cast<int16_t>(h)},
                       ACTION_GRID, value, fui::InputTouch);
  };

  // File Transfer beside the close button.
  const int menuX = closeX - float_ui::kGap - size;
  float_ui::drawRound(renderer, float_ui::Box{menuX, top, size, size}, icon_float_transfer_28, float_ui::kReaderFill);
  hit(menuX, top, size, size, kSpineTransfer);

  // The order as a pill button at the left: "By Title" and a down chevron.
  std::string order = shelfOrderText();
  if (!order.empty() && order[0] >= 'a' && order[0] <= 'z') order[0] = static_cast<char>(order[0] - 'a' + 'A');
  const int font = uiScaleSpec().bodyFontId;
  constexpr int chevronGap = 6;
  constexpr int pillPad = 28;  // text to the pill's rounded ends
  const int textWidth = renderer.getTextWidth(font, order.c_str(), EpdFontFamily::ITALIC);
  const int pillX = float_ui::kReaderMargin;
  const int pillWidth =
      std::min(menuX - float_ui::kGap - pillX, 2 * pillPad + textWidth + chevronGap + icon_float_chevron_down_28.w);
  float_ui::drawShape(renderer, pillX, top, pillWidth, size, float_ui::kReaderFill);
  const int textX = pillX + pillPad;
  renderer.drawText(font, textX, top + (size - renderer.getLineHeight(font)) / 2, order.c_str(), true,
                    EpdFontFamily::ITALIC);
  float_ui::drawIcon(renderer, icon_float_chevron_down_28, textX + textWidth + chevronGap,
                     top + (size - icon_float_chevron_down_28.h) / 2);
  hit(pillX, top, pillWidth, size, kSpineSort);

  float_ui::drawRound(renderer, float_ui::Box{closeX, top, size, size}, icon_float_close_28, float_ui::kReaderFill);
  hit(closeX, top, size, size, kSpineClose);

  if (battery_status::isLow()) battery_status::drawBadge(renderer, width / 2, kLowBadgeTop);
}

void LibraryListActivity::onSpineAction(const int16_t value) {
  app.clearTapFlash();
  switch (value) {
    case kSpineClose:
      handleBackAction();
      break;
    case kSpineTransfer:
      index.close();
      activityManager.goToFileTransfer();
      break;
    default:
      showSortPicker();
      break;
  }
}
