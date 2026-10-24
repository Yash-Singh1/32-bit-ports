#include "crypto_bridge.h"
#include "compat_runtime.h"
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <Security/SecRandom.h>
#include <Security/SecBase.h>

/* OpenSSL 0.9.8 exposed these structures to i386 callers. Keep guest limbs
 * 32-bit and rebuild native opaque keys for verification with system LibreSSL.
 * ABI reference: openssl/OpenSSL_0_9_8zh/crypto/{bn/bn,rsa/rsa}.h. */
struct bn32 {
  uint32_t limbs;
  int32_t top, capacity, negative, flags;
};
struct rsa32 {
  uint32_t pad, version, method, engine, numbers[8], extra[2], references,
      flags, caches[6];
};
static uint32_t allocate_callback, reallocate_callback, free_callback;
static int allocation_started;
static void *library;
static void *crypto_symbol(const char *name) {
  if (!library)
    library = dlopen("/usr/lib/libcrypto.46.dylib", RTLD_NOW | RTLD_LOCAL);
  return library ? dlsym(library, name) : NULL;
}
static uint32_t allocate32(uint32_t size) {
  allocation_started = 1;
  uint32_t p = allocate_callback
                   ? compat_runtime32_call(allocate_callback, &size, 1)
                   : compat_runtime32_allocate(size, 0);
  if (p)
    memset((void *)(uintptr_t)p, 0, size);
  return p;
}
static void free32(uint32_t p) {
  if (!p)
    return;
  if (free_callback)
    compat_runtime32_call(free_callback, &p, 1);
  else
    compat_runtime32_deallocate(p);
}

/* Native OpenSSL objects have 64-bit addresses and a different ABI. Keep
 * them behind allocated guest handles, never truncate native pointers or
 * expose a modern EVP_PKEY as the public OpenSSL 0.9.7 structure. */
enum native_crypto_kind { CRYPTO_BIO, CRYPTO_BIO_METHOD, CRYPTO_RSA, CRYPTO_PKEY };
static struct { uint32_t token; enum native_crypto_kind kind; void *object; }
    native_crypto[1024];
static pthread_mutex_t native_crypto_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t wrap_crypto(void *object, enum native_crypto_kind kind) {
  if (!object) return 0;
  uint32_t token = allocate32(4);
  if (!token) return 0;
  pthread_mutex_lock(&native_crypto_lock);
  for (unsigned i = 0; i < 1024; ++i) if (!native_crypto[i].token) {
    native_crypto[i].token = token; native_crypto[i].kind = kind;
    native_crypto[i].object = object;
    pthread_mutex_unlock(&native_crypto_lock);
    return token;
  }
  pthread_mutex_unlock(&native_crypto_lock);
  free32(token);
  return 0;
}
static void *unwrap_crypto(uint32_t token, enum native_crypto_kind kind, int take) {
  void *object = NULL;
  pthread_mutex_lock(&native_crypto_lock);
  for (unsigned i = 0; token && i < 1024; ++i)
    if (native_crypto[i].token == token && native_crypto[i].kind == kind) {
      object = native_crypto[i].object;
      if (take) memset(&native_crypto[i], 0, sizeof(native_crypto[i]));
      break;
    }
  pthread_mutex_unlock(&native_crypto_lock);
  if (object && take) free32(token);
  return object;
}
static uint32_t wrap_or_free(void *object, enum native_crypto_kind kind, const char *destructor) {
  uint32_t token = wrap_crypto(object, kind);
  if (object && !token) {
    if (kind == CRYPTO_BIO) {
      int (*release)(void *) = crypto_symbol(destructor);
      if (release) release(object);
    } else {
      void (*release)(void *) = crypto_symbol(destructor);
      if (release) release(object);
    }
  }
  return token;
}
static int legacy_evp_dispatch(const char *name, const uint32_t *a, uint64_t *out) {
  if (!strcmp(name, "_OPENSSL_add_all_algorithms_noconf") ||
      !strcmp(name, "_OpenSSL_add_all_digests") || !strcmp(name, "_EVP_cleanup") ||
      !strcmp(name, "_ERR_load_crypto_strings") || !strcmp(name, "_ERR_load_X509_strings")) {
    void (*initialize)(void) = crypto_symbol(name + 1);
    if (!initialize) return 0;
    initialize(); *out = 0; return 1;
  }
  if (!strcmp(name, "_BIO_new_mem_buf")) {
    void *(*create)(const void *, int) = crypto_symbol("BIO_new_mem_buf");
    if (!create) return 0;
    *out = wrap_or_free(create((const void *)(uintptr_t)a[0], (int32_t)a[1]), CRYPTO_BIO, "BIO_free");
    return 1;
  }
  if (!strcmp(name, "_BIO_free")) {
    int (*release)(void *) = crypto_symbol("BIO_free");
    if (!release) return 0;
    *out = (uint32_t)release(unwrap_crypto(a[0], CRYPTO_BIO, 1)); return 1;
  }
  if (!strcmp(name, "_BIO_s_mem")) {
    const void *(*method)(void) = crypto_symbol("BIO_s_mem");
    if (!method) return 0;
    /* Intern this immutable singleton, not a new handle for every call. */
    static uint32_t token;
    if (!token) token = wrap_crypto((void *)method(), CRYPTO_BIO_METHOD);
    *out = token; return 1;
  }
  if (!strcmp(name, "_BIO_new")) {
    void *(*create)(const void *) = crypto_symbol("BIO_new");
    if (!create) return 0;
    const void *method = unwrap_crypto(a[0], CRYPTO_BIO_METHOD, 0);
    *out = method ? wrap_or_free(create(method), CRYPTO_BIO, "BIO_free") : 0; return 1;
  }
  if (!strcmp(name, "_PEM_read_bio_RSA_PUBKEY")) {
    void *(*read_key)(void *, void **, void *, void *) = crypto_symbol("PEM_read_bio_RSA_PUBKEY");
    if (!read_key) return 0;
    /* The COD4 public key is unencrypted and supplies no password callback.
       Guest callbacks cannot be passed as native function pointers. */
    void *bio = unwrap_crypto(a[0], CRYPTO_BIO, 0);
    *out = 0;
    if (!bio || a[2]) return 1;
    uint32_t previous = a[1] ? *(uint32_t *)(uintptr_t)a[1] : 0;
    void *key = previous ? unwrap_crypto(previous, CRYPTO_RSA, 0) : NULL;
    if (previous && !key) return 1;
    void *value = read_key(bio, &key, NULL, (void *)(uintptr_t)a[3]);
    if (value) {
      if (previous) {
        pthread_mutex_lock(&native_crypto_lock);
        for (unsigned i = 0; i < 1024; ++i)
          if (native_crypto[i].token == previous && native_crypto[i].kind == CRYPTO_RSA) {
            native_crypto[i].object = value; break;
          }
        pthread_mutex_unlock(&native_crypto_lock);
      }
      *out = previous ? previous : wrap_or_free(value, CRYPTO_RSA, "RSA_free");
      if (a[1]) *(uint32_t *)(uintptr_t)a[1] = (uint32_t)*out;
    }
    return 1;
  }
  if (!strcmp(name, "_EVP_PKEY_new")) {
    void *(*create)(void) = crypto_symbol("EVP_PKEY_new");
    if (!create) return 0;
    *out = wrap_or_free(create(), CRYPTO_PKEY, "EVP_PKEY_free"); return 1;
  }
  if (!strcmp(name, "_EVP_PKEY_assign")) {
    int (*assign)(void *, int, void *) = crypto_symbol("EVP_PKEY_assign");
    if (!assign) return 0;
    void *key = unwrap_crypto(a[0], CRYPTO_PKEY, 0);
    void *rsa = unwrap_crypto(a[2], CRYPTO_RSA, 0);
    *out = key && rsa && a[1] == 6 ? (uint32_t)assign(key, 6, rsa) : 0;
    /* Success transfers the RSA to EVP_PKEY, including responsibility to free. */
    if (*out) unwrap_crypto(a[2], CRYPTO_RSA, 1);
    return 1;
  }
  if (!strcmp(name, "_EVP_PKEY_encrypt")) {
    /* 0.9.7 takes (output, input, length, key). Modern EVP_PKEY_encrypt
       takes a context and size_t*, so calling that symbol corrupts memory. */
    int (*encrypt_old)(unsigned char *, const unsigned char *, int, void *) =
        crypto_symbol("EVP_PKEY_encrypt_old");
    if (!encrypt_old) return 0;
    void *key = unwrap_crypto(a[3], CRYPTO_PKEY, 0);
    *out = key && (int32_t)a[2] >= 0 ? (uint32_t)encrypt_old(
        (void *)(uintptr_t)a[0], (const void *)(uintptr_t)a[1], (int32_t)a[2], key) : 0;
    return 1;
  }
  if (!strcmp(name, "_EVP_PKEY_free")) {
    void (*release)(void *) = crypto_symbol("EVP_PKEY_free");
    if (!release) return 0;
    release(unwrap_crypto(a[0], CRYPTO_PKEY, 1)); *out = 0; return 1;
  }
  return 0;
}
static void bn_free32(uint32_t p) {
  if (!p)
    return;
  struct bn32 *bn = (void *)(uintptr_t)p;
  free32(bn->limbs);
  if (bn->flags & 1)
    free32(p);
}
static void *native_bn(uint32_t p) {
  if (!p)
    return NULL;
  struct bn32 *bn = (void *)(uintptr_t)p;
  if (bn->top < 0 || bn->top > 16384 || bn->negative || (bn->top && !bn->limbs))
    return NULL;
  size_t length = (size_t)bn->top * 4;
  unsigned char *bytes = malloc(length ? length : 1);
  if (!bytes)
    return NULL;
  const unsigned char *limbs = (void *)(uintptr_t)bn->limbs;
  for (size_t i = 0; i < length; ++i)
    bytes[i] = limbs[length - 1 - i];
  void *(*convert)(const unsigned char *, int, void *) =
      crypto_symbol("BN_bin2bn");
  void *result = convert ? convert(bytes, (int)length, NULL) : NULL;
  free(bytes);
  return result;
}
int crypto_bridge32_dispatch(const char *name, const uint32_t *a,
                             uint64_t *out) {
  if (legacy_evp_dispatch(name, a, out)) return 1;
  if (strcmp(name, "_SecRandomCopyBytes") == 0) {
    /* kSecRandomDefault is NULL; Apple exposes no other generator handles. */
    *out = (uint32_t)(a[0] ? errSecParam :
        SecRandomCopyBytes(kSecRandomDefault, a[1], (void *)(uintptr_t)a[2]));
    return 1;
  }
  if (strcmp(name, "_CRYPTO_set_mem_functions") == 0) {
    *out = !allocation_started && a[0] && a[1] && a[2];
    if (*out) {
      allocate_callback = a[0];
      reallocate_callback = a[1];
      free_callback = a[2];
    }
    return 1;
  }
  if (strcmp(name, "_BN_bin2bn") == 0) {
    *out = 0;
    if ((int32_t)a[1] < 0 || a[1] > 65536 || (a[1] && !a[0]))
      return 1;
    uint32_t p = a[2] ? a[2] : allocate32(sizeof(struct bn32));
    if (!p)
      return 1;
    struct bn32 *bn = (void *)(uintptr_t)p;
    if (!a[2])
      bn->flags = 1;
    uint32_t count = (a[1] + 3) / 4;
    if (count > (uint32_t)bn->capacity) {
      uint32_t limbs = allocate32(count * 4);
      if (!limbs) {
        if (!a[2])
          free32(p);
        return 1;
      }
      free32(bn->limbs);
      bn->limbs = limbs;
      bn->capacity = (int32_t)count;
    }
    unsigned char *limbs = (void *)(uintptr_t)bn->limbs;
    const unsigned char *bytes = (void *)(uintptr_t)a[0];
    if (count)
      memset(limbs, 0, count * 4);
    for (uint32_t i = 0; i < a[1]; ++i)
      limbs[i] = bytes[a[1] - 1 - i];
    bn->top = (int32_t)count;
    bn->negative = 0;
    while (bn->top && !((uint32_t *)limbs)[bn->top - 1])
      --bn->top;
    *out = p;
    return 1;
  }
  if (strcmp(name, "_RSA_new") == 0) {
    *out = allocate32(sizeof(struct rsa32));
    if (*out)
      ((struct rsa32 *)(uintptr_t)*out)->references = 1;
    return 1;
  }
  if (strcmp(name, "_RSA_free") == 0) {
    void *native = unwrap_crypto(a[0], CRYPTO_RSA, 1);
    if (native) {
      void (*release)(void *) = crypto_symbol("RSA_free");
      if (release) release(native);
      *out = 0; return 1;
    }
    struct rsa32 *rsa = (void *)(uintptr_t)a[0];
    if (rsa && --rsa->references == 0) {
      for (unsigned i = 0; i < 8; ++i)
        bn_free32(rsa->numbers[i]);
      free32(a[0]);
    }
    *out = 0;
    return 1;
  }
  if (strcmp(name, "_RSA_verify") == 0) {
    void *(*new_key)(void) = crypto_symbol("RSA_new");
    int (*set_key)(void *, void *, void *, void *) =
        crypto_symbol("RSA_set0_key");
    int (*verify)(int, const void *, unsigned, const void *, unsigned, void *) =
        crypto_symbol("RSA_verify");
    void (*free_key)(void *) = crypto_symbol("RSA_free");
    void (*free_bn)(void *) = crypto_symbol("BN_free");
    if (!new_key || !set_key || !verify || !free_key || !free_bn)
      return 0;
    *out = 0;
    struct rsa32 *guest = (void *)(uintptr_t)a[5];
    if (!guest)
      return 1;
    void *key = new_key(), *n = native_bn(guest->numbers[0]),
         *e = native_bn(guest->numbers[1]);
    if (key && n && e && set_key(key, n, e, NULL)) {
      *out = verify((int)a[0], (void *)(uintptr_t)a[1], a[2],
                    (void *)(uintptr_t)a[3], a[4], key);
    } else {
      free_bn(n);
      free_bn(e);
    }
    free_key(key);
    return 1;
  }
  return 0;
}
