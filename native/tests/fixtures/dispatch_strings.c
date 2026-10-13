#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern int guest_toupper(int) __asm("___toupper");
extern int guest_tolower(int) __asm("___tolower");

int check_dispatch_strings(void)
{
    struct { char text[8]; uint32_t guard; } input = {{'A','b','C','a','b','c',0,0},0x1234abcd};
    const unsigned char high[] = {0x80,0}, low[] = {0x7f,0};
    /* Repeating the same imports exercises their memoized handlers after
       first resolution, including signed results, borrowed pointers and NUL. */
    for (unsigned i = 0; i < 8; ++i) {
        if (strlen(input.text) != 6 || strcmp(input.text,"AbCabc") ||
            strcmp((const char *)high,(const char *)low) <= 0 ||
            strncmp(input.text,"AbCzzz",3) || strncmp(input.text,"AbCzzz",4) >= 0 ||
            strcasecmp(input.text,"aBcAbC") || strcasecmp("b","A") <= 0 ||
            strrchr(input.text,'b') != input.text + 4 || strrchr(input.text,0) != input.text + 6 ||
            strrchr(input.text,'z') || memcmp(high,low,1) <= 0 || memcmp(high,low,0) ||
            guest_toupper('b') != 'B' || guest_tolower('B') != 'b' ||
            guest_toupper(EOF) != EOF || guest_tolower(EOF) != EOF ||
            input.guard != 0x1234abcd || input.text[6] || input.text[7]) return -1040;
    }
    return 0;
}
