#include <gtest/gtest.h>
#include "UdpPortPool.hpp"
#include <set>
#include <stdexcept>

using namespace sdr;

TEST(UdpPortPool, SingleAllocateAndRelease) {
    UdpPortPool pool(30000, 30010);
    EXPECT_EQ(pool.freeCount(), 11);
    EXPECT_EQ(pool.usedCount(),  0);

    int p = pool.allocate();
    EXPECT_EQ(p, 30000);
    EXPECT_EQ(pool.freeCount(), 10);
    EXPECT_EQ(pool.usedCount(),  1);

    pool.release(p);
    EXPECT_EQ(pool.freeCount(), 11);
    EXPECT_EQ(pool.usedCount(),  0);
}

TEST(UdpPortPool, AllocateNReturnsUniquePorts) {
    UdpPortPool pool(40000, 40099);
    auto ports = pool.allocateN(4);
    ASSERT_EQ(ports.size(), 4u);

    std::set<int> ps(ports.begin(), ports.end());
    EXPECT_EQ(ps.size(), 4u);
    for (int p : ports) {
        EXPECT_GE(p, 40000);
        EXPECT_LE(p, 40099);
    }
    EXPECT_EQ(pool.usedCount(), 4);
    EXPECT_EQ(pool.freeCount(), 96);
}

TEST(UdpPortPool, AllocateNFailsWhenExhausted) {
    UdpPortPool pool(50000, 50002);  // 3 ports total
    auto ok = pool.allocateN(3);
    ASSERT_EQ(ok.size(), 3u);

    auto fail = pool.allocateN(1);
    EXPECT_TRUE(fail.empty());

    EXPECT_EQ(pool.allocate(), -1);
}

TEST(UdpPortPool, ReleaseAllReturnsAllPorts) {
    UdpPortPool pool(60000, 60009);
    auto ports = pool.allocateN(5);
    ASSERT_EQ(ports.size(), 5u);

    pool.releaseAll(ports);
    EXPECT_EQ(pool.freeCount(), 10);
    EXPECT_EQ(pool.usedCount(),  0);
}

TEST(UdpPortPool, ReleaseOfUnallocatedPortIsIgnored) {
    UdpPortPool pool(70000, 70005);
    pool.release(99999);
    EXPECT_EQ(pool.freeCount(), 6);
    EXPECT_EQ(pool.usedCount(), 0);
}

TEST(UdpPortPool, ConstructorThrowsIfFirstGeqLast) {
    EXPECT_THROW(UdpPortPool(5000, 4999), std::invalid_argument);
    EXPECT_THROW(UdpPortPool(5000, 5000), std::invalid_argument);
}
