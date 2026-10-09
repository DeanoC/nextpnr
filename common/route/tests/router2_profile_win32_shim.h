/*
 * Behaviour-faithful Win32 API shim for router2 snapshot tests on POSIX.
 * This is not a Windows-native runtime.
 * A APIs interpret names as a non-UTF-8 ANSI page (bytes >= 0x80 become '?').
 * W APIs keep UTF-16 from MultiByteToWideChar(CP_UTF8) and map back to UTF-8
 * for POSIX open/rename. CreateFileW(CREATE_NEW) is O_EXCL; MoveFileExW with
 * MOVEFILE_REPLACE_EXISTING is rename(2) without an unlink gap.
 */
#ifndef ROUTER2_PROFILE_WIN32_SHIM_H
#define ROUTER2_PROFILE_WIN32_SHIM_H

#include <cerrno>
#include <cstdint>
#include <cstring>
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
using WCHAR = wchar_t;
using LPCWSTR = const wchar_t *;
using UINT = unsigned;

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
#ifndef ERROR_NO_UNICODE_TRANSLATION
#define ERROR_NO_UNICODE_TRANSLATION 1113UL
#endif
#ifndef CP_UTF8
#define CP_UTF8 65001U
#endif
#ifndef MB_ERR_INVALID_CHARS
#define MB_ERR_INVALID_CHARS 8UL
#endif

struct Router2Win32ShimState
{
    std::mutex mutex;
    DWORD last_error = 0;
    int create_new = 0;
    int replace_existing = 0;
    int delete_file = 0;
    int create_a = 0, create_w = 0;
    int replace_a = 0, replace_w = 0;
    int delete_a = 0, delete_w = 0;
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

inline std::string router2_ansi_mangle(const char *name)
{
    std::string out;
    for (unsigned char c : std::string(name ? name : ""))
        out.push_back(c >= 0x80 ? '?' : char(c));
    return out;
}

inline std::wstring router2_utf8_to_utf16(const char *src, int srclen)
{
    std::wstring out;
    const unsigned char *p = reinterpret_cast<const unsigned char *>(src);
    const unsigned char *end = p + srclen;
    while (p < end) {
        uint32_t cp;
        int need;
        if (*p < 0x80) {
            cp = *p++;
            need = 0;
        } else if ((*p & 0xe0) == 0xc0) {
            cp = *p++ & 0x1f;
            need = 1;
        } else if ((*p & 0xf0) == 0xe0) {
            cp = *p++ & 0x0f;
            need = 2;
        } else if ((*p & 0xf8) == 0xf0) {
            cp = *p++ & 0x07;
            need = 3;
        } else {
            return {};
        }
        for (int i = 0; i < need; ++i) {
            if (p >= end || (*p & 0xc0) != 0x80)
                return {};
            cp = (cp << 6) | (*p++ & 0x3f);
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(WCHAR(0xd800 + (cp >> 10)));
            out.push_back(WCHAR(0xdc00 + (cp & 0x3ff)));
        } else {
            out.push_back(WCHAR(cp));
        }
    }
    return out;
}

inline std::string router2_utf16_to_utf8(LPCWSTR src)
{
    std::string out;
    if (!src)
        return out;
    while (*src) {
        uint32_t cp = uint32_t(*src++);
        if (cp >= 0xd800 && cp <= 0xdbff && *src >= 0xdc00 && *src <= 0xdfff)
            cp = 0x10000 + ((cp - 0xd800) << 10) + (uint32_t(*src++) - 0xdc00);
        if (cp < 0x80)
            out.push_back(char(cp));
        else if (cp < 0x800) {
            out.push_back(char(0xc0 | (cp >> 6)));
            out.push_back(char(0x80 | (cp & 0x3f)));
        } else if (cp < 0x10000) {
            out.push_back(char(0xe0 | (cp >> 12)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(char(0x80 | (cp & 0x3f)));
        } else {
            out.push_back(char(0xf0 | (cp >> 18)));
            out.push_back(char(0x80 | ((cp >> 12) & 0x3f)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(char(0x80 | (cp & 0x3f)));
        }
    }
    return out;
}

inline int MultiByteToWideChar(UINT cp, DWORD flags, const char *src, int srclen, WCHAR *dst, int dstlen)
{
    auto &shim = router2_win32_shim();
    if (cp != CP_UTF8 || !src) {
        shim.last_error = ERROR_NO_UNICODE_TRANSLATION;
        return 0;
    }
    bool include_null = srclen < 0;
    if (include_null)
        srclen = int(std::strlen(src));
    std::wstring wide = router2_utf8_to_utf16(src, srclen);
    if (wide.empty() && srclen != 0) {
        shim.last_error = ERROR_NO_UNICODE_TRANSLATION;
        return 0;
    }
    (void)flags;
    int needed = int(wide.size()) + (include_null ? 1 : 0);
    if (dstlen == 0)
        return needed;
    if (dstlen < needed) {
        shim.last_error = ERROR_FILE_NOT_FOUND;
        return 0;
    }
    for (int i = 0; i < int(wide.size()); ++i)
        dst[i] = wide[size_t(i)];
    if (include_null)
        dst[wide.size()] = 0;
    return needed;
}

inline HANDLE create_file_utf8(const std::string &name, DWORD disposition, bool wide)
{
    auto &shim = router2_win32_shim();
    if (disposition != CREATE_NEW) {
        shim.last_error = ERROR_FILE_NOT_FOUND;
        return INVALID_HANDLE_VALUE;
    }
    int fd = ::open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0644);
    if (fd < 0) {
        shim.last_error = (errno == EEXIST) ? ERROR_ALREADY_EXISTS : ERROR_FILE_EXISTS;
        return INVALID_HANDLE_VALUE;
    }
    std::lock_guard<std::mutex> lock(shim.mutex);
    ++shim.create_new;
    if (wide)
        ++shim.create_w;
    else
        ++shim.create_a;
    shim.created.push_back(name);
    return reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(fd));
}

inline HANDLE CreateFileA(const char *name, DWORD access, DWORD, void *, DWORD disposition, DWORD, HANDLE)
{
    (void)access;
    return create_file_utf8(router2_ansi_mangle(name), disposition, false);
}

inline HANDLE CreateFileW(LPCWSTR name, DWORD access, DWORD, void *, DWORD disposition, DWORD, HANDLE)
{
    (void)access;
    return create_file_utf8(router2_utf16_to_utf8(name), disposition, true);
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

inline BOOL move_file_utf8(const std::string &from, const std::string &to, DWORD flags, bool wide)
{
    auto &shim = router2_win32_shim();
    if ((flags & MOVEFILE_REPLACE_EXISTING) == 0) {
        struct stat info;
        if (::stat(to.c_str(), &info) == 0) {
            shim.last_error = ERROR_ALREADY_EXISTS;
            return 0;
        }
    }
    if (::rename(from.c_str(), to.c_str()) != 0) {
        shim.last_error = ERROR_FILE_NOT_FOUND;
        return 0;
    }
    std::lock_guard<std::mutex> lock(shim.mutex);
    if (flags & MOVEFILE_REPLACE_EXISTING)
        ++shim.replace_existing;
    if (wide)
        ++shim.replace_w;
    else
        ++shim.replace_a;
    shim.replaced.push_back({from, to});
    return 1;
}

inline BOOL MoveFileExA(const char *from, const char *to, DWORD flags)
{
    return move_file_utf8(router2_ansi_mangle(from), router2_ansi_mangle(to), flags, false);
}

inline BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags)
{
    return move_file_utf8(router2_utf16_to_utf8(from), router2_utf16_to_utf8(to), flags, true);
}

inline BOOL delete_file_utf8(const std::string &name, bool wide)
{
    auto &shim = router2_win32_shim();
    std::lock_guard<std::mutex> lock(shim.mutex);
    ++shim.delete_file;
    if (wide)
        ++shim.delete_w;
    else
        ++shim.delete_a;
    shim.deleted.push_back(name);
    return ::unlink(name.c_str()) == 0;
}

inline BOOL DeleteFileA(const char *name) { return delete_file_utf8(router2_ansi_mangle(name), false); }

inline BOOL DeleteFileW(LPCWSTR name) { return delete_file_utf8(router2_utf16_to_utf8(name), true); }

#endif
