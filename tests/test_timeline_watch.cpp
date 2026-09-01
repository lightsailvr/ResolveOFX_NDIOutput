// Tests for the Windows timeline-watch support seam (src/WinTimelineWatch.h):
// PEP 514 tag parsing / candidate picking, CreateProcessW argument quoting,
// and the bundle-resource path derivation are pure string work exercised on
// every platform; the registry discovery and the pipe-spawn plumbing are
// exercised on Windows only, where they exist.
// Build & run: ctest (Windows); compiles clean on macOS but is not in the
// Makefile's test list (Windows-flavored, like test_win_file_dialog).

#include "WinTimelineWatch.h"

#include <cstdio>
#include <string>
#include <vector>

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

static void testParsePythonTag()
{
    using ndi_timelinewatch_win::parsePythonTag;
    using ndi_timelinewatch_win::PyVersion;

    PyVersion v;
    expectTrue(parsePythonTag(L"3.11", &v) && v.major == 3 && v.minor == 11,
               "plain X.Y tag parses");
    expectTrue(parsePythonTag(L"3.9", &v) && v.major == 3 && v.minor == 9,
               "single-digit minor parses");

    // fusionscript.dll is x64; a 32-bit interpreter can never load it.
    expectTrue(!parsePythonTag(L"3.11-32", &v), "32-bit tag rejected");
    // Resolve on Windows-on-ARM is out of scope (spec) and CPython arm64
    // could not load the x64 DLL anyway.
    expectTrue(!parsePythonTag(L"3.13-arm64", &v), "arm64 tag rejected");

    expectTrue(!parsePythonTag(L"2.7", &v), "python 2 rejected");
    expectTrue(!parsePythonTag(L"3", &v), "major-only tag rejected (PythonCore tags are X.Y)");
    expectTrue(!parsePythonTag(L"3.11.4", &v), "X.Y.Z tag rejected (not a PythonCore tag)");
    expectTrue(!parsePythonTag(L"", &v), "empty tag rejected");
    expectTrue(!parsePythonTag(L"pypy3.10", &v), "non-numeric tag rejected");
}

static void testPickBestPython()
{
    using ndi_timelinewatch_win::PythonCandidate;
    using ndi_timelinewatch_win::pickBestPython;

    // Numeric comparison, not lexical: "3.9" sorts after "3.11" as a string.
    {
        std::vector<PythonCandidate> c = {{L"3.9", L"C:\\Py39\\python.exe"},
                                          {L"3.11", L"C:\\Py311\\python.exe"}};
        expectTrue(pickBestPython(c) == L"C:\\Py311\\python.exe",
                   "3.11 beats 3.9 numerically");
    }
    // Unusable tags never win, whatever their version.
    {
        std::vector<PythonCandidate> c = {{L"3.12-32", L"C:\\Py32bit\\python.exe"},
                                          {L"3.10", L"C:\\Py310\\python.exe"}};
        expectTrue(pickBestPython(c) == L"C:\\Py310\\python.exe",
                   "32-bit candidate loses to an older 64-bit one");
    }
    // Callers feed HKCU before HKLM (PEP 514 precedence): on a version tie
    // the first-seen candidate wins.
    {
        std::vector<PythonCandidate> c = {{L"3.11", L"C:\\User\\python.exe"},
                                          {L"3.11", L"C:\\Machine\\python.exe"}};
        expectTrue(pickBestPython(c) == L"C:\\User\\python.exe",
                   "version tie keeps the first-seen (HKCU) candidate");
    }
    {
        std::vector<PythonCandidate> c = {{L"2.7", L"C:\\Py27\\python.exe"},
                                          {L"broken", L"C:\\X\\python.exe"}};
        expectTrue(pickBestPython(c).empty(), "no usable candidate yields empty");
    }
    expectTrue(pickBestPython({}).empty(), "no candidates yields empty");
}

static void testQuoteArg()
{
    using ndi_timelinewatch_win::quoteArg;

    // No metacharacters: unchanged (keeps logged command lines readable).
    expectTrue(quoteArg(L"C:\\Python311\\python.exe") == L"C:\\Python311\\python.exe",
               "plain path passes through unquoted");

    // The common real case: spaces in Program Files-style paths.
    expectTrue(quoteArg(L"C:\\Program Files\\python.exe") ==
                   L"\"C:\\Program Files\\python.exe\"",
               "path with spaces gets wrapped");

    // A trailing backslash before the closing quote must double, or the
    // closing quote would be eaten by CommandLineToArgvW.
    expectTrue(quoteArg(L"C:\\path with space\\") == L"\"C:\\path with space\\\\\"",
               "trailing backslash doubles before the closing quote");

    // Embedded quotes escape; the backslash run before them doubles.
    expectTrue(quoteArg(L"a\"b") == L"\"a\\\"b\"", "embedded quote escapes");
    expectTrue(quoteArg(L"a\\\"b") == L"\"a\\\\\\\"b\"",
               "backslash before an embedded quote doubles, then the quote escapes");

    // Empty argument must still produce a token.
    expectTrue(quoteArg(L"") == L"\"\"", "empty argument becomes empty quotes");
}

static void testBundleResourcePath()
{
    using ndi_timelinewatch_win::bundleResourcePath;

    // The real bundle layout (spec decision 16): the helper script rides in
    // Contents/Resources, one level up and over from Contents/Win64.
    expectTrue(bundleResourcePath(
                   L"C:\\Program Files\\Common Files\\OFX\\Plugins\\"
                   L"NDIOutput.ofx.bundle\\Contents\\Win64\\NDIOutput.ofx",
                   L"ndi_timeline_watch.py") ==
                   L"C:\\Program Files\\Common Files\\OFX\\Plugins\\"
                   L"NDIOutput.ofx.bundle\\Contents\\Resources\\ndi_timeline_watch.py",
               "bundle layout resolves to Contents\\Resources");

    // Forward slashes (CMake-staged dev trees) keep their style.
    expectTrue(bundleResourcePath(L"D:/repos/x/stage/Contents/Win64/NDIOutput.ofx",
                                  L"ndi_timeline_watch.py") ==
                   L"D:/repos/x/stage/Contents/Resources/ndi_timeline_watch.py",
               "forward-slash separators work");

    // Not enough directory levels to go up one: empty (caller logs and
    // soft-fails, same as the missing-script case).
    expectTrue(bundleResourcePath(L"NDIOutput.ofx", L"x.py").empty(),
               "bare filename yields empty");
    expectTrue(bundleResourcePath(L"Win64\\NDIOutput.ofx", L"x.py").empty(),
               "single directory level yields empty");
    expectTrue(bundleResourcePath(L"", L"x.py").empty(), "empty module path yields empty");
}

#ifdef _WIN32
static void testDiscoverPython()
{
    using ndi_timelinewatch_win::discoverPython;

    // Environment-dependent: a CI image may have no registry-registered or
    // PATH-visible interpreter. Assert only internal consistency — a
    // non-empty answer must name an existing file and say where it came from.
    ndi_timelinewatch_win::PythonDiscovery d = discoverPython();
    if (d.exePath.empty()) {
        expectTrue(true, "no python found (acceptable on a bare machine)");
    } else {
        expectTrue(GetFileAttributesW(d.exePath.c_str()) != INVALID_FILE_ATTRIBUTES,
                   "discovered python exists on disk");
        expectTrue(!d.source.empty(), "discovery names its source for the log");
        std::printf("     (found: %ls via %s)\n", d.exePath.c_str(), d.source.c_str());
    }
}

static void testSpawnHelperProcess()
{
    using ndi_timelinewatch_win::HelperProcess;
    using ndi_timelinewatch_win::spawnHelperProcess;

    // Spawn a real child with its stdout on our pipe and read it back
    // through the same CRT wrapping the watcher uses. This is the plumbing
    // where the classic bugs live (inherited read end never EOFs; console
    // window flashes; quoting breaks paths with spaces).
    HelperProcess proc;
    std::string error;
    expectTrue(spawnHelperProcess(L"cmd.exe /c \"echo !status& echo C:\\clip.braw\"",
                                  &proc, &error),
               ("spawn succeeds" + (error.empty() ? "" : " (" + error + ")")).c_str());
    if (proc.process == nullptr) {
        return;
    }

    FILE* stream = ndi_timelinewatch_win::fileStreamForReadHandle(proc.readPipe);
    expectTrue(stream != nullptr, "read handle wraps into a FILE*");
    if (stream != nullptr) {
        char line[256];
        bool sawStatus = false, sawPath = false, sawEof = false;
        while (fgets(line, sizeof(line), stream) != nullptr) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
                s.pop_back();
            }
            if (s == "!status") sawStatus = true;
            if (s == "C:\\clip.braw") sawPath = true;
        }
        sawEof = true;  // fgets returned NULL: child exit closed the pipe
        expectTrue(sawStatus, "first line arrives intact");
        expectTrue(sawPath, "second line arrives intact");
        expectTrue(sawEof, "child exit EOFs the pipe (read end not inherited)");
        fclose(stream);  // also closes the underlying handle
    } else {
        CloseHandle(proc.readPipe);
    }
    WaitForSingleObject(proc.process, 5000);
    CloseHandle(proc.process);
}
#endif // _WIN32

int main()
{
    testParsePythonTag();
    testPickBestPython();
    testQuoteArg();
    testBundleResourcePath();
#ifdef _WIN32
    testDiscoverPython();
    testSpawnHelperProcess();
#endif
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all timeline-watch tests passed\n");
    return 0;
}
