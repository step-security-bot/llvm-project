//===-- release.h -----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef SCUDO_RELEASE_H_
#define SCUDO_RELEASE_H_

#include "common.h"
#include "list.h"
#include "mutex.h"
#include "thread_annotations.h"

namespace scudo {

class ReleaseRecorder {
public:
  ReleaseRecorder(uptr Base, MapPlatformData *Data = nullptr)
      : Base(Base), Data(Data) {}

  uptr getReleasedRangesCount() const { return ReleasedRangesCount; }

  uptr getReleasedBytes() const { return ReleasedBytes; }

  uptr getBase() const { return Base; }

  // Releases [From, To) range of pages back to OS.
  void releasePageRangeToOS(uptr From, uptr To) {
    const uptr Size = To - From;
    releasePagesToOS(Base, From, Size, Data);
    ReleasedRangesCount++;
    ReleasedBytes += Size;
  }

private:
  uptr ReleasedRangesCount = 0;
  uptr ReleasedBytes = 0;
  uptr Base = 0;
  MapPlatformData *Data = nullptr;
};

// A Region page map is used to record the usage of pages in the regions. It
// implements a packed array of Counters. Each counter occupies 2^N bits, enough
// to store counter's MaxValue. Ctor will try to use a static buffer first, and
// if that fails (the buffer is too small or already locked), will allocate the
// required Buffer via map(). The caller is expected to check whether the
// initialization was successful by checking isAllocated() result. For
// performance sake, none of the accessors check the validity of the arguments,
// It is assumed that Index is always in [0, N) range and the value is not
// incremented past MaxValue.
class RegionPageMap {
public:
  RegionPageMap()
      : Regions(0),
        NumCounters(0),
        CounterSizeBitsLog(0),
        CounterMask(0),
        PackingRatioLog(0),
        BitOffsetMask(0),
        SizePerRegion(0),
        BufferSize(0),
        Buffer(nullptr) {}
  RegionPageMap(uptr NumberOfRegions, uptr CountersPerRegion, uptr MaxValue) {
    reset(NumberOfRegions, CountersPerRegion, MaxValue);
  }
  ~RegionPageMap() {
    if (!isAllocated())
      return;
    if (Buffer == &StaticBuffer[0])
      Mutex.unlock();
    else
      unmap(reinterpret_cast<void *>(Buffer),
            roundUpTo(BufferSize, getPageSizeCached()));
    Buffer = nullptr;
  }

  // Lock of `StaticBuffer` is acquired conditionally and there's no easy way to
  // specify the thread-safety attribute properly in current code structure.
  // Besides, it's the only place we may want to check thread safety. Therefore,
  // it's fine to bypass the thread-safety analysis now.
  void reset(uptr NumberOfRegion, uptr CountersPerRegion,
             uptr MaxValue) NO_THREAD_SAFETY_ANALYSIS {
    DCHECK_GT(NumberOfRegion, 0);
    DCHECK_GT(CountersPerRegion, 0);
    DCHECK_GT(MaxValue, 0);

    Regions = NumberOfRegion;
    NumCounters = CountersPerRegion;

    constexpr uptr MaxCounterBits = sizeof(*Buffer) * 8UL;
    // Rounding counter storage size up to the power of two allows for using
    // bit shifts calculating particular counter's Index and offset.
    const uptr CounterSizeBits =
        roundUpToPowerOfTwo(getMostSignificantSetBitIndex(MaxValue) + 1);
    DCHECK_LE(CounterSizeBits, MaxCounterBits);
    CounterSizeBitsLog = getLog2(CounterSizeBits);
    CounterMask = ~(static_cast<uptr>(0)) >> (MaxCounterBits - CounterSizeBits);

    const uptr PackingRatio = MaxCounterBits >> CounterSizeBitsLog;
    DCHECK_GT(PackingRatio, 0);
    PackingRatioLog = getLog2(PackingRatio);
    BitOffsetMask = PackingRatio - 1;

    SizePerRegion =
        roundUpTo(NumCounters, static_cast<uptr>(1U) << PackingRatioLog) >>
        PackingRatioLog;
    BufferSize = SizePerRegion * sizeof(*Buffer) * Regions;
    if (BufferSize <= (StaticBufferCount * sizeof(Buffer[0])) &&
        Mutex.tryLock()) {
      Buffer = &StaticBuffer[0];
      memset(Buffer, 0, BufferSize);
    } else {
      // When using a heap-based buffer, precommit the pages backing the
      // Vmar by passing |MAP_PRECOMMIT| flag. This allows an optimization
      // where page fault exceptions are skipped as the allocated memory
      // is accessed.
      const uptr MmapFlags =
          MAP_ALLOWNOMEM | (SCUDO_FUCHSIA ? MAP_PRECOMMIT : 0);
      Buffer = reinterpret_cast<uptr *>(
          map(nullptr, roundUpTo(BufferSize, getPageSizeCached()),
              "scudo:counters", MmapFlags, &MapData));
    }
  }

  bool isAllocated() const { return !!Buffer; }

  uptr getCount() const { return NumCounters; }

  uptr get(uptr Region, uptr I) const {
    DCHECK_LT(Region, Regions);
    DCHECK_LT(I, NumCounters);
    const uptr Index = I >> PackingRatioLog;
    const uptr BitOffset = (I & BitOffsetMask) << CounterSizeBitsLog;
    return (Buffer[Region * SizePerRegion + Index] >> BitOffset) & CounterMask;
  }

  void inc(uptr Region, uptr I) const {
    DCHECK_LT(get(Region, I), CounterMask);
    const uptr Index = I >> PackingRatioLog;
    const uptr BitOffset = (I & BitOffsetMask) << CounterSizeBitsLog;
    DCHECK_LT(BitOffset, SCUDO_WORDSIZE);
    DCHECK_EQ(isAllCounted(Region, I), false);
    Buffer[Region * SizePerRegion + Index] += static_cast<uptr>(1U)
                                              << BitOffset;
  }

  void incRange(uptr Region, uptr From, uptr To) const {
    DCHECK_LE(From, To);
    const uptr Top = Min(To + 1, NumCounters);
    for (uptr I = From; I < Top; I++)
      inc(Region, I);
  }

  // Set the counter to the max value. Note that the max number of blocks in a
  // page may vary. To provide an easier way to tell if all the blocks are
  // counted for different pages, set to the same max value to denote the
  // all-counted status.
  void setAsAllCounted(uptr Region, uptr I) const {
    DCHECK_LE(get(Region, I), CounterMask);
    const uptr Index = I >> PackingRatioLog;
    const uptr BitOffset = (I & BitOffsetMask) << CounterSizeBitsLog;
    DCHECK_LT(BitOffset, SCUDO_WORDSIZE);
    Buffer[Region * SizePerRegion + Index] |= CounterMask << BitOffset;
  }
  bool isAllCounted(uptr Region, uptr I) const {
    return get(Region, I) == CounterMask;
  }

  uptr getBufferSize() const { return BufferSize; }

  static const uptr StaticBufferCount = 2048U;

private:
  uptr Regions;
  uptr NumCounters;
  uptr CounterSizeBitsLog;
  uptr CounterMask;
  uptr PackingRatioLog;
  uptr BitOffsetMask;

  uptr SizePerRegion;
  uptr BufferSize;
  uptr *Buffer;
  [[no_unique_address]] MapPlatformData MapData = {};

  static HybridMutex Mutex;
  static uptr StaticBuffer[StaticBufferCount] GUARDED_BY(Mutex);
};

template <class ReleaseRecorderT> class FreePagesRangeTracker {
public:
  explicit FreePagesRangeTracker(ReleaseRecorderT &Recorder)
      : Recorder(Recorder), PageSizeLog(getLog2(getPageSizeCached())) {}

  void processNextPage(bool Released) {
    if (Released) {
      if (!InRange) {
        CurrentRangeStatePage = CurrentPage;
        InRange = true;
      }
    } else {
      closeOpenedRange();
    }
    CurrentPage++;
  }

  void skipPages(uptr N) {
    closeOpenedRange();
    CurrentPage += N;
  }

  void finish() { closeOpenedRange(); }

private:
  void closeOpenedRange() {
    if (InRange) {
      Recorder.releasePageRangeToOS((CurrentRangeStatePage << PageSizeLog),
                                    (CurrentPage << PageSizeLog));
      InRange = false;
    }
  }

  ReleaseRecorderT &Recorder;
  const uptr PageSizeLog;
  bool InRange = false;
  uptr CurrentPage = 0;
  uptr CurrentRangeStatePage = 0;
};

struct PageReleaseContext {
  PageReleaseContext(uptr BlockSize, uptr RegionSize, uptr NumberOfRegions) :
      BlockSize(BlockSize),
      RegionSize(RegionSize),
      NumberOfRegions(NumberOfRegions) {
    PageSize = getPageSizeCached();
    if (BlockSize <= PageSize) {
      if (PageSize % BlockSize == 0) {
        // Same number of chunks per page, no cross overs.
        FullPagesBlockCountMax = PageSize / BlockSize;
        SameBlockCountPerPage = true;
      } else if (BlockSize % (PageSize % BlockSize) == 0) {
        // Some chunks are crossing page boundaries, which means that the page
        // contains one or two partial chunks, but all pages contain the same
        // number of chunks.
        FullPagesBlockCountMax = PageSize / BlockSize + 1;
        SameBlockCountPerPage = true;
      } else {
        // Some chunks are crossing page boundaries, which means that the page
        // contains one or two partial chunks.
        FullPagesBlockCountMax = PageSize / BlockSize + 2;
        SameBlockCountPerPage = false;
      }
    } else {
      if (BlockSize % PageSize == 0) {
        // One chunk covers multiple pages, no cross overs.
        FullPagesBlockCountMax = 1;
        SameBlockCountPerPage = true;
      } else {
        // One chunk covers multiple pages, Some chunks are crossing page
        // boundaries. Some pages contain one chunk, some contain two.
        FullPagesBlockCountMax = 2;
        SameBlockCountPerPage = false;
      }
    }

    PagesCount = roundUpTo(RegionSize, PageSize) / PageSize;
    PageSizeLog = getLog2(PageSize);
    RoundedRegionSize = PagesCount << PageSizeLog;
    RoundedSize = NumberOfRegions * RoundedRegionSize;
  }

  // PageMap is lazily allocated when markFreeBlocks() is invoked.
  bool hasBlockMarked() const {
    return PageMap.isAllocated();
  }

  void ensurePageMapAllocated() {
    if (PageMap.isAllocated())
      return;
    PageMap.reset(NumberOfRegions, PagesCount, FullPagesBlockCountMax);
    DCHECK(PageMap.isAllocated());
  }

  template<class TransferBatchT, typename DecompactPtrT>
  void markFreeBlocks(const IntrusiveList<TransferBatchT> &FreeList,
                      DecompactPtrT DecompactPtr, uptr Base) {
    ensurePageMapAllocated();

    // Iterate over free chunks and count how many free chunks affect each
    // allocated page.
    if (BlockSize <= PageSize && PageSize % BlockSize == 0) {
      // Each chunk affects one page only.
      for (const auto &It : FreeList) {
        for (u16 I = 0; I < It.getCount(); I++) {
          const uptr P = DecompactPtr(It.get(I)) - Base;
          if (P >= RoundedSize)
            continue;
          const uptr RegionIndex = NumberOfRegions == 1U ? 0 : P / RegionSize;
          const uptr PInRegion = P - RegionIndex * RegionSize;
          PageMap.inc(RegionIndex, PInRegion >> PageSizeLog);
        }
      }
    } else {
      // In all other cases chunks might affect more than one page.
      DCHECK_GE(RegionSize, BlockSize);
      const uptr LastBlockInRegion =
          ((RegionSize / BlockSize) - 1U) * BlockSize;
      for (const auto &It : FreeList) {
        for (u16 I = 0; I < It.getCount(); I++) {
          const uptr P = DecompactPtr(It.get(I)) - Base;
          if (P >= RoundedSize)
            continue;
          const uptr RegionIndex = NumberOfRegions == 1U ? 0 : P / RegionSize;
          uptr PInRegion = P - RegionIndex * RegionSize;
          PageMap.incRange(RegionIndex, PInRegion >> PageSizeLog,
                            (PInRegion + BlockSize - 1) >> PageSizeLog);
          // The last block in a region might straddle a page, so if it's
          // free, we mark the following "pretend" memory block(s) as free.
          if (PInRegion == LastBlockInRegion) {
            PInRegion += BlockSize;
            while (PInRegion < RoundedRegionSize) {
              PageMap.incRange(RegionIndex, PInRegion >> PageSizeLog,
                                (PInRegion + BlockSize - 1) >> PageSizeLog);
              PInRegion += BlockSize;
            }
          }
        }
      }
    }
  }

  uptr BlockSize;
  uptr RegionSize;
  uptr NumberOfRegions;
  uptr PageSize;
  uptr PagesCount;
  uptr PageSizeLog;
  uptr RoundedRegionSize;
  uptr RoundedSize;
  uptr FullPagesBlockCountMax;
  bool SameBlockCountPerPage;
  RegionPageMap PageMap;
};

// Try to release the page which doesn't have any in-used block, i.e., they are
// all free blocks. The `PageMap` will record the number of free blocks in each
// page.
template <class ReleaseRecorderT, typename SkipRegionT>
NOINLINE void
releaseFreeMemoryToOS(PageReleaseContext &Context,
                      ReleaseRecorderT &Recorder, SkipRegionT SkipRegion) {
  const uptr PageSize = Context.PageSize;
  const uptr BlockSize = Context.BlockSize;
  const uptr PagesCount = Context.PagesCount;
  const uptr NumberOfRegions = Context.NumberOfRegions;
  const uptr FullPagesBlockCountMax = Context.FullPagesBlockCountMax;
  const bool SameBlockCountPerPage = Context.SameBlockCountPerPage;
  RegionPageMap &PageMap = Context.PageMap;

  // Iterate over pages detecting ranges of pages with chunk Counters equal
  // to the expected number of chunks for the particular page.
  FreePagesRangeTracker<ReleaseRecorderT> RangeTracker(Recorder);
  if (SameBlockCountPerPage) {
    // Fast path, every page has the same number of chunks affecting it.
    for (uptr I = 0; I < NumberOfRegions; I++) {
      if (SkipRegion(I)) {
        RangeTracker.skipPages(PagesCount);
        continue;
      }
      for (uptr J = 0; J < PagesCount; J++) {
        const bool CanRelease = PageMap.get(I, J) == FullPagesBlockCountMax;
        if (CanRelease)
          PageMap.setAsAllCounted(I, J);
        RangeTracker.processNextPage(CanRelease);
      }
    }
  } else {
    // Slow path, go through the pages keeping count how many chunks affect
    // each page.
    const uptr Pn = BlockSize < PageSize ? PageSize / BlockSize : 1;
    const uptr Pnc = Pn * BlockSize;
    // The idea is to increment the current page pointer by the first chunk
    // size, middle portion size (the portion of the page covered by chunks
    // except the first and the last one) and then the last chunk size, adding
    // up the number of chunks on the current page and checking on every step
    // whether the page boundary was crossed.
    for (uptr I = 0; I < NumberOfRegions; I++) {
      if (SkipRegion(I)) {
        RangeTracker.skipPages(PagesCount);
        continue;
      }
      uptr PrevPageBoundary = 0;
      uptr CurrentBoundary = 0;
      for (uptr J = 0; J < PagesCount; J++) {
        const uptr PageBoundary = PrevPageBoundary + PageSize;
        uptr BlocksPerPage = Pn;
        if (CurrentBoundary < PageBoundary) {
          if (CurrentBoundary > PrevPageBoundary)
            BlocksPerPage++;
          CurrentBoundary += Pnc;
          if (CurrentBoundary < PageBoundary) {
            BlocksPerPage++;
            CurrentBoundary += BlockSize;
          }
        }
        PrevPageBoundary = PageBoundary;
        const bool CanRelease = PageMap.get(I, J) == BlocksPerPage;
        if (CanRelease)
          PageMap.setAsAllCounted(I, J);
        RangeTracker.processNextPage(CanRelease);
      }
    }
  }
  RangeTracker.finish();
}

// An overload releaseFreeMemoryToOS which doesn't require the page usage
// information after releasing.
template <class TransferBatchT, class ReleaseRecorderT, typename DecompactPtrT,
          typename SkipRegionT>
NOINLINE void
releaseFreeMemoryToOS(const IntrusiveList<TransferBatchT> &FreeList,
                      uptr RegionSize, uptr NumberOfRegions, uptr BlockSize,
                      ReleaseRecorderT &Recorder, DecompactPtrT DecompactPtr,
                      SkipRegionT SkipRegion) {
  PageReleaseContext Context(BlockSize, RegionSize, NumberOfRegions);
  Context.markFreeBlocks(FreeList, DecompactPtr, Recorder.getBase());
  releaseFreeMemoryToOS(Context, Recorder, SkipRegion);
}

} // namespace scudo

#endif // SCUDO_RELEASE_H_
