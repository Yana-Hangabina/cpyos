#pragma once
#include <HalStorage.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "FootnoteEntry.h"
#include "PageLink.h"
#include "blocks/ImageBlock.h"
#include "blocks/TextBlock.h"

enum PageElementTag : uint8_t {
  TAG_PageLine = 1,
  TAG_PageImage = 2,
  TAG_PageHorizontalRule = 3,
  TAG_PageDropCap = 4,
  TAG_PageBorderBox = 5,
};

// represents something that has been added to a page
class PageElement {
 public:
  int16_t xPos;
  int16_t yPos;
  explicit PageElement(const int16_t xPos, const int16_t yPos) : xPos(xPos), yPos(yPos) {}
  virtual ~PageElement() = default;
  virtual void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) = 0;
  virtual bool serialize(HalFile& file) = 0;
  virtual PageElementTag getTag() const = 0;  // Add type identification
};

// a line from a block element
class PageLine final : public PageElement {
  std::unique_ptr<TextBlock> block;
  // Set by Page::setVertical() when the reader is in vertical writing mode.
  bool verticalMode = false;
  int verticalWidth = 0;
  int verticalColW = 0;

 public:
  PageLine(std::unique_ptr<TextBlock> block, const int16_t xPos, const int16_t yPos)
      : PageElement(xPos, yPos), block(std::move(block)) {}
  void setVertical(const bool on, const int width, const int colWidth) {
    verticalMode = on;
    verticalWidth = width;
    verticalColW = colWidth;
  }
  const std::unique_ptr<TextBlock>& getBlock() const { return block; }
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) override;
  bool serialize(HalFile& file) override;
  PageElementTag getTag() const override { return TAG_PageLine; }
  static std::unique_ptr<PageLine> deserialize(HalFile& file);
};

// New PageImage class
class PageImage final : public PageElement {
  std::unique_ptr<ImageBlock> imageBlock;

 public:
  PageImage(std::unique_ptr<ImageBlock> block, const int16_t xPos, const int16_t yPos)
      : PageElement(xPos, yPos), imageBlock(std::move(block)) {}
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) override;
  void renderPlaceholder(GfxRenderer& renderer, int xOffset, int yOffset) const;
  bool serialize(HalFile& file) override;
  PageElementTag getTag() const override { return TAG_PageImage; }
  static std::unique_ptr<PageImage> deserialize(HalFile& file);
  ImageBlock& getImageBlock() { return *imageBlock; }
  const ImageBlock& getImageBlock() const { return *imageBlock; }
};

class PageHorizontalRule final : public PageElement {
  uint16_t width;
  uint8_t thickness;

 public:
  PageHorizontalRule(uint16_t width, uint8_t thickness, const int16_t xPos, const int16_t yPos)
      : PageElement(xPos, yPos), width(width), thickness(thickness) {}

  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) override;
  bool serialize(HalFile& file) override;
  PageElementTag getTag() const override { return TAG_PageHorizontalRule; }
  static std::unique_ptr<PageHorizontalRule> deserialize(HalFile& file);
};

// A decorated block: an optional dithered background plus up to four solid
// edges. The chapter parser emits one when a block carries border or shade CSS;
// the box renders behind its lines, so Page draws backgrounds before elements
// and edges afterwards.
class PageBorderBox final : public PageElement {
  uint16_t width_;
  uint16_t height_;
  uint8_t shadeLevel_;  // ink 0..15, 0 = no background
  uint8_t borderTop_;
  uint8_t borderRight_;
  uint8_t borderBottom_;
  uint8_t borderLeft_;

 public:
  PageBorderBox(const int16_t xPos, const int16_t yPos, const uint16_t width, const uint16_t height,
                const uint8_t shadeLevel, const uint8_t top, const uint8_t right, const uint8_t bottom,
                const uint8_t left)
      : PageElement(xPos, yPos),
        width_(width),
        height_(height),
        shadeLevel_(shadeLevel),
        borderTop_(top),
        borderRight_(right),
        borderBottom_(bottom),
        borderLeft_(left) {}

  void renderBackground(GfxRenderer& renderer, int xOffset, int yOffset) const;
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) override;
  bool serialize(HalFile& file) override;
  PageElementTag getTag() const override { return TAG_PageBorderBox; }
  static std::unique_ptr<PageBorderBox> deserialize(HalFile& file);

  // The chapter parser appends the box before its block's first line, so the height is
  // only known once that block (or the part of it that fits on this page) ends. A page
  // break passes 0 for `bottomEdge` to cut the closed side.
  void setBoxHeight(const uint16_t height, const uint8_t bottomEdge) {
    height_ = height;
    borderBottom_ = bottomEdge;
  }

  uint16_t boxWidth() const { return width_; }
  uint16_t boxHeight() const { return height_; }
  uint8_t shade() const { return shadeLevel_; }
  uint8_t borderWidthTop() const { return borderTop_; }
  uint8_t borderWidthRight() const { return borderRight_; }
  uint8_t borderWidthBottom() const { return borderBottom_; }
  uint8_t borderWidthLeft() const { return borderLeft_; }
};

// The first letter of a paragraph, drawn enlarged so lines two and three wrap
// around it (layout milestone M6). The glyph is painted by the renderer's top-anchored
// scaled codepoint path; the element only carries the character, its scale and the ink
// top of the first reserved line. Selection and word hit-testing skip it: the enlarged
// letter is not part of any line's word run, so it cannot be selected or looked up.
class PageDropCap final : public PageElement {
  uint32_t codepoint_;
  uint16_t scale256_;  // 256 = 1:1; the parser caps it at 3x
  uint16_t width_;
  uint16_t height_;
  int32_t faceId_;     // 0 = the section's reader face

 public:
  PageDropCap(const uint32_t codepoint, const uint16_t scale256, const uint16_t width, const uint16_t height,
              const int32_t faceId, const int16_t xPos, const int16_t yPos)
      : PageElement(xPos, yPos),
        codepoint_(codepoint),
        scale256_(scale256),
        width_(width),
        height_(height),
        faceId_(faceId) {}

  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) override;
  bool serialize(HalFile& file) override;
  PageElementTag getTag() const override { return TAG_PageDropCap; }
  static std::unique_ptr<PageDropCap> deserialize(HalFile& file);

  uint32_t codepoint() const { return codepoint_; }
  uint16_t scale() const { return scale256_; }
  uint16_t capWidth() const { return width_; }
  uint16_t capHeight() const { return height_; }
};

class Page {
 public:
  // the list of block index and line numbers on this page
  std::vector<std::unique_ptr<PageElement>> elements;
  FootnoteList footnotes;
  static constexpr uint16_t MAX_FOOTNOTES_PER_PAGE = FootnoteList::MAX_SIZE;
  PageLinkList links;
  static constexpr uint16_t MAX_LINKS_PER_PAGE = PageLinkList::MAX_SIZE;

  // Zero-based visible-codepoint offset where this page starts. Not part of the serialized page
  // body (it lives in the section's visible-offset LUT); Section::loadPage* fills it in from the
  // build LUT or the on-disk LUT while the page file is already open, so the reader can persist
  // progress without a second section-file open per page turn.
  uint32_t visibleTextOffset = 0;

  // Set by setVertical(). Bordered boxes have no column mapping yet, so they are
  // skipped in vertical writing rather than drawn in the wrong place.
  bool verticalWritingMode = false;

  void addFootnote(const char* number, const char* href);

  bool addLink(const char* href, int16_t x, int16_t y, int16_t width, int16_t height);

  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) const;
  // Vertical writing mode: laid-out lines become right-to-left columns. colWidth
  // carries the reader's line height (font line height x line compression) so the
  // column pitch matches the horizontal line pitch.
  void setVertical(bool on, int viewportWidth, int colWidth);
  void renderImages(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) const;
  void renderWithImagePlaceholders(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) const;
  void extractImagesNeedingDecode();
  bool warmImages(GfxRenderer& renderer, int xOffset, int yOffset, CancelCheck cancellation);
  void cacheImagesNeedingDecode(GfxRenderer& renderer, int xOffset, int yOffset);
  bool serialize(HalFile& file) const;
  static std::unique_ptr<Page> deserialize(HalFile& file, bool collectTouchLinks = true);

  // Check if page contains any images (used to force full refresh)
  bool hasImages() const {
    return std::any_of(elements.begin(), elements.end(),
                       [](const std::unique_ptr<PageElement>& el) { return el->getTag() == TAG_PageImage; });
  }

  bool hasImagesNeedingDecode() const {
    return std::any_of(elements.begin(), elements.end(), [](const std::unique_ptr<PageElement>& element) {
      return element->getTag() == TAG_PageImage &&
             static_cast<const PageImage&>(*element).getImageBlock().needsDecode();
    });
  }

  // Get bounding box of all images on the page (union of image rects)
  // Returns false if no images. Coordinates are relative to page origin.
  bool getImageBoundingBox(int16_t& outX, int16_t& outY, int16_t& outW, int16_t& outH) const {
    bool found = false;
    int16_t minX = INT16_MAX, minY = INT16_MAX, maxX = INT16_MIN, maxY = INT16_MIN;
    for (const auto& el : elements) {
      if (el->getTag() == TAG_PageImage) {
        const auto& img = static_cast<const PageImage&>(*el);
        int16_t x = img.xPos;
        int16_t y = img.yPos;
        int16_t right = x + img.getImageBlock().getWidth();
        int16_t bottom = y + img.getImageBlock().getHeight();
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, right);
        maxY = std::max(maxY, bottom);
        found = true;
      }
    }
    if (found) {
      outX = minX;
      outY = minY;
      outW = maxX - minX;
      outH = maxY - minY;
    }
    return found;
  }
};
