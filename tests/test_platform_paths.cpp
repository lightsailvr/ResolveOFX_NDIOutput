// Tests for the wide-character path seam (src/PlatformPaths.h): UTF-8 →
// UTF-16 conversion (exercised on every platform) and the fopen/stat/remove
// shims that keep non-ASCII file paths working on Windows, where the narrow
// CRT calls go through the ANSI code page and would fail.
// Build & run: make test (macOS) / ctest (Windows)

#include "PlatformPaths.h"

#include <cstdio>
#include <cstring>
#include <string>

static int failures = 0;

static void expectTrue(bool actual, const char* name)
{
    if (!actual) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", name);
    } else {
        std::printf("ok   %s\n", name);
    }
}

static void expectInt(long long actual, long long expected, const char* name)
{
    if (actual != expected) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n  expected: %lld\n  actual:   %lld\n", name, expected, actual);
    } else {
        std::printf("ok   %s\n", name);
    }
}

// The converter emits UTF-16 code units on every platform (only Windows
// consumes them), so its logic is testable without Windows in the loop.
static void testUtf8ToWide()
{
    using ndi_path::detail::utf8ToWide;
    std::wstring w;

    expectTrue(utf8ToWide("abc", &w) && w == L"abc", "ASCII converts");
    expectTrue(utf8ToWide("", &w) && w.empty(), "empty string converts");

    // 2-byte sequence: U+00E9 LATIN SMALL LETTER E WITH ACUTE
    expectTrue(utf8ToWide("\xC3\xA9", &w) && w == L"\u00E9", "2-byte sequence (e-acute)");

    // 3-byte sequence: U+65E5 (CJK 'sun/day')
    expectTrue(utf8ToWide("\xE6\x97\xA5", &w) && w == L"\u65E5", "3-byte sequence (CJK)");

    // 4-byte sequence: U+1D11E MUSICAL SYMBOL G CLEF -> surrogate pair D834 DD1E
    expectTrue(utf8ToWide("\xF0\x9D\x84\x9E", &w) && w.size() == 2 &&
                   w[0] == 0xD834 && w[1] == 0xDD1E,
               "4-byte sequence becomes a surrogate pair");

    // Mixed path-like string survives intact.
    expectTrue(utf8ToWide("C:\\maps\\st\xC3\xA9r\xC3\xA9o\\map.exr", &w) &&
                   w == L"C:\\maps\\st\u00E9r\u00E9o\\map.exr",
               "mixed path converts");

    // A UTF-16 target keeps one code unit per BMP character.
    expectTrue(utf8ToWide("\xC3\xA9", &w) && w.size() == 1, "BMP char is one code unit");

    // Invalid sequences are rejected, not silently mangled.
    expectTrue(!utf8ToWide("\xFF", &w), "lone invalid byte rejected");
    expectTrue(!utf8ToWide("\xC3", &w), "truncated 2-byte sequence rejected");
    expectTrue(!utf8ToWide("\xE6\x97", &w), "truncated 3-byte sequence rejected");
    expectTrue(!utf8ToWide("\xC0\xAF", &w), "overlong encoding rejected");
    expectTrue(!utf8ToWide("\xED\xA0\x80", &w), "lone surrogate rejected");
}

// The reverse direction feeds the Windows browse dialog (ticket #24): the
// picked path comes back from COM as UTF-16 and must reach the OFX string
// param as UTF-8. Same strictness contract as utf8ToWide.
static void testWideToUtf8()
{
    using ndi_path::detail::utf8ToWide;
    using ndi_path::detail::wideToUtf8;
    std::string u;

    expectTrue(wideToUtf8(L"abc", &u) && u == "abc", "ASCII converts back");
    expectTrue(wideToUtf8(L"", &u) && u.empty(), "empty wide string converts");

    // U+00E9 -> 2-byte sequence
    expectTrue(wideToUtf8(L"\u00E9", &u) && u == "\xC3\xA9", "BMP 2-byte (e-acute)");

    // U+65E5 -> 3-byte sequence
    expectTrue(wideToUtf8(L"\u65E5", &u) && u == "\xE6\x97\xA5", "BMP 3-byte (CJK)");

    // Surrogate pair D834 DD1E -> U+1D11E -> 4-byte sequence
    {
        const wchar_t pair[] = {0xD834, 0xDD1E, 0};
        expectTrue(wideToUtf8(pair, &u) && u == "\xF0\x9D\x84\x9E",
                   "surrogate pair becomes a 4-byte sequence");
    }

    // Mixed path-like string survives intact.
    expectTrue(wideToUtf8(L"C:\\maps\\st\u00E9r\u00E9o\\map.exr", &u) &&
                   u == "C:\\maps\\st\xC3\xA9r\xC3\xA9o\\map.exr",
               "mixed path converts back");

    // Lone surrogates are rejected, not silently mangled (WTF-8 is not a
    // valid OFX param value).
    {
        const wchar_t loneHigh[] = {0xD834, 0};
        const wchar_t loneLow[] = {0xDD1E, L'x', 0};
        const wchar_t highBeforeBmp[] = {0xD834, 0x0041, 0};
        expectTrue(!wideToUtf8(loneHigh, &u), "lone high surrogate rejected");
        expectTrue(!wideToUtf8(loneLow, &u), "lone low surrogate rejected");
        expectTrue(!wideToUtf8(highBeforeBmp, &u), "high surrogate before BMP char rejected");
    }

    // Round trip through both converters is the identity.
    {
        const char* path = "D:\\\xE3\x82\xAF\xE3\x83\xAA\xE3\x83\x83\xE3\x83\x97\\st\xC3\xA9r\xC3\xA9o_\xF0\x9D\x84\x9E.exr";
        std::wstring w;
        expectTrue(utf8ToWide(path, &w) && wideToUtf8(w.c_str(), &u) && u == path,
                   "utf8 -> wide -> utf8 round trip is identity");
    }
}

// Round-trip a file whose name needs every byte width: write via the shim,
// stat it, read it back, remove it. On macOS/Linux the shims are thin
// passthroughs; on Windows this is the behavior the port exists for.
static void testFileRoundTrip()
{
    const char* path = "ndi_paths_t\xC3\xA9st_\xE6\x97\xA5.tmp";
    const char payload[] = "NDI wide-path payload \xF0\x9D\x84\x9E";
    const long long payloadLen = static_cast<long long>(sizeof(payload) - 1);

    std::FILE* f = ndi_path::fopenUtf8(path, "wb");
    expectTrue(f != nullptr, "open non-ASCII path for write");
    if (f) {
        std::fwrite(payload, 1, static_cast<size_t>(payloadLen), f);
        std::fclose(f);
    }

    long long mtime = 0, size = 0;
    expectTrue(ndi_path::statUtf8(path, &mtime, &size), "stat non-ASCII path");
    expectInt(size, payloadLen, "stat size matches bytes written");
    expectTrue(mtime > 0, "stat mtime is populated");

    f = ndi_path::fopenUtf8(path, "rb");
    expectTrue(f != nullptr, "open non-ASCII path for read");
    if (f) {
        char buf[sizeof(payload)] = {0};
        size_t got = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        expectInt(static_cast<long long>(got), payloadLen, "read back full payload");
        expectTrue(std::memcmp(buf, payload, static_cast<size_t>(payloadLen)) == 0,
                   "payload bytes intact");
    }

    expectInt(ndi_path::removeUtf8(path), 0, "remove non-ASCII path");
    expectTrue(!ndi_path::statUtf8(path, &mtime, &size), "stat fails after remove");
}

static void testMissingFile()
{
    long long mtime = 0, size = 0;
    expectTrue(!ndi_path::statUtf8("ndi_paths_missing_\xC3\xA9.tmp", &mtime, &size),
               "stat on a missing file fails");
    expectTrue(ndi_path::fopenUtf8("ndi_paths_missing_\xC3\xA9.tmp", "rb") == nullptr,
               "open on a missing file returns null");
}

int main()
{
    testUtf8ToWide();
    testWideToUtf8();
    testFileRoundTrip();
    testMissingFile();

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all platform-path tests passed\n");
    return 0;
}
