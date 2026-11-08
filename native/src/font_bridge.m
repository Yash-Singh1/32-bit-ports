#include "font_bridge.h"
#include "quicktime_support.h"
#include "objc_bridge.h"
#include "compat_runtime.h"
#import <Foundation/Foundation.h>
#import <CoreText/CoreText.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ATSUI's 32-bit layouts come from Apple's ATS/ATSUI SDK headers. Font and
   glyph measurements use floats; positions use signed 16.16 Fixed values. */
struct __attribute__((packed, aligned(2))) layout_record32 {
    uint16_t glyph; uint32_t flags, offset; int32_t position;
};
struct ideal_metrics32 { float advance[2], bearing[2], other[2]; };
struct screen_metrics32 {
    float advance[2], top_left[2]; uint32_t height, width;
    float bearing[2], other[2];
};
_Static_assert(sizeof(struct layout_record32) == 14, "ATSLayoutRecord i386 ABI");
_Static_assert(sizeof(struct screen_metrics32) == 40, "ATSGlyphScreenMetrics i386 ABI");

enum { OBJECT_LIMIT = 2048, FONT_LIMIT = 512, HANDLE_BASE = 0xff900000 };
enum object_kind { STYLE = 1, LAYOUT, BITMAP, COLORSPACE };
struct font_style { CTFontRef font; uint32_t font_id, rendering; CGFloat size, color[4]; bool bold, italic, underline; };
struct font_layout { CTLineRef line; uint32_t records, count, context; };
struct font_bitmap { CGContextRef context; uint32_t allocation; };
struct font_object {
    enum object_kind kind;
    union { struct font_style style; struct font_layout layout; struct font_bitmap bitmap; CGColorSpaceRef space; };
};
static struct font_object objects[OBJECT_LIMIT];
static CTFontRef fonts[FONT_LIMIT];
static unsigned font_count;
static pthread_mutex_t font_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;

static struct font_object *lookup(uint32_t handle, enum object_kind kind)
{
    unsigned index = handle - HANDLE_BASE - 1;
    return index < OBJECT_LIMIT && objects[index].kind == kind ? objects + index : NULL;
}
static uint32_t create(enum object_kind kind)
{
    for (unsigned i = 0; i < OBJECT_LIMIT; ++i) if (!objects[i].kind) {
        objects[i].kind = kind; return HANDLE_BASE + i + 1;
    }
    return 0;
}
static uint32_t register_font(CTFontRef font)
{
    if (!font) return 0;
    for (unsigned i = 0; i < font_count; ++i) if (CFEqual(fonts[i], font)) return i + 1;
    if (font_count == FONT_LIMIT) return 0;
    fonts[font_count++] = (CTFontRef)CFRetain(font);
    return font_count;
}
static CTFontRef font_for_id(uint32_t id) { return id && id <= font_count ? fonts[id - 1] : NULL; }
static bool font_matches(CTFontRef font, CFStringRef name, bool postscript)
{
    CFStringRef actual = postscript ? CTFontCopyPostScriptName(font) : CTFontCopyFullName(font);
    bool matches = actual && CFStringCompare(actual, name, kCFCompareCaseInsensitive) == kCFCompareEqualTo;
    if (actual) CFRelease(actual);
    if (!matches && !postscript) {
        actual = CTFontCopyFamilyName(font);
        matches = actual && CFStringCompare(actual, name, kCFCompareCaseInsensitive) == kCFCompareEqualTo;
        if (actual) CFRelease(actual);
    }
    return matches;
}
static bool update_style(struct font_style *style)
{
    CTFontRef base = font_for_id(style->font_id);
    CTFontRef sized = base ? CTFontCreateCopyWithAttributes(base, style->size, NULL, NULL) :
                           CTFontCreateWithName(CFSTR("Helvetica"), style->size, NULL);
    if (!sized) return false;
    CTFontSymbolicTraits traits = (style->bold ? kCTFontBoldTrait : 0) | (style->italic ? kCTFontItalicTrait : 0);
    CTFontRef styled = traits ? CTFontCreateCopyWithSymbolicTraits(sized, style->size, NULL, traits, traits) : NULL;
    if (style->font) CFRelease(style->font);
    style->font = styled ? styled : (CTFontRef)CFRetain(sized);
    CFRelease(sized);
    return true;
}
static CGContextRef context_for(uint32_t handle)
{
    struct font_object *object = lookup(handle, BITMAP);
    return object ? object->bitmap.context : (CGContextRef)objc_bridge32_host_object(handle);
}
static int32_t fixed(CGFloat value) { return (int32_t)llround(value * 65536.0); }
static float float_arg(uint32_t word) { float value; memcpy(&value, &word, 4); return value; }

static bool layout_records(struct font_layout *layout)
{
    if (layout->records) return true;
    CFIndex count = CTLineGetGlyphCount(layout->line);
    if (count < 0 || count > 1000000) return false;
    layout->count = (uint32_t)count + 1;
    layout->records = compat_runtime32_allocate(layout->count * sizeof(struct layout_record32), 1);
    if (!layout->records) return false;
    struct layout_record32 *records = (void *)(uintptr_t)layout->records;
    CFArrayRef runs = CTLineGetGlyphRuns(layout->line);
    unsigned output = 0;
    for (CFIndex i = 0; i < CFArrayGetCount(runs); ++i) {
        CTRunRef run = (CTRunRef)CFArrayGetValueAtIndex(runs, i);
        for (CFIndex j = 0; j < CTRunGetGlyphCount(run); ++j) {
            CGGlyph glyph; CGPoint position; CFIndex index;
            CTRunGetGlyphs(run, CFRangeMake(j, 1), &glyph);
            CTRunGetPositions(run, CFRangeMake(j, 1), &position);
            CTRunGetStringIndices(run, CFRangeMake(j, 1), &index);
            records[output++] = (struct layout_record32){glyph, 0, (uint32_t)index * 2, fixed(position.x)};
        }
    }
    CFRange range = CTLineGetStringRange(layout->line);
    records[output] = (struct layout_record32){0xffff, 0, (uint32_t)(range.location + range.length) * 2,
        fixed(CTLineGetTypographicBounds(layout->line, NULL, NULL, NULL))};
    return true;
}

static int dispatch(const char *name, const uint32_t *a, uint64_t *r)
{
#define IS(s) (!strcmp(name, s))
#define PTR(i) ((void *)(uintptr_t)a[i])
#define FAIL() do { *r = (uint32_t)-50; return 1; } while (0)
    *r = 0;
    if (IS("_ATSFontActivateFromMemory") || IS("_ATSFontActivateFromFileReference")) {
        bool from_file = IS("_ATSFontActivateFromFileReference");
        CFDataRef data = NULL;
        if (from_file) {
            char *path = quicktime_support32_reference_path(a[0]);
            NSData *contents = path ? [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path]] : nil;
            if (contents) data = CFDataCreate(NULL, [contents bytes], [contents length]);
            free(path);
        } else data = CFDataCreate(NULL, PTR(0), a[1]);
        CFArrayRef descriptors = data ? CTFontManagerCreateFontDescriptorsFromData(data) : NULL;
        uint32_t first = 0;
        for (CFIndex i = 0; descriptors && i < CFArrayGetCount(descriptors); ++i) {
            CTFontRef font = CTFontCreateWithFontDescriptor(CFArrayGetValueAtIndex(descriptors, i), 1, NULL);
            uint32_t id = register_font(font); if (!first) first = id;
            if (font) CFRelease(font);
        }
        if (descriptors) CFRelease(descriptors); if (data) CFRelease(data);
        unsigned output = from_file ? 5 : 6;
        if (a[output]) *(uint32_t *)PTR(output) = first;
        if (!first) FAIL();
    } else if (IS("_ATSFontFindFromName") || IS("_ATSFontFindFromPostScriptName")) {
        CFStringRef string = objc_bridge32_host_object(a[0]);
        if (!string) return 1;
        bool postscript = IS("_ATSFontFindFromPostScriptName");
        for (unsigned i = 0; i < font_count; ++i) if (font_matches(fonts[i], string, postscript)) { *r = i + 1; return 1; }
        CTFontRef font = CTFontCreateWithName(string, 1, NULL);
        if (font && font_matches(font, string, postscript)) *r = register_font(font);
        if (font) CFRelease(font);
    } else if (IS("_ATSFontGetName")) {
        CTFontRef font = font_for_id(a[0]); if (!font || !a[2]) FAIL();
        CFStringRef string = CTFontCopyFullName(font);
        *(uint32_t *)PTR(2) = objc_bridge32_guest_object((void *)string);
        if (string) CFRelease(string);
    } else if (IS("_ATSFontGetHorizontalMetrics")) {
        CTFontRef font = font_for_id(a[0]); if (!font || !a[2]) FAIL();
        uint32_t *version = PTR(2); float *metrics = (float *)(version + 1);
        *version = 0; memset(metrics, 0, 14 * sizeof(float));
        metrics[0] = CTFontGetAscent(font); metrics[1] = -CTFontGetDescent(font); metrics[2] = CTFontGetLeading(font);
        CGRect box = CTFontGetBoundingBox(font);
        metrics[3] = box.size.width / 2; metrics[4] = box.size.width;
        metrics[5] = box.origin.x; metrics[9] = CTFontGetCapHeight(font); metrics[10] = CTFontGetXHeight(font);
        metrics[11] = CTFontGetSlantAngle(font); metrics[12] = CTFontGetUnderlinePosition(font); metrics[13] = CTFontGetUnderlineThickness(font);
    } else if (IS("_ATSUCreateStyle")) {
        uint32_t handle = create(STYLE); if (!handle || !a[0]) FAIL();
        struct font_style *style = &lookup(handle, STYLE)->style;
        style->size = 12; style->color[3] = 1; update_style(style); *(uint32_t *)PTR(0) = handle;
    } else if (IS("_ATSUSetAttributes")) {
        struct font_object *object = lookup(a[0], STYLE); if (!object) FAIL();
        struct font_style *style = &object->style;
        uint32_t *tags = PTR(2), *sizes = PTR(3), *values = PTR(4);
        for (uint32_t i = 0; i < a[1]; ++i) {
            void *value = (void *)(uintptr_t)values[i];
            if (!value) FAIL();
            if (tags[i] == 261 && sizes[i] == 4) style->font_id = *(uint32_t *)value;
            else if (tags[i] == 262 && sizes[i] == 4) style->size = *(int32_t *)value / 65536.0;
            else if (tags[i] == 256 && sizes[i] == 1) style->bold = *(uint8_t *)value != 0;
            else if (tags[i] == 257 && sizes[i] == 1) style->italic = *(uint8_t *)value != 0;
            else if (tags[i] == 258 && sizes[i] == 1) style->underline = *(uint8_t *)value != 0;
            else if (tags[i] == 283 && sizes[i] == 4) style->rendering = *(uint32_t *)value;
            else if (tags[i] == 288 && sizes[i] == 16) {
                for (unsigned channel = 0; channel < 4; ++channel) style->color[channel] = ((float *)value)[channel];
            } else if (tags[i] == 263 && sizes[i] == 6) {
                for (unsigned channel = 0; channel < 3; ++channel) style->color[channel] = ((uint16_t *)value)[channel] / 65535.0;
                style->color[3] = 1;
            }
            else FAIL();
        }
        if (style->size <= 0 || !update_style(style)) FAIL();
    } else if (IS("_ATSUCreateTextLayoutWithTextPtr")) {
        uint32_t offset = a[1] == UINT32_MAX ? 0 : a[1];
        uint32_t length = a[2] == UINT32_MAX ? a[3] - offset : a[2];
        if (!a[0] || !a[7] || offset > a[3] || length > a[3] - offset) FAIL();
        NSString *string = [[NSString alloc] initWithCharacters:(const unichar *)PTR(0) + offset length:length];
        NSMutableAttributedString *text = [[NSMutableAttributedString alloc] initWithString:string];
        [string release];
        uint32_t *lengths = PTR(5), *styles = PTR(6), cursor = 0;
        for (uint32_t i = 0; i < a[4]; ++i) {
            struct font_object *object = lookup(styles[i], STYLE);
            uint32_t run_length = lengths[i] == UINT32_MAX ? length - cursor : lengths[i];
            if (!object || cursor > length || run_length > length - cursor) { [text release]; FAIL(); }
            struct font_style *style = &object->style;
            [text addAttribute:(id)kCTFontAttributeName value:(id)style->font range:NSMakeRange(cursor, run_length)];
            CGColorRef color = CGColorCreateGenericRGB(style->color[0], style->color[1], style->color[2], style->color[3]);
            if (color) { [text addAttribute:(id)kCTForegroundColorAttributeName value:(id)color range:NSMakeRange(cursor, run_length)]; CGColorRelease(color); }
            if (style->underline) [text addAttribute:(id)kCTUnderlineStyleAttributeName value:@1 range:NSMakeRange(cursor, run_length)];
            cursor += run_length;
        }
        CTLineRef line = CTLineCreateWithAttributedString((CFAttributedStringRef)text); [text release];
        uint32_t handle = line ? create(LAYOUT) : 0;
        if (!handle) { if (line) CFRelease(line); FAIL(); }
        lookup(handle, LAYOUT)->layout.line = line; *(uint32_t *)PTR(7) = handle;
    } else if (IS("_ATSUSetLayoutControls")) {
        struct font_object *object = lookup(a[0], LAYOUT); if (!object) FAIL();
        uint32_t *tags = PTR(2), *sizes = PTR(3), *values = PTR(4);
        for (uint32_t i = 0; i < a[1]; ++i) {
            if (tags[i] == 32767 && sizes[i] == 4) object->layout.context = *(uint32_t *)(uintptr_t)values[i];
            else if (tags[i] != 1 && tags[i] != 5) FAIL(); /* Single-line width and flush factor. */
        }
    } else if (IS("_ATSUSetTransientFontMatching")) {
        if (!lookup(a[0], LAYOUT)) FAIL(); /* CoreText performs fallback during shaping. */
    } else if (IS("_ATSUDirectGetLayoutDataArrayPtrFromTextLayout")) {
        struct font_object *object = lookup(a[0], LAYOUT);
        if (!object || a[1] || a[2] != 100 || !layout_records(&object->layout)) FAIL();
        if (a[3]) *(uint32_t *)PTR(3) = object->layout.records;
        if (a[4]) *(uint32_t *)PTR(4) = object->layout.count;
    } else if (IS("_ATSUDirectReleaseLayoutDataArrayPtr")) {
        /* The layout owns its immutable records until disposal. */
    } else if (IS("_ATSUGlyphGetIdealMetrics") || IS("_ATSUGlyphGetScreenMetrics")) {
        struct font_object *object = lookup(a[0], STYLE); if (!object) FAIL();
        bool screen = IS("_ATSUGlyphGetScreenMetrics");
        for (uint32_t i = 0; i < a[1]; ++i) {
            CGGlyph glyph; memcpy(&glyph, (const uint8_t *)PTR(2) + i * a[3], 2);
            CGSize advance; CGRect bounds;
            CTFontGetAdvancesForGlyphs(object->style.font, kCTFontOrientationHorizontal, &glyph, &advance, 1);
            CTFontGetBoundingRectsForGlyphs(object->style.font, kCTFontOrientationHorizontal, &glyph, &bounds, 1);
            if (screen) {
                struct screen_metrics32 *out = (struct screen_metrics32 *)PTR(6) + i;
                float left = floor(bounds.origin.x), right = ceil(CGRectGetMaxX(bounds));
                *out = (struct screen_metrics32){{round(advance.width), round(advance.height)}, {left, -ceil(CGRectGetMaxY(bounds))},
                    (uint32_t)(ceil(CGRectGetMaxY(bounds)) - floor(bounds.origin.y)), (uint32_t)(right - left), {left, 0}, {round(advance.width) - right, 0}};
            } else {
                struct ideal_metrics32 *out = (struct ideal_metrics32 *)PTR(4) + i;
                *out = (struct ideal_metrics32){{advance.width, advance.height}, {bounds.origin.x, 0}, {advance.width - CGRectGetMaxX(bounds), 0}};
            }
        }
    } else if (IS("_ATSUDrawText")) {
        struct font_object *object = lookup(a[0], LAYOUT); if (!object) FAIL();
        CGContextRef context = context_for(object->layout.context); if (!context) FAIL();
        CGContextSaveGState(context);
        CGContextSetTextMatrix(context, CGAffineTransformIdentity);
        CGContextSetTextPosition(context, (int32_t)a[3] / 65536.0, (int32_t)a[4] / 65536.0);
        CTLineDraw(object->layout.line, context);
        CGContextRestoreGState(context);
    } else if (IS("_ATSUDisposeTextLayout") || IS("_ATSUDisposeStyle")) {
        enum object_kind kind = IS("_ATSUDisposeStyle") ? STYLE : LAYOUT;
        struct font_object *object = lookup(a[0], kind); if (!object) FAIL();
        if (kind == STYLE) CFRelease(object->style.font);
        else { CFRelease(object->layout.line); if (object->layout.records) compat_runtime32_deallocate(object->layout.records); }
        memset(object, 0, sizeof(*object));
    } else if (IS("_CGColorSpaceCreateDeviceRGB")) {
        uint32_t handle = create(COLORSPACE); if (!handle) return 1;
        lookup(handle, COLORSPACE)->space = CGColorSpaceCreateDeviceRGB(); *r = handle;
    } else if (IS("_CGColorSpaceRelease")) {
        struct font_object *object = lookup(a[0], COLORSPACE); if (!object) return 0;
        CGColorSpaceRelease(object->space); memset(object, 0, sizeof(*object));
    } else if (IS("_CGBitmapContextCreate")) {
        struct font_object *space = lookup(a[5], COLORSPACE);
        size_t row_bytes = a[4] ? a[4] : (size_t)a[1] * 4;
        uint64_t bytes = (uint64_t)row_bytes * a[2];
        if (!space || !bytes || bytes > UINT32_MAX) return 1;
        uint32_t allocation = a[0] ? 0 : compat_runtime32_allocate((size_t)bytes, 1);
        void *data = a[0] ? PTR(0) : (void *)(uintptr_t)allocation;
        CGContextRef context = data ? CGBitmapContextCreate(data, a[1], a[2], a[3], row_bytes, space->space, a[6]) : NULL;
        uint32_t handle = context ? create(BITMAP) : 0;
        if (!handle) {
            if (context) CGContextRelease(context);
            if (allocation) compat_runtime32_deallocate(allocation);
            return 1;
        }
        lookup(handle, BITMAP)->bitmap = (struct font_bitmap){context, allocation}; *r = handle;
    } else if (IS("_CGBitmapContextGetData")) {
        struct font_object *object = lookup(a[0], BITMAP); if (!object) return 0;
        *r = (uint32_t)(uintptr_t)CGBitmapContextGetData(object->bitmap.context);
    } else if (IS("_CGContextRelease")) {
        struct font_object *object = lookup(a[0], BITMAP); if (!object) return 0;
        CGContextRelease(object->bitmap.context);
        if (object->bitmap.allocation) compat_runtime32_deallocate(object->bitmap.allocation);
        memset(object, 0, sizeof(*object));
    } else if (IS("_CGContextClearRect")) {
        CGContextRef context = context_for(a[0]); if (!context) return 0;
        CGContextClearRect(context, CGRectMake(float_arg(a[1]), float_arg(a[2]), float_arg(a[3]), float_arg(a[4])));
    } else if (IS("_CGContextFlush")) {
        CGContextRef context = context_for(a[0]); if (!context) return 0; CGContextFlush(context);
    } else if (IS("_CGContextSetAllowsAntialiasing") || IS("_CGContextSetShouldAntialias")) {
        CGContextRef context = context_for(a[0]); if (!context) return 0;
        if (IS("_CGContextSetAllowsAntialiasing")) CGContextSetAllowsAntialiasing(context, a[1]);
        else CGContextSetShouldAntialias(context, a[1]);
    } else if (IS("_CGContextSetLineWidth")) {
        CGContextRef context = context_for(a[0]); if (!context) return 0; CGContextSetLineWidth(context, float_arg(a[1]));
    } else if (IS("_CGContextSetRGBStrokeColor")) {
        CGContextRef context = context_for(a[0]); if (!context) return 0;
        CGContextSetRGBStrokeColor(context, float_arg(a[1]), float_arg(a[2]), float_arg(a[3]), float_arg(a[4]));
    } else if (IS("_CGContextSetTextDrawingMode")) {
        CGContextRef context = context_for(a[0]); if (!context) return 0; CGContextSetTextDrawingMode(context, a[1]);
    } else return 0;
    return 1;
#undef IS
#undef PTR
#undef FAIL
}

int font_bridge32_dispatch(const char *name, const uint32_t *arguments, uint64_t *result)
{
    if (strncmp(name, "_ATS", 4) && strncmp(name, "_CG", 3)) return 0;
    pthread_mutex_lock(&font_lock);
    int handled;
    @autoreleasepool { handled = dispatch(name, arguments, result); }
    if (handled && !strncmp(name, "_ATSU", 5) && (int32_t)*result < 0 && getenv("LP32_TRACE_FONT"))
        fprintf(stderr, "compat32: font call %s failed (%d) args=%08x,%08x,%08x,%08x\n", name,
            (int32_t)*result, arguments[0], arguments[1], arguments[2], arguments[3]);
    pthread_mutex_unlock(&font_lock);
    return handled;
}
