#include "EpubDictionaryActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstring>
#include <iterator>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int PANEL_MARGIN = 16;
constexpr int PANEL_PADDING = 18;
// The dictionary cursor is the compact counterpart of a selected menu row:
// the same airy rounded outline and sparse stipple, scaled down around a word.
constexpr int WORD_SELECTION_PAD_X = 6;
constexpr int WORD_SELECTION_PAD_Y = 3;
constexpr int WORD_SELECTION_SCREEN_INSET = 2;

std::string stripHtml(const std::string& input) {
  std::string out;
  out.reserve(input.size());
  bool inTag = false;
  bool lastSpace = false;
  for (size_t i = 0; i < input.size(); ++i) {
    const char c = input[i];
    if (c == '<') {
      const size_t close = input.find('>', i + 1);
      if (close != std::string::npos) {
        std::string tag = input.substr(i + 1, close - i - 1);
        std::transform(tag.begin(), tag.end(), tag.begin(), [](unsigned char ch) { return std::tolower(ch); });
        if (tag == "br" || tag == "br/" || tag == "/p" || tag == "/div" || tag == "/li" ||
            (!tag.empty() && tag.front() == 'h') || tag == "li") {
          if (!out.empty() && out.back() != '\n') out.push_back('\n');
          lastSpace = false;
        }
        i = close;
        continue;
      }
      inTag = true;
      continue;
    }
    if (inTag) {
      if (c == '>') inTag = false;
      continue;
    }
    if (c == '&') {
      const size_t semi = input.find(';', i + 1);
      if (semi != std::string::npos && semi - i <= 10) {
        const std::string entity = input.substr(i + 1, semi - i - 1);
        if (entity == "amp")
          out.push_back('&');
        else if (entity == "lt")
          out.push_back('<');
        else if (entity == "gt")
          out.push_back('>');
        else if (entity == "quot")
          out.push_back('"');
        else if (entity == "apos" || entity == "#39")
          out.push_back('\'');
        else if (entity == "nbsp")
          out.push_back(' ');
        else {
          out.append(input, i, semi - i + 1);
        }
        i = semi;
        lastSpace = false;
        continue;
      }
    }
    if (c == '\0' || c == '\r' || c == '\t') {
      if (!lastSpace) out.push_back(' ');
      lastSpace = true;
    } else if (c == '\n') {
      if (!out.empty() && out.back() != '\n') out.push_back('\n');
      lastSpace = false;
    } else if (c == ' ') {
      if (!lastSpace && !out.empty() && out.back() != '\n') out.push_back(' ');
      lastSpace = true;
    } else {
      out.push_back(c);
      lastSpace = false;
    }
  }
  return out;
}

std::string cleanLookupWord(const std::string& value) {
  size_t begin = 0;
  size_t end = value.size();
  const auto keep = [](unsigned char c) { return c >= 0x80 || std::isalnum(c) || c == '\'' || c == '-'; };
  while (begin < end && !keep(static_cast<unsigned char>(value[begin]))) ++begin;
  while (end > begin && !keep(static_cast<unsigned char>(value[end - 1]))) --end;
  return value.substr(begin, end - begin);
}
}  // namespace

EpubDictionaryActivity::EpubDictionaryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               std::unique_ptr<Page> page, const int fontId, const int marginLeft,
                                               const int marginTop)
    : Activity("EpubDictionary", renderer, mappedInput),
      page_(std::move(page)),
      fontId_(fontId),
      marginLeft_(marginLeft),
      marginTop_(marginTop) {}

void EpubDictionaryActivity::onEnter() {
  Activity::onEnter();
  if (page_) buildPageWordIndex(*page_, renderer, fontId_, marginLeft_, marginTop_, words_, &lineStarts_);
  requestUpdate();
}

void EpubDictionaryActivity::onExit() {
  dictionary_.close();
  for (auto& chunk : captureChunks_) chunk.reset();
  captureChunkCount_ = 0;
  captureValid_ = false;
  Activity::onExit();
}

void EpubDictionaryActivity::moveWord(const int delta) {
  if (words_.empty()) return;
  if (delta < 0 && focus_ > 0)
    --focus_;
  else if (delta > 0 && focus_ + 1 < words_.size())
    ++focus_;
}

void EpubDictionaryActivity::moveLine(const int delta) {
  if (lineStarts_.empty() || words_.empty()) return;
  size_t currentLine = 0;
  for (size_t i = 0; i < lineStarts_.size(); ++i) {
    const size_t end = i + 1 < lineStarts_.size() ? lineStarts_[i + 1] : words_.size();
    if (focus_ >= lineStarts_[i] && focus_ < end) {
      currentLine = i;
      break;
    }
  }
  const size_t targetLine =
      delta < 0 ? (currentLine == 0 ? 0 : currentLine - 1) : std::min(currentLine + 1, lineStarts_.size() - 1);
  if (targetLine == currentLine) return;
  const int targetX = words_[focus_].screenX + words_[focus_].screenW / 2;
  const size_t begin = lineStarts_[targetLine];
  const size_t end = targetLine + 1 < lineStarts_.size() ? lineStarts_[targetLine + 1] : words_.size();
  size_t closest = begin;
  int closestDistance = INT_MAX;
  for (size_t i = begin; i < end; ++i) {
    const int center = words_[i].screenX + words_[i].screenW / 2;
    const int distance = std::abs(center - targetX);
    if (distance < closestDistance) {
      closestDistance = distance;
      closest = i;
    }
  }
  focus_ = closest;
}

void EpubDictionaryActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (showingDefinition_) {
      showingDefinition_ = false;
      definition_.clear();
      definitionLines_.clear();
      scrollLine_ = 0;
      requestUpdate();
    } else {
      finish();
    }
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (!showingDefinition_) performLookup();
    return;
  }
  if (showingDefinition_) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      scrollLine_ = scrollLine_ > 3 ? scrollLine_ - 3 : 0;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      if (scrollLine_ + 1 < definitionLines_.size())
        scrollLine_ = std::min(scrollLine_ + 3, definitionLines_.size() - 1);
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    moveWord(-1);
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    moveWord(1);
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    moveLine(-1);
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    moveLine(1);
    requestUpdate();
  }
}

void EpubDictionaryActivity::performLookup() {
  if (words_.empty() || focus_ >= words_.size()) return;
  lookupWord_ = cleanLookupWord(words_[focus_].text);
  definition_.clear();
  bool truncated = false;
  if (SETTINGS.dictionaryFolder[0] == '\0') {
    definition_ = tr(STR_DICTIONARY_NOT_SELECTED);
  } else {
    GUI.drawPopup(renderer, tr(STR_LOOKING_UP));
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    if (!dictionaryOpenAttempted_) {
      dictionaryOpenAttempted_ = true;
      lastDictionaryProgress_ = -1;
      dictionary_.setProgressCallback(dictionaryProgress, this);
      dictionary_.open(std::string("/dictionaries/") + SETTINGS.dictionaryFolder);
    }
    if (!dictionary_.isOpen())
      definition_ = tr(STR_DICTIONARY_OPEN_FAILED);
    else if (!dictionary_.lookup(lookupWord_, definition_, &truncated))
      definition_ = tr(STR_DEFINITION_NOT_FOUND);
  }
  if (truncated) {
    while (!definition_.empty() && (static_cast<unsigned char>(definition_.back()) & 0xc0) == 0x80)
      definition_.pop_back();
    if (!definition_.empty() && static_cast<unsigned char>(definition_.back()) >= 0xc0) definition_.pop_back();
    definition_ += " \xE2\x80\xA6";
  }
  buildDefinitionLines();
  showingDefinition_ = true;
  requestUpdate();
}

void EpubDictionaryActivity::dictionaryProgress(void* context, const uint32_t completedBytes,
                                                const uint32_t totalBytes) {
  auto* self = static_cast<EpubDictionaryActivity*>(context);
  if (!self || totalBytes == 0) return;
  const int percent = static_cast<int>((static_cast<uint64_t>(completedBytes) * 100u) / totalBytes);
  if (percent == self->lastDictionaryProgress_) return;
  self->lastDictionaryProgress_ = percent;
  char message[64];
  snprintf(message, sizeof(message), "%s %d%%", tr(STR_INDEXING), percent);
  GUI.drawPopup(self->renderer, message);
  self->renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void EpubDictionaryActivity::buildDefinitionLines() {
  definitionLines_.clear();
  const std::string plain = stripHtml(definition_);
  const int maxWidth = std::max(40, renderer.getScreenWidth() - (PANEL_MARGIN + PANEL_PADDING) * 2);
  size_t start = 0;
  while (start <= plain.size() && definitionLines_.size() < 256) {
    const size_t end = plain.find('\n', start);
    const std::string paragraph = plain.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!paragraph.empty()) {
      auto wrapped = renderer.wrappedText(UI_10_FONT_ID, paragraph.c_str(), maxWidth, 64);
      definitionLines_.insert(definitionLines_.end(), std::make_move_iterator(wrapped.begin()),
                              std::make_move_iterator(wrapped.end()));
    }
    if (end == std::string::npos) break;
    if (!definitionLines_.empty() && definitionLines_.back() != "") definitionLines_.push_back("");
    start = end + 1;
  }
  if (definitionLines_.empty()) definitionLines_.push_back(tr(STR_DEFINITION_NOT_FOUND));
  scrollLine_ = 0;
}

void EpubDictionaryActivity::captureBaseFrame() {
  for (auto& chunk : captureChunks_) chunk.reset();
  captureChunkCount_ = 0;
  captureValid_ = false;
  uint8_t* frame = renderer.getFrameBuffer();
  const size_t bytes = renderer.getBufferSize();
  if (!frame || bytes == 0) return;
  const size_t count = (bytes + CAPTURE_CHUNK_BYTES - 1) / CAPTURE_CHUNK_BYTES;
  if (count > captureChunks_.size()) return;
  for (size_t i = 0; i < count; ++i) {
    const size_t offset = i * CAPTURE_CHUNK_BYTES;
    const size_t chunkBytes = std::min(CAPTURE_CHUNK_BYTES, bytes - offset);
    auto chunk = makeUniqueNoThrow<uint8_t[]>(chunkBytes);
    if (!chunk) {
      for (auto& allocated : captureChunks_) allocated.reset();
      captureChunkCount_ = 0;
      return;
    }
    memcpy(chunk.get(), frame + offset, chunkBytes);
    captureChunks_[i] = std::move(chunk);
    captureChunkCount_ = i + 1;
  }
  captureBytes_ = bytes;
  captureValid_ = true;
}

bool EpubDictionaryActivity::restoreBaseFrame() {
  uint8_t* frame = renderer.getFrameBuffer();
  if (!captureValid_ || !frame || captureBytes_ != renderer.getBufferSize()) return false;
  for (size_t i = 0; i < captureChunkCount_; ++i) {
    const size_t offset = i * CAPTURE_CHUNK_BYTES;
    memcpy(frame + offset, captureChunks_[i].get(), std::min(CAPTURE_CHUNK_BYTES, captureBytes_ - offset));
  }
  return true;
}

void EpubDictionaryActivity::drawHighlight() const {
  if (words_.empty() || focus_ >= words_.size()) return;
  const PageWordHit& word = words_[focus_];

  const int left = std::max(WORD_SELECTION_SCREEN_INSET, word.screenX - WORD_SELECTION_PAD_X);
  const int top = std::max(WORD_SELECTION_SCREEN_INSET, word.screenY - WORD_SELECTION_PAD_Y);
  const int right = std::min(renderer.getScreenWidth() - WORD_SELECTION_SCREEN_INSET,
                             word.screenX + word.screenW + WORD_SELECTION_PAD_X);
  const int bottom = std::min(renderer.getScreenHeight() - WORD_SELECTION_SCREEN_INSET,
                              word.screenY + word.screenH + WORD_SELECTION_PAD_Y);

  // Reuse the theme primitive instead of inventing a reader-only rectangle:
  // one-pixel rounded outline plus the light 1/16 stipple used throughout the
  // InkPoint X menus. Redraw the glyphs last so the selected word stays crisp.
  GUI.drawSelection(renderer, Rect{left, top, std::max(1, right - left), std::max(1, bottom - top)});
  renderer.drawText(word.fontId, word.screenX, word.screenY, word.text.c_str(), true, word.style);
}

void EpubDictionaryActivity::drawDefinitionPanel() {
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int panelX = PANEL_MARGIN;
  const int panelY = PANEL_MARGIN;
  const int panelW = screenW - PANEL_MARGIN * 2;
  const int panelH = screenH - PANEL_MARGIN * 2 - 46;
  renderer.fillRect(panelX, panelY, panelW, panelH, false);
  renderer.drawRect(panelX, panelY, panelW, panelH, 2, true);
  renderer.drawText(UI_14_FONT_ID, panelX + PANEL_PADDING, panelY + PANEL_PADDING, lookupWord_.c_str(), true,
                    EpdFontFamily::BOLD);
  const int separatorY = panelY + PANEL_PADDING + renderer.getLineHeight(UI_14_FONT_ID) + 5;
  renderer.drawLine(panelX + PANEL_PADDING, separatorY, panelX + panelW - PANEL_PADDING, separatorY);
  int y = separatorY + 8;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int bottom = panelY + panelH - PANEL_PADDING;
  for (size_t i = scrollLine_; i < definitionLines_.size() && y + lineHeight <= bottom; ++i) {
    renderer.drawText(UI_10_FONT_ID, panelX + PANEL_PADDING, y, definitionLines_[i].c_str());
    y += lineHeight;
  }
}

void EpubDictionaryActivity::render(RenderLock&&) {
  if (!page_ || words_.empty()) {
    renderer.clearScreen();
    GUI.drawReaderMessage(renderer, tr(STR_NO_ENTRIES));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
  if (!restoreBaseFrame()) {
    renderer.clearScreen();
    auto* cache = renderer.getFontCacheManager();
    auto scope = cache->createPrewarmScope();
    page_->render(renderer, fontId_, marginLeft_, marginTop_);
    scope.endScanAndPrewarm();
    page_->render(renderer, fontId_, marginLeft_, marginTop_);
    if (SETTINGS.readerInvertColors) renderer.invertScreen();
    captureBaseFrame();
  }
  if (showingDefinition_)
    drawDefinitionPanel();
  else
    drawHighlight();
  const auto labels =
      mappedInput.mapLabels(showingDefinition_ ? tr(STR_CLOSE) : tr(STR_BACK),
                            showingDefinition_ ? "" : tr(STR_LOOK_UP), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
