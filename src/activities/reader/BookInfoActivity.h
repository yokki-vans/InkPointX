#pragma once

#include <string>

#include "activities/Activity.h"
#include "util/LaunchInputGuard.h"

class BookInfoActivity final : public Activity {
 public:
  BookInfoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title, std::string author,
                   std::string language, std::string path, int currentPage, int totalPages, int progressPercent)
      : Activity("BookInfo", renderer, mappedInput),
        title(std::move(title)),
        author(std::move(author)),
        language(std::move(language)),
        path(std::move(path)),
        currentPage(currentPage),
        totalPages(totalPages),
        progressPercent(progressPercent) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string title;
  std::string author;
  std::string language;
  std::string path;
  int currentPage;
  int totalPages;
  int progressPercent;
  LaunchInputGuard inputGuard_;
};
