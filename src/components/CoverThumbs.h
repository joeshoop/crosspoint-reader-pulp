#pragma once

struct RecentBook;
struct Rect;
class GfxRenderer;

// Makes sure a recent book's cover thumb exists at `height`, generating it
// from the book when it does not. The first generation puts up the Loading
// popup (showingLoading, popupRect) for the caller to advance; a book with no
// usable cover gets its coverBmpPath cleared. Returns true when the book's
// thumb has just become usable (path found or thumb made). Only one book parser lives at a time: EPUB/XTC objects exceed the
// stack budget.
bool ensureCoverThumb(GfxRenderer& renderer, RecentBook& book, int height, bool& showingLoading, Rect& popupRect);
