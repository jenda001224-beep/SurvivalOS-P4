/**
 * @Description: Turns Wikipedia article HTML into blocks the UI can lay out
 * @License: GPL 3.0
 */
#include "survival/wiki_html.h"

#include <strings.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "base/logger.h"
#include "esp_heap_caps.h"

namespace lilygo_box::survival {
namespace {

// Long articles are cut off rather than allowed to spawn thousands of LVGL
// objects; the reader shows that it happened.
constexpr uint32_t kMaxBlocks = 600;
constexpr size_t kMaxBlockBytes = 8 * 1024;
constexpr uint32_t kMaxLinksPerBlock = 64;
// Links are drawn in the same blue everywhere so they read as links.
constexpr char kLinkColorOpen[] = "#4A90D9 ";
constexpr char kRecolorClose[] = "#";

// A growable text buffer in PSRAM.
struct TextBuffer {
  char* data = nullptr;
  size_t length = 0;
  size_t capacity = 0;
  // Rendered characters written so far, ignoring recolor markup.
  uint32_t characters = 0;
};

/**
 * @brief Makes sure a buffer can take more bytes.
 * @param buffer Buffer to grow.
 * @param extra Bytes about to be appended.
 * @return true when the buffer has room.
 */
bool Reserve(TextBuffer* buffer, size_t extra) {
  if (buffer->length + extra + 1 <= buffer->capacity) {
    return true;
  }
  size_t capacity = buffer->capacity == 0 ? 512 : buffer->capacity;
  while (capacity < buffer->length + extra + 1) {
    capacity *= 2;
  }
  if (capacity > kMaxBlockBytes) {
    capacity = kMaxBlockBytes;
    if (buffer->length + extra + 1 > capacity) {
      return false;
    }
  }
  auto* grown = static_cast<char*>(heap_caps_realloc(
      buffer->data, capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (grown == nullptr) {
    return false;
  }
  buffer->data = grown;
  buffer->capacity = capacity;
  return true;
}

/**
 * @brief Appends raw bytes that are not rendered characters, such as markup.
 * @param buffer Buffer to append to.
 * @param text Bytes to append.
 * @param length Number of bytes.
 */
void AppendMarkup(TextBuffer* buffer, const char* text, size_t length) {
  if (!Reserve(buffer, length)) {
    return;
  }
  std::memcpy(buffer->data + buffer->length, text, length);
  buffer->length += length;
  buffer->data[buffer->length] = '\0';
}

/**
 * @brief Appends one decoded code point as UTF-8 and counts it.
 * @param buffer Buffer to append to.
 * @param code_point Unicode code point.
 */
void AppendCodePoint(TextBuffer* buffer, uint32_t code_point) {
  char encoded[4] = {};
  size_t length = 0;
  if (code_point < 0x80) {
    encoded[0] = static_cast<char>(code_point);
    length = 1;
  } else if (code_point < 0x800) {
    encoded[0] = static_cast<char>(0xC0 | (code_point >> 6));
    encoded[1] = static_cast<char>(0x80 | (code_point & 0x3F));
    length = 2;
  } else if (code_point < 0x10000) {
    encoded[0] = static_cast<char>(0xE0 | (code_point >> 12));
    encoded[1] = static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
    encoded[2] = static_cast<char>(0x80 | (code_point & 0x3F));
    length = 3;
  } else {
    encoded[0] = static_cast<char>(0xF0 | (code_point >> 18));
    encoded[1] = static_cast<char>(0x80 | ((code_point >> 12) & 0x3F));
    encoded[2] = static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
    encoded[3] = static_cast<char>(0x80 | (code_point & 0x3F));
    length = 4;
  }
  if (!Reserve(buffer, length)) {
    return;
  }
  std::memcpy(buffer->data + buffer->length, encoded, length);
  buffer->length += length;
  buffer->data[buffer->length] = '\0';
  ++buffer->characters;
}

/**
 * @brief Appends a byte of already-decoded UTF-8 from the source document.
 * @param buffer Buffer to append to.
 * @param value Byte to append.
 */
void AppendSourceByte(TextBuffer* buffer, char value) {
  if (!Reserve(buffer, 1)) {
    return;
  }
  buffer->data[buffer->length++] = value;
  buffer->data[buffer->length] = '\0';
  // Continuation bytes belong to the character their lead byte already counted.
  if ((static_cast<unsigned char>(value) & 0xC0) != 0x80) {
    ++buffer->characters;
  }
}

/**
 * @brief Matches a named HTML entity.
 * @param name Entity name without the ampersand or semicolon.
 * @param length Length of the name.
 * @return The code point, or 0 when the entity is unknown.
 */
uint32_t NamedEntity(const char* name, size_t length) {
  struct Entity {
    const char* name;
    uint32_t code_point;
  };
  static constexpr Entity kEntities[] = {
      {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'},
      {"apos", '\''}, {"nbsp", ' '}, {"ndash", 0x2013}, {"mdash", 0x2014},
      {"hellip", 0x2026}, {"laquo", 0x00AB}, {"raquo", 0x00BB},
      {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"lsquo", 0x2018},
      {"rsquo", 0x2019}, {"deg", 0x00B0}, {"times", 0x00D7},
      {"middot", 0x00B7}, {"bull", 0x2022}, {"euro", 0x20AC},
  };
  for (const Entity& entity : kEntities) {
    if (std::strlen(entity.name) == length &&
        std::strncmp(entity.name, name, length) == 0) {
      return entity.code_point;
    }
  }
  return 0;
}

// Tags whose entire content is thrown away.
bool IsDiscardedTag(const char* name, size_t length) {
  static constexpr const char* kTags[] = {"script", "style", "head", "sup",
      "figcaption", "audio", "video", "math"};
  for (const char* tag : kTags) {
    if (std::strlen(tag) == length && strncasecmp(tag, name, length) == 0) {
      return true;
    }
  }
  return false;
}

// Tags that end the current block.
bool IsBlockTag(const char* name, size_t length) {
  static constexpr const char* kTags[] = {"p", "div", "br", "tr", "td", "th",
      "section", "blockquote", "dd", "dt", "caption", "figure"};
  for (const char* tag : kTags) {
    if (std::strlen(tag) == length && strncasecmp(tag, name, length) == 0) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Maps a tag name to the block type it should produce.
 * @param name Tag name.
 * @param length Tag name length.
 * @param type Block type output.
 * @return true when the tag starts a typed block.
 */
bool BlockTypeForTag(const char* name, size_t length, WikiBlockType* type) {
  if (length == 2 && (name[0] == 'h' || name[0] == 'H')) {
    if (name[1] == '1') {
      *type = WikiBlockType::kTitle;
      return true;
    }
    if (name[1] == '2') {
      *type = WikiBlockType::kHeading;
      return true;
    }
    if (name[1] >= '3' && name[1] <= '6') {
      *type = WikiBlockType::kSubheading;
      return true;
    }
  }
  if (length == 2 && strncasecmp(name, "li", 2) == 0) {
    *type = WikiBlockType::kListItem;
    return true;
  }
  return false;
}

// Builds the block list while walking the document.
struct Builder {
  WikiBlock* blocks = nullptr;
  uint32_t block_count = 0;
  bool truncated = false;

  TextBuffer text;
  WikiBlock current;
  WikiLink links[kMaxLinksPerBlock] = {};
  uint32_t link_count = 0;
};

/**
 * @brief Trims trailing whitespace from a buffer.
 * @param buffer Buffer to trim.
 */
void TrimTrailing(TextBuffer* buffer) {
  while (buffer->length > 0) {
    const char last = buffer->data[buffer->length - 1];
    if (last != ' ' && last != '\n' && last != '\t') {
      break;
    }
    --buffer->length;
    buffer->data[buffer->length] = '\0';
  }
}

/**
 * @brief Emits the pending text as a block and resets the accumulator.
 * @param builder Builder state.
 * @param type Type of the block being closed.
 */
void FlushBlock(Builder* builder, WikiBlockType type) {
  TrimTrailing(&builder->text);
  if (builder->text.length == 0) {
    builder->text.characters = 0;
    builder->link_count = 0;
    return;
  }
  if (builder->block_count >= kMaxBlocks) {
    builder->truncated = true;
    builder->text.length = 0;
    builder->text.characters = 0;
    builder->link_count = 0;
    return;
  }

  WikiBlock& block = builder->blocks[builder->block_count];
  block.type = type;
  block.text = builder->text.data;
  block.link_count = builder->link_count;
  block.links = nullptr;
  if (builder->link_count > 0) {
    const size_t bytes = builder->link_count * sizeof(WikiLink);
    block.links = static_cast<WikiLink*>(
        heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (block.links != nullptr) {
      std::memcpy(block.links, builder->links, bytes);
    } else {
      block.link_count = 0;
    }
  }
  ++builder->block_count;

  builder->text = TextBuffer();
  builder->link_count = 0;
}

}  // namespace

bool ParseWikiHtml(const char* html, size_t length, WikiDocument* document) {
  if (html == nullptr || document == nullptr || length == 0) {
    return false;
  }
  *document = WikiDocument();

  Builder builder;
  builder.blocks = static_cast<WikiBlock*>(heap_caps_calloc(
      kMaxBlocks, sizeof(WikiBlock), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (builder.blocks == nullptr) {
    return false;
  }

  WikiBlockType pending_type = WikiBlockType::kParagraph;
  int discard_depth = 0;
  char discard_tag[16] = {};
  bool in_link = false;
  uint32_t link_start = 0;
  char link_href[kWikiHrefMaxLength] = {};
  bool last_was_space = true;

  size_t position = 0;
  while (position < length) {
    const char character = html[position];

    if (character == '<') {
      const size_t tag_start = position + 1;
      size_t cursor = tag_start;
      while (cursor < length && html[cursor] != '>') {
        ++cursor;
      }
      if (cursor >= length) {
        break;
      }
      const bool closing = tag_start < length && html[tag_start] == '/';
      const size_t name_start = closing ? tag_start + 1 : tag_start;
      size_t name_end = name_start;
      while (name_end < cursor && html[name_end] != ' ' &&
          html[name_end] != '\t' && html[name_end] != '\n' &&
          html[name_end] != '/') {
        ++name_end;
      }
      const char* name = html + name_start;
      const size_t name_length = name_end - name_start;

      if (discard_depth > 0) {
        // Only the tag that opened the discarded region can close it.
        if (closing && name_length == std::strlen(discard_tag) &&
            strncasecmp(name, discard_tag, name_length) == 0) {
          --discard_depth;
        } else if (!closing && name_length == std::strlen(discard_tag) &&
            strncasecmp(name, discard_tag, name_length) == 0) {
          ++discard_depth;
        }
        position = cursor + 1;
        continue;
      }

      if (!closing && IsDiscardedTag(name, name_length)) {
        // Self-closing forms carry no content to skip.
        if (html[cursor - 1] != '/') {
          discard_depth = 1;
          const size_t copy = std::min(name_length, sizeof(discard_tag) - 1);
          std::memcpy(discard_tag, name, copy);
          discard_tag[copy] = '\0';
        }
        position = cursor + 1;
        continue;
      }

      WikiBlockType tag_type = WikiBlockType::kParagraph;
      if (BlockTypeForTag(name, name_length, &tag_type)) {
        FlushBlock(&builder, closing ? pending_type : pending_type);
        pending_type = closing ? WikiBlockType::kParagraph : tag_type;
        last_was_space = true;
        position = cursor + 1;
        continue;
      }
      if (IsBlockTag(name, name_length)) {
        FlushBlock(&builder, pending_type);
        pending_type = WikiBlockType::kParagraph;
        last_was_space = true;
        position = cursor + 1;
        continue;
      }

      if (name_length == 1 && (name[0] == 'a' || name[0] == 'A')) {
        if (closing) {
          if (in_link) {
            AppendMarkup(&builder.text, kRecolorClose, 1);
            if (builder.link_count < kMaxLinksPerBlock &&
                builder.text.characters > link_start) {
              WikiLink& link = builder.links[builder.link_count++];
              link.start_char = link_start;
              link.end_char = builder.text.characters;
              std::snprintf(link.href, sizeof(link.href), "%s", link_href);
            }
            in_link = false;
          }
        } else {
          // Pull the href out of the attribute list.
          link_href[0] = '\0';
          const char* attributes = html + name_end;
          const size_t attribute_length = cursor - name_end;
          for (size_t index = 0; index + 5 < attribute_length; ++index) {
            if (strncasecmp(attributes + index, "href=", 5) != 0) {
              continue;
            }
            size_t value = index + 5;
            char quote = '\0';
            if (value < attribute_length &&
                (attributes[value] == '"' || attributes[value] == '\'')) {
              quote = attributes[value];
              ++value;
            }
            size_t end = value;
            while (end < attribute_length &&
                (quote != '\0' ? attributes[end] != quote
                               : attributes[end] != ' ')) {
              ++end;
            }
            const size_t copy =
                std::min(end - value, sizeof(link_href) - 1);
            std::memcpy(link_href, attributes + value, copy);
            link_href[copy] = '\0';
            break;
          }
          if (link_href[0] != '\0') {
            AppendMarkup(&builder.text, kLinkColorOpen,
                std::strlen(kLinkColorOpen));
            link_start = builder.text.characters;
            in_link = true;
          }
        }
        position = cursor + 1;
        continue;
      }

      // Every other tag is inline decoration and simply disappears.
      position = cursor + 1;
      continue;
    }

    if (discard_depth > 0) {
      ++position;
      continue;
    }

    if (character == '&') {
      size_t cursor = position + 1;
      while (cursor < length && cursor - position < 12 &&
          html[cursor] != ';' && html[cursor] != '<' && html[cursor] != ' ') {
        ++cursor;
      }
      if (cursor < length && html[cursor] == ';') {
        const char* name = html + position + 1;
        const size_t name_length = cursor - position - 1;
        uint32_t code_point = 0;
        if (name_length > 1 && name[0] == '#') {
          code_point = (name[1] == 'x' || name[1] == 'X')
              ? static_cast<uint32_t>(strtoul(name + 2, nullptr, 16))
              : static_cast<uint32_t>(strtoul(name + 1, nullptr, 10));
        } else {
          code_point = NamedEntity(name, name_length);
        }
        if (code_point != 0) {
          AppendCodePoint(&builder.text, code_point);
          last_was_space = code_point == ' ';
          position = cursor + 1;
          continue;
        }
      }
    }

    if (character == ' ' || character == '\n' || character == '\t' ||
        character == '\r') {
      if (!last_was_space && builder.text.length > 0) {
        AppendSourceByte(&builder.text, ' ');
        last_was_space = true;
      }
      ++position;
      continue;
    }

    // A recolor marker in the source would be read as markup, so it is escaped.
    if (character == '#') {
      AppendSourceByte(&builder.text, '#');
      AppendMarkup(&builder.text, "#", 1);
      last_was_space = false;
      ++position;
      continue;
    }

    AppendSourceByte(&builder.text, character);
    last_was_space = false;
    ++position;
  }

  if (in_link) {
    AppendMarkup(&builder.text, kRecolorClose, 1);
  }
  FlushBlock(&builder, pending_type);

  if (builder.block_count == 0) {
    heap_caps_free(builder.blocks);
    heap_caps_free(builder.text.data);
    return false;
  }

  document->blocks = builder.blocks;
  document->block_count = builder.block_count;
  document->truncated = builder.truncated;
  return true;
}

void FreeWikiDocument(WikiDocument* document) {
  if (document == nullptr || document->blocks == nullptr) {
    return;
  }
  for (uint32_t index = 0; index < document->block_count; ++index) {
    heap_caps_free(document->blocks[index].text);
    heap_caps_free(document->blocks[index].links);
  }
  heap_caps_free(document->blocks);
  *document = WikiDocument();
}

bool ResolveWikiHref(const char* href, char* out, size_t out_size) {
  if (href == nullptr || out == nullptr || out_size == 0) {
    return false;
  }
  // Anything with a scheme points off the device and cannot be followed.
  if (std::strstr(href, "://") != nullptr || href[0] == '#' ||
      std::strncmp(href, "mailto:", 7) == 0) {
    return false;
  }

  const char* cursor = href;
  while (std::strncmp(cursor, "../", 3) == 0) {
    cursor += 3;
  }
  // ZIM 6 keeps content in namespace C; older links carry an A/ prefix.
  if (std::strncmp(cursor, "A/", 2) == 0 || std::strncmp(cursor, "C/", 2) == 0) {
    cursor += 2;
  }
  if (*cursor == '\0') {
    return false;
  }

  // Fragments and query strings are not part of the entry url.
  size_t length = 0;
  while (cursor[length] != '\0' && cursor[length] != '#' &&
      cursor[length] != '?' && length + 1 < out_size) {
    ++length;
  }
  std::memcpy(out, cursor, length);
  out[length] = '\0';

  // Percent decoding: hrefs are url encoded but entry urls are not.
  size_t read = 0;
  size_t write = 0;
  while (read < length) {
    if (out[read] == '%' && read + 2 < length) {
      char hex[3] = {out[read + 1], out[read + 2], '\0'};
      char* parse_end = nullptr;
      const long value = strtol(hex, &parse_end, 16);
      if (parse_end != nullptr && *parse_end == '\0') {
        out[write++] = static_cast<char>(value);
        read += 3;
        continue;
      }
    }
    out[write++] = out[read++];
  }
  out[write] = '\0';
  return write > 0;
}

}  // namespace lilygo_box::survival
