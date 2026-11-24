#pragma once

#include <algorithm>
#include <cstdint>

#include "Epub/css/CssStyle.h"

/**
 * BlockStyle - Block-level styling properties
 */
struct BlockStyle {
  // Upper bound (in em) for any single side's horizontal margin or padding.
  // Some EPUBs apply huge em-based insets to chapter-opener classes; without a
  // cap, effectiveWidth collapses to 1-2 words per line and justification dumps
  // the remaining space into a single gap.
  static constexpr float MAX_HORIZONTAL_INSET_EM = 2.0f;

  CssTextAlign alignment = CssTextAlign::Justify;

  // Extra pixels between glyphs during layout and drawing.
  int8_t characterSpacing = 0;
  // Spacing (in pixels)
  int16_t marginTop = 0;
  int16_t marginBottom = 0;
  int16_t marginLeft = 0;
  int16_t marginRight = 0;
  int16_t paddingTop = 0;     // treated same as margin for rendering
  int16_t paddingBottom = 0;  // treated same as margin for rendering
  int16_t paddingLeft = 0;    // treated same as margin for rendering
  int16_t paddingRight = 0;   // treated same as margin for rendering
  int16_t textIndent = 0;
  bool textIndentDefined = false;  // true if text-indent was explicitly set in CSS
  bool textAlignDefined = false;   // true if text-align was explicitly set in CSS
  bool isRtl = false;              // true if resolved direction is RTL
  bool directionDefined = false;   // true if direction was explicitly set in CSS/HTML

  // CSS font-size resolved for this block (layout milestone M2). `fontScale` is the
  // multiplier over the reader's chosen size (headings and `font-size` rules); it is
  // inherited by child blocks that do not set their own. `fontId` is the renderer face
  // chosen for that scale: 0 means "the section's reader font", and it is the only
  // field of this pair that a laid-out line needs at draw time, so TextBlock
  // serialises it (4 B/line) while fontScale stays layout-time only.
  float fontScale = 1.0f;
  bool fontScaleDefined = false;
  int32_t fontId = 0;

  // Forced page breaks and heading flow (layout milestone M3). Layout-time only: a
  // break is consumed once, when the block places its first content, and a cached
  // page already reflects it, so none of these are serialized.
  bool pageBreakBefore = false;
  bool pageBreakAfter = false;
  // A heading waits for the first line of the block that follows it.
  bool keepWithNext = false;

  // Box decoration (layout milestone M4b). Edge widths are already collapsed to 1..4 px
  // by the CSS parser and `shadeLevel` is the background ink (0 = none, 15 = black).
  // Layout-time only: the box is an element of the page that was laid out, so a cached
  // page carries it and nothing here is serialized.
  uint8_t borderTop = 0;
  uint8_t borderRight = 0;
  uint8_t borderBottom = 0;
  uint8_t borderLeft = 0;
  uint8_t shadeLevel = 0;
  bool hasBox = false;  // derived from the five fields above

  // Set when this block was created by a <br> element. Used by startNewTextBlock to inject
  // a full line-height gap when the <br> block stays empty (section-break use case).
  // NOT propagated through getCombinedBlockStyle so it can't leak into sibling blocks.
  bool fromBrElement = false;

  // Combined insets (margin + padding)
  [[nodiscard]] int16_t leftInset() const { return marginLeft + paddingLeft; }
  [[nodiscard]] int16_t rightInset() const { return marginRight + paddingRight; }
  [[nodiscard]] int16_t totalHorizontalInset() const { return leftInset() + rightInset(); }
  [[nodiscard]] int16_t topInset() const { return marginTop + paddingTop; }
  [[nodiscard]] int16_t bottomInset() const { return marginBottom + paddingBottom; }

  // Return a copy with bottom margins/padding zeroed out.
  [[nodiscard]] BlockStyle withoutBottom() const {
    BlockStyle result = *this;
    result.marginBottom = 0;
    result.paddingBottom = 0;
    return result;
  }

  // Return a copy with top margins/padding zeroed out.
  [[nodiscard]] BlockStyle withoutTop() const {
    BlockStyle result = *this;
    result.marginTop = 0;
    result.paddingTop = 0;
    return result;
  }

  // Replace the box decoration with `source`'s. Used when an empty block is reused for
  // the container being entered: the element that just closed must not hand its border
  // or shade to a following sibling.
  [[nodiscard]] BlockStyle withBoxFrom(const BlockStyle& source) const {
    BlockStyle result = *this;
    result.borderTop = source.borderTop;
    result.borderRight = source.borderRight;
    result.borderBottom = source.borderBottom;
    result.borderLeft = source.borderLeft;
    result.shadeLevel = source.shadeLevel;
    result.hasBox = source.hasBox;
    return result;
  }

  // Return a copy with bottom margins/padding collapsed (max) with the source's.
  // Uses CSS margin collapsing: adjacent parent-child margins resolve to the larger value.
  [[nodiscard]] BlockStyle addBottom(const BlockStyle& source) const {
    BlockStyle result = *this;
    result.marginBottom = std::max(marginBottom, source.marginBottom);
    result.paddingBottom = static_cast<int16_t>(paddingBottom + source.paddingBottom);
    return result;
  }

  enum class CombineAxis : uint8_t {
    Horizontal = 1,  // margins left/right, padding left/right, text-align, text-indent
    Vertical = 2,    // margins top/bottom, padding top/bottom
  };

  // Combine this style's properties with a child style along the specified axis.
  // Properties on the other axis are kept from the child unchanged.
  [[nodiscard]] BlockStyle getCombinedBlockStyle(const BlockStyle& child, CombineAxis axis) const {
    BlockStyle result = child;

    if (axis == CombineAxis::Horizontal) {
      result.marginLeft = static_cast<int16_t>(child.marginLeft + marginLeft);
      result.marginRight = static_cast<int16_t>(child.marginRight + marginRight);
      result.paddingLeft = static_cast<int16_t>(child.paddingLeft + paddingLeft);
      result.paddingRight = static_cast<int16_t>(child.paddingRight + paddingRight);
      if (!child.textIndentDefined && textIndentDefined) {
        result.textIndent = textIndent;
        result.textIndentDefined = true;
      }
      if (!child.textAlignDefined && textAlignDefined) {
        result.alignment = alignment;
        result.textAlignDefined = true;
      }
    } else {
      result.marginTop = std::max(child.marginTop, marginTop);
      result.marginBottom = std::max(child.marginBottom, marginBottom);
      result.paddingTop = static_cast<int16_t>(child.paddingTop + paddingTop);
      result.paddingBottom = static_cast<int16_t>(child.paddingBottom + paddingBottom);
    }

    // Direction is not axis-specific. Inherit from parent when child doesn't define it.
    if (!child.directionDefined && directionDefined) {
      result.isRtl = isRtl;
      result.directionDefined = true;
    }

    // font-size also inherits: a `<div class="title">` sizes its `<h1>` unless the
    // heading sets its own scale. `fontId` is never inherited -- it is resolved per
    // block at layout time.
    if (!child.fontScaleDefined && fontScaleDefined) {
      result.fontScale = fontScale;
      result.fontScaleDefined = true;
    }

    // CSS never inherits borders, but this model flattens ancestors into the block that
    // places text, so a bordered <div> hands its box to the blocks inside it. A child
    // that declares its own box keeps it: the parser emits one box per block, and a
    // nested second outline is not modelled yet.
    if (!child.hasBox && hasBox) {
      result.borderTop = borderTop;
      result.borderRight = borderRight;
      result.borderBottom = borderBottom;
      result.borderLeft = borderLeft;
      result.shadeLevel = shadeLevel;
      result.hasBox = true;
    }

    // A forced break belongs to the container's first block: an empty block that
    // already carries it (deposited when the container opened) keeps it through the
    // vertical merge. The style stack clears it on push, so siblings never re-break.
    result.pageBreakBefore = child.pageBreakBefore || pageBreakBefore;

    // fromBrElement is consumed by startNewTextBlock when an empty <br> block
    // is merged with the following paragraph; never propagate it further.
    result.fromBrElement = false;
    return result;
  }

  // Create a BlockStyle from CSS style properties, resolving CssLength values to pixels
  // emSize is the current font line height, used for em/rem unit conversion
  // paragraphAlignment is the user's paragraphAlignment setting preference
  static BlockStyle fromCssStyle(const CssStyle& cssStyle, const float emSize, const CssTextAlign paragraphAlignment,
                                 const uint16_t viewportWidth = 0) {
    BlockStyle blockStyle;
    const float vw = viewportWidth;
    const auto maxHorizontalInsetPx = static_cast<int16_t>(emSize * MAX_HORIZONTAL_INSET_EM);
    // Resolve all CssLength values to pixels using the current font's em size and viewport width
    blockStyle.marginTop = cssStyle.marginTop.toPixelsInt16(emSize, vw);
    blockStyle.marginBottom = cssStyle.marginBottom.toPixelsInt16(emSize, vw);
    blockStyle.marginLeft = std::min(cssStyle.marginLeft.toPixelsInt16(emSize, vw), maxHorizontalInsetPx);
    blockStyle.marginRight = std::min(cssStyle.marginRight.toPixelsInt16(emSize, vw), maxHorizontalInsetPx);

    blockStyle.paddingTop = cssStyle.paddingTop.toPixelsInt16(emSize, vw);
    blockStyle.paddingBottom = cssStyle.paddingBottom.toPixelsInt16(emSize, vw);
    blockStyle.paddingLeft = std::min(cssStyle.paddingLeft.toPixelsInt16(emSize, vw), maxHorizontalInsetPx);
    blockStyle.paddingRight = std::min(cssStyle.paddingRight.toPixelsInt16(emSize, vw), maxHorizontalInsetPx);

    // For textIndent: if it's a percentage we can't resolve (no viewport width),
    // leave textIndentDefined=false so the space-width fallback in resolveFirstLineIndent() is used
    if (cssStyle.hasTextIndent() && cssStyle.textIndent.isResolvable(vw)) {
      blockStyle.textIndent = cssStyle.textIndent.toPixelsInt16(emSize, vw);
      blockStyle.textIndentDefined = true;
    }
    blockStyle.textAlignDefined = cssStyle.hasTextAlign();
    // User setting overrides CSS, unless "Book's Style" alignment setting is selected
    if (paragraphAlignment == CssTextAlign::None) {
      blockStyle.alignment = blockStyle.textAlignDefined ? cssStyle.textAlign : CssTextAlign::Justify;
    } else {
      blockStyle.alignment = paragraphAlignment;
    }
    // RTL direction from CSS/HTML
    if (cssStyle.hasDirection()) {
      blockStyle.isRtl = (cssStyle.direction == CssTextDirection::Rtl);
      blockStyle.directionDefined = true;
    }
    blockStyle.pageBreakBefore = cssStyle.pageBreakBefore;
    blockStyle.pageBreakAfter = cssStyle.pageBreakAfter;
    // Borders hug the content lane instead of widening the insets, so a book without
    // border/shade CSS lays out byte-identically to before M4b.
    blockStyle.borderTop = cssStyle.borderTop.widthPx;
    blockStyle.borderRight = cssStyle.borderRight.widthPx;
    blockStyle.borderBottom = cssStyle.borderBottom.widthPx;
    blockStyle.borderLeft = cssStyle.borderLeft.widthPx;
    blockStyle.shadeLevel = cssStyle.shaded ? cssStyle.shadeLevel : 0;
    blockStyle.hasBox = blockStyle.borderTop != 0 || blockStyle.borderRight != 0 || blockStyle.borderBottom != 0 ||
                        blockStyle.borderLeft != 0 || blockStyle.shadeLevel != 0;
    return blockStyle;
  }
};
