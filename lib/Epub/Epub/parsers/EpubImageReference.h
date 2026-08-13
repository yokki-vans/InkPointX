#pragma once

#include <cstring>
#include <string>

namespace EpubImageReference {

inline bool isImageElement(const char* name) {
  return name != nullptr && (std::strcmp(name, "img") == 0 || std::strcmp(name, "image") == 0);
}

inline std::string withoutFragment(const char* value) {
  if (value == nullptr || value[0] == '\0') return {};

  std::string source(value);
  const size_t fragmentPos = source.find('#');
  if (fragmentPos != std::string::npos) source.resize(fragmentPos);
  return source;
}

// XHTML uses <img src>, while SVG 1.1 typically uses <image xlink:href>
// and SVG 2 uses <image href>. Prefer a non-empty src regardless of attribute
// order, then fall back to either SVG spelling.
inline std::string source(const char* const* attributes) {
  if (attributes == nullptr) return {};

  const char* svgSource = nullptr;
  for (size_t i = 0; attributes[i] != nullptr; i += 2) {
    const char* name = attributes[i];
    const char* value = attributes[i + 1];
    if (value == nullptr || value[0] == '\0') continue;

    if (std::strcmp(name, "src") == 0) return withoutFragment(value);
    if (svgSource == nullptr && (std::strcmp(name, "href") == 0 || std::strcmp(name, "xlink:href") == 0)) {
      svgSource = value;
    }
  }

  return withoutFragment(svgSource);
}

}  // namespace EpubImageReference
