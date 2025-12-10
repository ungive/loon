#include "test/common.hpp"

using namespace testing;

TEST(SharedReferenceCounter, AnyMethodThrowsWithNullPointer)
{
    loon::SharedReferenceCounter m;
    EXPECT_ANY_THROW(m.add(nullptr));
    EXPECT_ANY_THROW(m.remove(nullptr));
    EXPECT_ANY_THROW(m.count(nullptr));
}

TEST(SharedReferenceCounter, AddReturnsConsecutiveUniqueValues)
{
    loon::SharedReferenceCounter m;
    auto client = create_client(false);
    EXPECT_EQ(0, m.add(client));
    EXPECT_EQ(1, m.add(client));
    EXPECT_EQ(2, m.add(client));
    EXPECT_EQ(3, m.add(client));
    EXPECT_EQ(4, m.add(client));
}

TEST(SharedReferenceCounter, AddAfterRemoveContinuesToReturnConsecutiveValues)
{
    loon::SharedReferenceCounter m;
    auto client = create_client(false);
    EXPECT_EQ(0, m.add(client));
    EXPECT_EQ(1, m.add(client));
    EXPECT_EQ(2, m.add(client));
    m.remove(client);
    EXPECT_EQ(3, m.add(client));
    m.remove(client);
    EXPECT_EQ(4, m.add(client));
}

TEST(SharedReferenceCounter, CountReflectsTheCurrentReferenceCount)
{
    loon::SharedReferenceCounter m;
    auto client = create_client(false);
    for (size_t i = 0; i < 16; i++) {
        EXPECT_EQ(i, m.count(client));
        EXPECT_EQ(i, m.add(client));
        EXPECT_EQ(i + 1, m.count(client));
    }
}

TEST(SharedReferenceCounter, EqualAmountOfRemoveCallsResetsCountToZero)
{
    loon::SharedReferenceCounter m;
    for (size_t i = 1; i < 16; i++) {
        auto client = create_client(false);
        for (size_t j = 0; j < i; j++) {
            EXPECT_EQ(j, m.add(client));
        }
        for (size_t j = 0; j < i; j++) {
            m.remove(client);
            EXPECT_EQ(i - j - 1, m.count(client));
        }
    }
}

TEST(SharedReferenceCounter, RemovingNonExistentClientCausesDeath)
{
    {
        loon::SharedReferenceCounter m;
        auto client = create_client(false);
        EXPECT_DEATH(m.remove(client), "");
    }
    {
        loon::SharedReferenceCounter m;
        auto client = create_client(false);
        m.add(client);
        m.remove(client);
        EXPECT_DEATH(m.remove(client), "");
    }
}

TEST(SharedReferenceCounter, CountWithRequiredCausesDeathWhenThereIsNoClient)
{
    {
        loon::SharedReferenceCounter m;
        auto client = create_client(false);
        EXPECT_DEATH(m.count(client, true), "");
    }
    {
        loon::SharedReferenceCounter m;
        auto client = create_client(false);
        // This should not fail.
        m.count(client, false);
    }
}

TEST(SharedReferenceCounter, RemoveReturnsReferenceCountBeforeRemoveCall)
{
    loon::SharedReferenceCounter m;
    for (size_t i = 1; i < 16; i++) {
        auto client = create_client(false);
        for (size_t j = 0; j < i; j++) {
            EXPECT_EQ(j, m.add(client));
        }
        for (size_t j = 0; j < i; j++) {
            auto a = m.count(client);
            auto b = m.remove(client);
            auto c = m.count(client);
            EXPECT_EQ(a, b);
            EXPECT_EQ(a - 1, c);
        }
    }
}

TEST(SharedReferenceCounter, EraseReferencesRemovesAllReferences)
{
    loon::SharedReferenceCounter m;
    auto client = create_client(false);
    m.add(client);
    m.add(client);
    m.add(client);
    EXPECT_TRUE(m.erase_references(client));
    EXPECT_EQ(0, m.count(client));
}
