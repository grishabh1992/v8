// Copyright 2019 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_HEAP_BASIC_MEMORY_CHUNK_H_
#define V8_HEAP_BASIC_MEMORY_CHUNK_H_

#include <type_traits>
#include <unordered_map>

#include "src/base/atomic-utils.h"
#include "src/common/globals.h"
#include "src/flags/flags.h"
#include "src/heap/marking.h"
#include "src/utils/allocation.h"

namespace v8 {
namespace internal {

class BaseSpace;

class BasicMemoryChunk {
 public:
  // Use with std data structures.
  struct Hasher {
    size_t operator()(BasicMemoryChunk* const chunk) const {
      return reinterpret_cast<size_t>(chunk) >> kPageSizeBits;
    }
  };

  enum Flag {
    NO_FLAGS = 0u,
    IS_EXECUTABLE = 1u << 0,
    POINTERS_TO_HERE_ARE_INTERESTING = 1u << 1,
    POINTERS_FROM_HERE_ARE_INTERESTING = 1u << 2,
    // A page in the from-space or a young large page that was not scavenged
    // yet.
    FROM_PAGE = 1u << 3,
    // A page in the to-space or a young large page that was scavenged.
    TO_PAGE = 1u << 4,
    LARGE_PAGE = 1u << 5,
    EVACUATION_CANDIDATE = 1u << 6,
    NEVER_EVACUATE = 1u << 7,

    // Large objects can have a progress bar in their page header. These object
    // are scanned in increments and will be kept black while being scanned.
    // Even if the mutator writes to them they will be kept black and a white
    // to grey transition is performed in the value.
    HAS_PROGRESS_BAR = 1u << 8,

    // |PAGE_NEW_OLD_PROMOTION|: A page tagged with this flag has been promoted
    // from new to old space during evacuation.
    PAGE_NEW_OLD_PROMOTION = 1u << 9,

    // |PAGE_NEW_NEW_PROMOTION|: A page tagged with this flag has been moved
    // within the new space during evacuation.
    PAGE_NEW_NEW_PROMOTION = 1u << 10,

    // This flag is intended to be used for testing. Works only when both
    // FLAG_stress_compaction and FLAG_manual_evacuation_candidates_selection
    // are set. It forces the page to become an evacuation candidate at next
    // candidates selection cycle.
    FORCE_EVACUATION_CANDIDATE_FOR_TESTING = 1u << 11,

    // This flag is intended to be used for testing.
    NEVER_ALLOCATE_ON_PAGE = 1u << 12,

    // The memory chunk is already logically freed, however the actual freeing
    // still has to be performed.
    PRE_FREED = 1u << 13,

    // |POOLED|: When actually freeing this chunk, only uncommit and do not
    // give up the reservation as we still reuse the chunk at some point.
    POOLED = 1u << 14,

    // |COMPACTION_WAS_ABORTED|: Indicates that the compaction in this page
    //   has been aborted and needs special handling by the sweeper.
    COMPACTION_WAS_ABORTED = 1u << 15,

    // |COMPACTION_WAS_ABORTED_FOR_TESTING|: During stress testing evacuation
    // on pages is sometimes aborted. The flag is used to avoid repeatedly
    // triggering on the same page.
    COMPACTION_WAS_ABORTED_FOR_TESTING = 1u << 16,

    // |SWEEP_TO_ITERATE|: The page requires sweeping using external markbits
    // to iterate the page.
    SWEEP_TO_ITERATE = 1u << 17,

    // |INCREMENTAL_MARKING|: Indicates whether incremental marking is currently
    // enabled.
    INCREMENTAL_MARKING = 1u << 18,
    NEW_SPACE_BELOW_AGE_MARK = 1u << 19,

    // The memory chunk freeing bookkeeping has been performed but the chunk has
    // not yet been freed.
    UNREGISTERED = 1u << 20,

    // The memory chunk belongs to the read-only heap and does not participate
    // in garbage collection. This is used instead of owner for identity
    // checking since read-only chunks have no owner once they are detached.
    READ_ONLY_HEAP = 1u << 21,
  };

  static const intptr_t kAlignment =
      (static_cast<uintptr_t>(1) << kPageSizeBits);

  static const intptr_t kAlignmentMask = kAlignment - 1;

  BasicMemoryChunk(size_t size, Address area_start, Address area_end);

  static Address BaseAddress(Address a) { return a & ~kAlignmentMask; }

  Address address() const { return reinterpret_cast<Address>(this); }

  // Returns the offset of a given address to this page.
  inline size_t Offset(Address a) { return static_cast<size_t>(a - address()); }

  // Returns the address for a given offset to the this page.
  Address OffsetToAddress(size_t offset) {
    Address address_in_page = address() + offset;
    DCHECK_GE(address_in_page, area_start());
    DCHECK_LT(address_in_page, area_end());
    return address_in_page;
  }

  // Some callers rely on the fact that this can operate on both
  // tagged and aligned object addresses.
  inline uint32_t AddressToMarkbitIndex(Address addr) const {
    return static_cast<uint32_t>(addr - this->address()) >> kTaggedSizeLog2;
  }

  inline Address MarkbitIndexToAddress(uint32_t index) const {
    return this->address() + (index << kTaggedSizeLog2);
  }

  size_t size() const { return size_; }
  void set_size(size_t size) { size_ = size; }

  Address area_start() const { return area_start_; }

  Address area_end() const { return area_end_; }
  void set_area_end(Address area_end) { area_end_ = area_end; }

  size_t area_size() const {
    return static_cast<size_t>(area_end() - area_start());
  }

  Heap* heap() const {
    DCHECK_NOT_NULL(heap_);
    return heap_;
  }

  // Gets the chunk's owner or null if the space has been detached.
  BaseSpace* owner() const { return owner_; }

  void set_owner(BaseSpace* space) { owner_ = space; }

  template <AccessMode access_mode = AccessMode::NON_ATOMIC>
  void SetFlag(Flag flag) {
    if (access_mode == AccessMode::NON_ATOMIC) {
      flags_ |= flag;
    } else {
      base::AsAtomicWord::SetBits<uintptr_t>(&flags_, flag, flag);
    }
  }

  template <AccessMode access_mode = AccessMode::NON_ATOMIC>
  bool IsFlagSet(Flag flag) const {
    return (GetFlags<access_mode>() & flag) != 0;
  }

  void ClearFlag(Flag flag) { flags_ &= ~flag; }

  // Set or clear multiple flags at a time. The flags in the mask are set to
  // the value in "flags", the rest retain the current value in |flags_|.
  void SetFlags(uintptr_t flags, uintptr_t mask) {
    flags_ = (flags_ & ~mask) | (flags & mask);
  }

  // Return all current flags.
  template <AccessMode access_mode = AccessMode::NON_ATOMIC>
  uintptr_t GetFlags() const {
    if (access_mode == AccessMode::NON_ATOMIC) {
      return flags_;
    } else {
      return base::AsAtomicWord::Relaxed_Load(&flags_);
    }
  }

  bool InReadOnlySpace() const { return IsFlagSet(READ_ONLY_HEAP); }

  // TODO(v8:7464): Add methods for down casting to MemoryChunk.

  bool Contains(Address addr) const {
    return addr >= area_start() && addr < area_end();
  }

  // Checks whether |addr| can be a limit of addresses in this page. It's a
  // limit if it's in the page, or if it's just after the last byte of the page.
  bool ContainsLimit(Address addr) const {
    return addr >= area_start() && addr <= area_end();
  }

  void ReleaseMarkingBitmap();

  static BasicMemoryChunk* Initialize(Heap* heap, Address base, size_t size,
                                      Address area_start, Address area_end,
                                      BaseSpace* owner,
                                      VirtualMemory reservation);

  size_t wasted_memory() { return wasted_memory_; }
  void add_wasted_memory(size_t waste) { wasted_memory_ += waste; }
  size_t allocated_bytes() { return allocated_bytes_; }

  static const intptr_t kSizeOffset = 0;
  static const intptr_t kFlagsOffset = kSizeOffset + kSizetSize;
  static const intptr_t kMarkBitmapOffset = kFlagsOffset + kUIntptrSize;
  static const intptr_t kHeapOffset = kMarkBitmapOffset + kSystemPointerSize;
  static const intptr_t kAreaStartOffset = kHeapOffset + kSystemPointerSize;
  static const intptr_t kAreaEndOffset = kAreaStartOffset + kSystemPointerSize;

  static const size_t kHeaderSize =
      kSizeOffset + kSizetSize   // size_t size
      + kUIntptrSize             // uintptr_t flags_
      + kSystemPointerSize       // Bitmap* marking_bitmap_
      + kSystemPointerSize       // Heap* heap_
      + kSystemPointerSize       // Address area_start_
      + kSystemPointerSize       // Address area_end_
      + kSizetSize               // size_t allocated_bytes_
      + kSizetSize               // size_t wasted_memory_
      + kSystemPointerSize       // std::atomic<intptr_t> high_water_mark_
      + kSystemPointerSize       // Address owner_
      + 3 * kSystemPointerSize;  // VirtualMemory reservation_

  // Only works if the pointer is in the first kPageSize of the MemoryChunk.
  static BasicMemoryChunk* FromAddress(Address a) {
    DCHECK(!V8_ENABLE_THIRD_PARTY_HEAP_BOOL);
    return reinterpret_cast<BasicMemoryChunk*>(BaseAddress(a));
  }

  template <AccessMode mode>
  ConcurrentBitmap<mode>* marking_bitmap() const {
    return reinterpret_cast<ConcurrentBitmap<mode>*>(marking_bitmap_);
  }

  Address HighWaterMark() { return address() + high_water_mark_; }

  static inline void UpdateHighWaterMark(Address mark) {
    if (mark == kNullAddress) return;
    // Need to subtract one from the mark because when a chunk is full the
    // top points to the next address after the chunk, which effectively belongs
    // to another chunk. See the comment to Page::FromAllocationAreaAddress.
    BasicMemoryChunk* chunk = BasicMemoryChunk::FromAddress(mark - 1);
    intptr_t new_mark = static_cast<intptr_t>(mark - chunk->address());
    intptr_t old_mark = chunk->high_water_mark_.load(std::memory_order_relaxed);
    while ((new_mark > old_mark) &&
           !chunk->high_water_mark_.compare_exchange_weak(
               old_mark, new_mark, std::memory_order_acq_rel)) {
    }
  }

  VirtualMemory* reserved_memory() { return &reservation_; }

  void ResetAllocationStatistics() {
    allocated_bytes_ = area_size();
    wasted_memory_ = 0;
  }

  void IncreaseAllocatedBytes(size_t bytes) {
    DCHECK_LE(bytes, area_size());
    allocated_bytes_ += bytes;
  }

  void DecreaseAllocatedBytes(size_t bytes) {
    DCHECK_LE(bytes, area_size());
    DCHECK_GE(allocated_bytes(), bytes);
    allocated_bytes_ -= bytes;
  }

 protected:
  // Overall size of the chunk, including the header and guards.
  size_t size_;

  uintptr_t flags_ = NO_FLAGS;

  Bitmap* marking_bitmap_ = nullptr;

  // TODO(v8:7464): Find a way to remove this.
  // This goes against the spirit for the BasicMemoryChunk, but until C++14/17
  // is the default it needs to live here because MemoryChunk is not standard
  // layout under C++11.
  Heap* heap_;

  // Start and end of allocatable memory on this chunk.
  Address area_start_;
  Address area_end_;

  // Byte allocated on the page, which includes all objects on the page and the
  // linear allocation area.
  size_t allocated_bytes_;
  // Freed memory that was not added to the free list.
  size_t wasted_memory_;

  // Assuming the initial allocation on a page is sequential, count highest
  // number of bytes ever allocated on the page.
  std::atomic<intptr_t> high_water_mark_;

  // The space owning this memory chunk.
  std::atomic<BaseSpace*> owner_;

  // If the chunk needs to remember its memory reservation, it is stored here.
  VirtualMemory reservation_;

  friend class BasicMemoryChunkValidator;
  friend class ConcurrentMarkingState;
  friend class MajorMarkingState;
  friend class MajorAtomicMarkingState;
  friend class MajorNonAtomicMarkingState;
  friend class MemoryAllocator;
  friend class MinorMarkingState;
  friend class MinorNonAtomicMarkingState;
  friend class PagedSpace;
};

STATIC_ASSERT(std::is_standard_layout<BasicMemoryChunk>::value);

class BasicMemoryChunkValidator {
  // Computed offsets should match the compiler generated ones.
  STATIC_ASSERT(BasicMemoryChunk::kSizeOffset ==
                offsetof(BasicMemoryChunk, size_));
  STATIC_ASSERT(BasicMemoryChunk::kFlagsOffset ==
                offsetof(BasicMemoryChunk, flags_));
  STATIC_ASSERT(BasicMemoryChunk::kMarkBitmapOffset ==
                offsetof(BasicMemoryChunk, marking_bitmap_));
  STATIC_ASSERT(BasicMemoryChunk::kHeapOffset ==
                offsetof(BasicMemoryChunk, heap_));
};

}  // namespace internal
}  // namespace v8

#endif  // V8_HEAP_BASIC_MEMORY_CHUNK_H_
