#pragma once

#include <cstdint>
#include <string>

class Page;

// Text of the page the reader drew last, so Text Settings can preview the
// current book instead of a stock sample. Kept on the SD card between boots. Paragraphs are separated by '\n'.
namespace readerexcerpt {

struct Excerpt {
  std::string text;
  bool startsMidParagraph = false;  // the page opened partway through a paragraph
  uint32_t generation = 0;          // bumped on every capture
};

void capture(const Page& page);
// Loads the saved excerpt on first use, so the preview survives a restart.
const Excerpt& current();
// Writes the excerpt to the SD card if it changed since the last save.
void save();

}  // namespace readerexcerpt
