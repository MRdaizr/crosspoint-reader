#include <ContentOpfParser.h>
#include <Epub/BookMetadataCache.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>

namespace {
const std::string CACHE = "/cache";
const std::string BASE = "OEBPS/";

std::unique_ptr<ContentOpfParser> parse(const std::string& xml, bool extended = true, size_t chunk = 7,
                                        bool metadataOnly = true, BookMetadataCache* cache = nullptr) {
  auto parser = std::make_unique<ContentOpfParser>(CACHE, BASE, xml.size(), cache, metadataOnly, extended);
  EXPECT_TRUE(parser->setup());
  for (size_t offset = 0; offset < xml.size();) {
    const size_t count = std::min(chunk, xml.size() - offset);
    const size_t written = parser->write(reinterpret_cast<const uint8_t*>(xml.data() + offset), count);
    if (written != count) break;
    offset += count;
  }
  return parser;
}

std::string package(const std::string& metadata) {
  return "<opf:package xmlns:opf='urn:opf' xmlns:dc='urn:dc'><opf:metadata>" + metadata +
         "</opf:metadata><opf:manifest><opf:item id='s' href='s.css' media-type='text/css'/></opf:manifest>"
         "<opf:spine/></opf:package>";
}
}  // namespace

TEST(ContentOpfParser, Epub2SchemesAndCalibreSeries) {
  auto parser =
      parse(package("<dc:title> Title </dc:title><dc:creator>Jane &amp; Doe</dc:creator>"
                    "<dc:creator>John Doe</dc:creator><dc:language>en</dc:language>"
                    "<dc:identifier opf:scheme='ISBN'>978-1-234</dc:identifier>"
                    "<dc:identifier opf:scheme='AMAZON'>B012345678</dc:identifier>"
                    "<opf:meta name='calibre:series' content='Example Series'/>"
                    "<opf:meta name='calibre:series_index' content=' 2.5 '/>"));
  ASSERT_TRUE(parser->succeeded());
  EXPECT_EQ(parser->title, "Title");
  EXPECT_EQ(parser->author, "Jane & Doe, John Doe");
  EXPECT_EQ(parser->isbn, "978-1-234");
  EXPECT_EQ(parser->asin, "B012345678");
  EXPECT_EQ(parser->series, "Example Series");
  ASSERT_TRUE(parser->seriesIndex.has_value());
  EXPECT_FLOAT_EQ(*parser->seriesIndex, 2.5f);
}

TEST(ContentOpfParser, Epub3UrnsAndRefinementsInEitherOrder) {
  auto parser =
      parse(package("<dc:identifier>URN:ISBN:9781234567890</dc:identifier>"
                    "<dc:identifier>urn:asin:B098765432</dc:identifier>"
                    "<meta property='group-position' refines='#series'>3.25</meta>"
                    "<meta property='collection-type' refines='#series'>series</meta>"
                    "<meta property='belongs-to-collection' id='set'>A Set</meta>"
                    "<meta property='collection-type' refines='#set'>set</meta>"
                    "<meta property='belongs-to-collection' id='series'>Real Series</meta>"));
  ASSERT_TRUE(parser->succeeded());
  EXPECT_EQ(parser->isbn, "9781234567890");
  EXPECT_EQ(parser->asin, "B098765432");
  EXPECT_EQ(parser->series, "Real Series");
  ASSERT_TRUE(parser->seriesIndex.has_value());
  EXPECT_FLOAT_EQ(*parser->seriesIndex, 3.25f);
}

TEST(ContentOpfParser, Epub3OnixIdentifierTypesMayPrecedeIdentifiers) {
  auto parser =
      parse(package("<meta property='identifier-type' refines='#isbn' scheme='onix:codelist5'>15</meta>"
                    "<dc:identifier id='isbn'>9781234567890</dc:identifier>"
                    "<dc:identifier id='amazon'>B098765432</dc:identifier>"
                    "<meta property='identifier-type' refines='#amazon'>ASIN</meta>"));
  ASSERT_TRUE(parser->succeeded());
  EXPECT_EQ(parser->isbn, "9781234567890");
  EXPECT_EQ(parser->asin, "B098765432");
}

TEST(ContentOpfParser, CalibreSeriesWinsOverEpub3Collection) {
  auto parser =
      parse(package("<meta property='belongs-to-collection' id='s'>EPUB Series</meta>"
                    "<meta property='collection-type' refines='#s'>series</meta>"
                    "<meta property='group-position' refines='#s'>1</meta>"
                    "<meta name='calibre:series' content='Calibre Series'/>"
                    "<meta name='calibre:series_index' content='4'/>"));
  ASSERT_TRUE(parser->succeeded());
  EXPECT_EQ(parser->series, "Calibre Series");
  EXPECT_EQ(parser->seriesIndex, 4.0f);
}

TEST(ContentOpfParser, RejectsMalformedOrNonfiniteSeriesIndices) {
  for (const char* index : {"", "nan", "inf", "-inf", "2garbage", "1e99", "1e-99"}) {
    SCOPED_TRACE(index);
    auto calibre = parse(package(std::string("<meta name='calibre:series' content='Series'/>"
                                             "<meta name='calibre:series_index' content='") +
                                 index + "'/>"));
    ASSERT_TRUE(calibre->succeeded());
    EXPECT_FALSE(calibre->seriesIndex.has_value());
    auto epub3 = parse(package(std::string("<meta property='belongs-to-collection' id='s'>Series</meta>"
                                           "<meta property='collection-type' refines='#s'>series</meta>"
                                           "<meta property='group-position' refines='#s'>") +
                               index + "</meta>"));
    ASSERT_TRUE(epub3->succeeded());
    EXPECT_FALSE(epub3->seriesIndex.has_value());
  }
}

TEST(ContentOpfParser, ExtendedFieldsAreCollectedOnlyWhenRequested) {
  auto parser = parse(package("<dc:title>Core</dc:title><dc:identifier opf:scheme='ISBN'>123</dc:identifier>"
                              "<meta name='calibre:series' content='Series'/>"),
                      false);
  ASSERT_TRUE(parser->succeeded());
  EXPECT_EQ(parser->title, "Core");
  EXPECT_TRUE(parser->isbn.empty());
  EXPECT_TRUE(parser->series.empty());
}

TEST(ContentOpfParser, StopsAtMetadataBeforeInvalidTailOrCacheWrites) {
  Storage.reset();
  Storage.files[CACHE + "/.items.bin"] = "another parser owns this";
  const auto xml = package("<dc:title>Core</dc:title>") + "<invalid";
  {
    auto parser = parse(xml, true, xml.size());
    ASSERT_TRUE(parser->succeeded());
    EXPECT_TRUE(parser->cssFiles.empty());
    EXPECT_TRUE(parser->tocNavPath.empty());
  }
  EXPECT_EQ(Storage.writes, 0u);
  EXPECT_EQ(Storage.reads, 0u);
  EXPECT_EQ(Storage.removals, 0u);
  EXPECT_EQ(Storage.files.at(CACHE + "/.items.bin"), "another parser owns this");
}

TEST(ContentOpfParser, ShortWritesForBadOrMissingMetadataAreFailures) {
  EXPECT_FALSE(parse("<package><metadata><title>broken</metadata></package>")->succeeded());
  EXPECT_FALSE(parse("<package><manifest/></package>")->succeeded());
  EXPECT_FALSE(parse("<package><metadata><title>truncated")->succeeded());
}

TEST(ContentOpfParser, TextAndCandidateCountsAreBounded) {
  std::string metadata = "<dc:title>" + std::string(4000, 'x') + "</dc:title>";
  for (int i = 0; i < 64; ++i) {
    metadata += "<meta property='belongs-to-collection' id='s" + std::to_string(i) + "'>Set</meta>";
  }
  metadata += "<meta property='collection-type' refines='#s63'>series</meta>";
  auto parser = parse(package(metadata));
  ASSERT_TRUE(parser->succeeded());
  EXPECT_EQ(parser->title.size(), 512u);
  EXPECT_TRUE(parser->series.empty());
}

TEST(ContentOpfParser, FullParsingPreservesCoverGuideAndLegacyTextFormatting) {
  Storage.reset();
  BookMetadataCache cache;
  const std::string xml =
      "<package><metadata><title>  Title  </title><creator>A&amp;B</creator>"
      "<meta name='cover' content='wrapper'/></metadata><manifest>"
      "<item id='wrapper' href='cover.xhtml' media-type='application/xhtml+xml'/>"
      "<item id='image' href='cover.png' media-type='image/png' properties='cover-image'/>"
      "<item id='chapter' href='one.xhtml' media-type='application/xhtml+xml'/>"
      "</manifest><spine><itemref idref='chapter'/></spine><guide>"
      "<reference type='text' href='one.xhtml'/><reference type='start' href='start.xhtml'/>"
      "<reference type='cover' href='cover.xhtml'/></guide></package>";
  auto parser = parse(xml, false, xml.size(), false, &cache);
  ASSERT_TRUE(parser->succeeded());
  EXPECT_EQ(parser->title, "  Title  ");
  EXPECT_EQ(parser->author, "A, &, B");
  EXPECT_EQ(parser->coverItemHref, "OEBPS/cover.png");
  EXPECT_EQ(parser->textReferenceHref, "OEBPS/start.xhtml");
  EXPECT_EQ(parser->guideCoverPageHref, "OEBPS/cover.xhtml");
  ASSERT_EQ(cache.spine.size(), 1u);
  EXPECT_EQ(cache.spine.front(), "OEBPS/one.xhtml");
}

TEST(ContentOpfParser, CssReparseWithoutCacheDoesNotTouchSpineTemporaryFile) {
  Storage.reset();
  Storage.files[CACHE + "/.items.bin"] = "keep";
  {
    auto parser = parse(package("<dc:title>Core</dc:title>"), false, 1024, false);
    ASSERT_TRUE(parser->succeeded());
    ASSERT_EQ(parser->cssFiles.size(), 1u);
  }
  EXPECT_EQ(Storage.writes, 0u);
  EXPECT_EQ(Storage.reads, 0u);
  EXPECT_EQ(Storage.removals, 0u);
}
