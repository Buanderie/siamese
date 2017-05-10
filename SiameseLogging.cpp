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

#include "SiameseLogging.h"

#ifndef ANDROID
#include <iostream>
#endif

namespace logging {


//------------------------------------------------------------------------------
// Level

static const char* kLevelStrings[(int)Level::Count] = {
    "Trace", "Debug", "Info", "Warning", "Error", "Silent"
};

const char* LevelToString(Level level)
{
    static_assert((int)Level::Count == 6, "Update this switch");
    SIAMESE_DEBUG_ASSERT((int)level >= 0 && level < Level::Count);
    return kLevelStrings[(int)level];
}

static const char kLevelChars[(int)Level::Count] = {
    't', 'd', 'I', 'W', '!', '?'
};

char LevelToChar(Level level)
{
    static_assert((int)Level::Count == 6, "Update this switch");
    SIAMESE_DEBUG_ASSERT((int)level >= 0 && level < Level::Count);
    return kLevelChars[(int)level];
}


//------------------------------------------------------------------------------
// OutputWorker

OutputWorker& OutputWorker::GetInstance()
{
    static OutputWorker worker;
    return worker;
}

extern "C" void AtExitWrapper()
{
    OutputWorker::GetInstance().Stop();
}

OutputWorker::OutputWorker()
{
    Terminated = true;

    Start();

    std::atexit(AtExitWrapper);
}

void OutputWorker::Start()
{
    Stop();

    QueuePublic.clear();
    QueuePrivate.clear();

    Overrun    = 0;
    Terminated = false;
    Thread     = std::make_unique<std::thread>(&OutputWorker::Loop, this);
}

void OutputWorker::Stop()
{
    if (Thread)
    {
        Terminated = true;
        QueueCondition.notify_all();

        try
        {
            Thread->join();
        }
        catch (std::system_error& /*err*/)
        {
        }
    }
    Thread = nullptr;
}

void OutputWorker::Write(LogStringBuffer& buffer)
{
    std::string str = buffer.LogStream.str();

    {
        std::lock_guard<std::mutex> locker(QueueLock);

        if (QueuePublic.size() >= kWorkQueueLimit)
            Overrun++;
        else
            QueuePublic.emplace_back(buffer.LogLevel, buffer.ChannelName, str);
    }

    QueueCondition.notify_all();
}

void OutputWorker::Loop()
{
    while (!Terminated)
    {
        int overrun;
        bool flushRequested = false;
        {
            std::unique_lock<std::mutex> locker(QueueLock);

            if (QueuePublic.empty())
                QueueCondition.wait(locker);

            std::swap(QueuePublic, QueuePrivate);

            overrun = Overrun;
            Overrun = 0;

            flushRequested = FlushRequested;
            FlushRequested = false;
        }

        for (auto& log : QueuePrivate)
            Log(log);

        // Handle log message overrun
        if (overrun > 0)
        {
            std::ostringstream oss;
            oss << "Queue overrun. Lost " << overrun << " log messages";
            std::string str = oss.str();
            QueuedMessage qm(Level::Error, "Logging", str);
            Log(qm);
        }

        QueuePrivate.clear();

        if (flushRequested)
            FlushCondition.notify_all();
    }
}

void OutputWorker::Log(QueuedMessage& message)
{
    std::ostringstream ss;
    ss << '{' << LevelToChar(message.LogLevel) << '-' << message.ChannelName << "} " << message.Message;

#ifdef ANDROID
    std::string fmtstr = ss.str();
    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "%s", fmtstr.c_str());
#else
    ss << std::endl;
    std::string fmtstr = ss.str();
    std::cout << fmtstr;
    #ifdef _WIN32
        ::OutputDebugStringA(fmtstr.c_str());
    #endif
#endif
}

void OutputWorker::Flush()
{
    std::unique_lock<std::mutex> locker(QueueLock);

    FlushRequested = true;
    QueueCondition.notify_all();

    FlushCondition.wait(locker);
}


//------------------------------------------------------------------------------
// Channel

Channel::Channel(const char* name, Level minLevel)
    : ChannelName(name)
    , ChannelMinLevel(minLevel)
{
}

std::string Channel::GetPrefix() const
{
    siamese::Locker locker(PrefixLock);
    return Prefix;
}

void Channel::SetPrefix(const std::string& prefix)
{
    siamese::Locker locker(PrefixLock);
    Prefix = prefix;
}


} // namespace logging
