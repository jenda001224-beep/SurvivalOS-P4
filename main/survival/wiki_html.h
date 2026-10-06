/**
 * @Description: Turns Wikipedia article HTML into blocks the UI can lay out
 * @License: GPL 3.0
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace lilygo_box::survival {

inline constexpr size_t kWikiHrefMaxLength = 192;

// A link inside a block, addressed in rendered characters so it can be matched
// against what LVGL reports under a tap.
struct WikiLink {
  uint32_t start_char = 0;
  uint32_t end_char = 0;
  char href[kWikiHrefMaxLength] = {};
};

enum class WikiBlockType : uint8_t {
  kTitle,
  kHeading,
  kSubheading,
  kParagraph,
  kListItem,
  kSeparator,
};

// One laid-out run of the article.
struct WikiBlock {
  WikiBlockType type = WikiBlockType::kParagraph;
  // Text with LVGL recolor markup already applied to the links.
  char* text = nullptr;
  WikiLink* links = nullptr;
  uint32_t link_count = 0;
};

// A parsed article.
struct WikiDocument {
  WikiBlock* blocks = nullptr;
  uint32_t block_count = 0;
  // Set when the article was longer than the block budget.
  bool truncated = false;
};

/**
 * @brief Parses Wikipedia article HTML into renderable blocks.
 *
 * This is deliberately not a real HTML engine: it keeps the structure that
 * matters for reading — headings, paragraphs, list items and links — and throws
 * away scripts, styles, navigation furniture and reference markers.
 *
 * @param html Article HTML; does not need to be zero terminated.
 * @param length Length of the HTML in bytes.
 * @param document Parsed document output; release with FreeWikiDocument().
 * @return true when at least one block was produced.
 */
bool ParseWikiHtml(const char* html, size_t length, WikiDocument* document);

/**
 * @brief Releases a parsed document.
 * @param document Document to free.
 */
void FreeWikiDocument(WikiDocument* document);

/**
 * @brief Resolves a link href against the article it appeared in.
 *
 * ZIM articles link with relative urls like `../A/Praha` or `Praha`, which have
 * to become a plain entry url before the archive can be searched.
 *
 * @param href Raw href from the article.
 * @param out Resolved entry url output.
 * @param out_size Size of the output buffer.
 * @return true when the href points at another article in the archive.
 */
bool ResolveWikiHref(const char* href, char* out, size_t out_size);

}  // namespace lilygo_box::survival
