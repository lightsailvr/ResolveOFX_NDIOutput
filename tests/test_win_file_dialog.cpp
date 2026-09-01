// Tests for the pure logic behind the Windows browse dialog
// (src/WinFileDialog.h, ticket #24): the initial-folder derivation, the file
// type filter strings, and the truncation-safe copy-out that enforces the
// "failure leaves outPath untouched" contract. The COM half
// (win_open_file_dialog itself) needs a user and a desktop, so the Tier 1-2
// loop covers it; everything here runs headless — no COM, no window.
// Build & run: ctest (Windows only — the macOS Makefile doesn't build it,
// since the dialog these helpers serve exists only in the Windows plugin)

#include "WinFileDialog.h"

#include <cstdio>
#include <cstring>
#include <string>

static int failures = 0;

static void expectStr(const std::string& actual, const std::string& expected, const char* name)
{
    if (actual != expected) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n  expected: '%s'\n  actual:   '%s'\n",
                     name, expected.c_str(), actual.c_str());
    } else {
        std::printf("ok   %s\n", name);
    }
}

static void expectTrue(bool actual, const char* name)
{
    if (!actual) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", name);
    } else {
        std::printf("ok   %s\n", name);
    }
}

// The dialog opens in the directory of the current field value, like the
// NSOpenPanel's stringByDeletingLastPathComponent behavior. The trailing
// separator stays so a drive root ("D:\") parses as a folder, not a
// drive-relative name.
static void testParentDirectory()
{
    using ndi_windlg::parentDirectory;

    expectStr(parentDirectory("D:\\maps\\left.exr"), "D:\\maps\\", "backslash path");
    expectStr(parentDirectory("D:/maps/left.exr"), "D:/maps/", "forward-slash path");
    expectStr(parentDirectory("D:\\maps/mixed\\left.exr"), "D:\\maps/mixed\\", "mixed separators");
    expectStr(parentDirectory("D:\\left.exr"), "D:\\", "file at drive root keeps the root");
    expectStr(parentDirectory("\\\\server\\share\\map.exr"), "\\\\server\\share\\", "UNC path");
    expectStr(parentDirectory("left.exr"), "", "bare filename has no directory");
    expectStr(parentDirectory(""), "", "empty path has no directory");
    // Non-ASCII UTF-8 bytes pass through untouched (the conversion to wide
    // happens later, in the dialog itself).
    expectStr(parentDirectory("D:\\\xE3\x82\xAF\xE3\x83\xAA\xE3\x83\x83\xE3\x83\x97\\map.exr"),
              "D:\\\xE3\x82\xAF\xE3\x83\xAA\xE3\x83\x83\xE3\x83\x97\\",
              "non-ASCII directory survives");
}

// One extension in, the two strings COMDLG_FILTERSPEC wants out.
static void testFilterStrings()
{
    using ndi_windlg::filterLabel;
    using ndi_windlg::filterPattern;

    expectStr(filterPattern("exr"), "*.exr", "pattern for exr");
    expectStr(filterPattern("braw"), "*.braw", "pattern for braw");
    expectStr(filterLabel("exr"), "EXR files (*.exr)", "label uppercases the extension");
    expectStr(filterLabel("braw"), "BRAW files (*.braw)", "label for braw");
}

// The dialog's contract: outPath is written only on success; a path that
// doesn't fit is a failure, not a truncation (a truncated path silently
// pointing at the wrong file would be worse than no pick).
static void testCopyPathOut()
{
    using ndi_windlg::copyPathOut;

    char buf[16];
    std::memset(buf, 'x', sizeof(buf));

    expectTrue(copyPathOut("D:\\a.exr", buf, sizeof(buf)), "short path fits");
    expectTrue(std::strcmp(buf, "D:\\a.exr") == 0, "copied bytes match");

    expectTrue(copyPathOut("123456789012345", buf, sizeof(buf)), "exact fit (15 chars + NUL) fits");

    std::memset(buf, 'x', sizeof(buf));
    buf[15] = '\0';
    expectTrue(!copyPathOut("1234567890123456", buf, sizeof(buf)), "one char too long fails");
    expectTrue(std::strcmp(buf, "xxxxxxxxxxxxxxx") == 0, "failed copy leaves buffer untouched");

    expectTrue(!copyPathOut("", buf, 0), "zero-size buffer fails");
    expectTrue(!copyPathOut("a", nullptr, 8), "null buffer fails");
}

int main()
{
    testParentDirectory();
    testFilterStrings();
    testCopyPathOut();

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all win-file-dialog tests passed\n");
    return 0;
}
