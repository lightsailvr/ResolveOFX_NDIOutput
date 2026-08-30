#ifndef MAC_FILE_DIALOG_H
#define MAC_FILE_DIALOG_H

#ifdef __APPLE__

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Native modal NSOpenPanel for picking one file with the given extension.
// Exists because Resolve renders kOfxParamStringIsFilePath string params as
// plain text fields with no browse control (verified 2026-08-30) — the plugin
// pops its own panel from a push-button param instead.
//
// Main thread only: returns false immediately elsewhere (a modal nested event
// loop off the main thread is a deadlock risk, and user edits arrive on the
// main thread anyway). initialPath may be NULL/empty; when set, the panel
// opens in its directory. Returns true and fills outPath (UTF-8) only when
// the user picks a file; cancel and every failure return false with outPath
// untouched.
bool mac_open_file_dialog(const char* message, const char* extension,
                          const char* initialPath, char* outPath, size_t outPathSize);

#ifdef __cplusplus
}
#endif

#endif // __APPLE__

#endif // MAC_FILE_DIALOG_H
