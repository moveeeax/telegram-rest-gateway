#include "util/aws_sigv4.hpp"

#include <gtest/gtest.h>
#include <map>
#include <string>

using tgw::util::sha256Hex;
using tgw::util::sigv4Authorization;

namespace {

constexpr const char* kEmptySha256 =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

}  // namespace

TEST(AwsSigV4, Sha256KnownVectors) {
    EXPECT_EQ(sha256Hex(""), kEmptySha256);
    EXPECT_EQ(sha256Hex("hello"),
              "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
}

// Официальный вектор AWS: "GET Object" (docs.aws.amazon.com, Signature V4 examples).
// Регион us-east-1, сервис s3, дата 20130524T000000Z, объект examplebucket/test.txt с Range.
TEST(AwsSigV4, GetObjectOfficialVector) {
    const std::string access_key = "AKIAIOSFODNN7EXAMPLE";
    const std::string secret_key = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
    const std::string amz_date = "20130524T000000Z";

    std::map<std::string, std::string> headers;
    headers["host"] = "examplebucket.s3.amazonaws.com";
    headers["range"] = "bytes=0-9";
    headers["x-amz-content-sha256"] = kEmptySha256;
    headers["x-amz-date"] = amz_date;

    const std::string auth =
        sigv4Authorization("GET", "/test.txt", "", headers, kEmptySha256, amz_date, "us-east-1",
                           "s3", access_key, secret_key);

    EXPECT_EQ(auth,
              "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request"
              ", SignedHeaders=host;range;x-amz-content-sha256;x-amz-date"
              ", Signature=f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");
}

// Официальный вектор AWS: "PUT Object" — PUT examplebucket/test$file.text с телом.
TEST(AwsSigV4, PutObjectOfficialVector) {
    const std::string access_key = "AKIAIOSFODNN7EXAMPLE";
    const std::string secret_key = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
    const std::string amz_date = "20130524T000000Z";

    // Тело "Welcome to Amazon S3." — известный хэш из документации.
    const std::string payload_hash = sha256Hex("Welcome to Amazon S3.");
    EXPECT_EQ(payload_hash, "44ce7dd67c959e0d3524ffac1771dfbba87d2b6b4b4e99e42034a8b803f8b072");

    std::map<std::string, std::string> headers;
    headers["date"] = "Fri, 24 May 2013 00:00:00 GMT";
    headers["host"] = "examplebucket.s3.amazonaws.com";
    headers["x-amz-content-sha256"] = payload_hash;
    headers["x-amz-date"] = amz_date;
    headers["x-amz-storage-class"] = "REDUCED_REDUNDANCY";

    const std::string auth =
        sigv4Authorization("PUT", "/test%24file.text", "", headers, payload_hash, amz_date,
                           "us-east-1", "s3", access_key, secret_key);

    EXPECT_EQ(auth,
              "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request"
              ", SignedHeaders=date;host;x-amz-content-sha256;x-amz-date;x-amz-storage-class"
              ", Signature=98ad721746da40c64f1a55b78f14c238d841ea1380cd77a1b5971af0ece108bd");
}
