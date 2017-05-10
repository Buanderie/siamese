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
    Logging

    This is a simple multithreaded logging library supporting Levels and Flush.

    If messages are logged faster than we can write them to the console, it will
    drop data and write how many were dropped (above kWorkQueueLimit).
    Errors bypass this limit and will force a Flush.

    On Android it uses __android_log_print().
    On other platforms it uses std::cout.
    On Windows it also uses OutputDebugStringA().

    To reuse the code, modify this function to change how it writes:
        OutputWorker::Log()
*/

#include "SiameseTools.h"

#include <sstream>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <list>

namespace logging {


//------------------------------------------------------------------------------
// Level

enum class Level
{
    Trace,      // Trace-level logging (off by default)
    Debug,      // Debug logging (on by default)
    Info,       // Info (normal) logging
    Warning,    // Warnings
    Error,      // Errors
    Silent,     // Silent level (always off)

    Count // For static assert
};

const char* LevelToString(Level level);
char LevelToChar(Level level);


//------------------------------------------------------------------------------
// Buffer

struct LogStringBuffer
{
    const char* ChannelName;
    Level LogLevel;
    std::ostringstream LogStream;

    LogStringBuffer(const char* channel, Level level) :
        ChannelName(channel),
        LogLevel(level),
        LogStream()
    {
    }
};


//------------------------------------------------------------------------------
// Stringize

template<typename T>
GF256_FORCE_INLINE void LogStringize(LogStringBuffer& buffer, const T& first)
{
    buffer.LogStream << first;
}

// Overrides for various types we want to handle specially:

template<>
GF256_FORCE_INLINE void LogStringize(LogStringBuffer& buffer, const bool& first)
{
    buffer.LogStream << (first ? "true" : "false");
}


//------------------------------------------------------------------------------
// OutputWorker

class OutputWorker
{
    OutputWorker();

public:
    static OutputWorker& GetInstance();
    void Write(LogStringBuffer& buffer);
    void Start();
    void Stop();
    void Flush();

private:
    struct QueuedMessage
    {
        Level LogLevel;
        const char* ChannelName;
        std::string Message;

        QueuedMessage(Level level, const char* channel, std::string& message)
            : LogLevel(level)
            , ChannelName(channel)
            , Message(message)
        {
        }
    };

    static const size_t kWorkQueueLimit = 4096;

    mutable std::mutex QueueLock;

    std::condition_variable QueueCondition;
    std::list<QueuedMessage> QueuePublic;
    std::list<QueuedMessage> QueuePrivate;
    std::atomic_int Overrun = 0;
    std::atomic_bool FlushRequested = 0;

    std::condition_variable FlushCondition;

    std::unique_ptr<std::thread> Thread;
    std::atomic_bool Terminated;

    void Loop();

    void Log(QueuedMessage& message);
};


//------------------------------------------------------------------------------
// Channel

class Channel
{
public:
    const char* const ChannelName;
    const Level ChannelMinLevel;


    GF256_FORCE_INLINE bool ShouldLog(Level level) const
    {
        return level >= ChannelMinLevel;
    }

    explicit Channel(const char* name, Level minLevel);

    std::string GetPrefix() const;
    void SetPrefix(const std::string& prefix);

    template<typename... Args>
    GF256_FORCE_INLINE void Log(Level level, Args&&... args) const
    {
        if (ShouldLog(level))
            log(level, std::forward<Args>(args)...);
    }

    template<typename... Args>
    GF256_FORCE_INLINE void Error(Args&&... args) const
    {
        OutputWorker::GetInstance().Flush();

        Log(Level::Error, std::forward<Args>(args)...);

        OutputWorker::GetInstance().Flush();
    }

    template<typename... Args>
    GF256_FORCE_INLINE void Warning(Args&&... args) const
    {
        Log(Level::Warning, std::forward<Args>(args)...);
    }

    template<typename... Args>
    GF256_FORCE_INLINE void Info(Args&&... args) const
    {
        Log(Level::Info, std::forward<Args>(args)...);
    }

    template<typename... Args>
    GF256_FORCE_INLINE void Debug(Args&&... args) const
    {
        Log(Level::Debug, std::forward<Args>(args)...);
    }

    template<typename... Args>
    GF256_FORCE_INLINE void Trace(Args&&... args) const
    {
        Log(Level::Trace, std::forward<Args>(args)...);
    }

private:
    mutable siamese::Lock PrefixLock;
    std::string Prefix;

    template<typename T>
    GF256_FORCE_INLINE void writeLogBuffer(LogStringBuffer& buffer, T&& arg) const
    {
        LogStringize(buffer, arg);
    }

    template<typename T, typename... Args>
    GF256_FORCE_INLINE void writeLogBuffer(LogStringBuffer& buffer, T&& arg, Args&&... args) const
    {
        writeLogBuffer(buffer, arg);
        writeLogBuffer(buffer, args...);
    }

    template<typename... Args>
    GF256_FORCE_INLINE void log(Level level, Args&&... args) const
    {
        LogStringBuffer buffer(ChannelName, level);
        writeLogBuffer(buffer, Prefix, args...);
        OutputWorker::GetInstance().Write(buffer);
    }
};


} // namespace logging
