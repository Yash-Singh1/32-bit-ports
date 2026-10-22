#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern float legacy_strtof(const char *, char **) __asm("_strtof");
static uint32_t float_bits(float value)
{
    union { float value; uint32_t bits; } converted = {value};
    return converted.bits;
}

static int check_float(const char *text, uint32_t bits, unsigned offset, int error)
{
    struct { char *value; uint32_t guard; } end = {NULL,0xabcdef01};
    errno = EDOM;
    float value = strtof(text,&end.value);
    if (float_bits(value) != bits || end.value != text + offset ||
        end.guard != 0xabcdef01 || errno != error) return -1011;
    return 0;
}

int check_libc_io_float(void)
{
    FILE *file = fopen("libc-io.txt","rb");
    if (!file || feof(file) || ferror(file)) return -1012;
    struct { unsigned char data[3]; unsigned char guard; } buffer = {{0},0xa5};
    if (fread(buffer.data,1,3,file) != 3 || memcmp(buffer.data,"abc",3) ||
        buffer.guard != 0xa5 || feof(file)) return -1013;
    if (fgetc(file) != EOF || !feof(file) || ferror(file)) return -1014;
    if (fseek(file,0,SEEK_SET) || feof(file) || fgetc(file) != 'a') return -1015;
    if (fseek(file,0,SEEK_END) || fgetc(file) != EOF || !feof(file)) return -1016;
    clearerr(file);
    if (feof(file) || ferror(file) || fclose(file)) return -1017;
    errno = 0;
    if (feof(file) || errno != EBADF) return -1018;

    /* Native fread on a directory sets the error indicator, not EOF. No
       descriptor is closed underneath a live stream to induce this state. */
    file = fopen(".","rb");
    if (!file) return -1019;
    errno = 0;
    if (fread(buffer.data,1,1,file) || !ferror(file) || feof(file) ||
        errno != EISDIR || buffer.guard != 0xa5) return -1020;
    clearerr(file);
    if (feof(file) || ferror(file) || fclose(file)) return -1021;

    int result;
    result = check_float("  -12.5rest",0xc1480000,7,EDOM); if (result) return result;
    result = check_float("0x1.8p+2!",0x40c00000,8,EDOM); if (result) return result;
    result = check_float("-0",0x80000000,2,EDOM); if (result) return result;
    result = check_float("no number",0,0,EDOM); if (result) return result;
    result = check_float("1e10000",0x7f800000,7,ERANGE); if (result) return result;
    result = check_float("1e-10000",0,8,ERANGE); if (result) return result;
    const char *nan = "nan!";
    struct { char *value; uint32_t guard; } end = {NULL,0x1234abcd};
    errno = EDOM;
    uint32_t bits = float_bits(strtof(nan,&end.value));
    if ((bits & 0x7f800000) != 0x7f800000 || !(bits & 0x007fffff) ||
        end.value != nan + 3 || end.guard != 0x1234abcd || errno != EDOM) return -1022;
    errno = EDOM;
    if (float_bits(legacy_strtof("2.25",NULL)) != 0x40100000 || errno != EDOM) return -1023;
    errno = 0;
    if (float_bits(strtof("3",(char **)0xfffffffeu)) || errno != EINVAL) return -1024;
    return 0;
}
