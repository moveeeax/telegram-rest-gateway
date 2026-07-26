#include "http/byte_range.hpp"

#include <cstdint>
#include <gtest/gtest.h>

using tgw::http::parseByteRange;

namespace {

constexpr std::uintmax_t kFileSize = 1000;

}  // namespace

TEST(ByteRange, SimpleRangeIsAccepted) {
    std::uintmax_t offset = 0;
    std::uintmax_t length = 0;
    ASSERT_TRUE(parseByteRange("bytes=0-499", kFileSize, offset, length));
    EXPECT_EQ(offset, 0u);
    EXPECT_EQ(length, 500u);
}

TEST(ByteRange, OpenEndedRangeGoesToEof) {
    std::uintmax_t offset = 0;
    std::uintmax_t length = 0;
    ASSERT_TRUE(parseByteRange("bytes=500-", kFileSize, offset, length));
    EXPECT_EQ(offset, 500u);
    EXPECT_EQ(length, 500u);
}

TEST(ByteRange, SuffixRangeReturnsLastNBytes) {
    std::uintmax_t offset = 0;
    std::uintmax_t length = 0;
    ASSERT_TRUE(parseByteRange("bytes=-100", kFileSize, offset, length));
    EXPECT_EQ(offset, 900u);
    EXPECT_EQ(length, 100u);
}

// Суффикс больше файла целиком -> отдаём весь файл, а не выходим за границы.
TEST(ByteRange, SuffixLargerThanFileClampsToWholeFile) {
    std::uintmax_t offset = 0;
    std::uintmax_t length = 0;
    ASSERT_TRUE(parseByteRange("bytes=-5000", kFileSize, offset, length));
    EXPECT_EQ(offset, 0u);
    EXPECT_EQ(length, kFileSize);
}

// "to" за пределами файла — клампим на последний байт, не отклоняем как невалидный.
TEST(ByteRange, EndBeyondFileSizeClampsToLastByte) {
    std::uintmax_t offset = 0;
    std::uintmax_t length = 0;
    ASSERT_TRUE(parseByteRange("bytes=990-999999", kFileSize, offset, length));
    EXPECT_EQ(offset, 990u);
    EXPECT_EQ(length, 10u);
}

TEST(ByteRange, StartAtOrPastFileSizeIsUnsatisfiable) {
    std::uintmax_t offset = 0;
    std::uintmax_t length = 0;
    EXPECT_FALSE(parseByteRange("bytes=1000-", kFileSize, offset, length));
    EXPECT_FALSE(parseByteRange("bytes=1001-", kFileSize, offset, length));
}

TEST(ByteRange, ReversedRangeIsRejected) {
    std::uintmax_t offset = 0;
    std::uintmax_t length = 0;
    EXPECT_FALSE(parseByteRange("bytes=500-100", kFileSize, offset, length));
}

TEST(ByteRange, ZeroLengthSuffixIsRejected) {
    std::uintmax_t offset = 0;
    std::uintmax_t length = 0;
    EXPECT_FALSE(parseByteRange("bytes=-0", kFileSize, offset, length));
}

TEST(ByteRange, EmptyFileIsAlwaysUnsatisfiable) {
    std::uintmax_t offset = 0;
    std::uintmax_t length = 0;
    EXPECT_FALSE(parseByteRange("bytes=0-0", 0, offset, length));
}

TEST(ByteRange, MissingOrWrongUnitIsRejected) {
    std::uintmax_t offset = 0;
    std::uintmax_t length = 0;
    EXPECT_FALSE(parseByteRange("", kFileSize, offset, length));
    EXPECT_FALSE(parseByteRange("bytes=", kFileSize, offset, length));
    EXPECT_FALSE(parseByteRange("chunks=0-499", kFileSize, offset, length));
    EXPECT_FALSE(parseByteRange("0-499", kFileSize, offset, length));
}

TEST(ByteRange, MultiRangeIsRejected) {
    std::uintmax_t offset = 0;
    std::uintmax_t length = 0;
    EXPECT_FALSE(parseByteRange("bytes=0-99,200-299", kFileSize, offset, length));
}

TEST(ByteRange, NoDashIsRejected) {
    std::uintmax_t offset = 0;
    std::uintmax_t length = 0;
    EXPECT_FALSE(parseByteRange("bytes=500", kFileSize, offset, length));
}

// РЕГРЕССИЯ: std::stoull молча парсит числовой префикс и игнорирует хвост ("5abc" -> 5,
// "1-5abc" тоже даёт 1 без исключения). Раньше это давало занижение/искажение диапазона вместо
// честного 416 на синтаксически невалидный Range. Явная проверка "все символы — цифры" ДО
// stoull обязана отклонять такие заголовки целиком.
TEST(ByteRange, TrailingGarbageAfterDigitsIsRejected) {
    std::uintmax_t offset = 0;
    std::uintmax_t length = 0;
    EXPECT_FALSE(parseByteRange("bytes=5abc-10", kFileSize, offset, length));
    EXPECT_FALSE(parseByteRange("bytes=5-10zzz", kFileSize, offset, length));
    EXPECT_FALSE(parseByteRange("bytes=-1-5", kFileSize, offset, length));  // "-" внутри суффикса
    EXPECT_FALSE(parseByteRange("bytes= 0-499", kFileSize, offset, length));  // ведущий пробел
}

// Переполнение std::stoull (> 2^64-1 цифр) обязано отклоняться, а не бросать наружу
// необработанным исключением из HTTP-хендлера.
TEST(ByteRange, OverflowingNumberIsRejectedNotThrown) {
    std::uintmax_t offset = 0;
    std::uintmax_t length = 0;
    EXPECT_FALSE(
        parseByteRange("bytes=99999999999999999999999999-", kFileSize, offset, length));
}
