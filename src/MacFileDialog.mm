#ifdef __APPLE__

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "MacFileDialog.h"

#include <string.h>

bool mac_open_file_dialog(const char* message, const char* extension,
                          const char* initialPath, char* outPath, size_t outPathSize)
{
    if (!outPath || outPathSize == 0 || ![NSThread isMainThread]) {
        return false;
    }
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        if (message && *message) {
            panel.message = [NSString stringWithUTF8String:message];
        }
        if (extension && *extension) {
            if (@available(macOS 11.0, *)) {
                UTType* type = [UTType typeWithFilenameExtension:[NSString stringWithUTF8String:extension]];
                if (type) {
                    panel.allowedContentTypes = @[ type ];
                }
            }
        }
        if (initialPath && *initialPath) {
            NSString* current = [NSString stringWithUTF8String:initialPath];
            NSString* dir = [current stringByDeletingLastPathComponent];
            if (dir.length > 0) {
                panel.directoryURL = [NSURL fileURLWithPath:dir isDirectory:YES];
            }
        }
        if ([panel runModal] != NSModalResponseOK || panel.URL == nil) {
            return false;
        }
        const char* picked = panel.URL.path.fileSystemRepresentation;
        if (!picked || strlen(picked) + 1 > outPathSize) {
            return false;
        }
        strlcpy(outPath, picked, outPathSize);
        return true;
    }
}

#endif // __APPLE__
