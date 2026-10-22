#!/usr/bin/env python3
"""Build tiny original i386 fixtures with Apple's linker; no game/old SDK needed."""
import os
import signal
from pathlib import Path
import struct
import subprocess
import tempfile
import time

NATIVE = Path(__file__).resolve().parents[1]
LOADER = Path(os.getenv("LP32_TEST_LOADER", str(NATIVE / "build/game_loader"))).resolve()
TEST_GAME = os.getenv("LP32_TEST_GAME", "portal2")
if TEST_GAME not in ("portal2", "l4d2"):
    raise ValueError("LP32_TEST_GAME must select portal2 or l4d2")
TEST_EXECUTABLE = os.getenv("LP32_TEST_EXECUTABLE", "portal2_osx" if TEST_GAME == "portal2" else "")
if not TEST_EXECUTABLE or Path(TEST_EXECUTABLE).name != TEST_EXECUTABLE:
    raise ValueError("LP32_TEST_EXECUTABLE must name the audited original Source executable")
TEST_DATA_DIRECTORY = "Portal2" if TEST_GAME == "portal2" else "L4D2"


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


with tempfile.TemporaryDirectory(prefix="lp32-dyld-") as tmp:
    root = Path(tmp).resolve()
    (root / "bin").mkdir()
    # Link-time declarations only. All actual system calls go through the bridge.
    (root / "libSystem.tbd").write_text("""--- !tapi-tbd-v3
archs: [ i386 ]
platform: macosx
install-name: /usr/lib/libSystem.B.dylib
exports:
  - archs: [ i386 ]
    symbols: [
      _NewTimerUPP, _DisposeTimerUPP, _InstallTimeTask, _PrimeTime, _PrimeTimeTask, _RemoveTimeTask,
      _NewSndCallBackUPP, _DisposeSndCallBackUPP, _SndNewChannel, _SndDisposeChannel, _SndDoCommand, _SndDoImmediate, _SndChannelStatus, _OTAtomicAdd32,
      _wcstoul, _wcstoll, _wcstoull, _wcsdup, _wcspbrk, _wcsspn, _wcscspn, _wcscasecmp, _wcsncasecmp, _atoll, _vswscanf, _iswalnum, _iswalpha, _iswblank, _iswcntrl, _iswdigit, _iswgraph, _iswlower, _iswprint, _iswpunct, _iswspace, _iswupper, _iswxdigit, _towlower, _towupper, _iswctype, _wctype, _towctrans, _wctrans, ___error, _strerror, _strerror$UNIX2003,
      _wcsstr, _wcsrchr, _wcsncat, _wcscoll, _wcstod, _wcstof, _wcstol, _swscanf, _strtoull, _strtoll, _getenv, _bootstrap_look_up, _bootstrap_port, _mach_task_self_, _mach_port_allocate,
      _mach_port_type, _mach_port_deallocate, _mach_port_mod_refs, _mach_msg, _kill, _kill$UNIX2003,
      _opendir$INODE64, _readdir$INODE64, _readdir_r, _closedir, _closedir$UNIX2003, _setlocale, _vswprintf, _swprintf, _wcscmp, _strlen,
      _getrusage, _asinf, _atanf, _finite, ___fixunssfdi, _GetGlobalMouse,
      _CGDisplayHideCursor, _CGDisplayShowCursor, _CGCursorIsVisible,
      _CFStringGetBytes, _CFCharacterSetGetPredefined, _CFCharacterSetIsLongCharacterMember, _UpTime, _AbsoluteToNanoseconds, _NanosecondsToAbsolute,
      ___divdi3, ___moddi3, _setjmp, __setjmp, _sigsetjmp, _longjmp,
      __longjmp, _siglongjmp, _sigprocmask, _sigprocmask$UNIX2003, _memcpy, ___memset_chk, ___bzero,
      _pthread_create, _pthread_create_suspended_np, _pthread_self, _pthread_mach_thread_np, _thread_suspend, _thread_resume, _pthread_join, _pthread_join$UNIX2003, _usleep,
      _usleep$UNIX2003, _GetCurrentProcess, _AudioObjectGetPropertyData, _AudioQueueNewOutput, _AudioQueueAddPropertyListener, _AudioQueueAllocateBuffer, _AudioQueueSetParameter, _AudioQueueGetParameter,
      _AudioQueueEnqueueBuffer, _AudioQueueStart, _AudioQueueStop, _AudioQueueRemovePropertyListener, _AudioQueueFreeBuffer, _AudioQueueDispose,
      _AudioConverterNew, _AudioConverterDispose, _AudioConverterSetProperty, _AudioConverterFillComplexBuffer,
      _FSPathMakeRef, _FSGetCatalogInfo, _OpenMovieFile, _CloseMovieFile, _QTNewGWorld,
      _QTSetPixMapHandleGammaLevel, _QTSetPixMapHandleRequestedGammaLevel,
      _EnterMovies, _ExitMovies, _NewMovieFromFile, _DisposeMovie, _GetMoviesError,
      _GetMovieTimeScale, _GetMovieDuration, _GetMovieNaturalBoundsRect, _GetMovieNextInterestingTime,
      _SetMovieGWorld, _SetMovieTimeValue, _StartMovie, _StopMovie, _UpdateMovie, _MoviesTask,
      _FindNextComponent, _OpenAComponent, _CloseComponent, _AudioUnitSetProperty, _AudioUnitGetProperty, _AudioUnitGetPropertyInfo,
      _AudioUnitInitialize, _AudioUnitUninitialize, _AudioUnitRender, _alcCaptureOpenDevice, _alcCaptureCloseDevice, _alcGetError,
      _printf, _fflush, _malloc, _memcmp, _bsearch, _feof, _ferror, _fopen, _fopen$UNIX2003, _fclose,
      _fread, _fgetc, _fseek, _clearerr, _strtof, _strtof$UNIX2003,
      ___stdoutp, __NSGetExecutablePath, _getcwd,
      _strcmp, _strncmp, _strcasecmp, _strrchr, ___toupper, ___tolower, _strncpy, _lp32_unavailable, _stat, _stat$INODE64, _open$UNIX2003, _open,
      _close, _close$UNIX2003, _lseek, _scandir, _alphasort, _free,
      _sigaction, _raise, _getpid, _iconv_open, _iconv, _iconv_close,
      _mmap, _mmap$UNIX2003, _munmap, _munmap$UNIX2003, _mprotect, _mprotect$UNIX2003,
      _getpagesize, _memset, _dlopen, _dlsym, _dlerror, _dlclose,
      _CFRunLoopGetCurrent, _CFRunLoopSourceCreate, _CFRunLoopAddSource, _CFRunLoopContainsSource, _CFRunLoopSourceSignal, _CFRunLoopRunInMode,
      _CFRunLoopRemoveSource, _CFRunLoopTimerCreate, _CFRunLoopAddTimer, _CFRunLoopRemoveTimer, _CFRunLoopStop, _CFRelease,
      _kCFRunLoopDefaultMode, _CGMainDisplayID, _CGDisplayBounds, _mktime, _localtime_r, _timegm,
      _gethostname, _gethostbyname, _socket, _bind, _bind$UNIX2003, _getsockname,
      _setsockopt, _getsockopt, _sendto, _sendto$UNIX2003, _recvfrom, _recvfrom$UNIX2003,
      _select, _select$UNIX2003, _select$DARWIN_EXTSN, _ioctl, _popen, _popen$UNIX2003,
      _pclose, _valloc, _realloc, _malloc_size, _MPCreateCriticalRegion, _MPDeleteCriticalRegion,
      _MPEnterCriticalRegion, _MPExitCriticalRegion, _NSIsSymbolNameDefined, _NSLookupAndBindSymbol, _NSAddressOfSymbol, _NSModuleForSymbol,
      _NSLibraryNameForModule, _fgets, _fgets$UNIX2003, ___darwin_check_fd_set_overflow, _getsockname$UNIX2003, _mktime$UNIX2003,
      _CFStringCreateWithBytes, _CFStringGetLength, _CFRetain,
      _CFEqual, _CFStringCreateWithFormat, _CFStringGetCString,
      _CFStringCreateMutable, _CFStringAppendFormat, _CFStringTrimWhitespace,
      _CFDataCreate, _CFDataGetLength, _CFDataGetBytePtr, _CFDataCreateMutable,
      _CFDataAppendBytes, _CFDataGetMutableBytePtr, _CFArrayCreateMutable,
      _CFArrayAppendValue, _CFArrayApplyFunction, _CFArrayGetCount,
      _CFDictionaryCreateMutable, _CFDictionarySetValue, _CFDictionaryGetValue,
      _CFURLCreateWithBytes, _CFNetworkCopyProxiesForURL,
      _CFUUIDGetConstantUUIDWithBytes, _CFUUIDGetUUIDBytes,
      _objc_msgSend_stret,
      _CFBooleanGetValue, _CFStringGetMaximumSizeForEncoding,
      _CFAllocatorCreate, _CFAllocatorAllocate, _CFAllocatorDeallocate,
      _CFReadStreamCreate, _CFReadStreamOpen,
      _CFReadStreamRead, _CFReadStreamClose, _CFReadStreamSignalEvent,
      _CFTimeZoneCopySystem, _CFAbsoluteTimeGetGregorianDate,
      _CFGregorianDateGetAbsoluteTime, _kCFAbsoluteTimeIntervalSince1970,
      _kCFTypeArrayCallBacks, _kCFTypeDictionaryKeyCallBacks,
      _kCFTypeDictionaryValueCallBacks, _kCFCopyStringDictionaryKeyCallBacks,
      _kCFBooleanTrue, _kCFBooleanFalse,
      _CFStringCreateWithCString, _ATSFontFindFromName, _ATSUCreateStyle, _ATSUSetAttributes, _ATSUCreateTextLayoutWithTextPtr, _ATSUSetLayoutControls,
      _ATSUDirectGetLayoutDataArrayPtrFromTextLayout, _ATSUGlyphGetScreenMetrics, _ATSUDrawText, _ATSUDisposeTextLayout, _ATSUDisposeStyle, _CGColorSpaceCreateDeviceRGB,
      _CGColorSpaceRelease, _CGBitmapContextCreate, _CGBitmapContextGetData, _CGContextRelease, ___stack_chk_guard, ___stack_chk_fail,
      dyld_stub_binder
    ]
...
""")
    # Source, CEF and Steam together exceed the original 2048 import slots.
    # Keep uncalled strong references, then execute real imports above them.
    padding = [f"lp32_fixture_unused_{i}" for i in range(2200)]
    tbd = root / "libSystem.tbd"
    tbd.write_text(tbd.read_text().replace("dyld_stub_binder", ", ".join("_" + n for n in padding) + ", dyld_stub_binder"))
    (root / "many_imports.c").write_text(
        "\n".join(f"extern void {n}(void);" for n in padding) +
        "\nvoid (*fixture_imports[])(void) = {" + ",".join(padding) + "};\n")
    run("xcrun", "clang", "-arch", "x86_64", "-dynamiclib",
        str(NATIVE / "tests/fixtures/steam_host.c"), "-o", str(root / "steamclient.dylib"))
    (root / "dep.c").write_text("""
int value = 11;
int *value_pointer = &value;
__attribute__((constructor)) static void initialize(void) { *value_pointer *= 2; }
int dependency_value(void) { return *value_pointer; }
""")
    (root / "launcher.c").write_text("""
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <iconv.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <malloc/malloc.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netdb.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
extern unsigned bootstrap_port;
extern int bootstrap_look_up(unsigned, const char *, unsigned *);
extern int dependency_value(void);
extern int strcmp(const char *, const char *) __attribute__((weak_import));
extern int lp32_unavailable(void) __attribute__((weak_import));
extern int check_directory(void);
extern int check_font(void);
extern int check_cf_bridge(void);
extern int check_context(void);
extern int check_mach_ipc(void);
extern int check_steam(void);
extern int check_wide_scan(void);
extern int check_audio_threads(void);
extern int check_audio_converter(void);
extern int check_quicktime(void);
extern int check_strerror(void);
extern int check_bsearch(void);
extern int check_libc_io_float(void);
extern int check_dispatch_strings(void);
extern int _NSGetExecutablePath(char *, uint32_t *);
extern int check_audio_unit(void);
extern int check_sound_manager(void);
extern int check_time_manager(void);
extern int MPCreateCriticalRegion(void **);
extern int MPDeleteCriticalRegion(void *);
extern int MPEnterCriticalRegion(void *, int);
extern int MPExitCriticalRegion(void *);
extern int NSIsSymbolNameDefined(const char *);
extern void *NSLookupAndBindSymbol(const char *);
extern void *NSAddressOfSymbol(void *);
extern void *NSModuleForSymbol(void *);
extern const char *NSLibraryNameForModule(void *);
int (*dependency_pointer)(void) = dependency_value;
int initialized, count;
static volatile sig_atomic_t received_signal;
static void source_ready(void *info) { ++*(int *)info; }
static void timer_ready(CFRunLoopTimerRef timer, void *info) {
    ++*(int *)info; CFRunLoopStop(CFRunLoopGetCurrent());
}
static void child_changed(int signal_number) { received_signal = signal_number; }
static void *bad_stack_worker(void *unused) {
    (void)unused;
    __asm__ volatile("movl $1, %esp; ud2");
    __builtin_unreachable();
}
__attribute__((constructor)) static void initialize(void) {
    initialized = dependency_pointer() + 3;
    ++count;
}
int LauncherMain(void) {
    /* Query through real i386 imports, including a short buffer and guarded
       output cells. Report the original executable, not the native loader. */
    const char *expected_root = getenv("LP32_FIXTURE_EXPECTED_ROOT");
    const char *expected_executable = getenv("LP32_FIXTURE_EXPECTED_EXECUTABLE");
    if (expected_root && expected_executable) {
        struct { char path[4096]; uint32_t guard; } executable = {{0}, 0x1234abcd};
        struct { uint32_t size; uint32_t guard; } capacity = {1, 0x87654321};
        if (_NSGetExecutablePath(executable.path, &capacity.size) != -1 ||
            capacity.size != strlen(expected_root) + strlen(expected_executable) + 2 ||
            capacity.guard != 0x87654321 || executable.path[0] || executable.guard != 0x1234abcd)
            return -701;
        capacity.size = sizeof(executable.path);
        if (_NSGetExecutablePath(executable.path, &capacity.size) ||
            strncmp(executable.path, expected_root, strlen(expected_root)) ||
            executable.path[strlen(expected_root)] != '/' ||
            strcmp(executable.path + strlen(expected_root) + 1, expected_executable) ||
            capacity.guard != 0x87654321 || executable.guard != 0x1234abcd)
            return -702;
        if (!getcwd(executable.path, sizeof(executable.path)) || strcmp(executable.path, expected_root))
            return -703;
    }
    if (getenv("LP32_FIXTURE_BAD_STACK_WORKER")) {
        pthread_t worker;
        if (pthread_create(&worker, NULL, bad_stack_worker, NULL)) return -101;
        pthread_join(worker, NULL);
        return -102;
    }
    if (getenv("LP32_FIXTURE_BAD_STACK")) {
        __asm__ volatile("movl $1, %esp; ud2");
        __builtin_unreachable();
    }
    if (getenv("LP32_FIXTURE_LIFETIME")) {
        volatile unsigned *object = malloc(256);
        object[0] = 0x1234abcd;
        free((void *)object);
        __asm__ volatile("ud2" : : "a"(object) : "memory");
        __builtin_unreachable();
    }
    if (getenv("LP32_FIXTURE_STEAM_REQUIRED")) {
        printf("\\n ##### Sys_Error: %s", "Steam is not running. You must start Steam in order to play this game.");
        __asm__ volatile("ud2"); // The loader must explain the failure and exit first.
    }
    if (getenv("LP32_FIXTURE_IMPORT_CRASH")) {
        extern char *fixture_strncpy(char *, const char *, unsigned long) __asm("_strncpy");
        char destination[260];
        /* Warm import resolution before reproducing the report. */
        fixture_strncpy(destination, "valid", sizeof(destination));
        fixture_strncpy(destination, (const char *)0xe3f1fe67, sizeof(destination));
        return -99;
    }
    if (getenv("LP32_FIXTURE_SIGILL")) {
        printf("\\n ##### Sys_Error: %s", "diagnostic fixture fatal reason");
        fflush(NULL);
        __asm__ volatile("ud2");
    }
    if (getenv("LP32_FIXTURE_SIGSEGV"))
        (void)*(volatile unsigned char *)(uintptr_t)1;
    if (getenv("LP32_FIXTURE_SIGBUS")) raise(SIGBUS);
    if (getenv("LP32_FIXTURE_SIGABRT")) raise(SIGABRT);
    if (!strcmp || lp32_unavailable) return -4;
    unsigned missing_service = 123;
    if (!bootstrap_port || !bootstrap_look_up(bootstrap_port,
            "org.32bitgoofy.test.nonexistent-service", &missing_service) || missing_service) return -58;
    int scan_error = check_wide_scan(); if (scan_error) return scan_error;
    int steam_error = check_steam(); if (steam_error) return steam_error;
    int ipc_error = check_mach_ipc(); if (ipc_error) return ipc_error;
    int context_error = check_context(); if (context_error) return context_error;
    int unit_error = check_audio_unit(); if (unit_error) return unit_error;
    int audio_error = check_audio_threads(); if (audio_error) return audio_error;
    audio_error = check_audio_converter(); if (audio_error) return audio_error;
    audio_error = check_quicktime(); if (audio_error) return audio_error;
    audio_error = check_strerror(); if (audio_error) return audio_error;
    audio_error = check_bsearch(); if (audio_error) return audio_error;
    audio_error = check_libc_io_float(); if (audio_error) return audio_error;
    audio_error = check_dispatch_strings(); if (audio_error) return audio_error;
    int timer_error = check_time_manager(); if (timer_error) return timer_error;
    int sound_error = check_sound_manager(); if (sound_error) return sound_error;
    int font_error = check_font(); if (font_error) return font_error;
    int cf_error = check_cf_bridge(); if (cf_error) return cf_error;
    void *system = dlopen("/usr/lib/libSystem.B.dylib", RTLD_LAZY);
    if (!system || dlopen("/usr/lib/libSystem.B.dylib", RTLD_LAZY) != system) return -17;
    int (*compare)(const char *, const char *) = dlsym(system, "strcmp");
    if (!compare || compare("same", "same") || compare("a", "z") >= 0) return -18;
    if (dlsym(system, "lp32_unavailable") || !dlerror() || dlerror()) return -19;
    if (dlclose(system) || compare("still mapped", "still mapped")) return -20;
    CGRect bounds = CGDisplayBounds(CGMainDisplayID());
    if (bounds.size.width <= 0 || bounds.size.height <= 0) return -21;
    int performed = 0, fired = 0;
    CFRunLoopRef loop = CFRunLoopGetCurrent();
    CFRunLoopSourceContext context = {0};
    context.info = &performed; context.perform = source_ready;
    CFRunLoopSourceRef source = CFRunLoopSourceCreate(0, 0, &context);
    if (!source) return -22;
    CFRunLoopAddSource(loop, source, kCFRunLoopDefaultMode);
    if (!CFRunLoopContainsSource(loop, source, kCFRunLoopDefaultMode)) return -23;
    CFRunLoopSourceSignal(source);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, 1);
    CFRunLoopRemoveSource(loop, source, kCFRunLoopDefaultMode);
    if (performed != 1 || CFRunLoopContainsSource(loop, source, kCFRunLoopDefaultMode)) return -24;
    CFRelease(source);
    CFRunLoopTimerContext timer_context = {0}; timer_context.info = &fired;
    CFRunLoopTimerRef timer = CFRunLoopTimerCreate(0, 0, 0, 0, 0, timer_ready, &timer_context);
    if (!timer) return -25;
    CFRunLoopAddTimer(loop, timer, kCFRunLoopDefaultMode);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, 0);
    CFRunLoopRemoveTimer(loop, timer, kCFRunLoopDefaultMode);
    CFRelease(timer);
    if (fired != 1) return -26;
    struct { unsigned before; struct tm value; unsigned after; } calendar = {0};
    calendar.before = 0x12345678; calendar.after = 0x87654321;
    calendar.value.tm_year = 120; calendar.value.tm_mon = 1;
    calendar.value.tm_mday = 30; calendar.value.tm_hour = 12; calendar.value.tm_isdst = -1;
    time_t timestamp = mktime(&calendar.value);
    struct tm decoded;
    if (timestamp == (time_t)-1 || !localtime_r(&timestamp, &decoded) ||
        decoded.tm_mon != 2 || decoded.tm_mday != 1 || decoded.tm_hour != 12 ||
        calendar.value.tm_mon != 2 || calendar.value.tm_mday != 1 ||
        calendar.before != 0x12345678 || calendar.after != 0x87654321) return -27;
    calendar.value.tm_year = 200;
    if (timegm(&calendar.value) != (time_t)-1) return -28;
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) || !hostname[0]) return -29;
    struct hostent *host = gethostbyname("localhost");
    if (!host || host->h_addrtype != AF_INET || host->h_length != 4 ||
        !host->h_name[0] || !host->h_addr_list[0] || ((unsigned char *)host->h_addr_list[0])[0] != 127) return -30;
    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0 || socket_fd >= FD_SETSIZE) return -31;
    struct sockaddr_in endpoint = {0};
    endpoint.sin_len = sizeof(endpoint); endpoint.sin_family = AF_INET;
    endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t endpoint_size = sizeof(endpoint);
    struct timeval timeout = {0, 200000}, recovered = {0};
    socklen_t timeout_size = sizeof(recovered);
    if (bind(socket_fd, (struct sockaddr *)&endpoint, sizeof(endpoint)) ||
        getsockname(socket_fd, (struct sockaddr *)&endpoint, &endpoint_size) ||
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
        getsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &recovered, &timeout_size) ||
        timeout_size != sizeof(recovered) || recovered.tv_usec != timeout.tv_usec) return -32;
    if (sendto(socket_fd, "udp", 3, 0, (struct sockaddr *)&endpoint, endpoint_size) != 3) return -33;
    fd_set read_set; FD_ZERO(&read_set); FD_SET(socket_fd, &read_set);
    if (select(socket_fd + 1, &read_set, 0, 0, &timeout) != 1 || !FD_ISSET(socket_fd, &read_set)) return -34;
    int available = 0;
    if (ioctl(socket_fd, FIONREAD, &available) || available < 3) return -35;
    char packet[4] = {0};
    if (recvfrom(socket_fd, packet, 3, 0, (struct sockaddr *)&endpoint, &endpoint_size) != 3 ||
        strcmp(packet, "udp") || close(socket_fd)) return -36;
    FILE *command = popen("printf pipe; exit 7", "r");
    char command_output[8] = {0};
    if (!command || fgets(command_output, sizeof(command_output), command) != command_output ||
        strcmp(command_output, "pipe")) return -37;
    int child_status = pclose(command);
    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 7) return -38;
    void *region = 0;
    if (MPCreateCriticalRegion(&region) || !region || MPEnterCriticalRegion(region, 0) ||
        MPEnterCriticalRegion(region, 0x7fffffff) || MPExitCriticalRegion(region) ||
        MPExitCriticalRegion(region) || MPDeleteCriticalRegion(region)) return -39;
    char *aligned = valloc(7000);
    if (!aligned || ((unsigned long)aligned & (getpagesize() - 1)) || malloc_size(aligned) < 7000) return -52;
    aligned[6999] = 'v';
    if (mprotect(aligned, 7000, PROT_READ)) return -53;
    char *resized = realloc(aligned, 12000);
    if (!resized || resized[6999] != 'v' || malloc_size(resized) < 12000) return -54;
    free(resized);
    aligned = valloc(32); if (!aligned) return -55;
    free(aligned);
    if (!NSIsSymbolNameDefined("_LauncherMain") || NSIsSymbolNameDefined("_lp32_unavailable")) return -56;
    void *legacy_symbol = NSLookupAndBindSymbol("_LauncherMain");
    void *legacy_module = NSModuleForSymbol(legacy_symbol);
    const char *legacy_path = NSLibraryNameForModule(legacy_module);
    if (NSAddressOfSymbol(legacy_symbol) != (void *)&LauncherMain || !legacy_module ||
        !legacy_path || !legacy_path[0]) return -57;
    if (!check_directory()) return -5;
    struct sigaction action = {0}, old_action;
    action.sa_handler = child_changed;
    received_signal = 0;
    if (sigaction(SIGCHLD, &action, &old_action) || raise(SIGCHLD)) return -6;
    (void)getpid(); /* Pending signal delivery at the next guest bridge call. */
    if (received_signal != SIGCHLD || sigaction(SIGCHLD, &old_action, 0)) return -7;
    iconv_t converter = iconv_open("UTF-16LE", "UTF-8");
    if (converter == (iconv_t)-1) return -8;
    char input[] = "a", output[8] = {0}, *in = input, *out = output;
    size_t in_left = 1, out_left = sizeof(output);
    if (iconv(converter, &in, &in_left, &out, &out_left) == (size_t)-1 ||
        in_left || out_left != 6 || out != output + 2 || output[0] != 'a' || output[1]) return -9;
    if (iconv_close(converter)) return -10;
    struct { unsigned before; struct stat metadata; unsigned after; } result;
    result.before = 0x12345678; result.after = 0x87654321;
    if (stat("probe.txt", &result.metadata) || result.metadata.st_size != 7 ||
        !S_ISREG(result.metadata.st_mode) || result.before != 0x12345678 || result.after != 0x87654321) return -1;
    int fd = open("probe.txt", O_RDONLY);
    if (fd < 0) return -2;
    off_t end = lseek(fd, 0, SEEK_END);
    if (close(fd) || end != 7) return -3;
    fd = open("probe-large", O_RDONLY);
    if (fd < 0) return -11;
    off_t large_offset = ((off_t)5 << 30) + 7;
    off_t position = lseek(fd, large_offset, SEEK_SET);
    end = lseek(fd, 0, SEEK_END);
    char *mapped = mmap(0, 7, PROT_READ, MAP_PRIVATE, fd, (off_t)5 << 30);
    if (close(fd) || position != large_offset || end != large_offset) return -12;
    if (mapped == MAP_FAILED || mapped[6] || mprotect(mapped, 7, PROT_READ | PROT_WRITE)) return -13;
    mapped[6] = 'x';
    if (munmap(mapped, 7)) return -14;
    size_t page = getpagesize();
    mapped = mmap(0, page * 3, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapped == MAP_FAILED || mmap(mapped + page, page, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) != mapped + page) return -15;
    mapped[page] = 'y';
    if (munmap(mapped, page) || munmap(mapped + page * 2, page) || mapped[page] != 'y' ||
        munmap(mapped + page, page)) return -16;
    return initialized + count + strcmp("same", "same");
}
""")
    (root / "probe.txt").write_text("portal2")
    with (root / "probe-large").open("wb") as sparse:
        sparse.truncate((5 << 30) + 7)
    (root / "probe-dir").mkdir()
    for name in ("zeta", "alpha", "ignored"):
        (root / "probe-dir" / name).touch()
    (root / "directory.c").write_text("""
#define _DARWIN_NO_64_BIT_INODE
#include <dirent.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
struct __attribute__((packed, aligned(4))) dirent64_fixture {
    uint64_t inode, seekoff;
    uint16_t reclen, namlen;
    uint8_t type;
    char name[1024];
};
extern DIR *open64_directory(const char *) __asm("_opendir$INODE64");
extern struct dirent64_fixture *read64_directory(DIR *) __asm("_readdir$INODE64");
static int select_entry(const struct dirent *entry) {
    return entry->d_name[0] == 'a' || entry->d_name[0] == 'z';
}
int check_directory(void) {
    struct dirent **entries = 0;
    int count = scandir("probe-dir", &entries, select_entry, alphasort);
    int ok = count == 2 && entries[0]->d_name[0] == 'a' && entries[1]->d_name[0] == 'z';
    for (int i = 0; i < count; ++i) free(entries[i]);
    free(entries);
    DIR *directory = open64_directory("probe-dir");
    if (!directory) return 0;
    struct { struct dirent entry; uint32_t guard; } legacy = {{0}, 0xabcd1234};
    struct dirent *legacy_result = NULL;
    if (readdir_r(directory, &legacy.entry, &legacy_result) || legacy_result != &legacy.entry ||
        legacy.guard != 0xabcd1234 || !legacy.entry.d_namlen || !legacy.entry.d_name[0]) ok = 0;
    closedir(directory);
    directory = open64_directory("probe-dir");
    if (!directory) return 0;
    unsigned found = 0;
    struct dirent64_fixture *entry;
    while ((entry = read64_directory(directory))) {
        if (!strcmp(entry->name, "alpha") || !strcmp(entry->name, "zeta") || !strcmp(entry->name, "ignored")) {
            if (!entry->inode || entry->type != DT_REG || entry->namlen != strlen(entry->name) ||
                entry->reclen < 21 + entry->namlen + 1) ok = 0;
            ++found;
        }
    }
    return !closedir(directory) && found == 3 && ok;
}
""")
    (root / "start.S").write_text(".text\n.globl _start\n_start:\n ret\n")
    (root / "quicktime-file.mov").write_text("file-boundary fixture; not a decoded movie\n")
    (root / "libc-io.txt").write_bytes(b"abc")
    (root / "helper.S").write_text(".text\n.globl dyld_stub_binding_helper\ndyld_stub_binding_helper:\n ud2\n")
    variants = (("10.5", 0), ("10.6", 0),
                ("10.5", 0x90000000), ("10.6", 0x90000000))
    if os.getenv("LP32_CF_BRIDGE_ONLY"):
        variants = (("10.5", 0),)
    for version, address in variants:
        flags = ["xcrun", "clang", "-target", "i386-apple-macos" + version,
                 "-nostdlib", "-fno-builtin", "-L" + str(root), "-lSystem", str(root / "helper.S")]
        if os.getenv("LP32_CF_BRIDGE_ONLY"):
            optimization = os.getenv("LP32_CF_BRIDGE_OPTIMIZATION", "-O0")
            assert optimization in ("-O0", "-O2"), optimization
            flags.append(optimization)
        # Recent Xcode releases removed i386 from their default linker. A
        # private older ld64 can build fixtures without changing xcode-select.
        if os.getenv("LP32_I386_LINKER"):
            flags.append("-fuse-ld=" + os.environ["LP32_I386_LINKER"])
        run("xcrun", "clang", "-target", "i386-apple-macos" + version,
            "-c", str(root / "directory.c"), "-o", str(root / "directory.o"))
        run(*flags, "-dynamiclib", "-Wl,-install_name,@loader_path/libfixture_dep.dylib",
            f"-Wl,-seg1addr,0x{address:x}",
            str(root / "dep.c"), "-o", str(root / "bin/libfixture_dep.dylib"))
        dependency = "fixture_dep"
        if version == "10.6":
            (root / "exports.txt").write_text("_dependency_value\n")
            run(*flags, "-target", "i386-apple-macos10.7", "-dynamiclib", "-Wl,-install_name,@loader_path/libfixture_reexport.dylib",
                "-Wl,-reexported_symbols_list," + str(root / "exports.txt"),
                "-L" + str(root / "bin"), "-lfixture_dep", "-o", str(root / "bin/libfixture_reexport.dylib"))
            dependency = "fixture_reexport"
        run(*flags, "-dynamiclib", "-Wl,-install_name,@loader_path/launcher.dylib",
            str(root / "launcher.c"), str(root / "many_imports.c"), str(NATIVE / "tests/fixtures/font.c"), str(NATIVE / "tests/fixtures/cf_bridge.c"), str(NATIVE / "tests/fixtures/context.c"), str(NATIVE / "tests/fixtures/mach_ipc.c"), str(NATIVE / "tests/fixtures/steam_guest.c"), str(NATIVE / "tests/fixtures/wide_scan.c"), str(NATIVE / "tests/fixtures/audio_threads.c"), str(NATIVE / "tests/fixtures/audio_converter.c"), str(NATIVE / "tests/fixtures/quicktime.c"), str(NATIVE / "tests/fixtures/strerror.c"), str(NATIVE / "tests/fixtures/bsearch.c"), str(NATIVE / "tests/fixtures/libc_io_float.c"), str(NATIVE / "tests/fixtures/dispatch_strings.c"), str(NATIVE / "tests/fixtures/audio_unit.c"), str(NATIVE / "tests/fixtures/sound_manager.c"), str(NATIVE / "tests/fixtures/time_manager.c"), str(root / "directory.o"), "-L" + str(root / "bin"), "-l" + dependency,
            "-o", str(root / "bin/launcher.dylib"))
        image = root / TEST_EXECUTABLE
        run(*flags, "-Wl,-e,_start,-no_pie", str(root / "start.S"), "-o", str(image))
        # Exercise universal i386 selection on a real generated dylib.
        dep = root / "bin/libfixture_dep.dylib"
        thin = dep.read_bytes()
        cursor = 28
        commands = []
        for _ in range(struct.unpack_from("<I", thin, 16)[0]):
            command, size = struct.unpack_from("<II", thin, cursor)
            commands.append(command)
            cursor += size
        compressed = 0x22 in commands or 0x80000022 in commands
        assert compressed == (version == "10.6"), commands
        fat = struct.pack(">7I", 0xCAFEBABE, 1, 7, 3, 4096, len(thin), 12)
        dep.write_bytes(fat.ljust(4096, b"\0") + thin)
        # Force failure after read_module allocates its file, before any execution.
        malformed = bytearray(thin)
        struct.pack_into("<I", malformed, 20, 0xFFFFFFFF)
        (root / "bin/broken.dylib").write_bytes(malformed)
        environment = dict(os.environ, LP32_GAME=TEST_GAME, LP32_MUTE_AUDIO="1", LP32_DYLD_SELFTEST="1",
                           LP32_DYLD_FIXTURE_SELFTEST="1", LP32_LOG_DIR=str(root / "logs"),
                           LP32_STEAM_FIXTURE=str(root / "steamclient.dylib"),
                           LP32_FIXTURE_EXPECTED_ROOT=str(root), LP32_FIXTURE_EXPECTED_EXECUTABLE=TEST_EXECUTABLE)
        for index, error in enumerate((65, 2, 13, 0, -1, -2147483648, 2147483647, 123456789)):
            environment[f"LP32_STRERROR_{index}"] = os.strerror(error)
        if TEST_GAME == "l4d2":
            environment["LP32_FIXTURE_QUICKTIME"] = "1"
        if TEST_GAME == "portal2" and version == "10.5" and address == 0 and not os.getenv("LP32_CF_BRIDGE_ONLY"):
            logs = root / "logs"
            logs.mkdir()
            (logs / "last-run.log").write_text("legacy log retained\n")
            (logs / "user-note.log").write_text("keep this unrelated file\n")
            command = ["arch", "-x86_64", str(LOADER), str(image)]
            log_env = dict(environment, LP32_LOG_SELFTEST="1")
            # Finder discards both streams; the session must capture both.
            run(*command, env=log_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            assert (logs / "previous-run.log").read_text() == "legacy log retained\n"
            latest = (logs / "last-run.log").read_text()
            assert "diagnostic fixture stdout" in latest and "diagnostic fixture stderr" in latest
            assert "loader-uuid=" in latest and "build=" in latest
            # A second launch must not truncate the log an existing process owns.
            held = subprocess.Popen(command, env=dict(log_env, LP32_LOG_SELFTEST="wait"),
                stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                deadline = time.monotonic() + 10
                while True:
                    held_logs = list(logs.glob(f"run-*-{held.pid}.log"))
                    if held_logs and "diagnostic fixture stdout" in held_logs[0].read_text(): break
                    assert time.monotonic() < deadline, "log self-test did not start"
                    time.sleep(0.01)
                first_text = held_logs[0].read_text()
                # Exercise retention while the first process is still alive.
                for _ in range(12):
                    run(*command, env=log_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                assert held_logs[0].read_text() == first_text
                held.communicate(b"x", timeout=10)
                assert held.returncode == 0 and "diagnostic fixture done" in held_logs[0].read_text()
            finally:
                if held.poll() is None:
                    held.kill(); held.wait()
            # Terminal output remains attached to the caller's pipes.
            terminal = run(*command, env=log_env, capture_output=True, text=True)
            assert "diagnostic fixture stdout" in terminal.stdout and "diagnostic fixture stderr" in terminal.stderr
            assert len(list(logs.glob("run-*.log"))) == 10
            assert (logs / "user-note.log").read_text() == "keep this unrelated file\n"
            print("Persistent logs: PASS (Finder stdout/stderr, concurrent runs, retention, Terminal pipes)")
            steam_env = dict(environment, LP32_FIXTURE_STEAM_REQUIRED="1", LP32_HEADLESS_ERRORS="1")
            for finder in (False, True):
                failed = subprocess.run(command, env=steam_env, text=True, timeout=30,
                    stdout=subprocess.DEVNULL if finder else subprocess.PIPE,
                    stderr=subprocess.DEVNULL if finder else subprocess.PIPE)
                assert failed.returncode == 1, (failed.returncode, failed.stderr)
                output = (logs / "last-run.log").read_text() if finder else failed.stderr
                for expected in ("Steam startup failed; showing instructions before exiting",
                                 "Portal 2 couldn’t connect to Steam",
                                 "Open Steam and sign in", "then launch the game again"):
                    assert expected in output, output
                assert "compat32: signal " not in output, output
            print("Steam startup error: PASS (visible-error helper, clean exit before ud2, Finder and Terminal logs)")
            traced = run(*command, env=dict(environment, LP32_PHYSICS_TRACE_SELFTEST="1"),
                         capture_output=True, text=True)
            assert "Physics history self-test: PASS" in traced.stderr, traced.stderr
            assert "Physics lifetime regression: PASS" in traced.stderr, traced.stderr
            for kind in (5, 6, 7, 9, 10, 11, 13, 14, 15, 16):
                assert f"kind=0x{kind:016x}" in traced.stderr, (kind, traced.stderr)
            baseline = subprocess.run(command, env=dict(environment, LP32_PHYSICS_TRACE_SELFTEST="unfixed"),
                                      capture_output=True, text=True, timeout=30)
            assert baseline.returncode == -signal.SIGSEGV, (baseline.returncode, baseline.stderr)
            assert "rip=0x0000000000000000" in baseline.stderr, baseline.stderr
            print("Physics lifetime: PASS (unfixed fixture reproduces null-PC crash; fixed path preserves live updates)")
            for fixture in ("BAD_STACK", "BAD_STACK_WORKER", "LIFETIME"):
                crashed = subprocess.run(command, env=dict(environment, **{"LP32_FIXTURE_"+fixture: "1"}),
                                         capture_output=True, text=True, timeout=30)
                assert crashed.returncode == -signal.SIGILL, (fixture, crashed.returncode, crashed.stderr)
                report = (logs / "last-run.log").read_text()
                assert "capture begin version=1" in report and "capture end" in report, report
                assert "capture image base=" in report, report
                if fixture.startswith("BAD_STACK"):
                    assert "sp=0x0000000000000001" in report and "unreadable" in report, report
                else:
                    assert "kind=0x0000000000000001" in report, report
                    assert "kind=0x0000000000000002" in report, report
                    assert "detail=0x000000001234abcd" in report, report
                    assert "capture memory object" in report and "capture frame address=" in report, report
                    analyzed = run("python3", str(NATIVE / "tools/analyze_crash.py"), str(logs / "last-run.log"),
                                   capture_output=True, text=True)
                    assert "Capture: complete" in analyzed.stdout and "LauncherMain" in analyzed.stdout, analyzed.stdout
            print("Crash capture: PASS (broken main/worker guest stacks, freed object history, UUID-verified symbols, complete report)")
            for mode in ("host", "guest"):
                for fatal_signal in (signal.SIGILL, signal.SIGSEGV, signal.SIGBUS, signal.SIGABRT):
                    crash_env = dict(environment)
                    crash_env["LP32_CRASH_DIAGNOSTIC_SELFTEST" if mode == "host" else
                              "LP32_FIXTURE_" + fatal_signal.name] = fatal_signal.name
                    crashed = subprocess.run(["arch", "-x86_64", str(LOADER), str(image)],
                        env=crash_env, capture_output=True, text=True, timeout=30)
                    assert crashed.returncode == -fatal_signal, (mode, fatal_signal, crashed.returncode, crashed.stderr)
                    assert f"compat32: signal {int(fatal_signal)} " in crashed.stderr and "crash rax=" in crashed.stderr, crashed.stderr
                    if mode == "guest" and fatal_signal == signal.SIGILL:
                        assert "##### Sys_Error: diagnostic fixture fatal reason" in (logs / "last-run.log").read_text()
                    if mode == "host" or (mode == "guest" and fatal_signal not in (signal.SIGBUS, signal.SIGABRT)):
                        assert "active import " not in crashed.stderr, crashed.stderr
            print("Fatal-signal diagnostics: PASS (SIGILL/SIGSEGV/SIGBUS/SIGABRT, host and i386 guest; default signal termination preserved)")
            crashed = subprocess.run(["arch", "-x86_64", str(LOADER), str(image)],
                env=dict(environment, LP32_FIXTURE_IMPORT_CRASH="1"),
                capture_output=True, text=True, timeout=30)
            assert crashed.returncode == -signal.SIGSEGV, crashed.stderr
            for expected in ("active import _strncpy caller=0x", "import argument[1]=0xe3f1fe67",
                             "import argument[2]=0x00000104"):
                assert expected in crashed.stderr, crashed.stderr
                assert expected in (logs / "last-run.log").read_text()
            print("Import crash context: PASS (strncpy caller and original i386 arguments)")
            crashed = subprocess.run(["arch", "-x86_64", str(LOADER), str(image)],
                env=dict(environment, LP32_FIXTURE_NESTED_IMPORT_CRASH="1"),
                capture_output=True, text=True, timeout=30)
            assert crashed.returncode == -signal.SIGSEGV, crashed.stderr
            assert "active import _lp32_steam_method_34 caller=0x" in crashed.stderr, crashed.stderr
            assert "active import _getenv " not in crashed.stderr, crashed.stderr
            print("Nested import crash context: PASS (enclosing Steam call restored after callback)")
        run("arch", "-x86_64", str(LOADER), str(image), env=environment)
        # Re-run the same real i386 code under the newer Mac depot layout.
        # The launcher's explicit bin/ paths must find libraries in osx32 too.
        binaries = list((root / "bin").iterdir())
        osx32 = root / "bin/osx32"
        osx32.mkdir()
        for binary in binaries:
            binary.rename(osx32 / binary.name)
        try:
            run("arch", "-x86_64", str(LOADER), str(image), env=environment)
        finally:
            for binary in osx32.iterdir():
                binary.rename(root / "bin" / binary.name)
            osx32.rmdir()
        # The bundled image is beside its selected game tree. A symlink keeps
        # the generated data identical across these layout checks.
        support = root / "fixture.app/Contents/SharedSupport"
        support.mkdir(parents=True, exist_ok=True)
        data = support / TEST_DATA_DIRECTORY
        if not data.exists():
            data.symlink_to(root, target_is_directory=True)
        bundled_image = support / ("Portal2.image" if TEST_GAME == "portal2" else "L4D2.image")
        bundled_image.write_bytes(image.read_bytes())
        run("arch", "-x86_64", str(LOADER), str(bundled_image), env=environment)
        print(f"Guest dylib fixtures: PASS ({TEST_GAME}, macOS {version}, preferred address 0x{address:x}; bin, osx32, bundle)")
