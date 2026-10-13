#pragma once
#include <BoardConfig.h>
#include <FreeInkUIGfxRenderer.h>
#include <I18n.h>

#include "UITheme.h"

// Merges the active UITheme's shape (row gaps, radii, insets, selection
// style) with the uiScale-derived sizes into FreeInkUI theme tokens: the
// theme says what lists look like, the scale says how big they are.
// Everything read here is plain data from ThemeMetrics — the same values an
// SD-card theme file will eventually supply.
inline freeink::ui::ThemeTokens uiThemeTokens(const freeink::ui::GfxRendererTarget& target) {
  namespace fui = freeink::ui;
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();

  fui::ThemeTokens tokens = fui::themeTokensForLineHeight(target.lineHeight(fui::GfxRendererTarget::FONT_BODY));
  // Button-device lists retain the theme's preferred density. The SDK still
  // grows individual rows for wrapping, subtitles and icons.
  if (!BoardConfig::hasTouch()) tokens.listMinRowHeight = static_cast<int16_t>(metrics.listRowHeight);
  tokens.listRowGap = static_cast<int16_t>(metrics.listRowGap);
  tokens.listRowRadius = static_cast<uint8_t>(metrics.listRowRadius);
  tokens.listInset = static_cast<int16_t>(metrics.listInset);
  tokens.listSidePadding = static_cast<int16_t>(metrics.listSidePadding);
  tokens.listSelectionStyle = static_cast<fui::SelectionStyle>(metrics.listSelectionStyle);
  if (metrics.listSelectionBorder > 0) {
    tokens.listRow = fui::selectedOutlineListRowStyles(metrics.listRowRadius);
    tokens.listRow.selected.borderWidth = static_cast<uint8_t>(metrics.listSelectionBorder);
    tokens.listRow.active = tokens.listRow.selected;
  }
  tokens.listScrollWidth = static_cast<int16_t>(metrics.listScrollWidth);
  tokens.listScrollSide = static_cast<uint8_t>(metrics.listScrollSide);
  // A design gap only, 0 by default: the bezel is already compensated once at
  // the frame level (GfxRendererTarget::deviceContext() feeds the board's
  // viewable insets into the fui safe area, and every list band derives from
  // safeRect()). Adding the bezel inset here doubled the compensation and
  // floated the track a full bezel-width inside the visible edge.
  tokens.listScrollInset = static_cast<int16_t>(metrics.listScrollInset);
  tokens.listScrollMargin = static_cast<int16_t>(metrics.listScrollMargin);
  // Screen::header()/status() band height. Without this the SDK's
  // line-height-derived default applies and fui-drawn headers (OPDS) come out
  // a different height than every GUI.drawHeader band.
  tokens.headerHeight = static_cast<int16_t>(metrics.headerHeight);
  tokens.headerSidePadding = static_cast<int16_t>(metrics.headerSidePadding);
  tokens.headerUnderline = static_cast<uint8_t>(metrics.headerUnderlineSize);
  tokens.headerTitleAlign = static_cast<fui::TextAlign>(metrics.headerTitleAlign);
  // Control-panel shape (sheet corners, tiles, step buttons, capsule slider),
  // so the control center follows the theme like every list and header does.
  tokens.controlRadius = static_cast<uint8_t>(metrics.controlRadius);
  tokens.sheetRadius = static_cast<uint8_t>(metrics.sheetRadius);
  tokens.sheetPencilRule = metrics.sheetPencilRule;
  tokens.capsuleRadius = static_cast<uint8_t>(metrics.capsuleRadius);
  tokens.bodyText.bold = metrics.listTitleBold;
  tokens.titleText.bold = metrics.headerTitleBold;
  if (metrics.listOverflowCounts) {
    tokens.listMoreAboveFormat = tr(STR_ROWS_ABOVE);
    tokens.listMoreBelowFormat = tr(STR_ROWS_BELOW);
    tokens.listMoreText = tokens.bodyText;
    tokens.listMoreText.italic = true;
    tokens.listMoreText.align = fui::TextAlign::Center;
  }
  return tokens;
}

// Section headings for lists that use ListItem::sectionHeading: themes with
// bookStyleLists set them like a book's section titles (centered italic body
// text on a full row); other themes keep the list's default heading.
inline void applySectionHeadingStyle(const freeink::ui::ThemeTokens& theme, freeink::ui::ListProps& props) {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  if (!metrics.bookStyleLists) return;
  props.headerText = theme.bodyText;
  props.headerText.bold = false;
  props.headerText.italic = true;
  props.headerText.align = freeink::ui::TextAlign::Center;
  props.headerUnderline = false;
  props.sectionGap = 0;
  props.headerRowHeight = static_cast<int16_t>(metrics.listRowHeight);
}
