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

#pragma once

/*
    Custom Memory Allocator

    It turned out that malloc() and calloc() amount to a great deal (1/3) of
    the overhead for Windows builds of the encoder.  To fix this we are using a
    custom memory allocator.

    Tuned for packets:
    The allocator is tuned for allocations around 1000 bytes that are freed in
    roughly the same order that they are allocated.

    Advantages:
    + Eliminates codec performance bottleneck.
    + All allocation requests will be aligned for SIMD operations.
    + No thread safety or debug check overhead penalties.
    + No contention with allocations from users of the library.
    + Aligned realloc() is supported.
    + Simpler cleanup: All memory automatically freed in destructor.

    Disadvantages:
    + Uses more memory than strictly necessary.
    + Extra complexity.
*/

#include "SiameseTools.h"

namespace siamese {


//------------------------------------------------------------------------------
// Allocator

// Define this to shrink the allocated memory when it is no longer used
//#define SIAMESE_ALLOCATOR_SHRINK

// After freeing memory, resume scanning for free spots from it if it is earlier
#define SIAMESE_RESUME_SCANNING_FROM_HOLES

// Zero out allocated memory
//#define SIAMESE_SCRUB_MEMORY

// Disable the allocator
//#define SIAMESE_DISABLE_ALLOCATOR

// Reallocate in-place
//#define SIAMESE_REALLOCATE_INPLACE

// Maintain data in buffer during reallocation?
enum class ReallocBehavior
{
    Uninitialized,
    CopyExisting
};

class Allocator
{
public:
    Allocator();
    ~Allocator();

    // Allocation API
    uint8_t* Allocate(unsigned bytes);
    uint8_t* Reallocate(uint8_t* ptr, unsigned bytes, ReallocBehavior behavior);
    void Free(uint8_t* ptr);

    // Placement new/delete
    template<class T>
    inline T* Construct()
    {
        uint8_t* mem = Allocate((unsigned)sizeof(T));
        if (!mem)
            return nullptr;
        return new (mem)T();
    }
    template<class T>
    inline void Destruct(T* obj)
    {
        if (obj)
        {
            obj->~T();
            Free((uint8_t*)obj);
        }
    }

    // Statistics API
    unsigned GetMemoryUsedBytes() const;
    unsigned GetMemoryAllocatedBytes() const;
    bool IntegrityCheck() const;

protected:
    // Minimum allocation unit
    // Note: Must be a power of two
    static const unsigned kUnitSize = kAlignmentBytes;

    // Maximum number of units per window, tuned for packet-sized data of around 1000 bytes.
    // Note: Tune this if the data sizes are larger
    static const unsigned kWindowMaxUnits = 2048;

    // Preallocated windows (about 128 KB on desktop)
    static const unsigned kPreallocatedWindows = 2;

    // Tuned for Siamese: Make room for metadata header and length field if we
    // need to add those.
    static const unsigned kOverallocationBytes = 8;

    // Preallocated windows on startup
    uint8_t* HugeChunkStart = nullptr;

    typedef CustomBitSet<kWindowMaxUnits> UsedMaskT;

    // This is at the front of each allocation window
    struct WindowHeader
    {
        UsedMaskT Used;

        // Total number of free units
        unsigned FreeUnitCount;

        // Offset to resume scanning for a free spot
        unsigned ResumeScanOffset;

        // Next, prev window header in the set
        WindowHeader* Next;
        WindowHeader* Prev;

        // Set to true if this is part of the full list
        bool InFullList;

        // Set to true if this is part of the preallocated chunk
        bool Preallocated;
    };

    // This is tagged on the front of each allocation so that Realloc()
    // and Free() are faster.
    struct AllocationHeader
    {
        // Header for this window
        // Note: This will be set to nullptr for fallback allocations
        WindowHeader* Header;

        // Number of units used right now
        unsigned UsedUnits;

        // Is this allocation already freed? (some minimal self-diagnostics)
        bool Freed;

        // Calculate which unit this allocation starts at in the window
        unsigned GetUnitStart()
        {
            return (unsigned)((uint8_t*)this - ((uint8_t*)Header + kWindowHeaderBytes)) / kUnitSize;
        }
    };

    static_assert(kUnitSize >= sizeof(AllocationHeader), "too small");

    // Round the window header size up to alignment size
    static const unsigned kWindowHeaderBytes = (sizeof(WindowHeader) + kAlignmentBytes - 1) & ~(kAlignmentBytes - 1);

    // Number of bytes per window
    static const unsigned kWindowSizeBytes = kWindowHeaderBytes + kWindowMaxUnits * kUnitSize;

    // List of "preferred" windows with lower utilization
    // We switch Preferred to Full when a scan fails to find an empty slot
    WindowHeader* PreferredWindowsHead = nullptr;
    WindowHeader* PreferredWindowsTail = nullptr;
    unsigned PreferredWindowsCount = 0;

    // List of "full" windows with higher utilization
    WindowHeader* FullWindowsHead = nullptr;
    unsigned FullWindowsCount = 0;

    // We switch Full to Preferred when it drops below 1/4 utilization
    static const unsigned kPreferredThresholdUnits = 3 * kWindowMaxUnits / 4;

#ifdef SIAMESE_ALLOCATOR_SHRINK
    // Counter of the number of empty windows, which triggers us to clean house on Free()
    unsigned EmptyWindowCount = 0;

    // Keep some windows around
    static const unsigned kEmptyWindowMinimum = 32;

    // Lazy cleanup after a certain point
    static const unsigned kEmptyWindowCleanupThreshold = 64;

    // Walk the preferred list and free any empty windows
    void FreeEmptyWindows();
#endif

    // Move windows to full list between [PreferredWindowHead, stopWindow) not including stopWindow
    // If stopWindow is nullptr, then it will attempt to move all windows
    void MoveFirstFewWindowsToFull(WindowHeader* stopWindow);

    // Allocate the units from a new window
    uint8_t* AllocateFromNewWindow(unsigned units);

    // When we can only fit a few in a window, switch to fallback
#ifdef SIAMESE_DISABLE_ALLOCATOR
    static const unsigned kFallbackThresholdUnits = 0;
#else
    static const unsigned kFallbackThresholdUnits = kWindowMaxUnits / 4;
#endif

    // Fallback functions used when the custom allocator will not work
    uint8_t* FallbackAllocate(unsigned bytes);
    void FallbackFree(uint8_t* ptr);
};


} // namespace siamese
