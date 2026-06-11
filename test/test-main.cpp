#include <purple.h>
#include <stdio.h>
#include <gtest/gtest.h>
#include <td/telegram/td_api.h>
using namespace td::td_api;

class TestConfig : public ::testing::Environment
{
public:
    virtual void SetUp() override
    {
    }

    virtual void TearDown() override
    {
    }
};

int main(int argc, char * argv[])
{
	::testing::InitGoogleTest(&argc, argv);
	::testing::AddGlobalTestEnvironment(new TestConfig);
	return RUN_ALL_TESTS();
}
