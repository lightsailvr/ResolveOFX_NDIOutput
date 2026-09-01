// The COM half of the Windows browse dialog (ticket #24) — see
// WinFileDialog.h for the contract and the testable pure logic. Uses the
// Vista common item dialog (IFileOpenDialog); the wide/UTF-8 conversions go
// through the same strict converters as every other path seam
// (src/PlatformPaths.h), so a picked path round-trips into the OFX string
// param byte-identical to what fopenUtf8 will later decode.

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <shobjidl.h>

#include "PlatformPaths.h"
#include "WinFileDialog.h"

bool win_open_file_dialog(const char* message, const char* extension,
                          const char* initialPath, char* outPath, size_t outPathSize)
{
    using ndi_path::detail::utf8ToWide;
    using ndi_path::detail::wideToUtf8;

    if (!outPath || outPathSize == 0) {
        return false;
    }

    // Resolve's UI thread normally arrives here already STA-initialized, so
    // expect S_FALSE. RPC_E_CHANGED_MODE (thread is MTA) is not fatal to
    // CoCreateInstance — proceed without the matching CoUninitialize and let
    // the dialog creation decide.
    const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool needUninit = SUCCEEDED(hrInit);

    bool picked = false;
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dialog)))) {
        std::wstring wide;
        if (message && *message && utf8ToWide(message, &wide)) {
            dialog->SetTitle(wide.c_str());
        }

        // One row for the extension, one escape hatch — mirroring the
        // NSOpenPanel's allowedContentTypes plus the shell's usual "all
        // files" convention. The wstrings must outlive SetFileTypes' use
        // during Show(), hence the block-scope locals.
        std::wstring wLabel, wPattern, wExt;
        if (extension && *extension &&
            utf8ToWide(ndi_windlg::filterLabel(extension).c_str(), &wLabel) &&
            utf8ToWide(ndi_windlg::filterPattern(extension).c_str(), &wPattern) &&
            utf8ToWide(extension, &wExt)) {
            const COMDLG_FILTERSPEC filters[] = {
                {wLabel.c_str(), wPattern.c_str()},
                {L"All files (*.*)", L"*.*"},
            };
            dialog->SetFileTypes(ARRAYSIZE(filters), filters);
            dialog->SetFileTypeIndex(1); // 1-based: the extension row
            dialog->SetDefaultExtension(wExt.c_str());
        }

        // Open in the current field value's directory, like the mac panel's
        // directoryURL. SetFolder (not SetDefaultFolder) so the field wins
        // over the shell's remembered location; if the directory doesn't
        // resolve, skip it and let the shell pick.
        if (initialPath && *initialPath) {
            const std::string dir = ndi_windlg::parentDirectory(initialPath);
            std::wstring wDir;
            if (!dir.empty() && utf8ToWide(dir.c_str(), &wDir)) {
                IShellItem* folder = nullptr;
                if (SUCCEEDED(SHCreateItemFromParsingName(wDir.c_str(), nullptr,
                                                          IID_PPV_ARGS(&folder)))) {
                    dialog->SetFolder(folder);
                    folder->Release();
                }
            }
        }

        // GetActiveWindow: the browse button click makes Resolve's main
        // window active on this thread, so the dialog is modal to it; NULL
        // (no active window) still shows an unowned modal dialog.
        if (SUCCEEDED(dialog->Show(GetActiveWindow()))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR widePath = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &widePath))) {
                    std::string utf8;
                    picked = wideToUtf8(widePath, &utf8) &&
                             ndi_windlg::copyPathOut(utf8, outPath, outPathSize);
                    CoTaskMemFree(widePath);
                }
                item->Release();
            }
        }
        dialog->Release();
    }

    if (needUninit) {
        CoUninitialize();
    }
    return picked;
}

#endif // _WIN32
