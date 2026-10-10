#include <Epub/Page.h>
#include <Epub/ParsedText.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <Epub/parsers/ChapterHtmlSlimParser.h>
#include <activities/reader/EpubReaderFootnoteSelectActivity.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <new>
#include <string>
#include <vector>

static bool failNextCheckedArray = false;
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
  if (failNextCheckedArray) {
    failNextCheckedArray = false;
    return nullptr;
  }
  return ::operator new[](bytes);
}

namespace {
int checks = 0;
#define CHECK(value)                                           \
  do {                                                         \
    ++checks;                                                  \
    if (!(value)) {                                            \
      std::fprintf(stderr, "line %d: %s\n", __LINE__, #value); \
      std::exit(1);                                            \
    }                                                          \
  } while (false)
std::filesystem::path fixtureDirectory;
std::string CACHE, HTML;
bool fixtureDirectoryOwned = false;
void cleanupFixtures() {
  if (!fixtureDirectoryOwned) return;
  std::error_code error;
  std::filesystem::remove(CACHE, error);
  std::filesystem::remove(HTML, error);
  std::filesystem::remove(fixtureDirectory, error);  // Non-recursive, only our empty directory.
  if (error) std::fprintf(stderr, "Fixture cleanup failed: %s\n", error.message().c_str());
}
BlockStyle leftStyle();

PageLink link(const char* href, uint32_t id, int x = 10, int y = 10, int w = 16, int h = 16) {
  PageLink result;
  CHECK(result.setTarget(href));
  result.identity = id;
  result.x = x;
  result.y = y;
  result.width = w;
  result.height = h;
  return result;
}

std::vector<uint8_t> bytesOf(const Page& page) {
  HalFile file;
  CHECK(file.open(CACHE.c_str(), "wb"));
  CHECK(page.serialize(file));
  file.close();
  CHECK(file.open(CACHE.c_str(), "rb"));
  std::vector<uint8_t> bytes(file.size());
  CHECK(file.read(bytes.data(), bytes.size()) == bytes.size());
  return bytes;
}

std::unique_ptr<Page> readBytes(const std::vector<uint8_t>& bytes) {
  HalFile file;
  CHECK(file.open(CACHE.c_str(), "wb"));
  if (!bytes.empty())
    CHECK(file.write(bytes.data(), bytes.size()) == bytes.size());
  else
    CHECK(file.size() == 0);
  file.close();
  CHECK(file.open(CACHE.c_str(), "rb"));
  return Page::deserialize(file);
}

template <typename T>
void replace(std::vector<uint8_t>& bytes, size_t offset, T value) {
  CHECK(offset + sizeof(value) <= bytes.size());
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void targetsAndBounds() {
  CHECK(isInternalPageLink("#note"));
  CHECK(isInternalPageLink("../Text/chapter.xhtml#%E4%B8%80"));
  for (const char* target : {"", "http://a", "HTTPS://a", "JaVaScRiPt:alert(1)", "mailto:a", "data:text/a",
                             "//remote/a", "/absolute", " https://a", "a\nb", "a\\b", "a:thing", "#note\nbad"})
    CHECK(!isInternalPageLink(target));
  std::string target(512, 'a');
  CHECK(isInternalPageLink(target.c_str()));
  target.push_back('a');
  CHECK(!isInternalPageLink(target.c_str()));
  PageLinks links;
  for (uint32_t i = 1; i <= 32; ++i) CHECK(links.append(link("#same", i, i * 20, 10)));
  CHECK(!links.append(link("#overflow", 33)));
  CHECK(links.size() == 32);
  CHECK(links.uniqueCount() == 32);
  CHECK(links.hitTest(21, 11) == 0);
  CHECK(links.hitTest(19, 11) == 0);
  CHECK(links.hitTest(0, 0) == -1);
  PageLinks wrapped;
  CHECK(wrapped.append(link("#one", 1)));
  CHECK(wrapped.append(link("#one", 1, 10, 40)));
  CHECK(wrapped.uniqueCount() == 1);
  CHECK(!wrapped.firstOccurrence(1));
  auto moved = std::move(wrapped);
  CHECK(wrapped.empty());
  CHECK(moved.size() == 2);
  auto pending = link("#oom", 7);
  failNextCheckedArray = true;
  CHECK(!wrapped.append(std::move(pending)));
}

void serialization() {
  Page page;
  const std::string target(512, 'x');
  CHECK(page.links.append(link(target.c_str(), 7, -12, 1, 40, 20)));
  CHECK(page.links.append(link("#second", 8, 50, 20, 20, 16)));
  const auto original = bytesOf(page);
  auto restored = readBytes(original);
  CHECK(restored && restored->links.size() == 2);
  CHECK(restored->links[0].identity == 7);
  CHECK(std::strlen(restored->links[0].href.get()) == 512);
  CHECK(restored->links[0].x == -12);
  for (size_t n = 0; n < original.size(); ++n) {
    std::vector<uint8_t> shortBytes(original.begin(), original.begin() + n);
    CHECK(!readBytes(shortBytes));
  }
  auto bad = original;
  replace<uint16_t>(bad, 4, 33);
  CHECK(!readBytes(bad));
  for (uint16_t length : {uint16_t(0), uint16_t(513), uint16_t(65535)}) {
    bad = original;
    replace<uint16_t>(bad, 6, length);
    CHECK(!readBytes(bad));
  }
  bad = original;
  bad[8] = 0;
  CHECK(!readBytes(bad));
  const size_t identity = 8 + 512;
  bad = original;
  replace<uint32_t>(bad, identity, 0);
  CHECK(!readBytes(bad));
  for (int16_t width : {int16_t(0), int16_t(-1), int16_t(8193)}) {
    bad = original;
    replace<int16_t>(bad, identity + 8, width);
    CHECK(!readBytes(bad));
  }
  bad = original;
  replace<int16_t>(bad, identity + 4, 8190);
  CHECK(!readBytes(bad));
  bad = original;
  replace<int16_t>(bad, identity + 6, -8193);
  CHECK(!readBytes(bad));
  bad = original;
  replace<int16_t>(bad, identity + 10, -20);
  CHECK(!readBytes(bad));
  bad = original;
  bad.back() = 2;
  CHECK(!readBytes(bad));
  page.linkGeometryComplete = false;
  restored = readBytes(bytesOf(page));
  CHECK(restored && !restored->linkGeometryComplete);
  Page external;
  auto record = link("#safe", 1);
  std::strcpy(record.href.get(), "a:b");
  CHECK(external.links.append(std::move(record)));
  HalFile file;
  CHECK(file.open(CACHE.c_str(), "wb"));
  CHECK(!external.serialize(file));
  file.close();
  Page inconsistent;
  CHECK(inconsistent.links.append(link("#a", 9)));
  CHECK(inconsistent.links.append(link("#b", 9, 50)));
  CHECK(!readBytes(bytesOf(inconsistent)));
  Page empty;
  CHECK(readBytes(bytesOf(empty)) != nullptr);
}

void checkedAllocations() {
  PageLink target;
  failNextCheckedArray = true;
  CHECK(!target.setTarget("#oom"));
  CHECK(target.setTarget("#safe"));
  target.identity = 1;
  PageLinks links;
  failNextCheckedArray = true;
  CHECK(!links.append(std::move(target)));
  CHECK(links.empty());
  CHECK(links.append(std::move(target)));
  CHECK(links.size() == 1);
  failNextCheckedArray = true;
  CHECK(!links.reserve(32));
  CHECK(links.size() == 1);
  CHECK(std::strcmp(links[0].href.get(), "#safe") == 0);
  ParsedText text(false, false, false, leftStyle(), 0);
  failNextCheckedArray = true;
  CHECK(text.addLinkTarget("#oom") == 0);
  text.addWord("word", EpdFontFamily::REGULAR);
  GfxRenderer renderer;
  text.layoutAndExtractLines(
      renderer, 0, 120, [&](std::unique_ptr<TextBlock> line, uint32_t) { CHECK(!line->hasCompleteLinkGeometry()); });
}

BlockStyle leftStyle() {
  BlockStyle style;
  style.alignment = CssTextAlign::Left;
  style.textAlignDefined = true;
  return style;
}

void layoutIdentity() {
  GfxRenderer renderer;
  for (bool focus : {false, true})
    for (bool hyphen : {false, true})
      for (int8_t tracking = -2; tracking <= 2; ++tracking) {
        ParsedText text(false, hyphen, focus, leftStyle(), 0);
        const auto id = text.addLinkTarget("chapter.xhtml#n", 101);
        text.addWord("internationalization", EpdFontFamily::REGULAR, false, false, 100, id);
        text.addWord("plain", EpdFontFamily::REGULAR, false, false, 120);
        const auto second = text.addLinkTarget("chapter.xhtml#n", 102);
        text.addWord("again", EpdFontFamily::REGULAR, false, false, 125, second);
        bool sawFirst = false, sawSecond = false;
        text.layoutAndExtractLines(
            renderer, 0, 72,
            [&](std::unique_ptr<TextBlock> line, uint32_t offset) {
              auto spans = line->takeLinkSpans();
              for (const auto& span : spans) {
                CHECK(std::strcmp(span.href.get(), "chapter.xhtml#n") == 0);
                CHECK(span.width > 0 && span.height > 0);
                CHECK(span.identity == 101 || span.identity == 102);
                if (span.identity == 101) {
                  sawFirst = true;
                  CHECK(offset < 120);
                }
                if (span.identity == 102) sawSecond = true;
                bool intersects = false;
                for (size_t i = 0; i < line->wordCount(); ++i) {
                  if (span.contains(line->wordXpos(i), 1)) intersects = true;
                }
                CHECK(intersects);
              }
            },
            true, tracking, 175);
        CHECK(sawFirst && sawSecond);
      }
  ParsedText streamed(false, false, true, leftStyle(), 0);
  const auto id = streamed.addLinkTarget("#cjk", 500);
  size_t seen = 0;
  auto consume = [&](std::unique_ptr<TextBlock> line, uint32_t) {
    const auto spans = line->takeLinkSpans();
    CHECK(!spans.empty());
    for (const auto& span : spans) {
      CHECK(span.identity == 500);
      ++seen;
    }
  };
  for (int i = 0; i < 10; ++i) {
    streamed.addWord("中文测试段落", EpdFontFamily::REGULAR, false, false, i * 6, id);
    streamed.layoutAndExtractLines(renderer, 0, 48, consume, false, 2, 100);
    CHECK(streamed.linkTargetMatches(id, "#cjk", 500));
  }
  streamed.layoutAndExtractLines(renderer, 0, 48, consume, true, 2, 100);
  CHECK(seen > 10);
  ParsedText ruby(false, false, false, leftStyle(), 0);
  const auto rubyId = ruby.addLinkTarget("#ruby", 800);
  ruby.addWord("漢字", EpdFontFamily::REGULAR, false, false, 0, rubyId);
  ruby.setRubyGroupAt(0, ruby.size(), "longannotation");
  ruby.layoutAndExtractLines(renderer, 0, 120, [&](std::unique_ptr<TextBlock> line, uint32_t) {
    const auto spans = line->takeLinkSpans();
    CHECK(spans.size() == 1);
    CHECK(spans[0].y == -renderer.getFontAscenderSize(0));
    CHECK(spans[0].width >= 56);
  });
  BlockStyle rtl = leftStyle();
  rtl.isRtl = true;
  rtl.directionDefined = true;
  ParsedText bidi(false, false, true, rtl, 0);
  const auto bidiId = bidi.addLinkTarget("#rtl", 900);
  bidi.addWord("שלום", EpdFontFamily::REGULAR, false, false, 0, bidiId);
  bidi.addWord("Latin", EpdFontFamily::REGULAR, false, false, 4);
  bidi.layoutAndExtractLines(
      renderer, 0, 160,
      [&](std::unique_ptr<TextBlock> line, uint32_t) {
        const auto spans = line->takeLinkSpans();
        CHECK(spans.size() == 1);
        CHECK(spans[0].identity == 900);
        for (size_t i = 0; i < line->wordCount(); ++i)
          if (std::strcmp(line->wordText(i), "Latin") == 0) CHECK(!spans[0].contains(line->wordXpos(i) + 1, 1));
      },
      true, 2, 150);
}

std::vector<std::unique_ptr<Page>> parseHtml(const std::string& html, int chunkSize) {
  HalFile file;
  CHECK(file.open(HTML.c_str(), "wb"));
  CHECK(file.write(html.data(), html.size()) == html.size());
  file.close();
  GfxRenderer renderer;
  std::vector<std::unique_ptr<Page>> pages;
  pages.reserve(256);
  const std::string filename = HTML;
  ChapterHtmlSlimParser parser(
      nullptr, filename, renderer, 0, 1, false, static_cast<uint8_t>(CssTextAlign::Left), 96, 64, true, true,
      [&](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t) {
        if (page) pages.push_back(std::move(page));
      },
      true, "", "", 2, {}, nullptr, nullptr, 0, nullptr, static_cast<uint16_t>(chunkSize));
  parser.setParagraphIndentSpaces(0);
  parser.setTextSpacing(2, 150);
  CHECK(parser.beginParsing());
  for (;;) {
    const auto result = parser.parseNextChunk(1);
    CHECK(result != ChapterHtmlSlimParser::ParseResult::Failed);
    if (result == ChapterHtmlSlimParser::ParseResult::Complete) break;
  }
  return pages;
}

void parserAndIncrementalReplay() {
  std::string html = "<html><body><p><a href='../chapter.xhtml#n'>";
  for (int i = 0; i < 350; ++i) html += "international 中文 ";
  html +=
      "</a> plain <a href='../chapter.xhtml#n'><sup>2</sup></a>"
      "<a href='HTTPS://host'>external</a><a href='javascript:alert(1)'>bad</a>"
      "<a href='#ruby'><ruby>漢字<rt>kanji</rt></ruby></a></p></body></html>";
  auto full = parseHtml(html, 1024), incremental = parseHtml(html, 256), replayed = parseHtml(html, 256);
  CHECK(full.size() == incremental.size());
  CHECK(full.size() > 20);
  bool second = false, ruby = false;
  for (size_t i = 0; i < full.size(); ++i) {
    CHECK(bytesOf(*incremental[i]) == bytesOf(*replayed[i]));
    // The pre-existing pending-footnote word-index list can land on a different
    // page at different soft-flush boundaries. Geometry must not depend on it.
    CHECK(full[i]->links.size() == incremental[i]->links.size());
    for (size_t j = 0; j < full[i]->links.size(); ++j) {
      const auto& a = full[i]->links[j];
      const auto& b = incremental[i]->links[j];
      CHECK(a.identity == b.identity && std::strcmp(a.href.get(), b.href.get()) == 0);
      CHECK(a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height);
    }
    CHECK(full[i]->links.size() <= 32);
    for (const auto& span : full[i]->links) {
      CHECK(span.validGeometry());
      CHECK(span.identity == 1 || span.identity == 2 || span.identity == 3);
      if (span.identity == 1) CHECK(std::strcmp(span.href.get(), "../chapter.xhtml#n") == 0);
      if (span.identity == 2) second = true;
      if (span.identity == 3) ruby = true;
    }
    CHECK(readBytes(bytesOf(*full[i])) != nullptr);
  }
  CHECK(second && ruby);
  const std::string longTarget(512, 't');
  const auto longPages = parseHtml("<html><body><p><a href='" + longTarget + "'>1</a></p></body></html>", 256);
  CHECK(!longPages.empty() && longPages[0]->links.size() == 1);
  CHECK(std::strlen(longPages[0]->links[0].href.get()) == 512);
}

void selector() {
  for (int orientation = 0; orientation < 4; ++orientation) {
    GfxRenderer renderer;
    renderer.setOrientation(static_cast<GfxRenderer::Orientation>(orientation));
    renderer.rectangles.reserve(32);
    MappedInputManager input;
    auto page = makeUniqueNoThrow<Page>();
    CHECK(page->links.append(link("#first", 1, 10, 10)));
    CHECK(page->links.append(link("#first", 1, 10, 35)));
    CHECK(page->links.append(link("#second", 2, 70, 10)));
    EpubReaderFootnoteSelectActivity activity(renderer, input, std::move(page), 4, 5);
    activity.onEnter();
    activity.render(RenderLock{});
    CHECK(renderer.clears == 1);
    CHECK(renderer.captures == 1);
    CHECK(renderer.rectangles.size() == 2);
    input.pressed = static_cast<int>(MappedInputManager::Button::ScreenRight);
    activity.loop();
    activity.render(RenderLock{});
    CHECK(renderer.restores == 1);
    CHECK(renderer.clears == 1);
    CHECK(renderer.rectangles.back().x == 72);
    CHECK(renderer.largestCapture <= 4096);
    renderer.failCapture = true;
    activity.render(RenderLock{});
    CHECK(renderer.clears == 2);
    activity.render(RenderLock{});
    CHECK(renderer.clears == 3);
    renderer.failCapture = false;
    activity.handleForcedRefresh();
    activity.render(RenderLock{});
    CHECK(renderer.clears == 4);
    renderer.orientation = static_cast<GfxRenderer::Orientation>((orientation + 1) % 4);
    activity.render(RenderLock{});
    CHECK(renderer.clears == 5);
    input.pressed = -1;
    input.tap = true;
    input.tapX = 15;
    input.tapY = 41;
    activity.loop();
    CHECK(activity.finished);
    CHECK(std::get<FootnoteResult>(activity.result.data).href == "#first");
    activity.onExit();
  }
  GfxRenderer renderer;
  MappedInputManager input;
  auto page = makeUniqueNoThrow<Page>();
  CHECK(page->links.append(link("#large", 1, 0, 0, 470, 400)));
  EpubReaderFootnoteSelectActivity activity(renderer, input, std::move(page), 0, 0);
  failNextCheckedArray = true;
  activity.onEnter();
  activity.render(RenderLock{});
  activity.render(RenderLock{});
  CHECK(renderer.clears == 2);
  CHECK(renderer.captures == 0);
  input.released = static_cast<int>(MappedInputManager::Button::Confirm);
  activity.loop();
  CHECK(activity.finished);
}
}  // namespace

int main() {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  std::error_code error;
  fixtureDirectory = std::filesystem::temp_directory_path(error) / ("crosspoint-page-link-" + std::to_string(nonce));
  CHECK(!error);
  fixtureDirectoryOwned = std::filesystem::create_directory(fixtureDirectory, error);
  CHECK(fixtureDirectoryOwned && !error);
  CACHE = (fixtureDirectory / "cache.bin").string();
  HTML = (fixtureDirectory / "chapter.html").string();
  std::atexit(cleanupFixtures);  // CHECK uses exit: failed tests clean up too.
  Hyphenator::setPreferredLanguage("en");
  // Allocation failure is tested explicitly, not via the checked construction helper.
  targetsAndBounds();
  checkedAllocations();
  serialization();
  layoutIdentity();
  parserAndIncrementalReplay();
  selector();
  cleanupFixtures();
  fixtureDirectoryOwned = false;
  std::printf("Page links: %d checks passed\n", checks);
}
