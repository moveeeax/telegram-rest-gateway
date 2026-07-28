#include "http/upload_sanitize.hpp"

#include <gtest/gtest.h>

using tgw::http::sanitizeUploadFilename;

// Обычное имя проходит без изменений.
TEST(SanitizeUploadFilename, PlainNameUnchanged) {
    EXPECT_EQ(sanitizeUploadFilename("report.pdf"), "report.pdf");
    EXPECT_EQ(sanitizeUploadFilename("Photo 2024.JPG"), "Photo 2024.JPG");
}

// Каталоги (разделитель '/') отрезаются filename() — остаётся последний компонент.
TEST(SanitizeUploadFilename, StripsLeadingDirectories) {
    EXPECT_EQ(sanitizeUploadFilename("/etc/passwd"), "passwd");
    EXPECT_EQ(sanitizeUploadFilename("a/b/c.txt"), "c.txt");
    EXPECT_EQ(sanitizeUploadFilename("../../etc/passwd"), "passwd");
}

// Ядро фикса #2: ".." сам по себе — валидный результат filename() (последний компонент пути ".."),
// и dir/".." резолвился бы в РОДИТЕЛЬСКИЙ каталог. Пустое / "." / ".." -> "upload.bin".
TEST(SanitizeUploadFilename, DotSegmentsBecomeDefault) {
    EXPECT_EQ(sanitizeUploadFilename(""), "upload.bin");
    EXPECT_EQ(sanitizeUploadFilename("."), "upload.bin");
    EXPECT_EQ(sanitizeUploadFilename(".."), "upload.bin");
    // "dir/.." -> filename()=="..", тоже обязано схлопнуться в дефолт (иначе запись поверх
    // родителя).
    EXPECT_EQ(sanitizeUploadFilename("dir/.."), "upload.bin");
}

// Завершающий '/' даёт пустой filename() -> дефолт (а не запись в каталог как в файл).
TEST(SanitizeUploadFilename, TrailingSlashBecomesDefault) {
    EXPECT_EQ(sanitizeUploadFilename("foo/"), "upload.bin");
}

// Юникод в имени сохраняется: разделителей каталогов нет, компонент один.
TEST(SanitizeUploadFilename, UnicodePreserved) {
    EXPECT_EQ(sanitizeUploadFilename("файл.txt"), "файл.txt");
    EXPECT_EQ(sanitizeUploadFilename("写真.png"), "写真.png");
}

// На POSIX (сборка/CI — Linux) '\' НЕ является разделителем пути: filename() возвращает всю
// строку одним компонентом. Обхода каталогов нет (в дереве создастся файл с '\' в имени),
// поэтому имя проходит как есть. Фиксируем именно это поведение целевой платформы.
TEST(SanitizeUploadFilename, BackslashIsNotAPathSeparatorOnPosix) {
    EXPECT_EQ(sanitizeUploadFilename("..\\..\\secret.txt"), "..\\..\\secret.txt");
    EXPECT_EQ(sanitizeUploadFilename("a\\b"), "a\\b");
}
