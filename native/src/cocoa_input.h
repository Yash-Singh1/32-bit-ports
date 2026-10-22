#ifndef LP32_COCOA_INPUT_H
#define LP32_COCOA_INPUT_H
#import <AppKit/AppKit.h>
NSString *lp32_key_characters(unsigned short key);
NSEvent *lp32_key_event(NSWindow *window, unsigned short key, BOOL down,
                        NSEventModifierFlags flags);
#endif
