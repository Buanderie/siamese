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

    PreferredWindows.reserve(kPreallocatedWindows);
    FullWindows.reserve(kPreallocatedWindows);

    HugeChunkStart = SIMDSafeAllocate(kWindowSizeBytes * kPreallocatedWindows);
    if (HugeChunkStart)
    {
        // Preallocate space
        uint8_t* windowStart = HugeChunkStart;
        for (unsigned i = 0; i < kPreallocatedWindows; ++i, windowStart += kWindowSizeBytes)
        {
            WindowHeader* windowHeader = (WindowHeader*)windowStart;
            windowHeader->Used.ClearAll();
            windowHeader->FreeUnitCount       = kWindowMaxUnits;
            windowHeader->ResumeScanOffset    = 0;
            windowHeader->FullVectorSelfIndex = -1;
            windowHeader->Preallocated        = true;

            PreferredWindows.push_back(windowHeader);
        }
    }
}

Allocator::~Allocator()
{
    const unsigned preferredCount = (unsigned)PreferredWindows.size();
    for (unsigned i = 0; i < preferredCount; ++i)
    {
        if (!PreferredWindows[i]->Preallocated)
            SIMDSafeFree(PreferredWindows[i]);
    }
    const unsigned fullCount = (unsigned)FullWindows.size();
    for (unsigned i = 0; i < fullCount; ++i)
    {
        if (!FullWindows[i]->Preallocated)
            SIMDSafeFree(FullWindows[i]);
    }
    SIMDSafeFree(HugeChunkStart);
}

unsigned Allocator::GetMemoryUsedBytes() const
{
    unsigned sum = 0;
    const unsigned preferredCount = (unsigned)PreferredWindows.size();
    for (unsigned i = 0; i < preferredCount; ++i)
    {
        sum += kWindowMaxUnits - PreferredWindows[i]->FreeUnitCount;
    }
    const unsigned fullCount = (unsigned)FullWindows.size();
    for (unsigned i = 0; i < fullCount; ++i)
    {
        sum += kWindowMaxUnits - FullWindows[i]->FreeUnitCount;
    }
    return sum * kUnitSize;
}

unsigned Allocator::GetMemoryAllocatedBytes() const
{
    return (unsigned)((PreferredWindows.size() + FullWindows.size()) * kWindowMaxUnits * kUnitSize);
}

bool Allocator::IntegrityCheck() const
{
    unsigned emptyCount = 0;
    unsigned preallocatedCount = 0;
    const unsigned preferredCount = (unsigned)PreferredWindows.size();
    for (unsigned i = 0; i < preferredCount; ++i)
    {
        WindowHeader* windowHeader = PreferredWindows[i];
        for (unsigned j = 0; j < preferredCount; ++j)
        {
            if (windowHeader == PreferredWindows[j] && i != j)
            {
                SIAMESE_DEBUG_BREAK; // Should never happen
                return false;
            }
        }
        if (windowHeader->FullVectorSelfIndex != -1)
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
    const unsigned fullCount = (unsigned)FullWindows.size();
    for (unsigned i = 0; i < fullCount; ++i)
    {
        WindowHeader* windowHeader = FullWindows[i];
        for (unsigned j = 0; j < preferredCount; ++j)
        {
            if (windowHeader == PreferredWindows[j])
            {
                SIAMESE_DEBUG_BREAK; // Should never happen
                return false;
            }
        }
        for (unsigned j = 0; j < fullCount; ++j)
        {
            if (windowHeader == FullWindows[j] && i != j)
            {
                SIAMESE_DEBUG_BREAK; // Should never happen
                return false;
            }
        }
        if (windowHeader->FullVectorSelfIndex < 0 || (unsigned)windowHeader->FullVectorSelfIndex >= fullCount)
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
#endif
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

    const unsigned preferredCount = (unsigned)PreferredWindows.size();
    for (unsigned i = 0; i < preferredCount; ++i)
    {
        WindowHeader* windowHeader = PreferredWindows[i];
        SIAMESE_DEBUG_ASSERT(windowHeader->FullVectorSelfIndex == -1);

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
#ifdef SIAMESE_SCRUB_MEMORY
            memset(region, 0, units * kUnitSize);
#endif
            AllocationHeader* regionHeader = (AllocationHeader*)region;
            regionHeader->Header    = windowHeader;
            regionHeader->UsedUnits = units;
            regionHeader->Freed     = false;

            // Update window header
#ifdef SIAMESE_ALLOCATOR_SHRINK
            if (windowHeader->FreeUnitCount >= kWindowMaxUnits && !windowHeader->Preallocated)
                --EmptyWindowCount;
#endif
            windowHeader->FreeUnitCount -= units;
            if (windowHeader->FreeUnitCount == 0)
                ++i;
            usedMask.SetRange(regionStart, regionStart + units);
            windowHeader->ResumeScanOffset = regionStart + units;

            uint8_t* data = region + kUnitSize;
            SIAMESE_DEBUG_ASSERT((uintptr_t)data % kUnitSize == 0);

            if (i > 0)
                MoveFirstFewWindowsToFull(i);
            SIAMESE_DEBUG_ASSERT((uint8_t*)regionHeader >= (uint8_t*)regionHeader->Header + kWindowHeaderBytes);
            SIAMESE_DEBUG_ASSERT(regionHeader->GetUnitStart() < kWindowMaxUnits);
            SIAMESE_DEBUG_ASSERT(regionHeader->GetUnitStart() + regionHeader->UsedUnits <= kWindowMaxUnits);
            return data;
        }
    }

    // Move all preferred windows to full since none of them worked out
    if (preferredCount > 0)
        MoveFirstFewWindowsToFull(preferredCount);

    return AllocateNewWindow(units);
}

void Allocator::MoveFirstFewWindowsToFull(unsigned moveCount)
{
    SIAMESE_DEBUG_ASSERT(moveCount > 0);

    unsigned fullCount = (unsigned)FullWindows.size();
    unsigned preferredCount = (unsigned)PreferredWindows.size();

#ifdef SIAMESE_DEBUG
    for (unsigned i = 0; i < (unsigned)PreferredWindows.size(); ++i)
    {
        SIAMESE_DEBUG_ASSERT(PreferredWindows[i]->FullVectorSelfIndex == -1);
    }
    for (unsigned i = 0; i < (unsigned)FullWindows.size(); ++i)
    {
        SIAMESE_DEBUG_ASSERT(FullWindows[i]->FullVectorSelfIndex == (int)i);
    }
#endif

    for (unsigned i = 0; i < moveCount; ++i)
    {
        WindowHeader* windowHeader = PreferredWindows[i];

        // If the scan offset tripped us to think it was full but we should just start over scanning:
        if (windowHeader->FreeUnitCount >= kPreferredThresholdUnits)
        {
            // Resume from 0
            windowHeader->ResumeScanOffset = 0;

            if (i >= preferredCount - 1)
                break;

            // Move it to the end of the PreferredWindows to allow it to clear out more
            // Note: This may cycle in another spotty window but that should be pretty rare
            PreferredWindows[i] = PreferredWindows[preferredCount - 1];
            PreferredWindows[preferredCount - 1] = windowHeader;
        }
        else
        {
            // Move to the end of the FullWindows
            windowHeader->FullVectorSelfIndex = fullCount;
            FullWindows.resize(fullCount + 1);
            FullWindows[fullCount] = windowHeader;
            ++fullCount;

            // Remove it from the PreferredWindows
            --preferredCount;
            if (i < preferredCount)
                PreferredWindows[i] = PreferredWindows[preferredCount];
            PreferredWindows.resize(preferredCount);

            --i;
            --moveCount;

            SIAMESE_DEBUG_ASSERT(preferredCount == (unsigned)PreferredWindows.size());
            SIAMESE_DEBUG_ASSERT(fullCount == (unsigned)FullWindows.size());
        }
    }

#ifdef SIAMESE_DEBUG
    SIAMESE_DEBUG_ASSERT(preferredCount == (unsigned)PreferredWindows.size());
    SIAMESE_DEBUG_ASSERT(fullCount == (unsigned)FullWindows.size());
    for (unsigned i = 0; i < (unsigned)PreferredWindows.size(); ++i)
    {
        SIAMESE_DEBUG_ASSERT(PreferredWindows[i]->FullVectorSelfIndex == -1);
    }
    for (unsigned i = 0; i < (unsigned)FullWindows.size(); ++i)
    {
        SIAMESE_DEBUG_ASSERT(FullWindows[i]->FullVectorSelfIndex == (int)i);
    }
#endif
}

uint8_t* Allocator::AllocateNewWindow(unsigned units)
{
    uint8_t* headerStart = SIMDSafeAllocate(kWindowSizeBytes);
    if (!headerStart)
        return nullptr; // Allocation failure

    // Update window header
    WindowHeader* windowHeader = (WindowHeader*)headerStart;
    windowHeader->Used.ClearAll();
    windowHeader->Used.SetRange(0, units);
    windowHeader->FreeUnitCount = kWindowMaxUnits - units;
    windowHeader->ResumeScanOffset = units;
    windowHeader->FullVectorSelfIndex = -1;
    windowHeader->Preallocated = false;

    // Expand the list of preferred windows
    PreferredWindows.push_back(windowHeader);

    // Carve out region
    AllocationHeader* regionHeader = (AllocationHeader*)(headerStart + kWindowHeaderBytes);
    regionHeader->Header    = windowHeader;
    regionHeader->UsedUnits = units;
    regionHeader->Freed     = false;

#ifdef SIAMESE_SCRUB_MEMORY
    memset(regionHeader, 0, units * kUnitSize);
#endif
    uint8_t* data = (uint8_t*)regionHeader + kUnitSize;
    SIAMESE_DEBUG_ASSERT((uintptr_t)data % kUnitSize == 0);
    return data;
}

uint8_t* Allocator::Reallocate(uint8_t* ptr, unsigned bytes, ReallocBehavior behavior)
{
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
#endif

    // If the existing allocation is big enough:
    const unsigned requestedUnits = (bytes + kUnitSize - 1) / kUnitSize + 1;
    if (requestedUnits <= existingUnits)
        return ptr; // No change needed

#ifdef SIAMESE_REALLOCATE_INPLACE
    // If this is not a fallback allocation:
    WindowHeader* windowHeader = regionHeader->Header;
    if (windowHeader)
    {
        const unsigned regionStart = regionHeader->GetUnitStart();
        unsigned regionEnd = regionStart + existingUnits;
        unsigned targetEnd = regionStart + requestedUnits;
        SIAMESE_DEBUG_ASSERT(targetEnd > regionEnd);

        // If there may be room:
        if (targetEnd <= kWindowMaxUnits)
        {
            SIAMESE_DEBUG_ASSERT(regionEnd < kWindowMaxUnits);
            unsigned foundUsed = windowHeader->Used.FindFirstSet(regionEnd, targetEnd);

            // If there is room:
            if (foundUsed >= targetEnd)
            {
                windowHeader->Used.SetRange(regionEnd, targetEnd);
                windowHeader->FreeUnitCount -= requestedUnits - existingUnits;

                regionHeader->UsedUnits += requestedUnits - existingUnits;

                windowHeader->ResumeScanOffset = targetEnd;
                return ptr;
            }
        }
    }
#endif // SIAMESE_REALLOCATE_INPLACE

    // Allocate new data
    uint8_t* newPtr = Allocate(bytes);
    if (!newPtr)
        return nullptr;

    // Copy old data
    if (behavior == ReallocBehavior::CopyExisting)
        memcpy(newPtr, ptr, (existingUnits - 1) * kUnitSize);

    Free(ptr);

    return newPtr;
}

void Allocator::Free(uint8_t* ptr)
{
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

#ifdef SIAMESE_RESUME_SCANNING_FROM_HOLES
    // Resume scanning from this hole next time
    if (windowHeader->ResumeScanOffset > regionStart)
        windowHeader->ResumeScanOffset = regionStart;
#endif

    // Clear the units it was using
    windowHeader->Used.ClearRange(regionStart, regionEnd);

    // Give back the unit count
    windowHeader->FreeUnitCount += units;

    // If we may want to promote this to Preferred:
    if (windowHeader->FreeUnitCount >= kPreferredThresholdUnits &&
        windowHeader->FullVectorSelfIndex >= 0)
    {
        const unsigned fullVectorSize = (unsigned)FullWindows.size();
        const unsigned fullVectorIndex = windowHeader->FullVectorSelfIndex;
        SIAMESE_DEBUG_ASSERT(fullVectorIndex < fullVectorSize);
        SIAMESE_DEBUG_ASSERT(FullWindows[fullVectorIndex] == windowHeader);

        // Swap full
        if (fullVectorIndex < fullVectorSize - 1)
        {
            FullWindows[fullVectorIndex] = FullWindows[fullVectorSize - 1];
            FullWindows[fullVectorIndex]->FullVectorSelfIndex = fullVectorIndex;
        }
        FullWindows.resize(fullVectorSize - 1);

        // Add to end of preferred
        windowHeader->FullVectorSelfIndex = -1;
        PreferredWindows.push_back(windowHeader);

        // Start scanning from the front
        windowHeader->ResumeScanOffset = 0;
    }

#ifdef SIAMESE_ALLOCATOR_SHRINK
    if (windowHeader->FreeUnitCount >= kWindowMaxUnits && !windowHeader->Preallocated)
    {
        // If we should do some bulk cleanup:
        if (++EmptyWindowCount >= kEmptyWindowCleanupThreshold)
            FreeEmptyWindows();
    }
#endif
}

#ifdef SIAMESE_ALLOCATOR_SHRINK

void Allocator::FreeEmptyWindows()
{
    if (EmptyWindowCount <= kEmptyWindowMinimum)
        return;

    unsigned preferredCount = (unsigned)PreferredWindows.size();
    SIAMESE_DEBUG_ASSERT(preferredCount >= EmptyWindowCount);

    for (unsigned i = 0; i < preferredCount; ++i)
    {
        WindowHeader* windowHeader = PreferredWindows[i];
        if (windowHeader->FreeUnitCount >= kWindowMaxUnits && !windowHeader->Preallocated)
        {
            SIMDSafeFree(windowHeader);
            --preferredCount;
            PreferredWindows[i] = PreferredWindows[preferredCount];
            --i;
            if (--EmptyWindowCount <= kEmptyWindowMinimum)
                break;
        }
    }

    PreferredWindows.resize(preferredCount);
    SIAMESE_DEBUG_ASSERT(preferredCount >= EmptyWindowCount);

#ifdef SIAMESE_DEBUG
    IntegrityCheck();
#endif
}

#endif

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
