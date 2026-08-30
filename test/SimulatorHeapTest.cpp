#include "src/SimulatorHeap.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <string>

#if defined(__APPLE__)
#include <malloc/malloc.h>

extern "C" void *__real_realloc(void *ptr, std::size_t size);
extern "C" void __real_free(void *ptr);
#endif

namespace fs = std::filesystem;

namespace {

class SimulatorHeapTest : public ::testing::Test {
protected:
  void SetUp() override {
    unsetenv("CROSSPOINT_SIM_HEAP_VIZ");
    SimulatorHeap::shutdownForTests();
  }

  void TearDown() override {
    SimulatorHeap::shutdownForTests();
    unsetenv("CROSSPOINT_SIM_HEAP_VIZ");
  }
};

TEST_F(SimulatorHeapTest, FreshArenaReportsLiveMetrics) {
  ASSERT_TRUE(SimulatorHeap::resetForTests(4096));

  const std::size_t total = SimulatorHeap::totalBytes();
  const std::size_t free = SimulatorHeap::freeBytes();

  EXPECT_TRUE(SimulatorHeap::isActive());
  EXPECT_EQ(total, 4096U);
  EXPECT_GT(free, 0U);
  EXPECT_LT(free, total);
  EXPECT_EQ(SimulatorHeap::minFreeBytes(), free);
  EXPECT_LE(SimulatorHeap::largestFreeBlockBytes(), free);
}

TEST_F(SimulatorHeapTest, ShutdownDeactivatesArena) {
  ASSERT_TRUE(SimulatorHeap::resetForTests(4096));
  SimulatorHeap::shutdownForTests();

  EXPECT_FALSE(SimulatorHeap::isActive());
  EXPECT_FALSE(SimulatorHeap::isInitialized());
}

#if defined(__APPLE__)
TEST_F(SimulatorHeapTest, HostAllocatorUsesOwningMallocZone) {
  malloc_zone_t *zone = malloc_create_zone(0, 0);
  ASSERT_NE(zone, nullptr);

  void *ptr = malloc_zone_malloc(zone, 64);
  ASSERT_NE(ptr, nullptr);
  ptr = __real_realloc(ptr, 128);
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(malloc_zone_from_ptr(ptr), zone);

  __real_free(ptr);
  malloc_destroy_zone(zone);
}
#endif

TEST_F(SimulatorHeapTest, AllocateAndFreeRestoresFreeBytes) {
  ASSERT_TRUE(SimulatorHeap::resetForTests(4096));
  const std::size_t baselineFree = SimulatorHeap::freeBytes();
  const std::size_t baselineMaxAlloc = SimulatorHeap::largestFreeBlockBytes();

  void *ptr = SimulatorHeap::allocateForTests(128);
  ASSERT_NE(ptr, nullptr);
  EXPECT_LT(SimulatorHeap::freeBytes(), baselineFree);
  EXPECT_LT(SimulatorHeap::minFreeBytes(), baselineFree);

  SimulatorHeap::freeForTests(ptr);
  EXPECT_EQ(SimulatorHeap::freeBytes(), baselineFree);
  EXPECT_EQ(SimulatorHeap::largestFreeBlockBytes(), baselineMaxAlloc);
}

TEST_F(SimulatorHeapTest, CoalescingMergesAdjacentFreeBlocks) {
  ASSERT_TRUE(SimulatorHeap::resetForTests(4096));
  const std::size_t baselineFree = SimulatorHeap::freeBytes();
  const std::size_t baselineMaxAlloc = SimulatorHeap::largestFreeBlockBytes();

  void *a = SimulatorHeap::allocateForTests(512);
  void *b = SimulatorHeap::allocateForTests(512);
  void *c = SimulatorHeap::allocateForTests(512);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  ASSERT_NE(c, nullptr);

  SimulatorHeap::freeForTests(b);
  EXPECT_LT(SimulatorHeap::largestFreeBlockBytes(), baselineMaxAlloc);

  SimulatorHeap::freeForTests(a);
  void *mergedFront = SimulatorHeap::allocateForTests(1024);
  EXPECT_NE(mergedFront, nullptr);
  SimulatorHeap::freeForTests(mergedFront);

  SimulatorHeap::freeForTests(c);
  EXPECT_EQ(SimulatorHeap::freeBytes(), baselineFree);
  EXPECT_EQ(SimulatorHeap::largestFreeBlockBytes(), baselineMaxAlloc);
}

TEST_F(SimulatorHeapTest, ReallocShrinkAndGrowPreservesContent) {
  ASSERT_TRUE(SimulatorHeap::resetForTests(8192));

  auto *ptr = static_cast<std::uint8_t *>(SimulatorHeap::allocateForTests(512));
  ASSERT_NE(ptr, nullptr);
  std::memset(ptr, 0xAB, 512);

  const std::size_t freeAfterAlloc = SimulatorHeap::freeBytes();
  auto *shrunk =
      static_cast<std::uint8_t *>(SimulatorHeap::reallocForTests(ptr, 128));
  ASSERT_NE(shrunk, nullptr);
  EXPECT_GT(SimulatorHeap::freeBytes(), freeAfterAlloc);
  for (std::size_t i = 0; i < 128; ++i)
    EXPECT_EQ(shrunk[i], 0xAB);

  auto *grown =
      static_cast<std::uint8_t *>(SimulatorHeap::reallocForTests(shrunk, 1024));
  ASSERT_NE(grown, nullptr);
  for (std::size_t i = 0; i < 128; ++i)
    EXPECT_EQ(grown[i], 0xAB);
  SimulatorHeap::freeForTests(grown);
}

TEST_F(SimulatorHeapTest, ReallocMovePreservesContentAndReleasesOldBlock) {
  ASSERT_TRUE(SimulatorHeap::resetForTests(8192));
  const std::size_t baselineFree = SimulatorHeap::freeBytes();

  auto *a = static_cast<std::uint8_t *>(SimulatorHeap::allocateForTests(1024));
  auto *b = static_cast<std::uint8_t *>(SimulatorHeap::allocateForTests(1024));
  auto *c = static_cast<std::uint8_t *>(SimulatorHeap::allocateForTests(1024));
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  ASSERT_NE(c, nullptr);
  std::memset(b, 0x5A, 1024);

  auto *moved =
      static_cast<std::uint8_t *>(SimulatorHeap::reallocForTests(b, 1536));
  ASSERT_NE(moved, nullptr);
  EXPECT_NE(moved, b);
  for (std::size_t i = 0; i < 1024; ++i)
    EXPECT_EQ(moved[i], 0x5A);

  SimulatorHeap::freeForTests(a);
  SimulatorHeap::freeForTests(moved);
  SimulatorHeap::freeForTests(c);
  EXPECT_EQ(SimulatorHeap::freeBytes(), baselineFree);
}

TEST_F(SimulatorHeapTest, ReallocAlignedFallbackPreservesContentAndAlignment) {
  ASSERT_TRUE(SimulatorHeap::resetForTests(8192));

  auto *ptr = static_cast<std::uint8_t *>(SimulatorHeap::allocateForTests(512));
  ASSERT_NE(ptr, nullptr);
  std::memset(ptr, 0xC3, 512);

  auto *realigned = static_cast<std::uint8_t *>(
      SimulatorHeap::reallocAlignedForTests(ptr, 256, 64));
  ASSERT_NE(realigned, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(realigned) % 64U, 0U);
  for (std::size_t i = 0; i < 256; ++i)
    EXPECT_EQ(realigned[i], 0xC3);
  SimulatorHeap::freeForTests(realigned);
}

TEST_F(SimulatorHeapTest, HostScopeKeepsArenaOwnedReallocInArena) {
  ASSERT_TRUE(SimulatorHeap::resetForTests(8192));
  const std::size_t baselineFree = SimulatorHeap::freeBytes();

  auto *ptr = static_cast<std::uint8_t *>(SimulatorHeap::allocateForTests(256));
  ASSERT_NE(ptr, nullptr);
  std::memset(ptr, 0x7C, 256);

  auto *grown = static_cast<std::uint8_t *>(
      SimulatorHeap::reallocInHostScopeForTests(ptr, 512));
  ASSERT_NE(grown, nullptr);
  for (std::size_t i = 0; i < 256; ++i)
    EXPECT_EQ(grown[i], 0x7C);

  SimulatorHeap::freeForTests(grown);
  EXPECT_EQ(SimulatorHeap::freeBytes(), baselineFree);
}

TEST_F(SimulatorHeapTest,
       FragmentationCanBlockLargeAllocationWithFreeBytesRemaining) {
  ASSERT_TRUE(SimulatorHeap::resetForTests(4096));
  const std::size_t chunkSize = SimulatorHeap::largestFreeBlockBytes() / 5U;
  const std::size_t largeRequest = chunkSize * 2U;

  void *a = SimulatorHeap::allocateForTests(chunkSize);
  void *b = SimulatorHeap::allocateForTests(chunkSize);
  void *c = SimulatorHeap::allocateForTests(chunkSize);
  void *d = SimulatorHeap::allocateForTests(chunkSize);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  ASSERT_NE(c, nullptr);
  ASSERT_NE(d, nullptr);

  SimulatorHeap::freeForTests(a);
  SimulatorHeap::freeForTests(c);
  EXPECT_GT(SimulatorHeap::freeBytes(), largeRequest);
  EXPECT_LT(SimulatorHeap::largestFreeBlockBytes(), largeRequest);
  EXPECT_EQ(SimulatorHeap::allocateForTests(largeRequest), nullptr);

  SimulatorHeap::freeForTests(b);
  void *merged = SimulatorHeap::allocateForTests(largeRequest);
  EXPECT_NE(merged, nullptr);
  SimulatorHeap::freeForTests(merged);
  SimulatorHeap::freeForTests(d);
}

TEST_F(SimulatorHeapTest, CallocZeroesMemoryAndRejectsOverflow) {
  ASSERT_TRUE(SimulatorHeap::resetForTests(4096));

  auto *ptr = static_cast<std::uint32_t *>(
      SimulatorHeap::callocForTests(16, sizeof(std::uint32_t)));
  ASSERT_NE(ptr, nullptr);
  for (std::size_t i = 0; i < 16; ++i)
    EXPECT_EQ(ptr[i], 0U);
  SimulatorHeap::freeForTests(ptr);

  EXPECT_EQ(
      SimulatorHeap::callocForTests(std::numeric_limits<std::size_t>::max(), 2),
      nullptr);
}

TEST_F(SimulatorHeapTest, ZeroSizeAllocationApisReturnFreeableMinimumObjects) {
  ASSERT_TRUE(SimulatorHeap::resetForTests(4096));
  const std::size_t baselineFree = SimulatorHeap::freeBytes();

  void *plain = SimulatorHeap::allocateForTests(0);
  void *aligned = SimulatorHeap::allocateAlignedForTests(0, 64);
  void *zeroed = SimulatorHeap::callocForTests(0, 0);
  void *resized = SimulatorHeap::reallocForTests(nullptr, 0);
  ASSERT_NE(plain, nullptr);
  ASSERT_NE(aligned, nullptr);
  ASSERT_NE(zeroed, nullptr);
  ASSERT_NE(resized, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(aligned) % 64U, 0U);

  SimulatorHeap::freeForTests(plain);
  SimulatorHeap::freeForTests(aligned);
  SimulatorHeap::freeForTests(zeroed);
  SimulatorHeap::freeForTests(resized);
  EXPECT_EQ(SimulatorHeap::freeBytes(), baselineFree);
}

TEST_F(SimulatorHeapTest, InvalidAlignmentIsRejected) {
  ASSERT_TRUE(SimulatorHeap::resetForTests(4096));
  EXPECT_EQ(SimulatorHeap::allocateAlignedForTests(64, 24), nullptr);
}

TEST_F(SimulatorHeapTest, CppAllocationHelpersUseArenaAndRestoreIt) {
  ASSERT_TRUE(SimulatorHeap::resetForTests(4096));
  const std::size_t baselineFree = SimulatorHeap::freeBytes();

  void *plain = SimulatorHeap::cppNewForTests(128);
  ASSERT_NE(plain, nullptr);
  EXPECT_LT(SimulatorHeap::freeBytes(), baselineFree);
  SimulatorHeap::freeForTests(plain);
  EXPECT_EQ(SimulatorHeap::freeBytes(), baselineFree);

  EXPECT_EQ(SimulatorHeap::cppNewNoThrowForTests(baselineFree), nullptr);
  EXPECT_THROW(SimulatorHeap::cppNewForTests(baselineFree), std::bad_alloc);
}

TEST_F(SimulatorHeapTest, AlignedCppHelpersUseArena) {
  ASSERT_TRUE(SimulatorHeap::resetForTests(4096));

  void *aligned = SimulatorHeap::cppAlignedNewForTests(64, 64);
  void *alignedNoThrow = SimulatorHeap::cppAlignedNewNoThrowForTests(64, 128);
  ASSERT_NE(aligned, nullptr);
  ASSERT_NE(alignedNoThrow, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(aligned) % 64U, 0U);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(alignedNoThrow) % 128U, 0U);
  SimulatorHeap::freeForTests(aligned);
  SimulatorHeap::freeForTests(alignedNoThrow);
}

TEST_F(SimulatorHeapTest, ManualVisualizationDumpWritesInteractiveSvg) {
  const fs::path outputDir =
      fs::temp_directory_path() / "crosspoint-sim-heap-viz-test-manual";
  fs::remove_all(outputDir);
  setenv("CROSSPOINT_SIM_HEAP_VIZ", outputDir.c_str(), 1);

  ASSERT_TRUE(SimulatorHeap::resetForTests(4096));
  ASSERT_NE(SimulatorHeap::allocateForTests(128), nullptr);
  SimulatorHeap::dumpVisualizationForTests("manual_test");

  fs::path svgPath;
  for (const auto &entry : fs::directory_iterator(outputDir)) {
    if (entry.path().extension() == ".svg" &&
        entry.path().filename().string().find("manual_test") !=
            std::string::npos) {
      svgPath = entry.path();
      break;
    }
  }
  ASSERT_FALSE(svgPath.empty());

  std::ifstream svg(svgPath);
  const std::string content((std::istreambuf_iterator<char>(svg)),
                            std::istreambuf_iterator<char>());
  EXPECT_NE(content.find("reason: manual_test"), std::string::npos);
  EXPECT_NE(content.find("<pre class=\"detail\" id=\"detail\""),
            std::string::npos);
  fs::remove_all(outputDir);
}

TEST_F(SimulatorHeapTest, VisualizationDimensionsUseSquareRootArenaWidth) {
  const fs::path outputDir =
      fs::temp_directory_path() / "crosspoint-sim-heap-viz-test-geometry";
  fs::remove_all(outputDir);
  setenv("CROSSPOINT_SIM_HEAP_VIZ", outputDir.c_str(), 1);

  ASSERT_TRUE(SimulatorHeap::resetForTests(1024));
  ASSERT_NE(SimulatorHeap::allocateForTests(64), nullptr);
  SimulatorHeap::dumpVisualizationForTests("geometry");

  fs::path svgPath;
  for (const auto &entry : fs::directory_iterator(outputDir)) {
    if (entry.path().extension() == ".svg") {
      svgPath = entry.path();
      break;
    }
  }
  ASSERT_FALSE(svgPath.empty());

  std::ifstream svg(svgPath);
  const std::string content((std::istreambuf_iterator<char>(svg)),
                            std::istreambuf_iterator<char>());
  EXPECT_NE(content.find("width=\"96\" height=\"96\""), std::string::npos);
  fs::remove_all(outputDir);
}

} // namespace
