#pragma once

#include <vector>

#include "activities/UiListActivity.h"

// UiListActivity variant for screens with a tab band above the list (Settings,
// Text Settings). Navigation is a ring: position 0 is the tab bar, 1..N are
// the list rows, so props.selectedIndex = ring - 1 (-1 = tab band focused).
// Each tab owns its own ListNav (selection + viewport memory); activeNav()
// redirects the whole UiListActivity protocol (touch routing, swipe scroll,
// screen sync) to the active tab's state. Button navigation walks the ring on
// release and steps the TAB on continuous hold. The tab-bar chrome (pill
// styles, focused band wash) is shared verbatim via buildTabBar().
//
// Subclasses own the button semantics wholesale (handleButtons is pure here:
// the two existing tab screens differ on press-vs-release and what Back does)
// plus what activating a row or tapping a tab means.
class UiTabListActivity : public UiListActivity {
 public:
  void onEnter() override;

 protected:
  // Tab-bar action; subclass actions start at ACTION_TAB_USER.
  static constexpr freeink::ui::ActionId ACTION_TAB = ACTION_USER;
  static constexpr freeink::ui::ActionId ACTION_TAB_USER = ACTION_USER + 1;

  UiTabListActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                    bool wantsTouchLongPress = false);

  // --- subclass contract (in addition to UiListActivity's) ------------------
  virtual int tabCount() const = 0;
  virtual int activeTab() const = 0;
  virtual const char* tabLabel(int index) const = 0;
  virtual freeink::ui::TabIndicator tabIndicator(int) const { return freeink::ui::TabIndicator::None; }
  // Touch tap on a tab pill (bounds already checked).
  virtual void onTabAction(int index) = 0;
  // Advance the active tab by direction (continuous-hold navigation; also what
  // Confirm on the tab bar should do). Subclass owns wrap and any per-switch
  // state reset, and requests the update.
  virtual void stepTab(int direction) = 0;
  // The two tab screens disagree on press-vs-release and Back semantics, so
  // there is no shared default.
  bool handleButtons() override = 0;

  // --- ring plumbing ---------------------------------------------------------
  freeink::ui::ListNav& activeNav() override;
  // Ring position of the active tab (0 = tab bar) for const contexts.
  int ringPos() const;
  // ACTION_ROW lands as ring = row + 1, then activateIndex(row).
  void onRowAction(const freeink::ui::ActionEvent& event) override;
  // Press walks the ring; continuous hold steps the tab.
  void navigateButtons() override;
  // Move to a ring position: tab bar rewinds the viewport, a row pulls the
  // viewport to itself.
  void moveRingTo(int ringIndex);
  // Row lists without a tab bar (ring 1..count). A single press steps one row.
  // A rapid second press in the same direction jumps to the start of the
  // section the burst began in (Previous) or of the next section (Next); each
  // further rapid press jumps one more section.
  void stepRowsWithSectionJumps(int direction, int count);
  // Holding a button repeats single-row steps, stopping at either end.
  void holdStepRows(int direction, int count);
  // Whether row (0-based) begins a section; row 0 always does.
  virtual bool rowStartsSection(int row) const { return row == 0; }

  // --- screen helpers --------------------------------------------------------
  // The shared tab band: theme-driven pill treatment (label-hugging Lyra vs
  // full-slot RoundedRaff), Lyra focused band wash, always-on divider.
  void buildTabBar(UiScreen& screen);
  // Ring-aware counterpart of syncListViewport: measures rows, applies the
  // one-shot follow to the remembered row, clamps, and writes
  // props.selectedIndex = ring - 1.
  void syncTabListViewport(UiScreen& screen, freeink::ui::ListProps& props);

  // Per-tab selection/viewport state, sized in onEnter. Protected so subclass
  // tab-switch code can seed the target tab's ring/viewport.
  std::vector<freeink::ui::ListNav> tabNavs;
  bool navigationStartedOnTabs = false;

 private:
  // Rapid-press burst state for stepRowsWithSectionJumps.
  static constexpr unsigned long kRapidPressMs = 200;
  unsigned long lastStepMs = 0;
  int burstDirection = 0;
  int burstPresses = 0;
  int burstOriginRow = 0;

 protected:

  // When > 0, each tab pill is capped at its label width plus this padding
  // per side, centered in its unchanged equal-width slot. With few tabs the
  // default full-slot pill stretches across a third of the screen; screens
  // with more tabs (Settings) keep the default of 0 (fill the slot).
  int16_t tabPillMaxPad = 0;

 private:
  static void tabActionTrampoline(const freeink::ui::ActionEvent& event, void* user);
};
