#include <TxtToHtml.h>
#include <gtest/gtest.h>

#include "TestHelpers.h"

TEST(TxtToHtml, BaselineWhitespaceEscapesAndRawMarkdown) {
  StringPrint out;
  ASSERT_TRUE(TxtToHtml::stream("/books/A&b.MD", "  # 标题\r\nA   B & <x>\t\1 \r \nEND   ", out));
  EXPECT_NE(out.text.find("<!-- MD_CACHE_VERSION: 1 -->"), std::string::npos);
  EXPECT_NE(out.text.find("<title>A&amp;b</title>"), std::string::npos);
  EXPECT_NE(out.text.find("<body>\n&#160;&#160;# 标题<br />A&#160;&#160; B &amp; &lt;x&gt;\t <br />END\n</body>"),
            std::string::npos);
  EXPECT_EQ(out.text.find("<h1>"), std::string::npos);
  BodyOracle oracle;
  ASSERT_TRUE(oracle.parse(out.text));
  EXPECT_EQ(oracle.text, "\n\xC2\xA0\xC2\xA0# 标题A\xC2\xA0\xC2\xA0 B & <x>\t END\n");
}
TEST(TxtToHtml, BomAndUtf8AcrossEveryShortRead) {
  for (size_t chunk = 1; chunk < 8; ++chunk) {
    ShortReader input{"\xEF\xBB\xBF中😀\r\né", 0, chunk};
    StringPrint out;
    ASSERT_TRUE(TxtToHtml::stream("book.txt", &input, ShortReader::read, out));
    BodyOracle oracle;
    ASSERT_TRUE(oracle.parse(out.text));
    EXPECT_EQ(oracle.text, "\n中😀é\n");
    EXPECT_EQ(oracle.count, 5u);
  }
}
TEST(TxtToHtml, EmptyAndOnlyTrailingSpace) {
  StringPrint empty;
  ASSERT_TRUE(TxtToHtml::stream("b.txt", std::string_view{}, empty));
  BodyOracle emptyBody;
  ASSERT_TRUE(emptyBody.parse(empty.text));
  EXPECT_EQ(emptyBody.count, 2u);
  for (const auto& text : {"", "\xEF\xBB\xBF", "A   ", "A \r \n"}) {
    StringPrint out;
    ASSERT_TRUE(TxtToHtml::stream("b.txt", text, out));
    BodyOracle oracle;
    ASSERT_TRUE(oracle.parse(out.text));
    EXPECT_EQ(oracle.count, std::string(text).starts_with("A") ? 3u : 2u);
  }
}
TEST(TxtToHtml, CancelAndWriteFailure) {
  ShortReader reader{std::string(5000, 'a')};
  StringPrint out;
  unsigned ticks = 0;
  TxtToHtml::Callbacks cb{&ticks, [](void* ctx) { return ++*static_cast<unsigned*>(ctx) > 20; }, nullptr};
  TxtToHtml::State state;
  EXPECT_FALSE(TxtToHtml::convert("b.txt", &reader, ShortReader::read, out, 5000, &cb, nullptr, state));
  out.fail = true;
  EXPECT_FALSE(TxtToHtml::stream("b.txt", "hello", out));
}
TEST(TxtToHtml, MalformedUtf8FailsWithoutInvalidXmlPublication) {
  for (const auto& text : {"\x80", "\xC0\x80", "\xE0\x80\x80", "\xF4\x90\x80\x80", "\xED\xA0\x80", "\xE4\xB8"}) {
    StringPrint out;
    EXPECT_FALSE(TxtToHtml::stream("b.txt", text, out));
  }
}
