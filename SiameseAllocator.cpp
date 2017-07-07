/*
    Copyright (c) 2017 Christopher A. Taylor.  All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.
    * Neither the name of Siamese nor the names of its contributors may be
      used to endorse or promote products derived from this software without
      specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
    AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
    ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
    LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
    CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
    SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
    INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
    CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
    ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
    POSSIBILITY OF SUCH DAMAGE.
*/

#include "SiameseAllocator.h"

#ifdef SIAMESE_ENABLE_ALLOCATOR_INTEGRITY_CHECKS
    #define ALLOC_DEBUG_INTEGRITY_CHECK() IntegrityCheck();
#else // SIAMESE_ENABLE_ALLOCATOR_INTEGRITY_CHECKS
    #define ALLOC_DEBUG_INTEGRITY_CHECK() do {} while (false);
#endif // SIAMESE_ENABLE_ALLOCATOR_INTEGRITY_CHECKS

namespace siamese {


//------------------------------------------------------------------------------
// SIMD-Safe Aligned Memory Allocations

static inline uint8_t* SIMDSafeAllocate(size_t size)
{
    uint8_t* data = (uint8_t*)calloc(1, kAlignmentBytes + size);
    if (!data)
        return nullptr;
    unsigned offset = (unsigned)((uintptr_t)data % kAlignmentBytes);
    data += kAlignmentBytes - offset;
    data[-1] = (uint8_t)offset;
    return data;
}

static inline void SIMDSafeFree(void* ptr)
{
    if (!ptr)
        return;
    uint8_t* data = (uint8_t*)ptr;
    unsigned offset = data[-1];
    if (offset >= kAlignmentBytes)
    {
        SIAMESE_DEBUG_BREAK; // Should never happen
        return;
    }
    data -= kAlignmentBytes - offset;
    free(data);
}


//------------------------------------------------------------------------------
// Allocator

Allocator::Allocator()
{
    static_assert(kAlignmentBytes == kUnitSize, "update SIMDSafeAllocate");

    HugeChunkStart = SIMDSafeAllocate(kWindowSizeBytes * kPreallocatedWindows);
    if (HugeChunkStart)
    {
        uint8_t* windowStart = HugeChunkStart;

        PreferredWindowsHead = nullptr;
        PreferredWindowsTail = (WindowHeader*)windowStart;
        PreferredWindowsCount = kPreallocatedWindows;

        // For each window to preallocate:
        for (unsigned i = 0; i < kPreallocatedWindows; ++i)
        {
            WindowHeader* windowHeader = (WindowHeader*)windowStart;
            windowStart += kWindowSizeBytes;

            windowHeader->Used.ClearAll();
            windowHeader->FreeUnitCount    = kWindowMaxUnits;
            windowHeader->ResumeScanOffset = 0;
            windowHeader->Prev             = nullptr;
            windowHeader->Next             = PreferredWindowsHead;
            windowHeader->InFullList       = false;
            windowHeader->Preallocated     = true;
 
            PreferredWindowsHead = windowHeader;
        }
    }

    ALLOC_DEBUG_INTEGRITY_CHECK();
}

Allocator::~Allocator()
{
    for (WindowHeader* node = PreferredWindowsHead, *next; node; node = next)
    {
        next = node->Next;
        if (!node->Preallocated)
            SIMDSafeFree(node);
    }
    for (WindowHeader* node = FullWindowsHead, *next; node; node = next)
    {
        next = node->Next;
        if (!node->Preallocated)
            SIMDSafeFree(node);
    }
    SIMDSafeFree(HugeChunkStart);
}

unsigned Allocator::GetMemoryUsedBytes() const
{
    unsigned sum = 0;
    for (WindowHeader* node = PreferredWindowsHead; node; node = node->Next)
        sum += kWindowMaxUnits - node->FreeUnitCount;
    for (WindowHeader* node = FullWindowsHead; node; node = node->Next)
        sum += kWindowMaxUnits - node->FreeUnitCount;
    return sum * kUnitSize;
}

unsigned Allocator::GetMemoryAllocatedBytes() const
{
    return (unsigned)((PreferredWindowsCount + FullWindowsCount) * kWindowMaxUnits * kUnitSize);
}

bool Allocator::IntegrityCheck() const
{
#ifdef SIAMESE_ALLOCATOR_SHRINK
    SIAMESE_DEBUG_ASSERT(PreferredWindowsCount >= EmptyWindowCount);
#endif // SIAMESE_ALLOCATOR_SHRINK

    unsigned emptyCount = 0;
    unsigned preallocatedCount = 0;

    SIAMESE_DEBUG_ASSERT(!PreferredWindowsHead || PreferredWindowsHead->Prev == nullptr);
    SIAMESE_DEBUG_ASSERT(!PreferredWindowsTail || PreferredWindowsTail->Next == nullptr);

    unsigned ii = 0;
    for (WindowHeader* windowHeader = PreferredWindowsHead; windowHeader; windowHeader = windowHeader->Next, ++ii)
    {
        SIAMESE_DEBUG_ASSERT(windowHeader->Prev == nullptr);
        if (ii >= PreferredWindowsCount)
        {
            SIAMESE_DEBUG_BREAK; // Should never happen
            return false;
        }
        unsigned jj = 0;
        for (WindowHeader* other = PreferredWindowsHead; other; other = other->Next, ++jj)
        {
            if (windowHeader == other && ii != jj)
            {
                SIAMESE_DEBUG_BREAK; // Should never happen
                return false;
            }
        }
        if (windowHeader->InFullList)
        {
            SIAMESE_DEBUG_BREAK; // Should never happen
            return false;
        }
        if (windowHeader->FreeUnitCount <= 0 || windowHeader->FreeUnitCount > kWindowMaxUnits)
        {
            SIAMESE_DEBUG_BREAK; // Should never happen
            return false;
        }
        if (windowHeader->ResumeScanOffset > kWindowMaxUnits)
        {
            SIAMESE_DEBUG_BREAK; // Should never happen
            return false;
        }
        unsigned setCount = windowHeader->Used.RangePopcount(0, kWindowMaxUnits);
        if (setCount != kWindowMaxUnits - windowHeader->FreeUnitCount)
        {
            SIAMESE_DEBUG_BREAK; // Should never happen
            return false;
        }
        if (windowHeader->Preallocated)
            ++preallocatedCount;
        else if (windowHeader->FreeUnitCount == kWindowMaxUnits)
            ++emptyCount;
    }
    SIAMESE_DEBUG_ASSERT(ii == PreferredWindowsCount);

    ii = 0;
    for (WindowHeader* windowHeader = FullWindowsHead, *prev = nullptr; windowHeader; windowHeader = windowHeader->Next, ++ii)
    {
        SIAMESE_DEBUG_ASSERT(windowHeader->Prev == prev);
        prev = windowHeader;

        if (ii >= FullWindowsCount)
        {
            SIAMESE_DEBUG_BREAK; // Should never happen
            return false;
        }
        for (WindowHeader* other = PreferredWindowsHead; other; other = other->Next)
        {
            if (windowHeader == other)
            {
                SIAMESE_DEBUG_BREAK; // Should never happen
                return false;
            }
        }
        unsigned jj = 0;
        for (WindowHeader* other = FullWindowsHead; other; other = other->Next, ++jj)
        {
            if (windowHeader == other && ii != jj)
            {
                SIAMESE_DEBUG_BREAK; // Should never happen
                return false;
            }
        }
        if (!windowHeader->InFullList)
        {
            SIAMESE_DEBUG_BREAK; // Should never happen
            return false;
        }
        if (windowHeader->FreeUnitCount > kPreferredThresholdUnits)
        {
            SIAMESE_DEBUG_BREAK; // Should never happen
            return false;
        }
        if (windowHeader->ResumeScanOffset > kWindowMaxUnits)
        {
            SIAMESE_DEBUG_BREAK; // Should never happen
            return false;
        }
        unsigned setCount = windowHeader->Used.RangePopcount(0, kWindowMaxUnits);
        if (setCount != kWindowMaxUnits - windowHeader->FreeUnitCount)
        {
            SIAMESE_DEBUG_BREAK; // Should never happen
            return false;
        }
        if (windowHeader->Preallocated)
            ++preallocatedCount;
    }
    SIAMESE_DEBUG_ASSERT(ii == FullWindowsCount);

    if (preallocatedCount != kPreallocatedWindows)
    {
        SIAMESE_DEBUG_BREAK; // Should never happen
        return false;
    }
#ifdef SIAMESE_ALLOCATOR_SHRINK
    if (emptyCount != EmptyWindowCount)
    {
        SIAMESE_DEBUG_BREAK; // Should never happen
        return false;
    }
#endif // SIAMESE_ALLOCATOR_SHRINK
    return true;
}

uint8_t* Allocator::Allocate(unsigned bytes)
{
    if (bytes <= 0)
        return nullptr;

    // Calculate number of units required by this allocation
    // Note: +1 for the AllocationHeader
    const unsigned units = (bytes + kOverallocationBytes + kUnitSize - 1) / kUnitSize + 1;

    if (units > kFallbackThresholdUnits)
        return FallbackAllocate(bytes);

    for (WindowHeader* windowHeader = PreferredWindowsHead, *prev = nullptr; windowHeader; prev = windowHeader, windowHeader = windowHeader->Next)
    {
        SIAMESE_DEBUG_ASSERT(!windowHeader->InFullList);

        if (windowHeader->FreeUnitCount < units)
            continue;

        // Walk the holes in the bitmask:
        UsedMaskT& usedMask  = windowHeader->Used;
        unsigned regionStart = windowHeader->ResumeScanOffset;
        while (regionStart < usedMask.kValidBits)
        {
            regionStart = usedMask.FindFirstClear(regionStart);
            unsigned regionEnd = regionStart + units;

            // If we ran out of space:
            if (regionEnd > usedMask.kValidBits)
                break;

            regionEnd = usedMask.FindFirstSet(regionStart + 1, regionEnd);
            SIAMESE_DEBUG_ASSERT(regionEnd > regionStart);
            SIAMESE_DEBUG_ASSERT(regionEnd <= usedMask.kValidBits);

            if (regionEnd - regionStart < units)
            {
                regionStart = regionEnd + 1;
                continue;
            }
            regionEnd = regionStart + units;

            // Carve out region
            uint8_t* region = (uint8_t*)windowHeader + kWindowHeaderBytes + regionStart * kUnitSize;
            AllocationHeader* regionHeader = (AllocationHeader*)region;
            regionHeader->Header    = windowHeader;
            regionHeader->UsedUnits = units;
            regionHeader->Freed     = false;

            // Update window header
#ifdef SIAMESE_ALLOCATOR_SHRINK
            if (windowHeader->FreeUnitCount >= kWindowMaxUnits && !windowHeader->Preallocated)
                --EmptyWindowCount;
#endif // SIAMESE_ALLOCATOR_SHRINK
            windowHeader->FreeUnitCount -= units;
            usedMask.SetRange(regionStart, regionStart + units);
            windowHeader->ResumeScanOffset = regionStart + units;

            // Move this window to the full list if we cannot make another allocation of the same size
            const unsigned kMinRemaining = units;
            WindowHeader* moveStopWindow = (windowHeader->ResumeScanOffset + kMinRemaining > kWindowMaxUnits) ? windowHeader->Next : windowHeader;
            MoveFirstFewWindowsToFull(moveStopWindow);

            uint8_t* data = region + kUnitSize;
#ifdef SIAMESE_SCRUB_MEMORY
            memset(data, 0, (units - 1) * kUnitSize);
#endif // SIAMESE_SCRUB_MEMORY
            SIAMESE_DEBUG_ASSERT((uintptr_t)data % kUnitSize == 0);
            SIAMESE_DEBUG_ASSERT((uint8_t*)regionHeader >= (uint8_t*)regionHeader->Header + kWindowHeaderBytes);
            SIAMESE_DEBUG_ASSERT(regionHeader->GetUnitStart() < kWindowMaxUnits);
            SIAMESE_DEBUG_ASSERT(regionHeader->GetUnitStart() + regionHeader->UsedUnits <= kWindowMaxUnits);
            return data;
        }
    }

    // Move all preferred windows to full since none of them worked out
    MoveFirstFewWindowsToFull(nullptr);

    return AllocateFromNewWindow(units);
}

void Allocator::MoveFirstFewWindowsToFull(WindowHeader* stopWindow)
{
    ALLOC_DEBUG_INTEGRITY_CHECK();

    unsigned movedCount = 0;
    WindowHeader* moveHead = FullWindowsHead;
    WindowHeader* keepHead = nullptr;
    WindowHeader* keepTail = nullptr;

    for (WindowHeader* windowHeader = PreferredWindowsHead, *next; windowHeader != stopWindow; windowHeader = next)
    {
        next = windowHeader->Next;

        // If this window should stay in the preferred list:
        if (windowHeader->FreeUnitCount >= kPreferredThresholdUnits)
        {
            // Reset the free block scan from the top for this window since we missed some holes
            // But we will move it to the end of the preferred list since it seems spotty
            windowHeader->ResumeScanOffset = 0;

            // Place it in the "keep" list for now
            if (keepTail)
                keepTail->Next = windowHeader;
            else
                keepHead = keepTail = windowHeader;
        }
        else
        {
            // Move the window to the full list
            windowHeader->InFullList = true;
            ++movedCount;
            windowHeader->Next = moveHead;
            if (moveHead)
                moveHead->Prev = windowHeader;
            windowHeader->Prev = nullptr;
            moveHead = windowHeader;
        }
    }

    // Update FullWindows list
    FullWindowsHead = moveHead;
    FullWindowsCount += movedCount;

    // Update PreferredWindows list
    PreferredWindowsCount -= movedCount;
    if (stopWindow)
    {
#ifdef SIAMESE_DEBUG
        stopWindow->Prev = nullptr;
#endif // SIAMESE_DEBUG
        PreferredWindowsHead = stopWindow;
        SIAMESE_DEBUG_ASSERT(PreferredWindowsTail != nullptr);

        if (keepHead)
        {
            PreferredWindowsTail->Next = keepHead;
            PreferredWindowsTail = keepTail;
        }
    }
    else
    {
        PreferredWindowsHead = keepHead;
        PreferredWindowsTail = keepTail;
    }

    ALLOC_DEBUG_INTEGRITY_CHECK();
}

uint8_t* Allocator::AllocateFromNewWindow(unsigned units)
{
    ALLOC_DEBUG_INTEGRITY_CHECK();

    uint8_t* headerStart = SIMDSafeAllocate(kWindowSizeBytes);
    if (!headerStart)
        return nullptr; // Allocation failure

    // Update window header
    WindowHeader* windowHeader = (WindowHeader*)headerStart;
    windowHeader->Used.ClearAll();
    windowHeader->Used.SetRange(0, units);
    windowHeader->FreeUnitCount = kWindowMaxUnits - units;
    windowHeader->ResumeScanOffset = units;
    windowHeader->InFullList = false;
    windowHeader->Next = PreferredWindowsHead;
    windowHeader->Preallocated = false;

    // Insert into PreferredWindows list
    if (PreferredWindowsHead)
        PreferredWindowsHead->Prev = windowHeader;
    else
        PreferredWindowsTail = windowHeader;
    PreferredWindowsHead = windowHeader;
    ++PreferredWindowsCount;

    // Carve out region
    AllocationHeader* regionHeader = (AllocationHeader*)(headerStart + kWindowHeaderBytes);
    regionHeader->Header    = windowHeader;
    regionHeader->UsedUnits = units;
    regionHeader->Freed     = false;

    uint8_t* data = (uint8_t*)regionHeader + kUnitSize;
#ifdef SIAMESE_SCRUB_MEMORY
    memset(data, 0, (units - 1) * kUnitSize);
#endif // SIAMESE_SCRUB_MEMORY
    SIAMESE_DEBUG_ASSERT((uintptr_t)data % kUnitSize == 0);

    ALLOC_DEBUG_INTEGRITY_CHECK();

    return data;
}

uint8_t* Allocator::Reallocate(uint8_t* ptr, unsigned bytes, ReallocBehavior behavior)
{
    ALLOC_DEBUG_INTEGRITY_CHECK();

    if (!ptr)
        return Allocate(bytes);
    if (bytes <= 0)
    {
        Free(ptr);
        return nullptr;
    }
    SIAMESE_DEBUG_ASSERT((uintptr_t)ptr % kUnitSize == 0);

    AllocationHeader* regionHeader = (AllocationHeader*)(ptr - kUnitSize);
    if (regionHeader->Freed)
    {
        SIAMESE_DEBUG_BREAK; // Double-free
        return Allocate(bytes);
    }

    const unsigned existingUnits = regionHeader->UsedUnits;
#ifndef SIAMESE_DISABLE_ALLOCATOR
    SIAMESE_DEBUG_ASSERT(!regionHeader->Header || existingUnits <= kFallbackThresholdUnits);
#endif // SIAMESE_DISABLE_ALLOCATOR

    // If the existing allocation is big enough:
    const unsigned requestedUnits = (bytes + kUnitSize - 1) / kUnitSize + 1;
    if (requestedUnits <= existingUnits)
        return ptr; // No change needed

    // Allocate new data
    uint8_t* newPtr = Allocate(bytes);
    if (!newPtr)
        return nullptr;

    // Copy old data
    if (behavior == ReallocBehavior::CopyExisting)
        memcpy(newPtr, ptr, (existingUnits - 1) * kUnitSize);

    Free(ptr);

    ALLOC_DEBUG_INTEGRITY_CHECK();

    return newPtr;
}

void Allocator::Free(uint8_t* ptr)
{
    ALLOC_DEBUG_INTEGRITY_CHECK();

    if (!ptr)
        return;
    SIAMESE_DEBUG_ASSERT((uintptr_t)ptr % kUnitSize == 0);

    AllocationHeader* regionHeader = (AllocationHeader*)(ptr - kUnitSize);

    if (regionHeader->Freed)
    {
        SIAMESE_DEBUG_BREAK; // Double-free
        return;
    }
    regionHeader->Freed = true;

    WindowHeader* windowHeader = regionHeader->Header;
    if (!windowHeader)
    {
        FallbackFree(ptr);
        return;
    }

    const unsigned units = regionHeader->UsedUnits;
    SIAMESE_DEBUG_ASSERT(units >= 2 && units <= kFallbackThresholdUnits);

    SIAMESE_DEBUG_ASSERT((uint8_t*)regionHeader >= (uint8_t*)regionHeader->Header + kWindowHeaderBytes);
    unsigned regionStart = regionHeader->GetUnitStart();
    SIAMESE_DEBUG_ASSERT(regionStart < kWindowMaxUnits);
    SIAMESE_DEBUG_ASSERT(regionStart + regionHeader->UsedUnits <= kWindowMaxUnits);

    unsigned regionEnd = regionStart + units;

    // Resume scanning from this hole next time
    if (windowHeader->ResumeScanOffset > regionStart)
        windowHeader->ResumeScanOffset = regionStart;

    // Clear the units it was using
    windowHeader->Used.ClearRange(regionStart, regionEnd);

    // Give back the unit count
    windowHeader->FreeUnitCount += units;

    // If we may want to promote this to Preferred:
    if (windowHeader->FreeUnitCount >= kPreferredThresholdUnits &&
        windowHeader->InFullList)
    {
        windowHeader->InFullList = false;

        // Restart scanning from the front
        windowHeader->ResumeScanOffset = 0;

        // Remove from the FullWindows list
        WindowHeader* prev = windowHeader->Prev;
        WindowHeader* next = windowHeader->Next;
        if (prev)
            prev->Next = next;
        else
            FullWindowsHead = next;
        if (next)
            next->Prev = prev;
        SIAMESE_DEBUG_ASSERT(FullWindowsCount > 0);
        --FullWindowsCount;

        // Insert at end of the PreferredWindows list
        ++PreferredWindowsCount;
        windowHeader->Prev = nullptr;
        windowHeader->Next = nullptr;
        if (PreferredWindowsTail)
            PreferredWindowsTail->Next = windowHeader;
        else
            PreferredWindowsHead = windowHeader;
        PreferredWindowsTail = windowHeader;
    }

#ifdef SIAMESE_ALLOCATOR_SHRINK
    if (windowHeader->FreeUnitCount >= kWindowMaxUnits && !windowHeader->Preallocated)
    {
        // If we should do some bulk cleanup:
        if (++EmptyWindowCount >= kEmptyWindowCleanupThreshold)
            FreeEmptyWindows();
    }
#endif // SIAMESE_ALLOCATOR_SHRINK

    ALLOC_DEBUG_INTEGRITY_CHECK();
}

#ifdef SIAMESE_ALLOCATOR_SHRINK

void Allocator::FreeEmptyWindows()
{
    if (EmptyWindowCount <= kEmptyWindowMinimum)
        return;

    ALLOC_DEBUG_INTEGRITY_CHECK();

    for (WindowHeader* windowHeader = PreferredWindowsHead, *next, *prev = nullptr; windowHeader; prev = windowHeader, windowHeader = next)
    {
        next = windowHeader->Next;

        // If this window can be reclaimed:
        if (windowHeader->FreeUnitCount >= kWindowMaxUnits && !windowHeader->Preallocated)
        {
            if (prev)
                prev->Next = next;
            else
                PreferredWindowsHead = next;

            SIMDSafeFree(windowHeader);

            --PreferredWindowsCount;

            if (--EmptyWindowCount <= kEmptyWindowMinimum)
                break;
        }
    }

    ALLOC_DEBUG_INTEGRITY_CHECK();
}

#endif // SIAMESE_ALLOCATOR_SHRINK

uint8_t* Allocator::FallbackAllocate(unsigned bytes)
{
    // Calculate number of units required by this allocation
    // Note: +1 for the AllocationHeader
    const unsigned units = (bytes + kOverallocationBytes + kUnitSize - 1) / kUnitSize + 1;

    uint8_t* ptr = SIMDSafeAllocate(kUnitSize * units);

    AllocationHeader* regionHeader = (AllocationHeader*)ptr;
    regionHeader->Freed     = false;
    regionHeader->Header    = nullptr;
    regionHeader->UsedUnits = units;

    return ptr + kUnitSize;
}

void Allocator::FallbackFree(uint8_t* ptr)
{
    SIAMESE_DEBUG_ASSERT(ptr);
    SIMDSafeFree(ptr - kUnitSize);
}


} // namespace siamese
