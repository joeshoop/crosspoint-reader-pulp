#pragma once
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/UiAppHost.h"
#include "fontIds.h"

// Shared slider-dialog popup, drawn over the current screen (no clear) via
// fui::optionDialog with the same chrome OptionPopup uses: title caption, a
// large value readout as the headline, a full-width capsule slider with the
// range endpoints above it, and a [-] [+] [Confirm] option-button row on
// touch boards or two step-hint lines on button boards. Cancel is Back or a
// tap outside the dialog. Used by the interval and percent selection
// dialogs, which differ only in how they format the readout, endpoints, and
// hints and what the actions do.
struct UiSliderDialogSpec {
  const char* title = nullptr;    // dialog caption
  const char* readout = nullptr;  // preformatted current-value text
  int value = 0;                  // slider position (0-based within max)
  int max = 1;                    // slider range
  // Range endpoints, drawn small over the slider band's ends ("0%" / "100%").
  const char* minLabel = nullptr;
  const char* maxLabel = nullptr;
  freeink::ui::ActionId sliderAction = 0;
  freeink::ui::ActionId stepAction = 0;  // dispatched with value -1 / +1
  freeink::ui::ActionId okAction = 0;
  // Absorbs taps on the dialog body (needs no handler); the activity treats
  // an unrouted release with real coordinates as tap-outside = cancel.
  freeink::ui::ActionId chromeAction = 0;
  // Step hints for button boards (small step, large step); skipped on touch.
  const char* hintLine1 = nullptr;
  const char* hintLine2 = nullptr;
  // As a sheet: reach at least up to this y (-1: just fit), so a sheet it
  // opens over is covered, not peeking out above it.
  int sheetTop = -1;
};

inline void buildSliderDialogScreen(UiAppHost::UiScreen& screen, freeink::ui::GfxRendererTarget& uiTarget,
                                    const MappedInputManager& mappedInput, const UiSliderDialogSpec& spec) {
  namespace fui = freeink::ui;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& theme = screen.theme();

  // The readout is the dialog's hero: remap the title slot to the large sans
  // so it reads at a glance. Book-styled themes keep their reader font there.
  if (!metrics.uiUsesReaderFont) uiTarget.setFont(fui::GfxRendererTarget::FONT_TITLE, NOTOSANS_18_FONT_ID);

  const bool touch = mappedInput.hasTouch();
  // Touch boards get the finger-sized pill (same as the frontlight rows); on
  // button boards the capsule is a read-only gauge driven by the keys, so a
  // slim band is enough.
  const int16_t bandHeight = touch ? 56 : 24;
  const int16_t capLh = screen.target().lineHeight(theme.smallText.font);
  const int16_t sliderH = static_cast<int16_t>(capLh + theme.spaceMd + bandHeight);
  const bool hints = !touch && (spec.hintLine1 || spec.hintLine2);
  const int16_t hintsH = hints ? static_cast<int16_t>(capLh * 2 + theme.spaceSm + theme.spaceMd) : 0;

  // Dialog chrome and slots, styled exactly like OptionPopup: bordered popup
  // panel per theme frame metrics, body-font caption, [-] [+] [Confirm] as
  // one bottom row of dialog buttons ("-"/"+" fire stepAction with -/+1, so
  // the slider itself stays a bare capsule). Static because the props embed
  // StyleSets, well past the 256-byte local stack budget; every field is
  // reassigned each build.
  fui::DialogOption options[3];
  options[0].label = "-";
  options[0].action = spec.stepAction;
  options[0].value = -1;
  options[1].label = "+";
  options[1].action = spec.stepAction;
  options[1].value = +1;
  options[2].label = tr(STR_CONFIRM);
  options[2].action = spec.okAction;

  static fui::OptionDialogProps props;
  props.title = spec.title;
  props.headline = spec.readout;
  props.options = touch ? options : nullptr;
  props.optionCount = touch ? 3 : 0;
  props.inputMask = fui::InputTouch;
  props.titleText = theme.smallText;
  props.titleText.bold = true;
  props.titleText.align = fui::TextAlign::Center;
  props.headlineText = theme.titleText;
  props.headlineText.bold = true;
  props.headlineText.align = fui::TextAlign::Center;
  props.buttonText = theme.bodyText;
  const int16_t innerPadding = static_cast<int16_t>(metrics.optionPopupInnerPadding);
  props.padding = fui::Insets{innerPadding, innerPadding, innerPadding, innerPadding};
  props.gap = static_cast<int16_t>(metrics.optionPopupItemSpacing);
  // defaultPopupStyles() has no border, so opt in using the per-theme frame metrics.
  props.styles = fui::defaultPopupStyles();
  props.styles.normal.border = fui::Paint::solid(fui::Color::Black);
  props.styles.normal.borderWidth = static_cast<uint8_t>(metrics.popupFrameThickness);
  props.styles.normal.radius = static_cast<uint8_t>(metrics.popupCornerRadius);
  const bool asSheet = metrics.sliderDialogSheet;
  if (metrics.popupPencilFrame || asSheet) {
    // Outlined in pencil below, after the panel is drawn, or edged by the sheet.
    props.styles.normal.borderWidth = 0;
    props.styles.normal.radius = 0;
  }
  props.styles.selected = props.styles.normal;
  props.styles.focused = props.styles.normal;
  props.styles.active = props.styles.normal;
  props.styles.disabled = props.styles.normal;
  props.buttonHeight =
      fui::clampI16(screen.target().lineHeight(theme.bodyText.font) + metrics.optionPopupSelectionVPadding * 2);
  // Square -/+ buttons; Confirm flexes into the remaining row width so its
  // label never truncates on large-font themes.
  options[0].width = props.buttonHeight;
  options[1].width = props.buttonHeight;

  // Reserve the slider band (and hint lines) as the dialog's content band;
  // optionDialog returns its rect. Same width rule as OptionPopup.
  props.contentHeight = static_cast<int16_t>(sliderH + hintsH);
  const fui::Rect screenRect = screen.frame().screen();
  const int16_t width = asSheet
                            ? screenRect.width
                            : fui::clampI16(std::min<int>(screenRect.width * 3 / 4,
                                                          screenRect.width - metrics.optionPopupDialogSideMargin * 2));
  // Measured height minus one spaceMd: texts anchor to the top and the band/
  // buttons to the bottom, so the trim comes out of the air between the
  // readout and the slider band (which already carries its endpoints line).
  const int16_t height =
      fui::clampI16(fui::optionDialogHeight(screen.target(), props, width) - theme.spaceMd, 0, screenRect.height);
  fui::Rect dialog = fui::centeredRect(screenRect, fui::Size{width, height});
  if (asSheet) {
    // A sheet over the bottom of the page, as the reader menu opens: the dialog
    // sits on it above the button hints, under the sheet's top edge.
    const int16_t edge = static_cast<int16_t>(metrics.verticalSpacing);
    const int16_t hintsRow = static_cast<int16_t>(metrics.verticalSpacing + metrics.buttonHintsHeight);
    const fui::Rect safe = screen.frame().safeRect();
    int sheetH = edge + height + hintsRow;
    if (spec.sheetTop >= 0) sheetH = std::max(sheetH, safe.bottom() - spec.sheetTop);
    fui::SheetProps sheetProps;
    sheetProps.anchor = fui::SheetEdge::Bottom;
    sheetProps.grabberHeight = 0;
    sheetProps.grabberMargin = 0;
    sheetProps.grabberInset = edge;
    sheetProps.ruleWidth = metrics.sheetPencilRule ? 2 : 1;
    screen.sheet(sheetProps, static_cast<int16_t>(sheetH));
    // Centered in the room between the sheet's edge and the hints.
    const int roomTop = safe.bottom() - sheetH + edge;
    const int room = sheetH - edge - hintsRow;
    dialog = fui::Rect{screenRect.x, static_cast<int16_t>(roomTop + (room - height) / 2), width, height};
  }
  // Chrome guard first, controls after: routing scans newest-first, so the
  // buttons and slider win inside the dialog and the guard absorbs the rest —
  // same pattern as OptionPopup.
  screen.frame().hit(dialog, spec.chromeAction, 0, fui::InputTouch);
  const fui::Rect band = fui::optionDialog(screen.frame(), dialog, props);
  if (metrics.popupPencilFrame && !asSheet) {
    constexpr int16_t kLine = 2;
    auto& target = screen.target();
    const auto dot = [&target](const int x, const int y) {
      target.fill(fui::Rect{static_cast<int16_t>(x), static_cast<int16_t>(y), 1, 1},
                  fui::Paint::solid(fui::Color::Black));
    };
    fui::pencilStroke(fui::Rect{dialog.x, dialog.y, dialog.width, kLine}, 0, dot);
    fui::pencilStroke(fui::Rect{dialog.x, static_cast<int16_t>(dialog.bottom() - kLine), dialog.width, kLine}, 0, dot);
    fui::pencilStroke(fui::Rect{dialog.x, dialog.y, kLine, dialog.height}, 0, dot);
    fui::pencilStroke(fui::Rect{static_cast<int16_t>(dialog.right() - kLine), dialog.y, kLine, dialog.height}, 0, dot);
  }

  if (hints) {
    // Two-line step hint (front buttons = fine step, side buttons = coarse
    // step) under the slider, preformatted by the caller so the layout
    // doesn't depend on a separator hidden in translated text.
    fui::TextStyle hint = theme.smallText;
    hint.align = fui::TextAlign::Center;
    const int16_t hintY = static_cast<int16_t>(band.y + sliderH + theme.spaceMd);
    if (spec.hintLine1) screen.target().text(fui::Rect{band.x, hintY, band.width, capLh}, spec.hintLine1, hint);
    if (spec.hintLine2) {
      screen.target().text(fui::Rect{band.x, static_cast<int16_t>(hintY + capLh + theme.spaceSm), band.width, capLh},
                           spec.hintLine2, hint);
    }
  }

  // The slider at the top of the content band: the range endpoints on a small
  // caption line, then a full-width capsule (no step buttons of its own — the
  // dialog's [-]/[+] cover exact values). Drag/tap sends dragPermille.
  const fui::Rect caption{band.x, band.y, band.width, capLh};
  fui::TextStyle endpoint = theme.smallText;
  if (spec.minLabel) screen.target().text(caption, spec.minLabel, endpoint);
  if (spec.maxLabel) {
    endpoint.align = fui::TextAlign::Right;
    screen.target().text(caption, spec.maxLabel, endpoint);
  }

  const fui::Rect sliderRect{band.x, static_cast<int16_t>(band.y + capLh + theme.spaceMd), band.width, bandHeight};
  if (metrics.popupPencilFrame) {
    // Pencil-styled themes: a light pencil line with the position as a black
    // dot, instead of a capsule filling up. Same touch/drag target.
    if (spec.sliderAction != 0) {
      screen.frame().hit(sliderRect, spec.sliderAction, 0, fui::InputTouch | fui::InputDrag);
    }
    auto& target = screen.target();
    const auto dot = [&target](const int x, const int y) {
      target.fill(fui::Rect{static_cast<int16_t>(x), static_cast<int16_t>(y), 1, 1},
                  fui::Paint::solid(fui::Color::Black));
    };
    const int16_t radius = static_cast<int16_t>(std::min<int>(bandHeight / 2, 9));
    const int16_t centerY = static_cast<int16_t>(sliderRect.y + sliderRect.height / 2);
    fui::pencilStroke(fui::Rect{sliderRect.x, static_cast<int16_t>(centerY - 1), sliderRect.width, 2}, 0, dot, true,
                      40);
    const int max = spec.max <= 0 ? 1 : spec.max;
    const int value = std::clamp(spec.value, 0, max);
    const int travel = sliderRect.width - 2 * radius;
    const int16_t centerX = static_cast<int16_t>(sliderRect.x + radius + travel * value / max);
    target.fill(fui::Rect{static_cast<int16_t>(centerX - radius), static_cast<int16_t>(centerY - radius),
                          static_cast<int16_t>(2 * radius), static_cast<int16_t>(2 * radius)},
                fui::Paint::solid(fui::Color::Black), static_cast<uint8_t>(radius));
    return;
  }
  fui::CapsuleSliderProps capsule;
  capsule.value = spec.value;
  capsule.max = spec.max;
  capsule.action = spec.sliderAction;
  capsule.radius = theme.capsuleRadius;
  fui::capsuleSlider(screen.frame(), sliderRect, capsule);
}
