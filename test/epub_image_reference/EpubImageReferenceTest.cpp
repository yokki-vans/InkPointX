#include <expat.h>
#include <gtest/gtest.h>

#include "EpubImageReference.h"

namespace {

struct ParsedImage {
  std::string element;
  std::string source;
};

void XMLCALL captureImage(void* userData, const XML_Char* name, const XML_Char** attributes) {
  if (!EpubImageReference::isImageElement(name)) return;
  auto* parsed = static_cast<ParsedImage*>(userData);
  parsed->element = name;
  parsed->source = EpubImageReference::source(attributes);
}

}  // namespace

TEST(EpubImageReference, RecognisesXhtmlAndSvgImageElements) {
  EXPECT_TRUE(EpubImageReference::isImageElement("img"));
  EXPECT_TRUE(EpubImageReference::isImageElement("image"));
  EXPECT_FALSE(EpubImageReference::isImageElement("svg"));
  EXPECT_FALSE(EpubImageReference::isImageElement(nullptr));
}

TEST(EpubImageReference, ReadsXhtmlSrc) {
  const char* attributes[] = {"alt", "Cover", "src", "images/cover.jpg", nullptr};
  EXPECT_EQ(EpubImageReference::source(attributes), "images/cover.jpg");
}

TEST(EpubImageReference, ReadsSvg11XlinkHref) {
  const char* attributes[] = {"width", "314", "xlink:href", "cover.jpg", nullptr};
  EXPECT_EQ(EpubImageReference::source(attributes), "cover.jpg");
}

TEST(EpubImageReference, ExpatReadsStandardCalibreSvgCoverWrapper) {
  constexpr char document[] = R"xml(<?xml version="1.0"?>
    <html xmlns="http://www.w3.org/1999/xhtml">
      <body><svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
        viewBox="0 0 314 500" preserveAspectRatio="none">
        <image width="314" height="500" xlink:href="cover.jpg"/>
      </svg></body>
    </html>)xml";

  ParsedImage parsed;
  XML_Parser parser = XML_ParserCreate(nullptr);
  ASSERT_NE(parser, nullptr);
  XML_SetUserData(parser, &parsed);
  XML_SetElementHandler(parser, captureImage, nullptr);
  EXPECT_EQ(XML_Parse(parser, document, sizeof(document) - 1, XML_TRUE), XML_STATUS_OK);
  XML_ParserFree(parser);

  EXPECT_EQ(parsed.element, "image");
  EXPECT_EQ(parsed.source, "cover.jpg");
}

TEST(EpubImageReference, ReadsSvg2Href) {
  const char* attributes[] = {"href", "../Images/cover.png", "height", "500", nullptr};
  EXPECT_EQ(EpubImageReference::source(attributes), "../Images/cover.png");
}

TEST(EpubImageReference, PrefersNonEmptySrcRegardlessOfAttributeOrder) {
  const char* hrefFirst[] = {"xlink:href", "fallback.jpg", "src", "preferred.jpg", nullptr};
  const char* emptySrc[] = {"src", "", "href", "fallback.jpg", nullptr};
  EXPECT_EQ(EpubImageReference::source(hrefFirst), "preferred.jpg");
  EXPECT_EQ(EpubImageReference::source(emptySrc), "fallback.jpg");
}

TEST(EpubImageReference, RemovesFragmentBeforeResolvingArchivePath) {
  const char* attributes[] = {"xlink:href", "cover.jpg#view", nullptr};
  EXPECT_EQ(EpubImageReference::source(attributes), "cover.jpg");
}

TEST(EpubImageReference, SafelyHandlesMissingSource) {
  const char* attributes[] = {"alt", "Cover", nullptr};
  EXPECT_TRUE(EpubImageReference::source(attributes).empty());
  EXPECT_TRUE(EpubImageReference::source(nullptr).empty());
}
