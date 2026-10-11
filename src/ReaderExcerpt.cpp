#include "ReaderExcerpt.h"

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <HalStorage.h>

namespace readerexcerpt {
namespace {
// Enough for a preview pane of text at the smallest reader size. The string
// keeps its capacity, so page turns after the first do not allocate.
constexpr size_t kMaxBytes = 600;
constexpr char kExcerptFile[] = "/.crosspoint/excerpt.txt";
Excerpt excerpt;
bool loadAttempted = false;
uint32_t savedGeneration = 0;

// File format: '1' or '0' (startsMidParagraph), then the text.
void load() {
  loadAttempted = true;
  HalFile file;
  if (!Storage.openFileForRead("EXC", kExcerptFile, file)) return;
  const int flag = file.read();
  if (excerpt.text.capacity() < kMaxBytes) excerpt.text.reserve(kMaxBytes);
  excerpt.text.resize(kMaxBytes);
  const int bytesRead = flag < 0 ? 0 : file.read(excerpt.text.data(), kMaxBytes);
  file.close();
  excerpt.text.resize(bytesRead > 0 ? static_cast<size_t>(bytesRead) : 0);
  excerpt.startsMidParagraph = flag == '1';
  excerpt.generation++;
  savedGeneration = excerpt.generation;
}
}  // namespace

void capture(const Page& page) {
  loadAttempted = true;  // a live page supersedes the saved one
  excerpt.text.clear();
  excerpt.startsMidParagraph = false;
  excerpt.generation++;
  if (excerpt.text.capacity() < kMaxBytes) excerpt.text.reserve(kMaxBytes);

  bool first = true;
  TextBlock::SourceRange previous;
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const TextBlock* block = static_cast<const PageLine&>(*element).getBlock();
    if (!block || !block->valid()) continue;
    for (uint16_t i = 0; i < block->wordCount(); i++) {
      const uint16_t length = block->wordTextLen(i);
      if (excerpt.text.size() + length + 1 > kMaxBytes) return;
      const TextBlock::SourceRange range = block->wordSourceRange(i);
      if (first) {
        excerpt.startsMidParagraph = !block->wordStartsParagraph(i);
        first = false;
      } else if (block->wordStartsParagraph(i)) {
        excerpt.text.push_back('\n');
      } else if (range.start == UINT32_MAX || range.start != previous.end) {
        // Contiguous source ranges are one word split by a hyphen or by style.
        excerpt.text.push_back(' ');
      }
      excerpt.text.append(block->wordText(i), length);
      previous = range;
    }
  }
}

const Excerpt& current() {
  if (!loadAttempted) load();
  return excerpt;
}

void save() {
  if (excerpt.text.empty() || excerpt.generation == savedGeneration) return;
  HalFile file;
  if (!Storage.openFileForWrite("EXC", kExcerptFile, file)) return;
  const char flag = excerpt.startsMidParagraph ? '1' : '0';
  file.write(&flag, 1);
  file.write(excerpt.text.data(), excerpt.text.size());
  file.close();
  savedGeneration = excerpt.generation;
}

}  // namespace readerexcerpt
