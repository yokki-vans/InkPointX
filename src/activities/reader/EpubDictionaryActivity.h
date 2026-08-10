#pragma once

#include <Epub/Page.h>
#include <Epub/PageWordIndex.h>

#include <memory>
#include <array>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "dictionary/StarDictLookup.h"

class EpubDictionaryActivity final : public Activity {
 public:
  EpubDictionaryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Page> page, int fontId,
                         int marginLeft, int marginTop);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }

 private:
  void moveWord(int delta);
  void moveLine(int delta);
  void performLookup();
  void buildDefinitionLines();
  void captureBaseFrame();
  bool restoreBaseFrame();
  void drawHighlight() const;
  void drawDefinitionPanel();
  static void dictionaryProgress(void* context, uint32_t completedBytes, uint32_t totalBytes);

  std::unique_ptr<Page> page_;
  int fontId_;
  int marginLeft_;
  int marginTop_;
  std::vector<PageWordHit> words_;
  std::vector<size_t> lineStarts_;
  size_t focus_ = 0;

  StarDictLookup dictionary_;
  bool dictionaryOpenAttempted_ = false;
  bool showingDefinition_ = false;
  std::string lookupWord_;
  std::string definition_;
  std::vector<std::string> definitionLines_;
  size_t scrollLine_ = 0;

  static constexpr size_t CAPTURE_CHUNK_BYTES = 8000;
  static constexpr size_t MAX_CAPTURE_CHUNKS = 8;
  std::array<std::unique_ptr<uint8_t[]>, MAX_CAPTURE_CHUNKS> captureChunks_{};
  size_t captureChunkCount_ = 0;
  size_t captureBytes_ = 0;
  bool captureValid_ = false;
  int lastDictionaryProgress_ = -1;
};
