/* Original i386 guest objects exercise direct and superclass autorelease,
 * with an explicit guest pool and with Carbon-style pool-free startup. */
@interface NSObject { void *isa; }
+ (id)alloc;
- (id)init;
- (id)retain;
- (oneway void)release;
- (id)autorelease;
- (void)dealloc;
@end
@interface NSAutoreleasePool : NSObject
- (void)drain;
@end
#if defined(NATIVE_FACTORY) || defined(NATIVE_FACTORY_OWNED)
@interface LP32NativeLifetimeFactory : NSObject
+ (id)temporaryGuestObject;
+ (id)newGuestObject;
@end
#endif

static unsigned int deallocations;
@interface LP32AutoreleaseTest : NSObject { unsigned int marker; }
- (unsigned int)marker;
#ifdef NATIVE_CRASH_DIAGNOSTIC
- (void)crash;
#endif
@end
@implementation LP32AutoreleaseTest
- (id)init { self = [super init]; marker = 0x1234abcd; return self; }
#ifdef SUPER_AUTORELEASE
- (id)autorelease { return [super autorelease]; }
#endif
- (unsigned int)marker { return marker; }
#ifdef NATIVE_CRASH_DIAGNOSTIC
- (void)crash {
    extern void *memcpy(void *, const void *, unsigned long);
    unsigned int value = marker;
    memcpy((void *)1, &value, sizeof(value));
}
#endif
- (void)dealloc { ++deallocations; [super dealloc]; }
@end

int main(void)
{
#ifdef STALE_OBJECT_DIAGNOSTIC
    LP32AutoreleaseTest *stale = [[LP32AutoreleaseTest alloc] init];
    [stale release];
    [stale retain];
    return 99;
#elif defined(NATIVE_FACTORY_OWNED)
    id seed = [LP32AutoreleaseTest alloc];
    [seed release];
    deallocations = 0;
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    id owned = [LP32NativeLifetimeFactory newGuestObject];
    [owned release];
    if (deallocations != 1) return 6;
    [pool drain];
    return deallocations == 1 ? 0 : 7;
#else
#ifdef EXPLICIT_POOL
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
#endif
#ifdef NATIVE_FACTORY
    /* Register the guest class before the native factory looks it up. */
    id seed = [LP32AutoreleaseTest alloc];
    [seed release];
    deallocations = 0;
    LP32AutoreleaseTest *object = [LP32NativeLifetimeFactory temporaryGuestObject];
#else
    LP32AutoreleaseTest *object = [[[LP32AutoreleaseTest alloc] init] autorelease];
#endif
    if ([object retain] != object) return 1;
    if ([object marker] != 0x1234abcd || deallocations) return 2;
#ifdef NATIVE_CRASH_DIAGNOSTIC
    /* Wrap the recorder, then fault inside a nested native import. The outer
       guest message and its method must still be available to the handler. */
    for (unsigned int i = 0; i < 200; ++i) [object marker];
    [object crash];
    return 98;
#endif
#ifdef EXPLICIT_POOL
    [pool drain];
    if ([object marker] != 0x1234abcd || deallocations) return 3;
#else
    /* A later explicit pool must not consume the fallback's ownership. */
    NSAutoreleasePool *nested = [[NSAutoreleasePool alloc] init];
    [nested drain];
    if ([object marker] != 0x1234abcd || deallocations) return 3;
#endif
    [object release];
#ifdef EXPLICIT_POOL
    if (deallocations != 1) return 4;
#else
    if (deallocations) return 5;
#endif
    return 0;
#endif
}
