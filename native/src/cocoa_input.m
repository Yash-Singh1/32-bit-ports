#include "cocoa_input.h"

NSString *lp32_key_characters(unsigned short key_code)
{
    unichar character;
    switch (key_code) {
        case 36: return @"\r";
        case 49: return @" ";
        case 53: return @"\x1b";
        case 32: return @"u";
        case 1: return @"s";
        case 2: return @"d";
        case 13: return @"w";
        case 12: return @"q";
        case 123: character = NSLeftArrowFunctionKey; return [NSString stringWithCharacters:&character length:1];
        case 124: character = NSRightArrowFunctionKey; return [NSString stringWithCharacters:&character length:1];
        case 125: character = NSDownArrowFunctionKey; return [NSString stringWithCharacters:&character length:1];
        case 126: character = NSUpArrowFunctionKey; return [NSString stringWithCharacters:&character length:1];
        case 0: return @"a";
        default: break;
    }
    /* The rest of the ANSI letters and digits by virtual key code, so
       scripted game binds (F, G, K...) arrive with their characters. */
    static const char ansi[] = {
        [3] = 'f', [4] = 'h', [5] = 'g', [6] = 'z', [7] = 'x', [8] = 'c', [9] = 'v', [11] = 'b', [14] = 'e',
        [15] = 'r', [16] = 'y', [17] = 't', [18] = '1', [19] = '2', [20] = '3', [21] = '4', [22] = '6', [23] = '5',
        [25] = '9', [26] = '7', [28] = '8', [29] = '0', [31] = 'o', [34] = 'i', [35] = 'p', [37] = 'l', [38] = 'j',
        [40] = 'k', [45] = 'n', [46] = 'm', [48] = '\t'};
    if (key_code < sizeof ansi && ansi[key_code]) {
        character = (unichar)ansi[key_code];
        return [NSString stringWithCharacters:&character length:1];
    }
    switch (key_code) {
        case 115: character = NSHomeFunctionKey; break;
        case 119: character = NSEndFunctionKey; break;
        case 116: character = NSPageUpFunctionKey; break;
        case 121: character = NSPageDownFunctionKey; break;
        default: return @"";
    }
    return [NSString stringWithCharacters:&character length:1];
}


NSEvent *lp32_key_event(NSWindow *window, unsigned short key, BOOL down,
                        NSEventModifierFlags flags)
{
    if (!window) return nil;
    if (key >= 123 && key <= 126)
        flags |= NSEventModifierFlagFunction | NSEventModifierFlagNumericPad;
    NSString *characters = lp32_key_characters(key);
    return [NSEvent keyEventWithType:down ? NSEventTypeKeyDown : NSEventTypeKeyUp
        location:NSZeroPoint modifierFlags:flags
        timestamp:NSProcessInfo.processInfo.systemUptime
        windowNumber:window.windowNumber context:nil characters:characters
        charactersIgnoringModifiers:characters isARepeat:NO keyCode:key];
}
