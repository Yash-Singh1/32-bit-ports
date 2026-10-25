#include "../src/cfnetwork_bridge.h"
#include <CFNetwork/CFNetwork.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>

static void *serve_challenge(void *raw) {
  int listener = *(int *)raw;
  int peer = accept(listener, NULL, NULL);
  assert(peer >= 0);
  char buffer[4096];
  size_t length = 0;
  do {
    ssize_t n = read(peer, buffer + length, sizeof(buffer) - length - 1);
    assert(n > 0);
    length += n; buffer[length] = 0;
  } while (!strstr(buffer, "\r\n\r\n") && length + 1 < sizeof(buffer));
  const char challenge[] = "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"compat-test\"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  assert(write(peer, challenge, sizeof(challenge) - 1) == sizeof(challenge) - 1);
  close(peer);
  return NULL;
}
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static CFHTTPMessageRef authentication_response(void) {
  int listener = socket(AF_INET, SOCK_STREAM, 0);
  assert(listener >= 0);
  struct sockaddr_in address = {.sin_len = sizeof(address), .sin_family = AF_INET,
                                .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  assert(bind(listener, (const void *)&address, sizeof(address)) == 0);
  socklen_t size = sizeof(address);
  assert(getsockname(listener, (void *)&address, &size) == 0);
  assert(listen(listener, 1) == 0);
  pthread_t thread;
  assert(pthread_create(&thread, NULL, serve_challenge, &listener) == 0);
  char text[128];
  snprintf(text, sizeof(text), "http://127.0.0.1:%u/", ntohs(address.sin_port));
  CFURLRef url = CFURLCreateWithBytes(NULL, (const UInt8 *)text, strlen(text), kCFStringEncodingUTF8, NULL);
  CFHTTPMessageRef request = CFHTTPMessageCreateRequest(NULL, CFSTR("GET"), url, kCFHTTPVersion1_1);
  CFReadStreamRef stream = CFReadStreamCreateForHTTPRequest(NULL, request);
  assert(CFReadStreamOpen(stream));
  UInt8 byte;
  assert(CFReadStreamRead(stream, &byte, 1) == 0);
  CFHTTPMessageRef response = (CFHTTPMessageRef)CFReadStreamCopyProperty(stream, kCFStreamPropertyHTTPResponseHeader);
  assert(response && CFHTTPMessageGetResponseStatusCode(response) == 401);
  CFReadStreamClose(stream); CFRelease(stream); CFRelease(request); CFRelease(url);
  pthread_join(thread, NULL); close(listener);
  return response;
}
#pragma clang diagnostic pop

uint64_t compat_runtime32_return_double(double x){uint64_t result;memcpy(&result,&x,8);return result;}
static CFTypeRef objects[128];
static unsigned count, retained, released, timer_fired, stream_fired;
static unsigned source_scheduled, source_cancelled, source_performed, source_hashed, source_compared;
uint32_t objc_bridge32_guest_object(void *object) {
  if (!object)
    return 0;
  for (unsigned i = 1; i <= count; ++i)
    if (objects[i] == object)
      return i;
  assert(count + 1 < 128);
  objects[++count] = CFRetain(object);
  return count;
}
void *objc_bridge32_host_object(uint32_t token) {
  assert(token <= count);
  return (void *)objects[token];
}
uint32_t compat_runtime32_call(uint32_t callback, const uint32_t *a,
                               uint32_t n) {
  if (callback == 1) {
    assert(n == 1 && (a[0] == 123 || a[0] == 456));
    retained++;
    return 456;
  } else if (callback == 2) {
    assert(n == 1 && a[0] == 456);
    released++;
  } else if (callback == 3) {
    assert(n == 2 && a[0] && a[1] == 456);
    timer_fired++;
  } else if (callback == 4) {
    assert(n == 3 && a[0] && a[2] == 456);
    if (a[1] == kCFStreamEventHasBytesAvailable)
      stream_fired++;
  } else if (callback == 5 || callback == 6) {
    assert(n == 3 && a[0] == 456);
    assert(objc_bridge32_host_object(a[1]) == CFRunLoopGetCurrent());
    assert(CFEqual(objc_bridge32_host_object(a[2]), kCFRunLoopDefaultMode));
    if (callback == 5) source_scheduled++; else source_cancelled++;
  } else if (callback == 7) {
    assert(n == 1 && a[0] == 456); source_performed++;
  } else if (callback == 8) {
    assert(n == 1 && a[0] == 456); source_hashed++; return 42;
  } else if (callback == 9) {
    assert(n == 2 && a[0] == 456 && a[1] == 456); source_compared++; return 1;
  } else
    assert(!"unknown callback");
  return 0;
}
static uint32_t token(CFTypeRef value) {
  return objc_bridge32_guest_object((void *)value);
}
static uint32_t dispatch(const char *name, uint32_t *a) {
  uint64_t result = 0;
  assert(cfnetwork_bridge32_dispatch(name, a, &result));
  return result;
}
int main(void) {
  uint32_t *low = mmap((void *)0x30000000, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
  assert(low != MAP_FAILED);
  uint32_t *ctx = low;
  ctx[0] = 0;
  ctx[1] = 123;
  ctx[2] = 1;
  ctx[3] = 2;
  ctx[4] = 0;
  uint32_t a[12] = {0};
  CFURLRef url =
      CFURLCreateWithString(NULL, CFSTR("http://127.0.0.1/compat-test"), NULL);
  a[1] = token(CFSTR("POST"));
  a[2] = token(url);
  a[3] = cfnetwork_bridge32_pointer_import("_kCFHTTPVersion1_1");
  assert(a[3]);
  uint32_t request = dispatch("_CFHTTPMessageCreateRequest", a);
  assert(request);
  const UInt8 body[] = {0, 'a', 0xff};
  CFDataRef data = CFDataCreate(NULL, body, sizeof(body));
  a[0] = request;
  a[1] = token(data);
  dispatch("_CFHTTPMessageSetBody", a);
  uint32_t body_copy = dispatch("_CFHTTPMessageCopyBody", a);
  assert(CFEqual(objc_bridge32_host_object(body_copy), data));
  assert(cfnetwork_bridge32_pointer_import(
      "_kCFStreamPropertyHTTPResponseHeader"));
  assert(cfnetwork_bridge32_pointer_import("_kCFProxyHostNameKey"));
  assert(!cfnetwork_bridge32_pointer_import("_kCFNotARealConstant"));
  a[0] = 0;
  a[1] = 0;
  uint32_t response = dispatch("_CFHTTPMessageCreateEmpty", a);
  const char *wire = "HTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok";
  memcpy(low + 32, wire, strlen(wire));
  a[0] = response;
  a[1] = (uint32_t)(uintptr_t)(low + 32);
  a[2] = strlen(wire);
  assert(dispatch("_CFHTTPMessageAppendBytes", a));
  assert(dispatch("_CFHTTPMessageGetResponseStatusCode", a) == 201);
  assert(dispatch("_CFHTTPMessageIsHeaderComplete", a));
  uint32_t text = dispatch("_CFHTTPMessageCopyVersion", a);
  assert(text); assert(CFEqual(objc_bridge32_host_object(text), kCFHTTPVersion1_1));
  text = dispatch("_CFHTTPMessageCopyResponseStatusLine", a);
  assert(CFStringFind(objc_bridge32_host_object(text), CFSTR("201 Created"), 0).location != kCFNotFound);
  assert(dispatch("_CFMakeCollectable", a) == response);

  /* A real loopback response supplies CFNetwork's origin metadata. A parsed
     standalone response cannot be used for a valid authentication challenge. */
  CFHTTPMessageRef auth_response = authentication_response();
  a[0] = 0; a[1] = token(auth_response);
  uint32_t auth = dispatch("_CFHTTPAuthenticationCreateFromResponse", a);
  assert(auth);
  a[0] = auth;
  text = dispatch("_CFHTTPAuthenticationCopyMethod", a);
  assert(text); assert(CFEqual(objc_bridge32_host_object(text), kCFHTTPAuthenticationSchemeBasic));
  text = dispatch("_CFHTTPAuthenticationCopyRealm", a);
  assert(text); assert(CFEqual(objc_bridge32_host_object(text), CFSTR("compat-test")));
  assert(dispatch("_CFHTTPAuthenticationRequiresUserNameAndPassword", a));
  assert(!dispatch("_CFHTTPAuthenticationRequiresAccountDomain", a));
  low[80] = 0x12345678; low[81] = low[82] = 0xffffffff; low[83] = 0x87654321;
  a[1] = (uint32_t)(uintptr_t)(low + 81);
  assert(dispatch("_CFHTTPAuthenticationIsValid", a));
  assert(low[80] == 0x12345678 && low[81] == 0 && low[82] == 0 && low[83] == 0x87654321);
  const void *keys[] = {kCFHTTPAuthenticationUsername, kCFHTTPAuthenticationPassword};
  const void *values[] = {CFSTR("compat-user"), CFSTR("fixture-password")};
  CFDictionaryRef credentials = CFDictionaryCreate(NULL, keys, values, 2,
      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  a[0] = request; a[1] = auth; a[2] = token(credentials); a[3] = (uint32_t)(uintptr_t)(low + 81);
  assert(dispatch("_CFHTTPMessageApplyCredentialDictionary", a));
  assert(low[80] == 0x12345678 && low[81] == 0 && low[82] == 0 && low[83] == 0x87654321);
  CFStringRef authorization = CFHTTPMessageCopyHeaderFieldValue(objc_bridge32_host_object(request), CFSTR("Authorization"));
  assert(authorization && CFStringHasPrefix(authorization, CFSTR("Basic ")));
  CFRelease(authorization); CFRelease(credentials); CFRelease(auth_response);
  const char *unsupported_challenge = "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: CompatUnsupported realm=\"test\"\r\n\r\n";
  CFHTTPMessageRef bad_response = CFHTTPMessageCreateEmpty(NULL, false);
  assert(CFHTTPMessageAppendBytes(bad_response, (const UInt8 *)unsupported_challenge, strlen(unsupported_challenge)));
  a[0] = 0; a[1] = token(bad_response);
  uint32_t bad_auth = dispatch("_CFHTTPAuthenticationCreateFromResponse", a);
  assert(bad_auth);
  a[0] = bad_auth; a[1] = (uint32_t)(uintptr_t)(low + 81);
  assert(!dispatch("_CFHTTPAuthenticationIsValid", a));
  assert(low[80] == 0x12345678 && low[81] && low[82] && low[83] == 0x87654321);
  CFRelease(bad_response);

  a[0] = token(CFSTR("function FindProxyForURL(url, host) { return 'DIRECT'; }"));
  a[1] = token(url); a[2] = (uint32_t)(uintptr_t)(low + 81);
  low[81] = 0xffffffff; low[82] = 0x99887766;
  uint32_t proxies = dispatch("_CFNetworkCopyProxiesForAutoConfigurationScript", a);
  assert(proxies && low[81] == 0 && low[80] == 0x12345678 && low[82] == 0x99887766);
  CFArrayRef proxy_array = objc_bridge32_host_object(proxies);
  assert(CFArrayGetCount(proxy_array) == 1);
  assert(CFEqual(CFDictionaryGetValue(CFArrayGetValueAtIndex(proxy_array, 0), kCFProxyTypeKey), kCFProxyTypeNone));

  uint32_t loop = dispatch("_CFRunLoopGetCurrent", a);
  assert(objc_bridge32_host_object(loop) == CFRunLoopGetCurrent());
  double date = CFAbsoluteTimeGetCurrent(), interval = 0;
  memset(a, 0, sizeof(a));
  memcpy(a + 1, &date, 8);
  memcpy(a + 3, &interval, 8);
  a[7] = 3;
  a[8] = (uint32_t)(uintptr_t)ctx;
  uint32_t timer = dispatch("_CFRunLoopTimerCreate", a);
  assert(timer && retained == 1);
  a[0] = loop;
  a[1] = timer;
  a[2] = token(kCFRunLoopDefaultMode);
  dispatch("_CFRunLoopAddTimer", a);
  CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.03, false);
  assert(timer_fired == 1);
  /* The source context has ten 32-bit words, including guest callbacks.
     Poison the original context after creation to verify that CF owns a copy. */
  uint32_t *source_ctx = low + 8;
  uint32_t source_words[] = {0, 123, 1, 2, 0, 9, 8, 5, 6, 7};
  memcpy(source_ctx, source_words, sizeof(source_words));
  a[0] = 0; a[1] = 0; a[2] = (uint32_t)(uintptr_t)source_ctx;
  uint32_t source = dispatch("_CFRunLoopSourceCreate", a);
  uint32_t same_source = dispatch("_CFRunLoopSourceCreate", a);
  assert(source && same_source && CFEqual(objc_bridge32_host_object(source), objc_bridge32_host_object(same_source)));
  assert(CFHash(objc_bridge32_host_object(source)) == 42 && source_hashed && source_compared);
  source_ctx[5] = source_ctx[6] = 0;
  uint32_t plain_source = dispatch("_CFRunLoopSourceCreate", a);
  uint32_t plain_copy = dispatch("_CFRunLoopSourceCreate", a);
  assert(CFEqual(objc_bridge32_host_object(plain_source), objc_bridge32_host_object(plain_copy)));
  assert(CFHash(objc_bridge32_host_object(plain_source)) == 456);
  assert(!CFEqual(objc_bridge32_host_object(source), objc_bridge32_host_object(plain_source)));
  memset(source_ctx, 0xa5, sizeof(source_words));
  assert(!dispatch("_CFRunLoopSourceCreate", a));
  a[0] = loop; a[1] = source; a[2] = token(kCFRunLoopDefaultMode);
  dispatch("_CFRunLoopAddSource", a);
  assert(source_scheduled == 1 && dispatch("_CFRunLoopContainsSource", a));
  a[0] = source; dispatch("_CFRunLoopSourceSignal", a);
  CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.03, true);
  assert(source_performed == 1);
  dispatch("_CFRunLoopSourceInvalidate", a);
  assert(source_cancelled == 1);
  CFReadStreamRef stream = CFReadStreamCreateWithBytesNoCopy(
      NULL, body, sizeof(body), kCFAllocatorNull);
  uint32_t stream_token = token(stream);
  a[0] = stream_token;
  a[1] = kCFStreamEventHasBytesAvailable;
  a[2] = 4;
  a[3] = (uint32_t)(uintptr_t)ctx;
  assert(dispatch("_CFReadStreamSetClient", a));
  assert(retained >= 2);
  a[1] = loop;
  a[2] = token(kCFRunLoopDefaultMode);
  dispatch("_CFReadStreamScheduleWithRunLoop", a);
  assert(dispatch("_CFReadStreamOpen", a));
  assert(dispatch("_CFReadStreamHasBytesAvailable", a));
  assert(!dispatch("_CFReadStreamCopyError", a));
  CFReadStreamRef upload_body = CFReadStreamCreateWithBytesNoCopy(NULL, body, sizeof(body), kCFAllocatorNull);
  a[0] = 0; a[1] = request; a[2] = token(upload_body);
  assert(dispatch("_CFReadStreamCreateForStreamedHTTPRequest", a));
  CFRelease(upload_body);
  a[0] = stream_token;
  CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.03, false);
  assert(stream_fired);
  a[1] = (uint32_t)(uintptr_t)(low + 64);
  a[2] = 8;
  assert(dispatch("_CFReadStreamRead", a) == sizeof(body));
  assert(!memcmp(low + 64, body, sizeof(body)));
  assert(!dispatch("_CFReadStreamHasBytesAvailable", a));
  a[1] = 0;
  a[2] = 0;
  a[3] = 0;
  assert(dispatch("_CFReadStreamSetClient", a));
  assert(released <= retained);
  dispatch("_CFReadStreamClose", a);
  CFRelease(stream);

  /* MW2 reads its startup plist through file streams. Check 32-bit format
     and error outputs with canaries, then exercise the inverse write path. */
  char file_path[] = "/tmp/lp32-plist-XXXXXX";
  int fd = mkstemp(file_path);
  assert(fd >= 0);
  close(fd);
  CFURLRef file_url = CFURLCreateFromFileSystemRepresentation(NULL,
      (const UInt8 *)file_path, strlen(file_path), false);
  memset(a, 0, sizeof(a));
  a[1] = token(file_url);
  uint32_t output = dispatch("_CFWriteStreamCreateWithFile", a);
  a[0] = output;
  assert(dispatch("_CFWriteStreamOpen", a));
  low[100] = 0xabcddcba; low[101] = 0xffffffff; low[102] = 0x12344321;
  a[0] = token(CFSTR("MW2 plist round trip")); a[1] = output;
  a[2] = kCFPropertyListXMLFormat_v1_0; a[3] = 0;
  a[4] = (uint32_t)(uintptr_t)(low + 101);
  assert((int32_t)dispatch("_CFPropertyListWrite", a) > 0);
  assert(low[100] == 0xabcddcba && low[101] == 0 && low[102] == 0x12344321);
  a[0] = output; dispatch("_CFWriteStreamClose", a);
  a[0] = 0; a[1] = token(file_url);
  uint32_t input = dispatch("_CFReadStreamCreateWithFile", a);
  a[0] = input; assert(dispatch("_CFReadStreamOpen", a));
  low[104] = 0xfeedbeef; low[105] = 0; low[106] = 0xcafebabe;
  a[0] = 0; a[1] = input; a[2] = 0; a[3] = kCFPropertyListImmutable;
  a[4] = (uint32_t)(uintptr_t)(low + 105);
  a[5] = (uint32_t)(uintptr_t)(low + 101);
  uint32_t value = dispatch("_CFPropertyListCreateWithStream", a);
  assert(CFEqual(objc_bridge32_host_object(value), CFSTR("MW2 plist round trip")));
  assert(low[105] == kCFPropertyListXMLFormat_v1_0 && !low[101]);
  assert(low[104] == 0xfeedbeef && low[106] == 0xcafebabe);
  assert(low[100] == 0xabcddcba && low[102] == 0x12344321);
  a[0] = input; dispatch("_CFReadStreamClose", a);
  FILE *bad = fopen(file_path, "w"); assert(bad); fputs("invalid plist", bad); fclose(bad);
  a[0] = 0; a[1] = token(file_url);
  input = dispatch("_CFReadStreamCreateWithFile", a);
  a[0] = input; assert(dispatch("_CFReadStreamOpen", a));
  a[0] = 0; a[1] = input;
  assert(!dispatch("_CFPropertyListCreateWithStream", a));
  assert(low[101] && CFGetTypeID(objc_bridge32_host_object(low[101])) == CFErrorGetTypeID());
  assert(low[100] == 0xabcddcba && low[102] == 0x12344321);
  a[0] = input; dispatch("_CFReadStreamClose", a);
  unlink(file_path);
  a[0] = 0; a[1] = token(file_url);
  uint32_t missing_file = dispatch("_CFReadStreamCreateWithFile", a);
  a[0] = missing_file;
  assert(!dispatch("_CFReadStreamOpen", a));
  uint32_t stream_error = dispatch("_CFReadStreamCopyError", a);
  assert(stream_error && CFErrorGetCode(objc_bridge32_host_object(stream_error)) != 0);
  dispatch("_CFReadStreamClose", a);
  CFRelease(file_url);
  CFRelease(data);
  CFRelease(url);
  for (unsigned i = 1; i <= count; ++i)
    CFRelease(objects[i]);
  assert(released == retained);
  munmap(low, 4096);
  puts("CFNetwork bridge: HTTP data, constants, run loops, timers, stream "
       "callbacks, sources, authentication, PAC, file plists, error canaries and ownership PASS");
}
