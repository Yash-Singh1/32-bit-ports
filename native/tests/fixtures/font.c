/* Original i386 caller for ATSUI APIs removed from modern SDK declarations.
   Public ABI: Apple's ATSUnicodeObjects/DirectAccess/Glyphs headers (10.6). */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <stdint.h>
#include <stdlib.h>
typedef void *FixtureStyle;
typedef void *FixtureLayout;
extern uint32_t ATSFontFindFromName(CFStringRef, uint32_t);
extern int ATSUCreateStyle(FixtureStyle *);
extern int ATSUSetAttributes(FixtureStyle, uint32_t, const uint32_t *, const uint32_t *, const void **);
extern int ATSUCreateTextLayoutWithTextPtr(const UniChar *, uint32_t, uint32_t, uint32_t, uint32_t, const uint32_t *, const FixtureStyle *, FixtureLayout *);
extern int ATSUSetLayoutControls(FixtureLayout, uint32_t, const uint32_t *, const uint32_t *, const void **);
extern int ATSUDirectGetLayoutDataArrayPtrFromTextLayout(FixtureLayout, uint32_t, uint32_t, void **, uint32_t *);
extern int ATSUGlyphGetScreenMetrics(FixtureStyle, uint32_t, const void *, uint32_t, uint8_t, uint8_t, void *);
extern int ATSUDrawText(FixtureLayout, uint32_t, uint32_t, int32_t, int32_t);
extern int ATSUDisposeTextLayout(FixtureLayout);
extern int ATSUDisposeStyle(FixtureStyle);

extern int32_t FSPathMakeRef(const uint8_t *, void *, uint8_t *);
extern int32_t ATSFontActivateFromFileReference(const void *, uint32_t, uint32_t, void *, uint32_t, uint32_t *);
int check_font(void)
{
    if (getenv("LP32_FIXTURE_QUICKTIME")) {
        uint32_t ref[20];
        struct { uint32_t before, container, after; } output = {0x11223344, 0, 0xaabbccdd};
        if (FSPathMakeRef((const uint8_t *)"/System/Library/Fonts/Helvetica.ttc", ref, 0) ||
            ATSFontActivateFromFileReference(ref, 2, 0, 0, 0, &output.container) ||
            !output.container || output.before != 0x11223344 || output.after != 0xaabbccdd)
            return -56;
        ref[0] = 0;
        if (!ATSFontActivateFromFileReference(ref, 2, 0, 0, 0, 0)) return -57;
    }
    CFStringRef unicode = CFStringCreateWithCString(0, "A\xc3\xa9", kCFStringEncodingUTF8);
    uint16_t characters[2] = {0};
    struct { CFIndex used; uint32_t guard; } converted = {0, 0x12345678};
    if (CFStringGetBytes(unicode, CFRangeMake(0, 2), kCFStringEncodingUnicode, 0, false,
        (UInt8 *)characters, sizeof(characters), &converted.used) != 2 || converted.used != 4 ||
        converted.guard != 0x12345678 || characters[0] != 'A' || characters[1] != 0xe9) return -52;
    CFRelease(unicode);
    if (!CFCharacterSetIsLongCharacterMember(CFCharacterSetGetPredefined(kCFCharacterSetDecomposable), 0xe9)) return -53;
    /* Font-cache misses create and release one CFString per glyph. Cover
       distinct values beyond the persistent proxy table's former limit. */
    for (unsigned i = 0; i < 5000; ++i) {
        UniChar glyph[] = {(UniChar)(0x100 + i)};
        CFStringRef first = CFStringCreateWithBytes(0, (const UInt8 *)glyph, sizeof(glyph), kCFStringEncodingUnicode, false);
        CFStringRef second = CFStringCreateWithBytes(0, (const UInt8 *)glyph, sizeof(glyph), kCFStringEncodingUnicode, false);
        if (!first || !second || CFStringGetLength(first) != 1) return -54;
        CFRetain(first);
        CFRelease(first);
        CFRelease(second);
        if (CFStringGetLength(first) != 1) return -55;
        CFRelease(first);
    }
    CFStringRef name = CFStringCreateWithCString(0, "Helvetica", kCFStringEncodingUTF8);
    uint32_t font = ATSFontFindFromName(name, 0);
    CFRelease(name);
    if (!font) return -40;
    FixtureStyle style;
    if (ATSUCreateStyle(&style)) return -41;
    int32_t size = 20 << 16;
    float color[] = {1, 0, 0, 1};
    uint32_t tags[] = {261, 262, 288}, sizes[] = {4, 4, 16};
    const void *values[] = {&font, &size, color};
    if (ATSUSetAttributes(style, 3, tags, sizes, values)) return -42;
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(0, 64, 32, 8, 64 * 4, space, kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(space);
    if (!context) return -43;
    /* Exercise reclamation beyond the number of available layout slots. */
    for (unsigned i = 0; i < 2050; ++i) {
        UniChar text[] = {'A'}; uint32_t length = 1;
        FixtureLayout layout;
        if (ATSUCreateTextLayoutWithTextPtr(text, 0, 1, 1, 1, &length, &style, &layout)) return -44;
        if (!i) {
            uint32_t tag = 32767, bytes = sizeof(context); const void *value = &context;
            if (ATSUSetLayoutControls(layout, 1, &tag, &bytes, &value)) return -45;
            unsigned char *records = 0; uint32_t count = 0;
            if (ATSUDirectGetLayoutDataArrayPtrFromTextLayout(layout, 0, 100, (void **)&records, &count) ||
                count != 2 || !*(uint16_t *)records || *(uint16_t *)(records + 14) != 0xffff ||
                *(int32_t *)(records + 14 + 10) <= 0) return -46;
            struct { unsigned before; unsigned char data[40]; unsigned after; } metrics;
            metrics.before = 0x12345678; metrics.after = 0x87654321;
            if (ATSUGlyphGetScreenMetrics(style, 1, records, 14, 0, 0, metrics.data) ||
                *(float *)metrics.data <= 0 || *(uint32_t *)(metrics.data + 20) == 0 ||
                metrics.before != 0x12345678 || metrics.after != 0x87654321) return -47;
            if (ATSUDrawText(layout, 0, 1, 2 << 16, 6 << 16)) return -48;
            const unsigned char *pixels = CGBitmapContextGetData(context);
            unsigned covered = 0;
            for (unsigned pixel = 0; pixel < 64 * 32; ++pixel) if (pixels[pixel * 4] && pixels[pixel * 4 + 3]) ++covered;
            if (covered < 10 || covered > 500) return -49;
        }
        if (ATSUDisposeTextLayout(layout)) return -50;
    }
    CGContextRelease(context);
    if (ATSUDisposeStyle(style)) return -51;
    return 0;
}
