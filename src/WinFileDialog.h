// Windows counterpart of MacFileDialog.h (ticket #24): a native modal
// IFileOpenDialog for picking one file with the given extension, popped from
// the plugin's browse push-button params because Resolve renders
// kOfxParamStringIsFilePath string params as plain text fields with no
// browse control (verified 2026-08-30 on macOS, re-verified on Windows in
// ticket #21's probe notes).
//
// The pure logic (initial-folder derivation, filter strings, the
// truncation-safe copy-out) lives here as inline helpers so
// tests/test_win_file_dialog.cpp can run it headless on any platform; the
// COM half stays in WinFileDialog.cpp and is only covered by the Tier 1-2
// loop, which needs a human and a desktop.

#ifndef WIN_FILE_DIALOG_H
#define WIN_FILE_DIALOG_H

#include <cstring>
#include <string>

namespace ndi_windlg {

// Directory portion of a file path, trailing separator kept — "D:\" must
// parse as a folder, not the drive-relative name "D:". Empty when the path
// has no separator (the dialog then opens wherever the shell remembers).
// Byte-level on purpose: UTF-8 goes to UTF-16 later, inside the dialog.
inline std::string parentDirectory(const std::string& path)
{
    const size_t pos = path.find_last_of("\\/");
    return (pos == std::string::npos) ? std::string() : path.substr(0, pos + 1);
}

// The two strings a COMDLG_FILTERSPEC row wants, from one extension.
inline std::string filterPattern(const std::string& extension)
{
    return "*." + extension;
}

inline std::string filterLabel(const std::string& extension)
{
    std::string upper = extension;
    for (char& c : upper) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return upper + " files (" + filterPattern(extension) + ")";
}

// Copy the picked path into the caller's buffer, or touch nothing: a path
// that doesn't fit is a failure, never a truncation — a silently shortened
// path pointing at the wrong file would be worse than no pick at all.
inline bool copyPathOut(const std::string& utf8, char* outPath, size_t outPathSize)
{
    if (!outPath || outPathSize == 0 || utf8.size() + 1 > outPathSize) {
        return false;
    }
    std::memcpy(outPath, utf8.c_str(), utf8.size() + 1);
    return true;
}

} // namespace ndi_windlg

#ifdef _WIN32

// Same contract as mac_open_file_dialog: initialPath may be NULL/empty; when
// set, the dialog opens in its directory. Returns true and fills outPath
// (UTF-8) only when the user picks a file; cancel and every failure return
// false with outPath untouched. Runs a modal dialog on the calling thread —
// Resolve delivers browse-button edits on its UI thread, which is where a
// modal dialog belongs.
bool win_open_file_dialog(const char* message, const char* extension,
                          const char* initialPath, char* outPath, size_t outPathSize);

#endif // _WIN32

#endif // WIN_FILE_DIALOG_H
