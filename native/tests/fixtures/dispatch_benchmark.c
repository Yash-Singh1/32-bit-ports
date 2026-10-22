#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint64_t guest_ticks(void) __asm("_UpTime");
extern uint64_t guest_nanoseconds(uint64_t ticks) __asm("_AbsoluteToNanoseconds");
static volatile uint32_t checksum;
extern int guest_toupper(int) __asm("___toupper");
extern int guest_tolower(int) __asm("___tolower");
extern int check_dispatch_strings(void);

static void report(const char *name, uint64_t started, unsigned operations)
{
    uint64_t elapsed = guest_nanoseconds(guest_ticks() - started);
    printf("LP32_BENCH case=%s ns=%llu ops=%u checksum=%u\n",
           name,(unsigned long long)elapsed,operations,checksum);
}

int LauncherMain(void)
{
    enum { iterations = 20000, block = 32 };
    if (check_dispatch_strings()) return -7;
    struct { unsigned char data[block]; uint32_t guard; } buffer = {{0},0x1234abcd};
    FILE *file = fopen("archive.bin","rb");
    if (!file) return -1;
    uint64_t started = guest_ticks();
    for (unsigned i = 0; i < iterations; ++i) {
        unsigned offset = (i * block) % 65536;
        if (fseek(file,(long)offset,SEEK_SET) || fread(buffer.data,1,block,file) != block ||
            buffer.guard != 0x1234abcd || buffer.data[0] != (unsigned char)offset ||
            buffer.data[block - 1] != (unsigned char)(offset + block - 1)) return -2;
        checksum += buffer.data[0];
    }
    report("seek-read",started,iterations * 2);
    started = guest_ticks();
    for (unsigned i = 0; i < iterations; ++i) {
        if (ftell(file) != 50176 || feof(file) || ferror(file)) return -3;
        checksum += i & 1;
    }
    report("tell-status",started,iterations * 3);
    if (fclose(file)) return -4;

    const char *text = "dispatch-cache";
    started = guest_ticks();
    for (unsigned i = 0; i < iterations; ++i) {
        if (strlen(text) != 14 || strcmp(text,"dispatch-cache") ||
            strncmp(text,"dispatch-caps",12) >= 0) return -5;
        checksum += (unsigned char)text[i % 14];
    }
    report("strings",started,iterations * 3);
    started = guest_ticks();
    for (unsigned i = 0; i < iterations; ++i) {
        if (strcasecmp(text,"DISPATCH-CACHE") || strrchr(text,'c') != text + 11 ||
            memcmp(text,"dispatch-cache",14) || guest_toupper('b') != 'B' ||
            guest_tolower('B') != 'b') return -8;
        checksum += i & 1;
    }
    report("scan-fold",started,iterations * 5);
    const char *number = "2.25tail";
    started = guest_ticks();
    for (unsigned i = 0; i < iterations; ++i) {
        struct { char *value; uint32_t guard; } end = {NULL,0xabcdef01};
        float value = strtof(number,&end.value);
        if (value != 2.25f || end.value != number + 4 || end.guard != 0xabcdef01) return -6;
        checksum += (unsigned)value;
    }
    report("strtof",started,iterations);
    printf("LP32_BENCH result=%u\n",checksum);
    return 26;
}
