#include "esp_heap_summary.h"
#ifdef ESP_PLATFORM
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <inttypes.h>
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#endif

#ifdef ESP_PLATFORM
namespace
{
constexpr size_t HEAP_REGION_COUNT_MAX = 16;

//===================================================================================
//===================================================================================
// Stores statistics accumulated for one independently registered heap region.
struct HeapRegionStats
{
  intptr_t start = 0;
  intptr_t end = 0;
  uint32_t capabilities = 0;
  size_t freeBytes = 0;
  size_t allocatedBytes = 0;
  size_t largestFreeBlock = 0;
  size_t allocatedBlocks = 0;
  size_t freeBlocks = 0;
};

//===================================================================================
//===================================================================================
// Owns the fixed-capacity region list used while the allocator is being walked.
struct HeapTableStats
{
  HeapRegionStats regions[HEAP_REGION_COUNT_MAX];
  size_t regionCount = 0;
};

//===================================================================================
//===================================================================================
// Carries one capability flag through a filtered heap walk.
struct HeapCapabilityPass
{
  HeapTableStats& table;
  uint32_t capability;
};

//===================================================================================
//===================================================================================
// Finds or creates the fixed-size statistics slot for one registered heap region.
HeapRegionStats* findHeapRegion(HeapTableStats& table, walker_heap_into_t heapInfo)
{
  for (size_t i = 0; i < table.regionCount; ++i)
  {
    if (table.regions[i].start == heapInfo.start)
    {
      return &table.regions[i];
    }
  }

  if (table.regionCount >= HEAP_REGION_COUNT_MAX)
  {
    return nullptr;
  }

  HeapRegionStats& region = table.regions[table.regionCount++];
  region.start = heapInfo.start;
  region.end = heapInfo.end;
  return &region;
}

//===================================================================================
//===================================================================================
// Collects allocation and fragmentation statistics without allocating while walking heaps.
bool collectHeapBlock(walker_heap_into_t heapInfo, walker_block_info_t blockInfo, void* userData)
{
  HeapTableStats& table = *static_cast<HeapTableStats*>(userData);
  HeapRegionStats* region = findHeapRegion(table, heapInfo);
  if (region == nullptr)
  {
    return false;
  }

  if (blockInfo.used)
  {
    region->allocatedBytes += blockInfo.size;
    ++region->allocatedBlocks;
  }
  else
  {
    region->freeBytes += blockInfo.size;
    region->largestFreeBlock = std::max(region->largestFreeBlock, blockInfo.size);
    ++region->freeBlocks;
  }
  return true;
}

//===================================================================================
//===================================================================================
// Marks a heap region as satisfying one capability requested from ESP-IDF.
bool markHeapCapability(walker_heap_into_t heapInfo, walker_block_info_t, void* userData)
{
  HeapCapabilityPass& pass = *static_cast<HeapCapabilityPass*>(userData);
  HeapRegionStats* region = findHeapRegion(pass.table, heapInfo);
  if (region != nullptr)
  {
    region->capabilities |= pass.capability;
  }
  return false;
}

//===================================================================================
//===================================================================================
// Separates external PSRAM from all on-chip SRAM regions, including IRAM and RTC fast RAM.
const char* getHeapRegionName(intptr_t address)
{
  const void* ptr = reinterpret_cast<const void*>(address);
  if (esp_ptr_external_ram(ptr))
  {
    return "PSRAM";
  }
  return "SRAM";
}

//===================================================================================
//===================================================================================
// Formats byte counts compactly with binary units and one fractional digit when useful.
void formatMemorySize(size_t bytes, char* output, size_t outputSize)
{
  constexpr size_t KIB = 1024;
  constexpr size_t MIB = 1024 * KIB;
  if (bytes >= MIB)
  {
    std::snprintf(output, outputSize, "%zu.%zu MiB", bytes / MIB, (bytes % MIB) * 10 / MIB);
  }
  else if (bytes >= KIB)
  {
    std::snprintf(output, outputSize, "%zu.%zu KiB", bytes / KIB, (bytes % KIB) * 10 / KIB);
  }
  else
  {
    std::snprintf(output, outputSize, "%zu B", bytes);
  }
}

//===================================================================================
//===================================================================================
// Formats a signed byte-count change using the same compact binary units as totals.
void formatMemorySizeDifference(int64_t bytes, char* output, size_t outputSize)
{
  if (outputSize == 0)
  {
    return;
  }

  if (bytes == 0)
  {
    std::snprintf(output, outputSize, "0 B");
    return;
  }

  const char sign = bytes > 0 ? '+' : '-';
  const uint64_t magnitude = bytes > 0
    ? static_cast<uint64_t>(bytes)
    : static_cast<uint64_t>(-(bytes + 1)) + 1;
  output[0] = sign;
  if (outputSize == 1)
  {
    output[0] = 0;
    return;
  }
  formatMemorySize(static_cast<size_t>(magnitude), output + 1, outputSize - 1);
}

//===================================================================================
//===================================================================================
// Formats the capability flags relevant to the firmware's heap diagnostics.
void formatHeapCapabilities(uint32_t capabilities, char* output, size_t outputSize)
{
  output[0] = 0;
  auto appendCapability = [output, outputSize](const char* name)
  {
    if (output[0] != 0)
    {
      std::strncat(output, ",", outputSize - std::strlen(output) - 1);
    }
    std::strncat(output, name, outputSize - std::strlen(output) - 1);
  };

  if ((capabilities & MALLOC_CAP_EXEC) != 0)
  {
    appendCapability("EXEC");
  }
  if ((capabilities & MALLOC_CAP_8BIT) != 0)
  {
    appendCapability("8BIT");
  }
  if ((capabilities & MALLOC_CAP_DMA) != 0)
  {
    appendCapability("DMA");
  }
}
}

//===================================================================================
//===================================================================================
// Prints every byte-addressable data heap once with its additional capabilities.
void printHeapInfoTable()
{
  HeapTableStats table;
  heap_caps_walk(MALLOC_CAP_8BIT, collectHeapBlock, &table);

  // Keep 32-bit-only storage out of the region list, but report its aggregate below.
  HeapTableStats data32BitTable;
  heap_caps_walk(MALLOC_CAP_32BIT, collectHeapBlock, &data32BitTable);

  constexpr uint32_t displayedCapabilities[] =
  {
    MALLOC_CAP_EXEC,
    MALLOC_CAP_8BIT,
    MALLOC_CAP_DMA,
  };
  for (uint32_t capability : displayedCapabilities)
  {
    HeapCapabilityPass pass = {table, capability};
    heap_caps_walk(capability, markHeapCapability, &pass);
  }

  std::sort(table.regions, table.regions + table.regionCount,
            [](const HeapRegionStats& lhs, const HeapRegionStats& rhs)
            {
              return lhs.start < rhs.start;
            });

  printf("\033[34mHeap summary\n");
  printf("%-16s %-18s %10s %8s %8s %10s %10s\n",
         "Region", "Capabilities", "Size", "AllocBlk", "FreeBlk", "Free", "Largest");
  printf("-------------------------------------------------------------------------------------\n");

  size_t sramTotalSize = 0;
  size_t sramTotalFree = 0;
  size_t sramLargestFreeBlock = 0;
  size_t sramAllocatedBlocks = 0;
  size_t sramFreeBlocks = 0;
  size_t psramTotalSize = 0;
  size_t psramTotalFree = 0;
  size_t psramLargestFreeBlock = 0;
  size_t psramAllocatedBlocks = 0;
  size_t psramFreeBlocks = 0;
  size_t data32BitSramTotalSize = 0;
  size_t data32BitSramTotalFree = 0;
  size_t data32BitSramLargestFreeBlock = 0;
  size_t data32BitSramAllocatedBlocks = 0;
  size_t data32BitSramFreeBlocks = 0;
  for (size_t i = 0; i < table.regionCount; ++i)
  {
    const HeapRegionStats& region = table.regions[i];
    const size_t regionSize = static_cast<size_t>(region.end - region.start);
    const char* regionName = getHeapRegionName(region.start);
    char sizeText[16];
    char freeText[16];
    char largestText[16];
    char capabilitiesText[24];
    formatMemorySize(regionSize, sizeText, sizeof(sizeText));
    formatMemorySize(region.freeBytes, freeText, sizeof(freeText));
    formatMemorySize(region.largestFreeBlock, largestText, sizeof(largestText));
    formatHeapCapabilities(region.capabilities, capabilitiesText, sizeof(capabilitiesText));
    printf("%-16s %-18s %10s %8zu %8zu %10s %10s\n",
           regionName, capabilitiesText, sizeText,
           region.allocatedBlocks, region.freeBlocks, freeText, largestText);

    if (std::strcmp(regionName, "SRAM") == 0)
    {
      sramTotalSize += regionSize;
      sramTotalFree += region.freeBytes;
      sramLargestFreeBlock = std::max(sramLargestFreeBlock, region.largestFreeBlock);
      sramAllocatedBlocks += region.allocatedBlocks;
      sramFreeBlocks += region.freeBlocks;
    }
    else if (std::strcmp(regionName, "PSRAM") == 0)
    {
      psramTotalSize += regionSize;
      psramTotalFree += region.freeBytes;
      psramLargestFreeBlock = std::max(psramLargestFreeBlock, region.largestFreeBlock);
      psramAllocatedBlocks += region.allocatedBlocks;
      psramFreeBlocks += region.freeBlocks;
    }
  }

  for (size_t i = 0; i < data32BitTable.regionCount; ++i)
  {
    const HeapRegionStats& region = data32BitTable.regions[i];
    bool isByteAddressable = false;
    for (size_t dataRegionIndex = 0; dataRegionIndex < table.regionCount; ++dataRegionIndex)
    {
      if (table.regions[dataRegionIndex].start == region.start)
      {
        isByteAddressable = true;
        break;
      }
    }

    if (isByteAddressable || esp_ptr_external_ram(reinterpret_cast<const void*>(region.start)))
    {
      continue;
    }

    data32BitSramTotalSize += static_cast<size_t>(region.end - region.start);
    data32BitSramTotalFree += region.freeBytes;
    data32BitSramLargestFreeBlock = std::max(data32BitSramLargestFreeBlock, region.largestFreeBlock);
    data32BitSramAllocatedBlocks += region.allocatedBlocks;
    data32BitSramFreeBlocks += region.freeBlocks;
  }

  char sizeText[16];
  char freeText[16];
  char largestText[16];
  printf("-------------------------------------------------------------------------------------\n");

  formatMemorySize(sramTotalSize, sizeText, sizeof(sizeText));
  formatMemorySize(sramTotalFree, freeText, sizeof(freeText));
  formatMemorySize(sramLargestFreeBlock, largestText, sizeof(largestText));
  printf("%-16s %-18s %10s %8zu %8zu %10s %10s\n",
         "TOTAL SRAM", "-", sizeText, sramAllocatedBlocks, sramFreeBlocks, freeText, largestText);

  formatMemorySize(psramTotalSize, sizeText, sizeof(sizeText));
  formatMemorySize(psramTotalFree, freeText, sizeof(freeText));
  formatMemorySize(psramLargestFreeBlock, largestText, sizeof(largestText));
  printf("%-16s %-18s %10s %8zu %8zu %10s %10s\n",
         "TOTAL PSRAM", "-", sizeText, psramAllocatedBlocks, psramFreeBlocks, freeText, largestText);

  formatMemorySize(data32BitSramTotalSize, sizeText, sizeof(sizeText));
  formatMemorySize(data32BitSramTotalFree, freeText, sizeof(freeText));
  formatMemorySize(data32BitSramLargestFreeBlock, largestText, sizeof(largestText));
  printf("%-16s %-18s %10s %8zu %8zu %10s %10s\n",
         "TOTAL 32BIT SRAM", "32BIT only", sizeText,
         data32BitSramAllocatedBlocks, data32BitSramFreeBlocks, freeText, largestText);

  static bool hasPreviousTotals = false;
  static size_t previousSramTotalSize = 0;
  static size_t previousSramTotalFree = 0;
  static size_t previousSramLargestFreeBlock = 0;
  static size_t previousSramAllocatedBlocks = 0;
  static size_t previousSramFreeBlocks = 0;
  static size_t previousPsramTotalSize = 0;
  static size_t previousPsramTotalFree = 0;
  static size_t previousPsramLargestFreeBlock = 0;
  static size_t previousPsramAllocatedBlocks = 0;
  static size_t previousPsramFreeBlocks = 0;

  const int64_t sramSizeDifference = hasPreviousTotals ? static_cast<int64_t>(sramTotalSize) - static_cast<int64_t>(previousSramTotalSize) : 0;
  const int64_t sramFreeDifference = hasPreviousTotals ? static_cast<int64_t>(sramTotalFree) - static_cast<int64_t>(previousSramTotalFree) : 0;
  const int64_t sramLargestDifference = hasPreviousTotals ? static_cast<int64_t>(sramLargestFreeBlock) - static_cast<int64_t>(previousSramLargestFreeBlock) : 0;
  const int64_t sramAllocatedBlockDifference = hasPreviousTotals ? static_cast<int64_t>(sramAllocatedBlocks) - static_cast<int64_t>(previousSramAllocatedBlocks) : 0;
  const int64_t sramFreeBlockDifference = hasPreviousTotals ? static_cast<int64_t>(sramFreeBlocks) - static_cast<int64_t>(previousSramFreeBlocks) : 0;
  const int64_t psramSizeDifference = hasPreviousTotals ? static_cast<int64_t>(psramTotalSize) - static_cast<int64_t>(previousPsramTotalSize) : 0;
  const int64_t psramFreeDifference = hasPreviousTotals ? static_cast<int64_t>(psramTotalFree) - static_cast<int64_t>(previousPsramTotalFree) : 0;
  const int64_t psramLargestDifference = hasPreviousTotals ? static_cast<int64_t>(psramLargestFreeBlock) - static_cast<int64_t>(previousPsramLargestFreeBlock) : 0;
  const int64_t psramAllocatedBlockDifference = hasPreviousTotals ? static_cast<int64_t>(psramAllocatedBlocks) - static_cast<int64_t>(previousPsramAllocatedBlocks) : 0;
  const int64_t psramFreeBlockDifference = hasPreviousTotals ? static_cast<int64_t>(psramFreeBlocks) - static_cast<int64_t>(previousPsramFreeBlocks) : 0;

  // A difference needs one extra byte for its sign in addition to the size text and terminator.
  char sizeDifferenceText[17];
  char freeDifferenceText[17];
  char largestDifferenceText[17];
  printf("-------------------------------------------------------------------------------------\n");
  formatMemorySizeDifference(sramSizeDifference, sizeDifferenceText, sizeof(sizeDifferenceText));
  formatMemorySizeDifference(sramFreeDifference, freeDifferenceText, sizeof(freeDifferenceText));
  formatMemorySizeDifference(sramLargestDifference, largestDifferenceText, sizeof(largestDifferenceText));
  printf("%-16s %-18s %10s %+8" PRId64 " %+8" PRId64 " %10s %10s\n",
         "SRAM DIFF", "vs previous", sizeDifferenceText,
         sramAllocatedBlockDifference, sramFreeBlockDifference, freeDifferenceText, largestDifferenceText);

  formatMemorySizeDifference(psramSizeDifference, sizeDifferenceText, sizeof(sizeDifferenceText));
  formatMemorySizeDifference(psramFreeDifference, freeDifferenceText, sizeof(freeDifferenceText));
  formatMemorySizeDifference(psramLargestDifference, largestDifferenceText, sizeof(largestDifferenceText));
  printf("%-16s %-18s %10s %+8" PRId64 " %+8" PRId64 " %10s %10s\033[0m\n\n",
         "PSRAM DIFF", "vs previous", sizeDifferenceText,
         psramAllocatedBlockDifference, psramFreeBlockDifference, freeDifferenceText, largestDifferenceText);

  previousSramTotalSize = sramTotalSize;
  previousSramTotalFree = sramTotalFree;
  previousSramLargestFreeBlock = sramLargestFreeBlock;
  previousSramAllocatedBlocks = sramAllocatedBlocks;
  previousSramFreeBlocks = sramFreeBlocks;
  previousPsramTotalSize = psramTotalSize;
  previousPsramTotalFree = psramTotalFree;
  previousPsramLargestFreeBlock = psramLargestFreeBlock;
  previousPsramAllocatedBlocks = psramAllocatedBlocks;
  previousPsramFreeBlocks = psramFreeBlocks;
  hasPreviousTotals = true;
}
#endif
