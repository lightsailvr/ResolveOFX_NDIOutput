// Wide-character path seam. OFX hands the plugin UTF-8 strings; on Windows
// the narrow CRT calls (fopen/stat) route through the ANSI code page, so any
// non-ASCII STMap or BRAW path would fail to open. Every file-touching seam
// goes through these shims instead: on Windows they convert UTF-8 to UTF-16
// and call the wide CRT; elsewhere they are thin passthroughs.
//
// The UTF-8 decoder is hand-rolled (and strict: overlongs, lone surrogates,
// and truncated sequences are rejected) so this header needs no <windows.h>
// and the conversion is unit-testable on every platform. It always emits
// UTF-16 code units — one per wchar_t — which is exactly what the Windows
// wide CRT expects; the tests assert the same code units on macOS, where
// wchar_t is wider but the values are identical.

#ifndef PLATFORM_PATHS_H
#define PLATFORM_PATHS_H

#include <cstdio>
#include <string>
#include <sys/stat.h>

#ifdef _WIN32
#include <wchar.h>
#endif

namespace ndi_path {

namespace detail {

// Strict UTF-8 -> UTF-16 (in wchar_t units). Returns false on any invalid
// sequence; callers fall back to the narrow call so a mis-encoded path can
// still work when it happens to be ANSI-representable.
inline bool utf8ToWide(const char* utf8, std::wstring* out)
{
    out->clear();
    const unsigned char* s = reinterpret_cast<const unsigned char*>(utf8);
    while (*s) {
        unsigned int cp = 0;
        int extra = 0;
        unsigned int minCp = 0;
        if (s[0] < 0x80) {
            cp = s[0];
        } else if ((s[0] & 0xE0) == 0xC0) {
            cp = s[0] & 0x1F; extra = 1; minCp = 0x80;
        } else if ((s[0] & 0xF0) == 0xE0) {
            cp = s[0] & 0x0F; extra = 2; minCp = 0x800;
        } else if ((s[0] & 0xF8) == 0xF0) {
            cp = s[0] & 0x07; extra = 3; minCp = 0x10000;
        } else {
            return false; // continuation byte or invalid lead
        }
        for (int i = 1; i <= extra; ++i) {
            if ((s[i] & 0xC0) != 0x80) {
                return false; // truncated or malformed sequence
            }
            cp = (cp << 6) | (s[i] & 0x3F);
        }
        if (extra && cp < minCp) {
            return false; // overlong encoding
        }
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false; // out of range, or a raw surrogate
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out->push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            out->push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out->push_back(static_cast<wchar_t>(cp));
        }
        s += 1 + extra;
    }
    return true;
}

// Strict UTF-16 (in wchar_t units) -> UTF-8, the reverse of utf8ToWide: the
// Windows browse dialog gets its picked path from COM as UTF-16 and the OFX
// string param wants UTF-8 (ticket #24). Returns false on a lone surrogate —
// mangled WTF-8 in a param value would fail later in fopenUtf8 anyway.
inline bool wideToUtf8(const wchar_t* wide, std::string* out)
{
    out->clear();
    for (const wchar_t* s = wide; *s; ++s) {
        unsigned int cp = static_cast<unsigned int>(*s) & 0xFFFF;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            const unsigned int low = static_cast<unsigned int>(s[1]) & 0xFFFF;
            if (low < 0xDC00 || low > 0xDFFF) {
                return false; // high surrogate without its pair
            }
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            ++s;
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return false; // low surrogate without a preceding high
        }
        if (cp < 0x80) {
            out->push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return true;
}

} // namespace detail

// fopen that survives non-ASCII paths on Windows.
inline std::FILE* fopenUtf8(const char* path, const char* mode)
{
#ifdef _WIN32
    std::wstring wpath, wmode;
    if (detail::utf8ToWide(path, &wpath) && detail::utf8ToWide(mode, &wmode)) {
        return _wfopen(wpath.c_str(), wmode.c_str());
    }
#endif
    return std::fopen(path, mode);
}

// stat limited to what the plugin needs (mtime + size for cache keying).
// Returns false when the file is missing or unreadable.
inline bool statUtf8(const char* path, long long* mtime, long long* size)
{
#ifdef _WIN32
    std::wstring wpath;
    struct _stat64 wst;
    if (detail::utf8ToWide(path, &wpath)) {
        if (_wstat64(wpath.c_str(), &wst) != 0) {
            return false;
        }
        *mtime = static_cast<long long>(wst.st_mtime);
        *size = static_cast<long long>(wst.st_size);
        return true;
    }
#endif
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    *mtime = static_cast<long long>(st.st_mtime);
    *size = static_cast<long long>(st.st_size);
    return true;
}

// remove that survives non-ASCII paths on Windows. Returns 0 on success,
// matching std::remove.
inline int removeUtf8(const char* path)
{
#ifdef _WIN32
    std::wstring wpath;
    if (detail::utf8ToWide(path, &wpath)) {
        return _wremove(wpath.c_str());
    }
#endif
    return std::remove(path);
}

} // namespace ndi_path

#endif // PLATFORM_PATHS_H
