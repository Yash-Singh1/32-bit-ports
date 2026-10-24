@interface NSObject { void *isa; }
+ (id)alloc;
- (id)init;
- (id)retain;
- (oneway void)release;
- (id)autorelease;
@end
@interface NSAutoreleasePool : NSObject
- (void)drain;
@end
@interface NSDictionary : NSObject
- (unsigned int)count;
- (id)allKeys;
@end
@interface NSArray : NSObject
- (unsigned int)count;
- (id)objectAtIndex:(unsigned int)index;
@end
@interface NSData : NSObject
- (unsigned int)length;
- (void)getBytes:(void *)bytes length:(unsigned int)length;
@end
extern unsigned int IORegistryEntryFromPath(unsigned int, const char *);
extern int IOObjectRelease(unsigned int);
extern id IORegistryEntryCreateCFProperty(unsigned int, id, void *, unsigned int);
extern int IORegistryEntryCreateCFProperties(unsigned int, id *, void *, unsigned int);
extern id IORegistryEntrySearchCFProperty(unsigned int, const char *, id, void *, unsigned int);
extern unsigned int CFGetTypeID(id);

int main(void)
{
#ifdef EXPLICIT_POOL
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
#endif
    unsigned int entry = IORegistryEntryFromPath(0, "IOService:/");
    if (!entry) return 1;
    id value = 0;
#ifdef TEST_SEARCH
    value = IORegistryEntrySearchCFProperty(entry, "IOService", @"vendor-id", 0, 0);
#elif defined(TEST_PROPERTY)
    id properties = 0;
    if (IORegistryEntryCreateCFProperties(entry, &properties, 0, 0)) return 2;
    if (!properties || ![properties count]) return 3;
    id keys = [properties allKeys];
    if (!keys || ![keys count]) return 3;
    value = IORegistryEntryCreateCFProperty(entry, [keys objectAtIndex:0], 0, 0);
    [properties release];
#else
    if (IORegistryEntryCreateCFProperties(entry, &value, 0, 0)) return 2;
#endif
    IOObjectRelease(entry);
    if (!value) return 3;
#ifdef TEST_SEARCH
    unsigned int expected = [value length];
#elif !defined(TEST_PROPERTY)
    unsigned int expected = [value count];
#endif
    [value autorelease];
#ifdef TEST_SEARCH
    unsigned int bytes = 0;
    [value getBytes:&bytes length:4];
    if (expected != 4 || bytes != 0x106b) return 4;
#endif
    [value retain];
#ifdef EXPLICIT_POOL
    [pool drain];
#endif
#ifdef TEST_SEARCH
    if ([value length] != expected) return 5;
#elif defined(TEST_PROPERTY)
    if (!CFGetTypeID(value)) return 5;
#else
    if ([value count] != expected) return 5;
#endif
    [value release];
#ifdef EXPLICIT_POOL
    /* The final release removes the owned proxy instead of leaving a stale
       native pointer registered under its old guest handle. */
    if (CFGetTypeID(value)) return 6;
#endif
    return 0;
}
