#include "application-credentials-test-backend.h"
#include "telegram-application-credentials.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

namespace {

class ApplicationCredentialsTest : public testing::Test {
protected:
    void SetUp() override
    {
        tdlib_purple_test_application_credentials_set_unavailable();
    }

    void TearDown() override
    {
        tdlib_purple_test_application_credentials_set_unavailable();
    }

    static std::string validHash()
    {
        return std::string(TDLIB_PURPLE_API_HASH_LENGTH, 'a');
    }
};

TEST_F(ApplicationCredentialsTest, ParsesCanonicalBoundaryApiIds)
{
    const std::string hash = validHash();
    TdlibPurpleApplicationCredentials credentials = {};

    ASSERT_TRUE(
        tdlib_purple_application_credentials_parse_compatibility_override(
            "1", hash.c_str(), &credentials));
    EXPECT_EQ(credentials.api_id, 1);
    EXPECT_STREQ(credentials.api_hash, hash.c_str());

    ASSERT_TRUE(
        tdlib_purple_application_credentials_parse_compatibility_override(
            "2147483647", hash.c_str(), &credentials));
    EXPECT_EQ(credentials.api_id, G_MAXINT32);
    EXPECT_STREQ(credentials.api_hash, hash.c_str());
}

TEST_F(ApplicationCredentialsTest, RejectsNonCanonicalApiIdsAndClearsOutput)
{
    static const char *const invalid_ids[] = {
        "",
        "0",
        "01",
        "-1",
        "+1",
        " 1",
        "1 ",
        "1x",
        "2147483648",
    };
    const std::string hash = validHash();

    for (const char *api_id : invalid_ids) {
        TdlibPurpleApplicationCredentials credentials;
        std::memset(&credentials, 0x7f, sizeof(credentials));

        EXPECT_FALSE(
            tdlib_purple_application_credentials_parse_compatibility_override(
            api_id, hash.c_str(), &credentials))
            << "unexpectedly accepted API ID case";
        EXPECT_EQ(credentials.api_id, 0);
        EXPECT_EQ(credentials.api_hash[0], '\0');
    }
}

TEST_F(ApplicationCredentialsTest, RejectsMalformedApiHashes)
{
    const std::string valid_hash = validHash();
    const std::string short_hash =
        valid_hash.substr(0, TDLIB_PURPLE_API_HASH_LENGTH - 1);
    const std::string long_hash = valid_hash + "a";
    std::string non_hex_hash = valid_hash;
    non_hex_hash.back() = 'z';

    for (const std::string &api_hash :
         {std::string(), short_hash, long_hash, non_hex_hash})
    {
        TdlibPurpleApplicationCredentials credentials;
        std::memset(&credentials, 0x7f, sizeof(credentials));

        EXPECT_FALSE(
            tdlib_purple_application_credentials_parse_compatibility_override(
            "1", api_hash.c_str(), &credentials));
        EXPECT_EQ(credentials.api_id, 0);
        EXPECT_EQ(credentials.api_hash[0], '\0');
    }
}

TEST_F(ApplicationCredentialsTest, ReturnsImmutableValidEmbeddedPair)
{
    const std::string hash = validHash();
    tdlib_purple_test_application_credentials_set(
        123, hash.data(), hash.size());

    const TdlibPurpleApplicationCredentials *first =
        tdlib_purple_application_credentials_get();
    const TdlibPurpleApplicationCredentials *second =
        tdlib_purple_application_credentials_get();

    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first, second);
    EXPECT_EQ(first->api_id, 123);
    EXPECT_STREQ(first->api_hash, hash.c_str());
}

TEST_F(ApplicationCredentialsTest, RejectsUnavailableOrMalformedEmbeddedPair)
{
    EXPECT_EQ(tdlib_purple_application_credentials_get(), nullptr);

    const std::string hash = validHash();
    tdlib_purple_test_application_credentials_set(
        0, hash.data(), hash.size());
    EXPECT_EQ(tdlib_purple_application_credentials_get(), nullptr);

    std::string unterminated_hash(
        TDLIB_PURPLE_API_HASH_LENGTH + 1, 'a');
    tdlib_purple_test_application_credentials_set(
        1, unterminated_hash.data(), unterminated_hash.size());
    EXPECT_EQ(tdlib_purple_application_credentials_get(), nullptr);

    std::string non_hex_hash = validHash();
    non_hex_hash.front() = 'z';
    tdlib_purple_test_application_credentials_set(
        1, non_hex_hash.data(), non_hex_hash.size());
    EXPECT_EQ(tdlib_purple_application_credentials_get(), nullptr);
}

} // namespace
