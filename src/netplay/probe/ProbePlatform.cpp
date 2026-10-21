/*
    Copyright 2016-2025 melonDS team, 2026 KH Days co-op mod

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

// melonDS's Platform interface for the headless determinism probe: plain stdio files, standard
// threads, the wireless link through the lockstep scheduler, no camera, mic, audio out or add-ons.

#include "Platform.h"
#include "../Lockstep.h"
#include "Probe.h"

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <thread>

namespace melonDS::Platform
{

static int ConsoleID(void* userdata)
{
    return userdata ? static_cast<Probe::ConsoleContext*>(userdata)->ID : 0;
}

void SignalStop(StopReason, void*) {}

std::string GetLocalFilePath(const std::string& filename)
{
    return filename;
}

static const char* ModeString(FileMode mode, bool exists)
{
    bool text = mode & FileMode::Text;
    if (mode & FileMode::Append)
        return text ? "a+" : "a+b";
    if ((mode & FileMode::ReadWrite) == FileMode::ReadWrite)
    {
        if ((mode & FileMode::Preserve) && exists) return text ? "r+" : "r+b";
        return text ? "w+" : "w+b";
    }
    if (mode & FileMode::Write)
    {
        if ((mode & FileMode::Preserve) && exists) return text ? "r+" : "r+b";
        return text ? "w" : "wb";
    }
    return text ? "r" : "rb";
}

FileHandle* OpenFile(const std::string& path, FileMode mode)
{
    if ((mode & (FileMode::ReadWrite | FileMode::Append)) == FileMode::None)
        return nullptr;
    bool exists = FileExists(path);
    if ((mode & FileMode::NoCreate) && !exists)
        return nullptr;
    FILE* f = std::fopen(path.c_str(), ModeString(mode, exists));
    return reinterpret_cast<FileHandle*>(f);
}

FileHandle* OpenLocalFile(const std::string& path, FileMode mode)
{
    return OpenFile(path, mode);
}

bool FileExists(const std::string& name)
{
    FILE* f = std::fopen(name.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

bool LocalFileExists(const std::string& name) { return FileExists(name); }
bool CheckFileWritable(const std::string&) { return true; }
bool CheckLocalFileWritable(const std::string&) { return true; }

bool CloseFile(FileHandle* file)
{
    return std::fclose(reinterpret_cast<FILE*>(file)) == 0;
}

bool IsEndOfFile(FileHandle* file)
{
    return std::feof(reinterpret_cast<FILE*>(file)) != 0;
}

bool FileReadLine(char* str, int count, FileHandle* file)
{
    return std::fgets(str, count, reinterpret_cast<FILE*>(file)) != nullptr;
}

u64 FilePosition(FileHandle* file)
{
    return static_cast<u64>(std::ftell(reinterpret_cast<FILE*>(file)));
}

bool FileSeek(FileHandle* file, s64 offset, FileSeekOrigin origin)
{
    int whence = origin == FileSeekOrigin::Start ? SEEK_SET :
                 origin == FileSeekOrigin::Current ? SEEK_CUR : SEEK_END;
    return std::fseek(reinterpret_cast<FILE*>(file), static_cast<long>(offset), whence) == 0;
}

void FileRewind(FileHandle* file)
{
    std::rewind(reinterpret_cast<FILE*>(file));
}

u64 FileRead(void* data, u64 size, u64 count, FileHandle* file)
{
    return std::fread(data, size, count, reinterpret_cast<FILE*>(file));
}

bool FileFlush(FileHandle* file)
{
    return std::fflush(reinterpret_cast<FILE*>(file)) == 0;
}

u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* file)
{
    return std::fwrite(data, size, count, reinterpret_cast<FILE*>(file));
}

u64 FileWriteFormatted(FileHandle* file, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = std::vfprintf(reinterpret_cast<FILE*>(file), fmt, args);
    va_end(args);
    return n < 0 ? 0 : static_cast<u64>(n);
}

u64 FileLength(FileHandle* file)
{
    FILE* f = reinterpret_cast<FILE*>(file);
    long pos = std::ftell(f);
    std::fseek(f, 0, SEEK_END);
    long len = std::ftell(f);
    std::fseek(f, pos, SEEK_SET);
    return static_cast<u64>(len);
}

void Log(LogLevel level, const char* fmt, ...)
{
    if (level < Probe::LogThreshold) return;
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
}

struct Thread
{
    std::thread T;
};

Thread* Thread_Create(std::function<void()> func)
{
    return new Thread{std::thread(std::move(func))};
}

void Thread_Free(Thread* thread)
{
    if (thread && thread->T.joinable()) thread->T.detach();
    delete thread;
}

void Thread_Wait(Thread* thread)
{
    if (thread && thread->T.joinable()) thread->T.join();
}

struct Semaphore
{
    std::mutex M;
    std::condition_variable CV;
    int Count = 0;
};

Semaphore* Semaphore_Create() { return new Semaphore; }
void Semaphore_Free(Semaphore* sema) { delete sema; }

void Semaphore_Reset(Semaphore* sema)
{
    std::lock_guard<std::mutex> lock(sema->M);
    sema->Count = 0;
}

void Semaphore_Wait(Semaphore* sema)
{
    std::unique_lock<std::mutex> lock(sema->M);
    sema->CV.wait(lock, [sema] { return sema->Count > 0; });
    sema->Count--;
}

bool Semaphore_TryWait(Semaphore* sema, int timeout_ms)
{
    std::unique_lock<std::mutex> lock(sema->M);
    if (timeout_ms == 0)
    {
        if (sema->Count == 0) return false;
    }
    else if (!sema->CV.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                [sema] { return sema->Count > 0; }))
    {
        return false;
    }
    sema->Count--;
    return true;
}

void Semaphore_Post(Semaphore* sema, int count)
{
    {
        std::lock_guard<std::mutex> lock(sema->M);
        sema->Count += count;
    }
    sema->CV.notify_all();
}

struct Mutex
{
    std::mutex M;
};

Mutex* Mutex_Create() { return new Mutex; }
void Mutex_Free(Mutex* mutex) { delete mutex; }
void Mutex_Lock(Mutex* mutex) { mutex->M.lock(); }
void Mutex_Unlock(Mutex* mutex) { mutex->M.unlock(); }
bool Mutex_TryLock(Mutex* mutex) { return mutex->M.try_lock(); }

void Sleep(u64 usecs)
{
    std::this_thread::sleep_for(std::chrono::microseconds(usecs));
}

static const auto StartTime = std::chrono::steady_clock::now();

u64 GetMSCount()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - StartTime).count();
}

u64 GetUSCount()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - StartTime).count();
}

void WriteNDSSave(const u8*, u32, u32, u32, void*) {}
void WriteGBASave(const u8*, u32, u32, u32, void*) {}
void WriteFirmware(const Firmware&, u32, u32, void*) {}
void WriteDateTime(int, int, int, int, int, int, void*) {}

void MP_Begin(void* userdata)
{
    Probe::Session().Begin(ConsoleID(userdata));
}

void MP_End(void* userdata)
{
    Probe::Session().End(ConsoleID(userdata));
}

int MP_SendPacket(u8* data, int len, u64 timestamp, void* userdata)
{
    return Probe::Session().SendPacket(ConsoleID(userdata), 0, data, len, timestamp);
}

int MP_RecvPacket(u8* data, u64* timestamp, void* userdata)
{
    return Probe::Session().RecvPacket(ConsoleID(userdata), data, timestamp, false);
}

int MP_SendCmd(u8* data, int len, u64 timestamp, void* userdata)
{
    return Probe::Session().SendPacket(ConsoleID(userdata), 1, data, len, timestamp);
}

int MP_SendReply(u8* data, int len, u64 timestamp, u16 aid, void* userdata)
{
    return Probe::Session().SendPacket(ConsoleID(userdata), 2 | (static_cast<u32>(aid) << 16),
                                       data, len, timestamp);
}

int MP_SendAck(u8* data, int len, u64 timestamp, void* userdata)
{
    return Probe::Session().SendPacket(ConsoleID(userdata), 3, data, len, timestamp);
}

int MP_RecvHostPacket(u8* data, u64* timestamp, void* userdata)
{
    return Probe::Session().RecvPacket(ConsoleID(userdata), data, timestamp, true);
}

u16 MP_RecvReplies(u8* data, u64 timestamp, u16 aidmask, void* userdata)
{
    return Probe::Session().RecvReplies(ConsoleID(userdata), data, timestamp, aidmask);
}

int Net_SendPacket(u8*, int len, void*) { return len; }
int Net_RecvPacket(u8*, void*) { return 0; }

void Camera_Start(int, void*) {}
void Camera_Stop(int, void*) {}
void Camera_CaptureFrame(int, u32*, int, int, bool, void*) {}

void Mic_Start(void*) {}
void Mic_Stop(void*) {}
int Mic_ReadInput(s16*, int, void*) { return 0; }

AACDecoder* AAC_Init() { return nullptr; }
void AAC_DeInit(AACDecoder*) {}
bool AAC_Configure(AACDecoder*, int, int) { return false; }
bool AAC_DecodeFrame(AACDecoder*, const void*, int, void*, int) { return false; }

bool Addon_KeyDown(KeyType, void*) { return false; }
void Addon_RumbleStart(u32, void*) {}
void Addon_RumbleStop(void*) {}
float Addon_MotionQuery(MotionQueryType, void*) { return 0.0f; }

DynamicLibrary* DynamicLibrary_Load(const char*) { return nullptr; }
void DynamicLibrary_Unload(DynamicLibrary*) {}
void* DynamicLibrary_LoadFunction(DynamicLibrary*, const char*) { return nullptr; }

}
