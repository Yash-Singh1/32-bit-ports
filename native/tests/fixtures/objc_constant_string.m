/* Compile both with and without -fno-constant-cfstrings. No Apple SDK or
 * game executable is needed for this i386 guest. */
@interface NSString { void *isa; }
- (id)retain;
- (oneway void)release;
- (id)autorelease;
- (id)copy;
- (unsigned int)length;
- (const char *)UTF8String;
- (signed char)isEqualToString:(NSString *)other;
@end
@interface NSConstantString : NSString { const char *bytes; unsigned int length; }
@end

extern int strcmp(const char *, const char *);
extern unsigned int CFStringGetLength(const void *);

int main(void)
{
    NSString *value = @"COD4 legacy constant string";
#ifdef MISALIGNED_LITERAL
    value = (NSString *)((char *)value + 4);
#endif
    for (unsigned int iteration = 0; iteration < 64; ++iteration) {
        NSString *held = [value retain];
        if (!held || ![held isEqualToString:value]) return 1;
#ifdef LEGACY_LITERAL
        if (held != value) return 2;
#endif
        if ([value length] != 27 || CFStringGetLength(value) != 27) return 3;
        if (strcmp([value UTF8String], "COD4 legacy constant string")) return 4;
        [held release];
        NSString *copy = [value copy];
        if (![copy isEqualToString:value]) return 5;
        [copy release];
#ifdef LEGACY_LITERAL
        [value autorelease];
#endif
    }
#ifdef LEGACY_LITERAL
    NSString *empty = @"";
    NSString *accent = @"caf\u00e9";
    NSString *embedded = @"a\0b";
    if ([empty length] != 0 || [empty retain] != empty) return 6;
    if ([accent length] != 4 || strcmp([accent UTF8String], "caf\xc3\xa9")) return 7;
    if ([embedded length] != 3) return 8;
#endif
    return 0;
}
