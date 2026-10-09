/*
 * Behaviour-faithful Win32 API shim for router2 snapshot tests on POSIX.
 * This is not a Windows-native runtime: CreateFileA(CREATE_NEW) is O_EXCL,
 * MoveFileExA(MOVEFILE_REPLACE_EXISTING) is rename(2) without an unlink gap.
 */
#ifndef ROUTER2_PROFILE_WIN32_SHIM_H
#define ROUTER2_PROFILE_WIN32_SHIM_H

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

using HANDLE = void *;
using BOOL = int;
using DWORD = unsigned long;

#ifndef INVALID_HANDLE_VALUE
#define INVALID_HANDLE_VALUE ((HANDLE)(std::intptr_t) - 1)
#endif
#ifndef GENERIC_WRITE
#define GENERIC_WRITE 0x40000000UL
#endif
#ifndef CREATE_NEW
#define CREATE_NEW 1
#endif
#ifndef FILE_ATTRIBUTE_NORMAL
#define FILE_ATTRIBUTE_NORMAL 0x00000080UL
#endif
#ifndef MOVEFILE_REPLACE_EXISTING
#define MOVEFILE_REPLACE_EXISTING 0x00000001UL
#endif
#ifndef ERROR_FILE_EXISTS
#define ERROR_FILE_EXISTS 80UL
#endif
#ifndef ERROR_ALREADY_EXISTS
#define ERROR_ALREADY_EXISTS 183UL
#endif
#ifndef ERROR_FILE_NOT_FOUND
#define ERROR_FILE_NOT_FOUND 2UL
#endif

struct Router2Win32ShimState
{
    std::mutex mutex;
    DWORD last_error = 0;
    int create_new = 0;
    int replace_existing = 0;
    int delete_file = 0;
    std::vector<std::string> created;
    std::vector<std::pair<std::string, std::string>> replaced;
    std::vector<std::string> deleted;
};

inline Router2Win32ShimState &router2_win32_shim()
{
    static Router2Win32ShimState state;
    return state;
}

inline DWORD GetLastError() { return router2_win32_shim().last_error; }

inline DWORD GetCurrentProcessId() { return DWORD(::getpid()); }

inline HANDLE CreateFileA(const char *name, DWORD access, DWORD, void *, DWORD disposition, DWORD, HANDLE)
{
    (void)access;
    auto &shim = router2_win32_shim();
    if (disposition != CREATE_NEW) {
        shim.last_error = ERROR_FILE_NOT_FOUND;
        return INVALID_HANDLE_VALUE;
    }
    int fd = ::open(name, O_CREAT | O_EXCL | O_RDWR, 0644);
    if (fd < 0) {
        shim.last_error = (errno == EEXIST) ? ERROR_ALREADY_EXISTS : ERROR_FILE_EXISTS;
        return INVALID_HANDLE_VALUE;
    }
    std::lock_guard<std::mutex> lock(shim.mutex);
    ++shim.create_new;
    shim.created.push_back(name);
    return reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(fd));
}

inline BOOL CloseHandle(HANDLE handle)
{
    int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(handle));
    return ::close(fd) == 0;
}

inline BOOL WriteFile(HANDLE handle, const void *buffer, DWORD bytes, DWORD *written, void *)
{
    int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(handle));
    ssize_t n = ::write(fd, buffer, bytes);
    if (n < 0)
        return 0;
    if (written)
        *written = DWORD(n);
    return 1;
}

inline BOOL FlushFileBuffers(HANDLE handle)
{
    int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(handle));
    return ::fsync(fd) == 0;
}

inline BOOL MoveFileExA(const char *from, const char *to, DWORD flags)
{
    auto &shim = router2_win32_shim();
    if ((flags & MOVEFILE_REPLACE_EXISTING) == 0) {
        struct stat info;
        if (::stat(to, &info) == 0) {
            shim.last_error = ERROR_ALREADY_EXISTS;
            return 0;
        }
    }
    if (::rename(from, to) != 0) {
        shim.last_error = ERROR_FILE_NOT_FOUND;
        return 0;
    }
    std::lock_guard<std::mutex> lock(shim.mutex);
    if (flags & MOVEFILE_REPLACE_EXISTING)
        ++shim.replace_existing;
    shim.replaced.push_back({from, to});
    return 1;
}

inline BOOL DeleteFileA(const char *name)
{
    auto &shim = router2_win32_shim();
    std::lock_guard<std::mutex> lock(shim.mutex);
    ++shim.delete_file;
    shim.deleted.push_back(name);
    return ::unlink(name) == 0;
}

#endif
