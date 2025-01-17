#include <chrono>
#include <memory>
#include <thread>
#include <gtest/gtest.h>

#include <Storages/DiskCache/Buffer.h>
#include <Storages/DiskCache/Device.h>
#include <Storages/DiskCache/FifoPolicy.h>
#include <Storages/DiskCache/LruPolicy.h>
#include <Storages/DiskCache/Region.h>
#include <Storages/DiskCache/RegionManager.h>
#include <Storages/DiskCache/Types.h>
#include <Storages/DiskCache/tests/BufferGen.h>
#include <Storages/DiskCache/tests/MockDevice.h>
#include <Storages/DiskCache/tests/MockJobScheduler.h>
#include <Storages/DiskCache/tests/SeqPoints.h>
#include <common/types.h>


namespace DB::HybridCache
{
namespace
{
    const Region kRegion0{RegionId{0}, 100};
    const Region kRegion1{RegionId{1}, 100};
    const Region kRegion2{RegionId{2}, 100};
    const Region kRegion3{RegionId{3}, 100};
    constexpr UInt16 kFlushRetryLimit = 10;
}

TEST(RegionManager, ReclaimLruAsFifo)
{
    auto policy = std::make_unique<LruPolicy>(4);
    policy->track(kRegion0);
    policy->track(kRegion1);
    policy->track(kRegion2);
    policy->track(kRegion3);

    constexpr UInt32 k_num_regions = 4;
    constexpr UInt32 k_region_size = 4 * 1024;
    auto device = createMemoryDevice(k_num_regions * k_region_size);
    RegionEvictCallback evict_callback{[](RegionId, BufferView) { return 0; }};
    RegionCleanupCallback cleanup_callback{[](RegionId, BufferView) {}};
    MockJobScheduler ms;
    auto rm = std::make_unique<RegionManager>(
        k_num_regions,
        k_region_size,
        0,
        *device,
        1,
        ms,
        std::move(evict_callback),
        std::move(cleanup_callback),
        std::move(policy),
        k_num_regions,
        0,
        kFlushRetryLimit);

    EXPECT_EQ(kRegion0.id(), rm->evict());
    EXPECT_EQ(kRegion1.id(), rm->evict());
    EXPECT_EQ(kRegion2.id(), rm->evict());
    EXPECT_EQ(kRegion3.id(), rm->evict());
}

TEST(RegionManager, ReclaimLru)
{
    auto policy = std::make_unique<LruPolicy>(4);
    policy->track(kRegion0);
    policy->track(kRegion1);
    policy->track(kRegion2);
    policy->track(kRegion3);

    constexpr UInt32 k_num_regions = 4;
    constexpr UInt32 k_region_size = 4 * 1024;
    auto device = createMemoryDevice(k_num_regions * k_region_size);
    RegionEvictCallback evict_callback{[](RegionId, BufferView) { return 0; }};
    RegionCleanupCallback cleanup_callback{[](RegionId, BufferView) {}};
    MockJobScheduler ms;
    auto rm = std::make_unique<RegionManager>(
        k_num_regions,
        k_region_size,
        0,
        *device,
        1,
        ms,
        std::move(evict_callback),
        std::move(cleanup_callback),
        std::move(policy),
        k_num_regions,
        0,
        kFlushRetryLimit);

    rm->touch(kRegion0.id());
    rm->touch(kRegion1.id());

    EXPECT_EQ(kRegion2.id(), rm->evict());
    EXPECT_EQ(kRegion3.id(), rm->evict());
    EXPECT_EQ(kRegion0.id(), rm->evict());
    EXPECT_EQ(kRegion1.id(), rm->evict());
}


TEST(RegionManager, ReadWrite)
{
    constexpr UInt64 k_base_offset = 1024;
    constexpr UInt32 k_num_regions = 4;
    constexpr UInt32 k_region_size = 4 * 1024;

    auto device = createMemoryDevice(k_base_offset + k_num_regions * k_region_size);
    auto * device_ptr = device.get();
    RegionEvictCallback evict_callback{[](RegionId, BufferView) { return 0; }};
    RegionCleanupCallback cleanup_callback{[](RegionId, BufferView) {}};
    MockJobScheduler ms;
    auto rm = std::make_unique<RegionManager>(
        k_num_regions,
        k_region_size,
        k_base_offset,
        *device,
        1,
        ms,
        std::move(evict_callback),
        std::move(cleanup_callback),
        std::make_unique<FifoPolicy>(),
        k_num_regions,
        0,
        kFlushRetryLimit);

    constexpr UInt32 k_local_offset = 3 * 1024;
    constexpr UInt32 k_size = 1024;
    BufferGen gen;
    RegionId rid;

    rm->startReclaim();
    ASSERT_TRUE(ms.runFirst());
    ASSERT_EQ(OpenStatus::Ready, rm->getCleanRegion(rid));
    ASSERT_EQ(0, rid.index());
    rm->startReclaim();
    ASSERT_TRUE(ms.runFirst());
    ASSERT_EQ(OpenStatus::Ready, rm->getCleanRegion(rid));
    ASSERT_EQ(1, rid.index());

    auto & region = rm->getRegion(rid);
    auto [wdesc, addr] = region.openAndAllocate(4 * k_size);
    EXPECT_EQ(OpenStatus::Ready, wdesc.getStatus());
    auto buf = gen.gen(k_size);
    auto waddr = RelAddress{rid, k_local_offset};
    rm->write(waddr, buf.copy());
    auto rdesc = rm->openForRead(rid, 1);
    auto buf_read = rm->read(rdesc, waddr, k_size);
    EXPECT_TRUE(buf_read.size() == k_size);
    EXPECT_EQ(buf.view(), buf_read.view());

    region.close(std::move(wdesc));
    EXPECT_EQ(Region::FlushRes::kSuccess, rm->flushBuffer(rid));
    auto expected_offset = k_base_offset + k_region_size + k_local_offset;
    Buffer buf_read_direct{k_size};
    EXPECT_TRUE(device_ptr->read(expected_offset, k_size, buf_read_direct.data()));
    EXPECT_EQ(buf.view(), buf_read_direct.view());
}

using testing::_;
using testing::Return;
TEST(RegionManager, cleanupRegionFailureSync)
{
    constexpr UInt32 k_num_regions = 4;
    constexpr UInt32 k_region_size = 4096;
    constexpr UInt16 k_num_in_mem_buffer = 2;
    auto device = std::make_unique<MockDevice>(k_num_regions * k_region_size, 1024);
    auto policy = std::make_unique<LruPolicy>(k_num_regions);
    MockJobScheduler js;
    RegionEvictCallback evict_callback{[](RegionId, BufferView) { return 0; }};
    RegionCleanupCallback cleanup_callback{[](RegionId, BufferView) {}};
    auto rm = std::make_unique<RegionManager>(
        k_num_regions,
        k_region_size,
        0,
        *device,
        1,
        js,
        std::move(evict_callback),
        std::move(cleanup_callback),
        std::move(policy),
        k_num_in_mem_buffer,
        0,
        kFlushRetryLimit);

    BufferGen generator;
    RegionId rid;
    rm->startReclaim();
    ASSERT_TRUE(js.runFirst());
    ASSERT_EQ(OpenStatus::Ready, rm->getCleanRegion(rid));
    ASSERT_EQ(0, rid.index());

    auto & region = rm->getRegion(rid);
    auto [wdesc, addr] = region.openAndAllocate(k_region_size);
    ASSERT_EQ(OpenStatus::Ready, wdesc.getStatus());
    auto buf = generator.gen(1024);
    auto waddr = RelAddress{rid, 0};
    rm->write(waddr, buf.copy());
    region.close(std::move(wdesc));

    SeqPoints sp;
    std::thread read_thread([&sp, &region] {
        auto rdesc = region.openForRead();
        EXPECT_EQ(OpenStatus::Ready, rdesc.getStatus());
        sp.reached(0);

        sp.wait(1);
        region.close(std::move(rdesc));
    });

    std::thread flush_thread([&sp, &device, &rm, &rid] {
        EXPECT_CALL(*device, writeImpl(_, _, _)).WillRepeatedly(Return(false));
        sp.wait(0);
        rm->doFlush(rid, false);
    });

    std::thread cthread([&sp] {
        for (int i = 0; i < 20; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        sp.reached(1);
    });

    read_thread.join();
    flush_thread.join();
    cthread.join();
}

TEST(RegionManager, cleanupRegionFailureAsync)
{
    constexpr UInt32 k_num_regions = 4;
    constexpr UInt32 k_region_size = 4096;
    constexpr UInt16 k_num_in_mem_buffer = 2;
    auto device = std::make_unique<MockDevice>(k_num_regions * k_region_size, 1024);
    auto policy = std::make_unique<LruPolicy>(k_num_regions);
    MockJobScheduler js;
    RegionEvictCallback evict_callback{[](RegionId, BufferView) { return 0; }};
    RegionCleanupCallback cleanup_callback{[](RegionId, BufferView) {}};
    auto rm = std::make_unique<RegionManager>(
        k_num_regions,
        k_region_size,
        0,
        *device,
        1,
        js,
        std::move(evict_callback),
        std::move(cleanup_callback),
        std::move(policy),
        k_num_in_mem_buffer,
        0,
        kFlushRetryLimit);

    BufferGen generator;
    RegionId rid;
    rm->startReclaim();

    ASSERT_TRUE(js.runFirst());
    ASSERT_EQ(OpenStatus::Ready, rm->getCleanRegion(rid));
    ASSERT_EQ(0, rid.index());

    auto & region = rm->getRegion(rid);
    auto [wdesc, addr] = region.openAndAllocate(k_region_size);
    EXPECT_EQ(OpenStatus::Ready, wdesc.getStatus());
    auto buf = generator.gen(1024);
    auto waddr = RelAddress{rid, 0};
    rm->write(waddr, buf.copy());
    region.close(std::move(wdesc));

    SeqPoints sp;
    std::thread read_thread([&sp, &region]{
        auto rdesc = region.openForRead();
        EXPECT_EQ(OpenStatus::Ready, rdesc.getStatus());
        sp.reached(0);

        sp.wait(1);
        region.close(std::move(rdesc));
    });

    std::thread flush_thread([&sp, &device, &rm, &rid, &js] {
        EXPECT_CALL(*device, writeImpl(_, _, _)).WillRepeatedly(Return(false));
        sp.wait(0);
        rm->doFlush(rid, true);
        while(js.getQueueSize() > 0)
            js.runFirst();
    });

    std::thread cthread([&sp] {
        for (int i = 0; i < 20; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        sp.reached(1);
    });

    read_thread.join();
    flush_thread.join();
    cthread.join();
}
}
