#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <stdint.h>
#include <string.h>

extern void static_display_bounds(void), static_uuid_bytes(void);
extern void static_gregorian_date(void), static_objc_rect(void);
extern int check_stret_stack(void *, unsigned, const uint32_t *, unsigned);

/* Deliberate i386 assembly callers model both historical caller-cleanup and
   Source's callee-cleanup ABI, independent of the modern C compiler's choice. */
int stret_fixture(const uint32_t *configuration)
{
    /* Tail-call wrappers force real main-image import stubs. External
       function addresses alone can use dyld text bindings instead. */
    void *functions[] = {static_display_bounds, static_uuid_bytes,
        static_gregorian_date, static_objc_rect};
    CFUUIDRef uuid = CFUUIDGetConstantUUIDWithBytes(NULL,
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16);
    CFTimeZoneRef zone = CFTimeZoneCopySystem();
    if (!uuid || !zone) return 1;
    for (unsigned dynamic = 0; dynamic < 2; ++dynamic) {
        for (unsigned repeat = 0; repeat < 32; ++repeat) {
            for (unsigned which = 0; which < 4; ++which) {
                struct { uint32_t before; unsigned char bytes[16]; uint32_t after; } output;
                output.before = 0x11223344; output.after = 0xaabbccdd;
                memset(output.bytes, 0xa5, sizeof(output.bytes));
                uint32_t words[4] = {(uintptr_t)output.bytes, 0, 0, 0};
                unsigned count = 2;
                if (which == 0) words[1] = CGMainDisplayID();
                else if (which == 1) words[1] = (uintptr_t)uuid;
                else if (which == 2) { words[3] = (uintptr_t)zone; count = 4; }
                else { words[2] = (uintptr_t)"frame"; count = 3; }
                void *function = dynamic ? (void *)(uintptr_t)configuration[which + 1] : functions[which];
                if (check_stret_stack(function, count, words, configuration[0])) return 2;
                if (output.before != 0x11223344 || output.after != 0xaabbccdd) return 3;
                if (which == 0) {
                    CGRect rect; memcpy(&rect, output.bytes, sizeof(rect));
                    if (rect.size.width <= 0 || rect.size.height <= 0) return 4;
                } else if (which == 1) {
                    for (unsigned i = 0; i < 16; ++i) if (output.bytes[i] != i + 1) return 5;
                } else if (which == 2) {
                    CFGregorianDate date; memcpy(&date, output.bytes, sizeof(date));
                    double time = CFGregorianDateGetAbsoluteTime(date, zone);
                    if (time < -0.001 || time > 0.001) return 6;
                } else {
                    for (unsigned i = 0; i < 16; ++i) if (output.bytes[i]) return 7;
                }
            }
        }
    }
    CFRelease(zone);
    return 0;
}
