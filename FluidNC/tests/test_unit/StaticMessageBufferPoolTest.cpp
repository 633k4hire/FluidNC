#include <gtest/gtest.h>

#include "WebUI/StaticMessageBufferPool.h"

#include <cstring>

TEST(StaticMessageBufferPool, TransferredLeaseRemainsReservedUntilCompletion) {
    using Pool = StaticMessageBufferPool<32, 1>;
    Pool pool;

    auto first = pool.tryAcquire();
    ASSERT_TRUE(first);

    std::strcpy(first.data(), "queued telemetry");
    char* queued_bytes = first.data();
    void* queued = first.transfer();

    EXPECT_FALSE(pool.tryAcquire());
    EXPECT_STREQ(queued_bytes, "queued telemetry");

    Pool::release(queued);
    auto reused = pool.tryAcquire();
    ASSERT_TRUE(reused);
    EXPECT_EQ(reused.data(), queued_bytes);
}
