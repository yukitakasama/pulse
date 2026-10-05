#pragma once
#include "elevated_transfer_protocol.h"
#include <functional>
#include <utility>

namespace pulse::elevated {
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.Release()) {}
    Handle& operator=(Handle&& other) noexcept { if (this != &other) Reset(other.Release()); return *this; }
    HANDLE Get() const { return value_; }
    explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
    HANDLE Release() { return std::exchange(value_, nullptr); }
    void Reset(HANDLE value = nullptr) { if (*this) CloseHandle(value_); value_ = value; }
private:
    HANDLE value_ = nullptr;
};
using Tick = std::function<bool()>;
bool ReadFrame(HANDLE pipe, HANDLE peer, const Nonce& nonce, Header& header,
               std::vector<unsigned char>& payload, const Tick& tick = {}, DWORD timeout_ms = INFINITE);
bool WriteFrame(HANDLE pipe, HANDLE peer, const Nonce& nonce, Kind kind, uint64_t request,
                const Writer& payload, DWORD timeout_ms = 10000);
bool WaitPipeConnection(HANDLE pipe, HANDLE peer, DWORD timeout_ms);
std::wstring ProcessImage(HANDLE process);
std::vector<unsigned char> UserSid(HANDLE process);
bool SameUser(HANDLE first, HANDLE second);
bool IsElevated(HANDLE process);
std::wstring PipeName(DWORD parent, const Nonce& nonce);
Handle CreateSessionPipe(const std::wstring& name);
}
