#include "PageWordIndex.h"

#include <GfxRenderer.h>

#include <algorithm>

void buildPageWordIndex(const Page& page, const GfxRenderer& renderer, const int bodyFontId, const int marginLeft,
                        const int marginTop, std::vector<PageWordHit>& out, std::vector<size_t>* lineStartsOut) {
  out.clear();
  if (lineStartsOut) lineStartsOut->clear();
  for (size_t elementIndex = 0; elementIndex < page.elements.size(); ++elementIndex) {
    const auto& element = page.elements[elementIndex];
    if (!element || element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    if (!line->getBlock()) continue;
    const size_t lineStart = out.size();
    const int baseX = line->xPos + marginLeft;
    const int baseY = line->yPos + marginTop;
    line->getBlock()->forEachWord([&](const size_t wordIndex, const std::string& text, const int16_t relativeX,
                                      const EpdFontFamily::Style style) {
      PageWordHit hit;
      hit.elementIndex = elementIndex;
      hit.wordIndexInElement = wordIndex;
      hit.screenX = baseX + relativeX;
      hit.screenY = baseY;
      hit.screenW = std::max(1, renderer.getTextWidth(bodyFontId, text.c_str(), style));
      hit.screenH = renderer.getLineHeight(bodyFontId);
      hit.fontId = bodyFontId;
      hit.style = style;
      hit.text = text;
      out.push_back(std::move(hit));
    });
    // Blank PageLine elements must not create duplicate boundaries. Duplicate
    // starts form zero-length lines, which can make vertical dictionary
    // navigation select an index that does not belong to the target line.
    if (lineStartsOut && out.size() > lineStart) lineStartsOut->push_back(lineStart);
  }
}
