/*
    Copyright (c) 2017 Christopher A. Taylor.  All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.
    * Neither the name of Tonkinese nor the names of its contributors may be
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

#include "SiameseTools.h"

namespace siamese {


//------------------------------------------------------------------------------
// Timing

uint64_t GetTimeUsec()
{
#ifdef _WIN32
    LARGE_INTEGER timeStamp = {};
    if (!::QueryPerformanceCounter(&timeStamp))
        return 0;
    static double PerfFrequencyInverse = 0.;
    if (PerfFrequencyInverse == 0.)
    {
        LARGE_INTEGER freq = {};
        if (!::QueryPerformanceFrequency(&freq) || freq.QuadPart == 0)
            return 0;
        PerfFrequencyInverse = 1000000. / (double)freq.QuadPart;
    }
    return (uint64_t)(PerfFrequencyInverse * timeStamp.QuadPart);
#else
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return 1000000 * tv.tv_sec + tv.tv_usec;
#endif // _WIN32
}

uint64_t GetTimeMsec()
{
#ifdef _WIN32
    LARGE_INTEGER timeStamp = {};
    if (!::QueryPerformanceCounter(&timeStamp))
        return 0;
    static double PerfFrequencyInverse = 0.;
    if (PerfFrequencyInverse == 0.)
    {
        LARGE_INTEGER freq = {};
        if (!::QueryPerformanceFrequency(&freq) || freq.QuadPart == 0)
            return 0;
        PerfFrequencyInverse = 1000. / (double)freq.QuadPart;
    }
    return (uint64_t)(PerfFrequencyInverse * timeStamp.QuadPart);
#else
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return 1000 * tv.tv_sec + tv.tv_usec / 1000;
#endif // _WIN32
}

uint64_t GetSloppyMsec()
{
#ifdef _WIN32
    return ::GetTickCount64();
#else
    return GetTimeMsec();
#endif // _WIN32
}


} // namespace siamese
