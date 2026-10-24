#import <Foundation/Foundation.h>
#import <objc/runtime.h>

/* Model a native framework returning an autoreleased guest subclass. This
   autorelease runs in the host, so the guest message bridge never sees it. */
@interface LP32NativeLifetimeFactory : NSObject
+ (id)temporaryGuestObject;
+ (id)newGuestObject;
@end
@implementation LP32NativeLifetimeFactory
+ (id)temporaryGuestObject {
    Class cls = objc_getClass("LP32AutoreleaseTest");
    return [[[cls alloc] init] autorelease];
}
+ (id)newGuestObject {
    Class cls = objc_getClass("LP32AutoreleaseTest");
    return [[cls alloc] init];
}
@end
