#include <BufferedFile.h>
#include <Serialization.h>
#include <gtest/gtest.h>

#include <sstream>

namespace {
class SerializationTest : public ::testing::Test {
 protected:
  void SetUp() override { Storage.reset(); }
  HalFile open(const std::string& bytes) {
    Storage.files["/cache"].bytes = bytes;
    HalFile file;
    Storage.openFileForRead("TEST", "/cache", file);
    return file;
  }
  std::string record(uint32_t length, const std::string& bytes = {}) {
    return std::string(reinterpret_cast<const char*>(&length), sizeof(length)) + bytes;
  }
};
}  // namespace

TEST_F(SerializationTest, RejectsLengthsBeforeAllocatingAndClearsOutput) {
  for (const auto& bytes : {std::string(), std::string(3, '\xff'), record(UINT32_MAX), record(3, "ab"),
                            record(4097, std::string(4097, 'x'))}) {
    auto file = open(bytes);
    std::string out = "previous";
    EXPECT_FALSE(serialization::readString(file, out));
    EXPECT_TRUE(out.empty());
    EXPECT_LT(out.capacity(), 4097u);
  }
}

TEST_F(SerializationTest, AcceptsEmptyAndBoundedUtf8Records) {
  auto file = open(record(0) + record(6, "中文"));
  std::string out = "old";
  ASSERT_TRUE(serialization::readString(file, out));
  EXPECT_TRUE(out.empty());
  ASSERT_TRUE(serialization::readString(file, out, 6));
  EXPECT_EQ(out, "中文");
}

TEST_F(SerializationTest, RejectsShortPayloadReadsAndInvalidBooleans) {
  auto file = open(record(6, "abcdef"));
  Storage.maxRead = 4;
  std::string out;
  EXPECT_FALSE(serialization::readString(file, out));
  EXPECT_TRUE(out.empty());
  file = open(std::string(1, '\x02'));
  bool flag = true;
  EXPECT_FALSE(serialization::readPod(file, flag));
  EXPECT_FALSE(flag);
}

TEST_F(SerializationTest, BufferedReaderChecksRemainingBytesAndTruncation) {
  for (const auto& bytes : {record(UINT32_MAX), record(4, "abc"), std::string(2, '\xff')}) {
    auto file = open(bytes);
    serialization::BufferedFileReader reader(file, 8);
    std::string out = "old";
    EXPECT_FALSE(serialization::readString(reader, out));
    EXPECT_TRUE(out.empty());
  }
  auto file = open(record(12, "hello world!"));
  serialization::BufferedFileReader reader(file, 8);
  std::string out;
  ASSERT_TRUE(serialization::readString(reader, out));
  EXPECT_EQ(out, "hello world!");
  EXPECT_EQ(reader.available(), 0u);
}

TEST_F(SerializationTest, StreamReaderRejectsOversizedAndIncompleteRecords) {
  for (const auto& bytes : {record(UINT32_MAX), record(4, "abc"), std::string(2, '\xff')}) {
    std::istringstream input(bytes);
    std::string out = "old";
    EXPECT_FALSE(serialization::readString(input, out));
    EXPECT_TRUE(out.empty());
  }
}
