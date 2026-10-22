#include "cocoa_input.h"
#include <assert.h>
#include <stdio.h>

/* Only the event factory's window-number property is needed. No application,
   game, or visible window is created by this test. */
@interface InputTestWindow : NSObject
- (NSInteger)windowNumber;
@end
@implementation InputTestWindow
- (NSInteger)windowNumber { return 42; }
@end

int main(void)
{
    @autoreleasepool {
        NSWindow *window = (NSWindow *)[[InputTestWindow alloc] init];
        const unichar arrows[] = {NSLeftArrowFunctionKey, NSRightArrowFunctionKey,
                                  NSDownArrowFunctionKey, NSUpArrowFunctionKey};
        for (unsigned i = 0; i < 4; ++i) {
            NSEvent *event = lp32_key_event(window, 123 + i, YES, 0);
            assert(event.type == NSEventTypeKeyDown && event.keyCode == 123 + i);
            assert(event.characters.length == 1);
            assert([event.characters characterAtIndex:0] == arrows[i]);
            assert([event.characters isEqual:event.charactersIgnoringModifiers]);
            assert(event.modifierFlags & NSEventModifierFlagFunction);
            assert(event.modifierFlags & NSEventModifierFlagNumericPad);
            assert(!event.isARepeat && event.windowNumber == 42);
        }
        const unsigned codes[] = {49, 53, 18, 19, 20};
        NSArray *characters = @[@" ", @"\x1b", @"1", @"2", @"3"];
        for (unsigned i = 0; i < 5; ++i) {
            NSEvent *event = lp32_key_event(window, codes[i], NO, NSEventModifierFlagShift);
            assert(event.type == NSEventTypeKeyUp && !event.isARepeat);
            assert([event.characters isEqual:characters[i]]);
            assert(event.modifierFlags & NSEventModifierFlagShift);
        }
        assert(!lp32_key_event(nil, 49, YES, 0));
        [window release];
    }
    puts("Cocoa input event tests: PASS");
    return 0;
}
