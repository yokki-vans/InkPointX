#pragma once

#include <array>
#include <string>

#include "activities/Activity.h"
#include "util/LaunchInputGuard.h"

class FileInfoActivity final : public Activity {
 public:
  FileInfoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path, bool directory,
                   uint64_t size)
      : Activity("FileInfo", renderer, mappedInput), path(std::move(path)), directory(directory), size(size) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string path;
  bool directory;
  uint64_t size;
  LaunchInputGuard inputGuard_;
};
