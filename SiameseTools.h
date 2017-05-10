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
    Tools:

    + System headers
    + Debug breakpoints/asserts
    + Compiler-specific code wrappers
    + PCGRandom, Int32Hash implementations
    + Reentrant mutexes
    + CustomBitSet structure for fast operations on arrays of bits
    + Microsecond timing
*/

//------------------------------------------------------------------------------
// Common Includes

#include "gf256.h"

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN

    #ifndef _WINSOCKAPI_
        #define DID_DEFINE_WINSOCKAPI
        #define _WINSOCKAPI_
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef _WIN32_WINNT
        #define _WIN32_WINNT 0x0601 /* Windows 7+ */
    #endif

    #include <windows.h>
#endif

#ifdef DID_DEFINE_WINSOCKAPI
    #undef _WINSOCKAPI_
    #undef DID_DEFINE_WINSOCKAPI
#endif

#ifdef _WIN32
    #include <intrin.h>
#endif


//------------------------------------------------------------------------------
// Tools

namespace siamese {

// Some bugs only repro in release mode, so this can be helpful
//#define SIAMESE_DEBUG_IN_RELEASE

#if defined(_DEBUG) || defined(DEBUG) || defined(SIAMESE_DEBUG_IN_RELEASE)
    #define SIAMESE_DEBUG
    #ifdef _WIN32
        #define SIAMESE_DEBUG_BREAK __debugbreak()
    #else
        #define SIAMESE_DEBUG_BREAK __builtin_trap()
    #endif
    #define SIAMESE_DEBUG_ASSERT(cond) { if (!(cond)) { SIAMESE_DEBUG_BREAK; } }
#else
    #define SIAMESE_DEBUG_BREAK ;
    #define SIAMESE_DEBUG_ASSERT(cond) ;
#endif


//------------------------------------------------------------------------------
// Platform

static const unsigned kAlignmentBytes = GF256_ALIGN_BYTES;

GF256_FORCE_INLINE unsigned NextAlignedOffset(unsigned offset)
{
    return (offset + kAlignmentBytes - 1) & ~(kAlignmentBytes - 1);
}


//------------------------------------------------------------------------------
// Portable Intrinsics

// Returns number of bits set in the 64-bit value
GF256_FORCE_INLINE unsigned PopCount64(uint64_t x)
{
#ifdef _MSC_VER
#ifdef _WIN64
    return (unsigned)__popcnt64(x);
#else
    return (unsigned)(__popcnt((uint32_t)x) + __popcnt((uint32_t)(x >> 32)));
#endif
#else // GCC
    return (unsigned)__builtin_popcountll(x);
#endif
}

// Returns lowest bit index 0..63 where the first non-zero bit is found
// Precondition: x != 0
GF256_FORCE_INLINE unsigned FirstNonzeroBit64(uint64_t x)
{
#ifdef _MSC_VER
#ifdef _WIN64
    unsigned long index;
    // Note: Ignoring result because x != 0
    _BitScanForward64(&index, x);
    return (unsigned)index;
#else
    unsigned long index;
    if (0 != _BitScanForward(&index, (uint32_t)x))
        return (unsigned)index;
    // Note: Ignoring result because x != 0
    _BitScanForward(&index, (uint32_t)(x >> 32));
    return (unsigned)index + 32;
#endif
#else
    // Note: Ignoring return value of 0 because x != 0
    return (unsigned)__builtin_ffsll(x) - 1;
#endif
}


//------------------------------------------------------------------------------
// PCG PRNG
// From http://www.pcg-random.org/

class PCGRandom
{
public:
    inline void Seed(uint64_t y, uint64_t x = 0)
    {
        State = 0;
        Inc = (y << 1u) | 1u;
        Next();
        State += x;
        Next();
    }

    inline uint32_t Next()
    {
        const uint64_t oldstate = State;
        State = oldstate * UINT64_C(6364136223846793005) + Inc;
        const uint32_t xorshifted = (uint32_t)(((oldstate >> 18) ^ oldstate) >> 27);
        const uint32_t rot = oldstate >> 59;
        return (xorshifted >> rot) | (xorshifted << ((uint32_t)(-(int32_t)rot) & 31));
    }

    uint64_t State = 0, Inc = 0;
};


//------------------------------------------------------------------------------
// Int32Hash

// Thomas Wang's 32-bit -> 32-bit integer hash function
// http://burtleburtle.net/bob/hash/integer.html
inline uint32_t Int32Hash(uint32_t key)
{
    key += ~(key << 15);
    key ^= (key >> 10);
    key += (key << 3);
    key ^= (key >> 6);
    key += ~(key << 11);
    key ^= (key >> 16);
    return key;
}


//------------------------------------------------------------------------------
// CustomBitSet

// Custom std::bitset implementation for speed
template<unsigned N>
struct CustomBitSet
{
    static const unsigned kValidBits = N;
    typedef uint64_t WordT;
    static const unsigned kWordBits = sizeof(WordT) * 8;
    static const unsigned kWords = (kValidBits + kWordBits - 1) / kWordBits;
    static const WordT kAllOnes = UINT64_C(0xffffffffffffffff);

    WordT Words[kWords];


    CustomBitSet()
    {
        ClearAll();
    }

    void ClearAll()
    {
        for (unsigned i = 0; i < kWords; ++i)
            Words[i] = 0;
    }
    void SetAll()
    {
        for (unsigned i = 0; i < kWords; ++i)
            Words[i] = kAllOnes;
    }
    void Set(unsigned bit)
    {
        const unsigned word = bit / kWordBits;
        const WordT mask = (WordT)1 << (bit % kWordBits);
        Words[word] |= mask;
    }
    void Clear(unsigned bit)
    {
        const unsigned word = bit / kWordBits;
        const WordT mask = (WordT)1 << (bit % kWordBits);
        Words[word] &= ~mask;
    }
    bool Check(unsigned bit) const
    {
        const unsigned word = bit / kWordBits;
        const WordT mask = (WordT)1 << (bit % kWordBits);
        return 0 != (Words[word] & mask);
    }

    /*
        Returns the popcount of the bits within the given range.

        bitStart < kValidBits: First bit to test
        bitEnd <= kValidBits: Bit to stop at (non-inclusive)
    */
    unsigned RangePopcount(unsigned bitStart, unsigned bitEnd)
    {
        static_assert(kWordBits == 64, "Update this");

        if (bitStart >= bitEnd)
            return 0;

        unsigned wordIndex = bitStart / kWordBits;
        const unsigned wordEnd = bitEnd / kWordBits;

        // Eliminate low bits of first word
        WordT word = Words[wordIndex] >> (bitStart % kWordBits);

        // Eliminate high bits of last word if there is just one word
        if (wordEnd == wordIndex)
            return PopCount64(word << (kWordBits - (bitEnd - bitStart)));

        // Count remainder of first word
        unsigned count = PopCount64(word);

        // Accumulate popcount of full words
        while (++wordIndex < wordEnd)
            count += PopCount64(Words[wordIndex]);

        // Eliminate high bits of last word if there is one
        unsigned lastWordBits = bitEnd - wordIndex * kWordBits;
        if (lastWordBits > 0)
            count += PopCount64(Words[wordIndex] << (kWordBits - lastWordBits));

        return count;
    }

    /*
        Returns the bit index where the first cleared bit is found.
        Returns kValidBits if all bits are set.

        bitStart < kValidBits: Index to start looking
    */
    unsigned FindFirstClear(unsigned bitStart)
    {
        static_assert(kWordBits == 64, "Update this");

        unsigned wordStart = bitStart / kWordBits;

        WordT word = ~Words[wordStart] >> (bitStart % kWordBits);
        if (word != 0)
        {
            unsigned offset = 0;
            if ((word & 1) == 0)
                offset = FirstNonzeroBit64(word);
            return bitStart + offset;
        }

        for (unsigned i = wordStart + 1; i < kWords; ++i)
        {
            word = ~Words[i];
            if (word != 0)
                return i * kWordBits + FirstNonzeroBit64(word);
        }

        return kValidBits;
    }

    /*
        Returns the bit index where the first set bit is found.
        Returns 'bitEnd' if all bits are clear.

        bitStart < kValidBits: Index to start looking
        bitEnd <= kValidBits: Index to stop looking at
    */
    unsigned FindFirstSet(unsigned bitStart, unsigned bitEnd = kValidBits)
    {
        static_assert(kWordBits == 64, "Update this");

        unsigned wordStart = bitStart / kWordBits;

        WordT word = Words[wordStart] >> (bitStart % kWordBits);
        if (word != 0)
        {
            unsigned offset = 0;
            if ((word & 1) == 0)
                offset = FirstNonzeroBit64(word);
            return bitStart + offset;
        }

        const unsigned wordEnd = (bitEnd + kWordBits - 1) / kWordBits;

        for (unsigned i = wordStart + 1; i < wordEnd; ++i)
        {
            word = Words[i];
            if (word != 0)
                return i * kWordBits + FirstNonzeroBit64(word);
        }

        return bitEnd;
    }

    /*
        Set a range of bits

        bitStart < kValidBits: Index at which to start setting
        bitEnd <= kValidBits: Bit to stop at (non-inclusive)
    */
    void SetRange(unsigned bitStart, unsigned bitEnd)
    {
        if (bitStart >= bitEnd)
            return;

        unsigned wordStart = bitStart / kWordBits;
        const unsigned wordEnd = bitEnd / kWordBits;

        bitStart %= kWordBits;

        if (wordEnd == wordStart)
        {
            // This implies x=(bitStart % kWordBits) and y=(bitEnd % kWordBits)
            // are in the same word.  Also: x < y, y < 64, y - x < 64.
            bitEnd %= kWordBits;
            WordT mask = ((WordT)1 << (bitEnd - bitStart)) - 1; // 1..63 bits
            mask <<= bitStart;
            Words[wordStart] |= mask;
            return;
        }

        // Set the end of the first word
        Words[wordStart] |= kAllOnes << bitStart;

        // Whole words at a time
        for (unsigned i = wordStart + 1; i < wordEnd; ++i)
            Words[i] = kAllOnes;

        // Set first few bits of the last word
        unsigned lastWordBits = bitEnd - wordEnd * kWordBits;
        if (lastWordBits > 0)
        {
            WordT mask = ((WordT)1 << lastWordBits) - 1; // 1..63 bits
            Words[wordEnd] |= mask;
        }
    }

    /*
        Clear a range of bits

        bitStart < kValidBits: Index at which to start clearing
        bitEnd <= kValidBits: Bit to stop at (non-inclusive)
    */
    void ClearRange(unsigned bitStart, unsigned bitEnd)
    {
        if (bitStart >= bitEnd)
            return;

        unsigned wordStart = bitStart / kWordBits;
        const unsigned wordEnd = bitEnd / kWordBits;

        bitStart %= kWordBits;

        if (wordEnd == wordStart)
        {
            // This implies x=(bitStart % kWordBits) and y=(bitEnd % kWordBits)
            // are in the same word.  Also: x < y, y < 64, y - x < 64.
            bitEnd %= kWordBits;
            WordT mask = ((WordT)1 << (bitEnd - bitStart)) - 1; // 1..63 bits
            mask <<= bitStart;
            Words[wordStart] &= ~mask;
            return;
        }

        // Clear the end of the first word
        Words[wordStart] &= ~(kAllOnes << bitStart);

        // Whole words at a time
        for (unsigned i = wordStart + 1; i < wordEnd; ++i)
            Words[i] = 0;

        // Clear first few bits of the last word
        unsigned lastWordBits = bitEnd - wordEnd * kWordBits;
        if (lastWordBits > 0)
        {
            WordT mask = ((WordT)1 << lastWordBits) - 1; // 1..63 bits
            Words[wordEnd] &= ~mask;
        }
    }
};


//------------------------------------------------------------------------------
// Mutex

#ifdef _WIN32

struct Lock
{
    CRITICAL_SECTION cs;

    Lock() { ::InitializeCriticalSectionAndSpinCount(&cs, 1000); }
    ~Lock() { ::DeleteCriticalSection(&cs); }
    bool TryEnter() { return ::TryEnterCriticalSection(&cs) != FALSE; }
    void Enter() { ::EnterCriticalSection(&cs); }
    void Leave() { ::LeaveCriticalSection(&cs); }
};

#else

#include <mutex>

struct Lock
{
    std::recursive_mutex cs;

    bool TryEnter() { return cs.try_lock(); }
    void Enter() { cs.lock(); }
    void Leave() { cs.unlock(); }
};

#endif

class Locker
{
public:
    Locker(Lock& lock) {
        TheLock = &lock;
        if (TheLock)
            TheLock->Enter();
    }
    ~Locker() { Clear(); }
    bool TrySet(Lock& lock) {
        Clear();
        if (!lock.TryEnter())
            return false;
        TheLock = &lock;
        return true;
    }
    void Set(Lock& lock) {
        Clear();
        lock.Enter();
        TheLock = &lock;
    }
    void Clear() {
        if (TheLock)
            TheLock->Leave();
        TheLock = nullptr;
    }
private:
    Lock* TheLock;
};


//------------------------------------------------------------------------------
// Timing

uint64_t GetTimeUsec();
uint64_t GetTimeMsec();
uint64_t GetSloppyMsec();


} // namespace siamese
