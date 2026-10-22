#include <CoreFoundation/CoreFoundation.h>
#include <CFNetwork/CFNetwork.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Apple's version-zero private stream callback layout used by bundled CEF. */
struct read_stream_callbacks_v0 {
    CFIndex version;
    void *open, *open_completed, *read, *get_buffer, *can_read;
    void *close, *copy_property, *schedule, *unschedule;
};
extern CFReadStreamRef CFReadStreamCreate(CFAllocatorRef,
    const struct read_stream_callbacks_v0 *, CFStreamClientContext *);
extern void CFReadStreamSignalEvent(CFReadStreamRef, CFStreamEventType,
                                    const void *);

struct nil_rect { float x, y, width, height; };
extern struct nil_rect nil_rect_return(void *, const char *)
    __asm__("_objc_msgSend_stret");

static Boolean stream_open(CFReadStreamRef stream, CFStreamError *error,
                           Boolean *complete, void *info)
{
    (void)stream; (void)error; (void)info;
    *complete = true;
    return true;
}

static CFIndex stream_read(CFReadStreamRef stream, UInt8 *buffer,
                           CFIndex length, CFStreamError *error,
                           Boolean *eof, void *info)
{
    (void)stream; (void)error; (void)info;
    if (length < 2) return -1;
    buffer[0] = 'o'; buffer[1] = 'k';
    *eof = true;
    return 2;
}

static Boolean stream_can_read(CFReadStreamRef stream, void *info)
{
    (void)stream; (void)info;
    return true;
}

static void count_value(const void *value, void *context)
{
    if (value) ++*(int *)context;
}

int check_cf_bridge(void)
{
    /* Exercise the fourth hidden-result import with an actual i386 caller.
       A nil receiver needs no AppKit object or UI; guards and ESP verify the
       destination width and the callee's hidden-pointer stack cleanup. */
    for (unsigned repeat = 0; repeat < 32; ++repeat) {
        struct { uint32_t before; struct nil_rect value; uint32_t after; } guarded = {
            .before = 0x11223344, .value = {1, 2, 3, 4}, .after = 0xaabbccdd
        };
        uintptr_t before, after;
        __asm__ volatile("movl %%esp,%0" : "=r"(before));
        guarded.value = nil_rect_return(NULL, "frame");
        __asm__ volatile("movl %%esp,%0" : "=r"(after));
        if (before != after || guarded.before != 0x11223344 || guarded.after != 0xaabbccdd ||
            guarded.value.x || guarded.value.y || guarded.value.width || guarded.value.height)
            return -124;
    }
    CFStringRef left = CFStringCreateWithCString(NULL, "portal", kCFStringEncodingUTF8);
    CFStringRef right = CFStringCreateWithCString(NULL, "portal", kCFStringEncodingUTF8);
    if (!left || !right || !CFEqual(left, right)) return -101;
    CFStringRef format = CFStringCreateWithCString(NULL, "%@:%d:%lld",
                                                    kCFStringEncodingUTF8);
    CFStringRef formatted = CFStringCreateWithFormat(NULL, NULL,
        format, left, 42, 1234567890123LL);
    char text[64] = {0};
    if (!formatted || !CFStringGetCString(formatted, text, sizeof(text),
        kCFStringEncodingUTF8) || strcmp(text, "portal:42:1234567890123")) return -102;
    CFRelease(formatted);
    CFRelease(format);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CFTimeZoneRef zone = CFTimeZoneCopySystem();
    CFGregorianDate date = CFAbsoluteTimeGetGregorianDate(0, zone);
    double roundtrip = CFGregorianDateGetAbsoluteTime(date, zone);
#pragma clang diagnostic pop
    if (!zone || roundtrip < -1 || roundtrip > 1 ||
        kCFAbsoluteTimeIntervalSince1970 < 900000000.0) return -121;
    CFRelease(zone);


    const UInt8 original[] = {1, 2, 3};
    CFDataRef data = CFDataCreate(NULL, original, sizeof(original));
    if (!data || CFDataGetLength(data) != 3 ||
        memcmp(CFDataGetBytePtr(data), original, 3)) return -103;
    CFMutableDataRef mutable = CFDataCreateMutable(NULL, 0);
    if (!mutable) return -104;
    CFDataAppendBytes(mutable, original, sizeof(original));
    UInt8 *bytes = CFDataGetMutableBytePtr(mutable);
    if (!bytes || CFDataGetLength(mutable) != 3) return -105;
    bytes[1] = 9;
    if (CFDataGetBytePtr(mutable)[1] != 9) return -106;


    CFMutableArrayRef array = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    if (!array) return -107;
    CFArrayAppendValue(array, left);
    CFArrayAppendValue(array, right);
    int count = 0;
    CFArrayApplyFunction(array, CFRangeMake(0, 2), count_value, &count);
    if (count != 2 || CFArrayGetCount(array) != 2) return -108;

    CFMutableDictionaryRef dictionary = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!dictionary) return -109;
    CFDictionarySetValue(dictionary, left, right);
    if (!CFEqual(CFDictionaryGetValue(dictionary, left), right)) return -110;

    CFMutableStringRef mutable_key = CFStringCreateMutable(NULL, 0);
    CFStringRef key_format = CFStringCreateWithCString(NULL, "%@ ",
                                                       kCFStringEncodingUTF8);
    CFStringRef original_key = CFStringCreateWithCString(NULL, "portal ",
                                                         kCFStringEncodingUTF8);
    CFMutableDictionaryRef copied_keys = CFDictionaryCreateMutable(NULL, 0,
        &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!mutable_key || !key_format || !original_key || !copied_keys)
        return -122;
    CFStringAppendFormat(mutable_key, NULL, key_format, left);
    CFDictionarySetValue(copied_keys, mutable_key, right);
    CFStringTrimWhitespace(mutable_key);
    if (!CFEqual(CFDictionaryGetValue(copied_keys, original_key), right))
        return -123;
    CFRelease(copied_keys);
    CFRelease(original_key);
    CFRelease(key_format);
    CFRelease(mutable_key);


    const UInt8 url_bytes[] = "https://example.com/a";
    CFURLRef url = CFURLCreateWithBytes(NULL, url_bytes,
        sizeof(url_bytes) - 1, kCFStringEncodingUTF8, NULL);
    if (!url) return -111;
    CFArrayRef proxies = CFNetworkCopyProxiesForURL(url, dictionary);
    if (!proxies) return -112;
    CFRelease(proxies);

    CFUUIDRef uuid = CFUUIDGetConstantUUIDWithBytes(NULL,
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16);
    CFUUIDBytes uuid_bytes = CFUUIDGetUUIDBytes(uuid);
    if (uuid_bytes.byte0 != 1 || uuid_bytes.byte15 != 16) return -113;
    if (!CFBooleanGetValue(kCFBooleanTrue) || CFBooleanGetValue(kCFBooleanFalse))
        return -114;
    if (CFStringGetMaximumSizeForEncoding(4, kCFStringEncodingUTF8) < 4)
        return -115;

    CFAllocatorContext allocator_context = {0};
    allocator_context.allocate = (CFAllocatorAllocateCallBack)malloc;
    allocator_context.reallocate = (CFAllocatorReallocateCallBack)realloc;
    allocator_context.deallocate = (CFAllocatorDeallocateCallBack)free;
    CFAllocatorRef allocator = CFAllocatorCreate(NULL, &allocator_context);
    if (!allocator) return -116;
    void *allocation = CFAllocatorAllocate(allocator, 32, 0);
    if (!allocation) return -120;
    ((uint8_t *)allocation)[31] = 7;
    CFAllocatorDeallocate(allocator, allocation);
    CFRelease(allocator);

    const struct read_stream_callbacks_v0 callbacks = {
        .version = 0, .open = (void *)stream_open,
        .read = (void *)stream_read, .can_read = (void *)stream_can_read
    };
    CFStreamClientContext stream_context = {0};
    CFReadStreamRef stream = CFReadStreamCreate(NULL, &callbacks,
                                               &stream_context);
    if (!stream) return -117;
    UInt8 received[4] = {0};
    if (!CFReadStreamOpen(stream)) return -118;
    CFReadStreamSignalEvent(stream, kCFStreamEventHasBytesAvailable, NULL);
    if (
        CFReadStreamRead(stream, received, sizeof(received)) != 2 ||
        received[0] != 'o' || received[1] != 'k') return -119;
    CFReadStreamClose(stream);
    CFRelease(stream);

    CFRelease(url);
    CFRelease(dictionary);
    CFRelease(array);
    CFRelease(mutable);
    CFRelease(data);
    CFRelease(right);
    CFRelease(left);
    return 0;
}
