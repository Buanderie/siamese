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

#include <stdint.h>


//------------------------------------------------------------------------------
// Portability macros

// Compiler-specific debug break
#if defined(_DEBUG) || defined(DEBUG)
    #define SIAMESE_DEBUG
    #ifdef _WIN32
        #define SIAMESE_DEBUG_BREAK() __debugbreak()
    #else
        #define SIAMESE_DEBUG_BREAK() __builtin_trap()
    #endif
    #define SIAMESE_DEBUG_ASSERT(cond) { if (!(cond)) { SIAMESE_DEBUG_BREAK(); } }
#else
    #define SIAMESE_DEBUG_BREAK() do {} while (false);
    #define SIAMESE_DEBUG_ASSERT(cond) do {} while (false);
#endif

// Compiler-specific force inline keyword
#ifdef _MSC_VER
    #define SIAMESE_FORCE_INLINE inline __forceinline
#else
    #define SIAMESE_FORCE_INLINE inline __attribute__((always_inline))
#endif


namespace siamese {


//------------------------------------------------------------------------------
// PCG PRNG
// From http://www.pcg-random.org/

class PCGRandom
{
public:
    void Seed(uint64_t y, uint64_t x = 0)
    {
        State = 0;
        Inc = (y << 1u) | 1u;
        Next();
        State += x;
        Next();
    }

    uint32_t Next()
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
SIAMESE_FORCE_INLINE uint32_t Int32Hash(uint32_t key)
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
// Timing

uint64_t GetTimeUsec();
uint64_t GetTimeMsec();
uint64_t GetSloppyMsec();


} // namespace siamese
