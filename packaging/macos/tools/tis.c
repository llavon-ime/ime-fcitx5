#include <Carbon/Carbon.h>
#include <stdio.h>
#include <string.h>

static int register_app(const char *app_path) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        NULL, (const UInt8 *)app_path, strlen(app_path), false);
    if (url == NULL) {
        fprintf(stderr, "invalid path: %s\n", app_path);
        return 1;
    }
    OSStatus status = TISRegisterInputSource(url);
    CFRelease(url);
    if (status != noErr) {
        fprintf(stderr, "TISRegisterInputSource failed: %d\n", (int)status);
        return 1;
    }
    return 0;
}

static int enable_input_source(const char *bundle_id) {
    CFStringRef bundle = CFStringCreateWithCString(NULL, bundle_id, kCFStringEncodingUTF8);
    if (bundle == NULL) {
        fprintf(stderr, "invalid bundle id: %s\n", bundle_id);
        return 1;
    }

    // Match every input mode of the bundle, not just the entry whose input
    // source id happens to equal the bundle id.
    const void *keys[] = { kTISPropertyBundleID };
    const void *values[] = { bundle };
    CFDictionaryRef conditions = CFDictionaryCreate(
        NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(bundle);
    if (conditions == NULL) {
        fprintf(stderr, "failed to build input source filter\n");
        return 1;
    }

    CFArrayRef sources = TISCreateInputSourceList(conditions, true);
    CFRelease(conditions);
    if (sources == NULL) {
        return 0;
    }

    int enabled = 0;
    CFIndex count = CFArrayGetCount(sources);
    for (CFIndex i = 0; i < count; ++i) {
        TISInputSourceRef source = (TISInputSourceRef)CFArrayGetValueAtIndex(sources, i);
        if (source != NULL && TISEnableInputSource(source) == noErr) {
            ++enabled;
        }
    }
    CFRelease(sources);
    if (enabled == 0) {
        fprintf(stderr, "no input source matched %s\n", bundle_id);
        return 1;
    }
    return 0;
}

static int select_input_source(const char *source_id) {
    CFStringRef target = CFStringCreateWithCString(NULL, source_id, kCFStringEncodingUTF8);
    if (target == NULL) {
        fprintf(stderr, "invalid input source id: %s\n", source_id);
        return 1;
    }

    const void *keys[] = { kTISPropertyInputSourceID };
    const void *values[] = { target };
    CFDictionaryRef conditions = CFDictionaryCreate(
        NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(target);
    if (conditions == NULL) {
        fprintf(stderr, "failed to build input source filter\n");
        return 1;
    }

    CFArrayRef sources = TISCreateInputSourceList(conditions, false);
    CFRelease(conditions);
    if (sources == NULL) {
        return 1;
    }

    int selected = 0;
    CFIndex count = CFArrayGetCount(sources);
    for (CFIndex i = 0; i < count; ++i) {
        TISInputSourceRef source = (TISInputSourceRef)CFArrayGetValueAtIndex(sources, i);
        if (source != NULL && TISSelectInputSource(source) == noErr) {
            ++selected;
        }
    }
    CFRelease(sources);
    if (selected == 0) {
        fprintf(stderr, "no input source matched %s\n", source_id);
        return 1;
    }
    return 0;
}

static int list_input_sources(const char *bundle_id) {
    CFStringRef bundle = CFStringCreateWithCString(NULL, bundle_id, kCFStringEncodingUTF8);
    if (bundle == NULL) {
        fprintf(stderr, "invalid bundle id: %s\n", bundle_id);
        return 1;
    }

    const void *keys[] = { kTISPropertyBundleID };
    const void *values[] = { bundle };
    CFDictionaryRef conditions = CFDictionaryCreate(
        NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(bundle);
    if (conditions == NULL) {
        fprintf(stderr, "failed to build input source filter\n");
        return 1;
    }

    CFArrayRef sources = TISCreateInputSourceList(conditions, true);
    CFRelease(conditions);
    if (sources == NULL) {
        return 1;
    }

    CFIndex count = CFArrayGetCount(sources);
    for (CFIndex i = 0; i < count; ++i) {
        TISInputSourceRef source = (TISInputSourceRef)CFArrayGetValueAtIndex(sources, i);
        if (source == NULL) continue;
        CFStringRef source_id = (CFStringRef)TISGetInputSourceProperty(source, kTISPropertyInputSourceID);
        if (source_id == NULL) continue;
        char buffer[256] = {0};
        if (CFStringGetCString(source_id, buffer, sizeof(buffer), kCFStringEncodingUTF8)) {
            printf("%s\n", buffer);
        }
    }
    CFRelease(sources);
    return count > 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "register") == 0) {
        return register_app(argv[2]);
    }
    if (argc == 3 && strcmp(argv[1], "enable") == 0) {
        return enable_input_source(argv[2]);
    }
    if (argc == 3 && strcmp(argv[1], "select") == 0) {
        return select_input_source(argv[2]);
    }
    if (argc == 3 && strcmp(argv[1], "list") == 0) {
        return list_input_sources(argv[2]);
    }
    fprintf(stderr, "usage: %s register <app-path> | enable <bundle-id> | select <input-source-id> | list <bundle-id>\n",
            argv[0]);
    return 2;
}
