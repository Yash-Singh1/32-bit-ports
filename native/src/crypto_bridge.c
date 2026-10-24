#include "crypto_bridge.h"
#include "compat_runtime.h"
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <Security/SecRandom.h>
#include <Security/SecBase.h>
#include <Security/CMSDecoder.h>
#include <Security/SecPolicy.h>
#include <limits.h>
#include <stdio.h>

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
/* Mac App Store receipt validation (Fast Five) reads the receipt into a
 * memory BIO, decodes it with d2i_PKCS7_bio and walks the PKCS7 structures
 * directly: p7->type, p7->d.sign->contents->type and ->d.data.  Those are
 * OpenSSL 0.9.8's i386 layouts (crypto/pkcs7/pkcs7.h, crypto/asn1/asn1.h).
 * Security's CMS decoder reads the message, BER as receipts are encoded. */
struct asn1_object32 {
  uint32_t short_name, long_name;
  int32_t nid, length;
  uint32_t data;
  int32_t flags;
};
struct asn1_string32 {
  int32_t length, type;
  uint32_t data;
  int32_t flags;
};
struct pkcs7_32 {
  uint32_t asn1;
  int32_t length, state, detached;
  uint32_t type, d;
};
struct pkcs7_signed32 {
  uint32_t version, digest_algorithms, certificates, crls, signer_info, contents;
};
enum { NID_UNDEF = 0, NID_PKCS7_DATA = 21, NID_PKCS7_SIGNED = 22 };
/* A BIO's fields are private to OpenSSL; callers only pass the pointer on. */
struct bio32 {
  uint32_t method, data, length, position, capacity;
};
static uint32_t file_method, memory_method;

static uint32_t new_bio(uint32_t method) {
  uint32_t p = allocate32(sizeof(struct bio32));
  if (p)
    ((struct bio32 *)(uintptr_t)p)->method = method;
  return p;
}

static int bio_append(struct bio32 *bio, const void *bytes, uint32_t length) {
  /* BIO_new_mem_buf's BIO reads the caller's buffer and cannot grow. */
  if ((!bio->capacity && bio->length) || length > UINT32_MAX - bio->length)
    return 0;
  if (bio->length + length > bio->capacity) {
    uint32_t capacity = bio->length + length;
    uint32_t data = allocate32(capacity ? capacity : 1);
    if (!data)
      return 0;
    memcpy((void *)(uintptr_t)data, (void *)(uintptr_t)bio->data, bio->length);
    free32(bio->data);
    bio->data = data;
    bio->capacity = capacity;
  }
  memcpy((unsigned char *)(uintptr_t)bio->data + bio->length, bytes, length);
  bio->length += length;
  return 1;
}

static uint32_t new_object(int32_t nid) {
  uint32_t p = allocate32(sizeof(struct asn1_object32));
  if (p)
    ((struct asn1_object32 *)(uintptr_t)p)->nid = nid;
  return p;
}

/* id-signedData and id-data: 1.2.840.113549.1.7.2 and .1. */
static const unsigned char pkcs7_oid[] = {0x2a, 0x86, 0x48, 0x86, 0xf7,
                                          0x0d, 0x01, 0x07};

/* The ContentInfo's type, from the OID after its SEQUENCE header. */
static int32_t content_info_nid(const unsigned char *p, size_t length) {
  if (length < 2 || p[0] != 0x30)
    return NID_UNDEF;
  size_t header = 2;
  if (p[1] & 0x80 && p[1] != 0x80)
    header += p[1] & 0x7f;
  if (length < header + 2 + sizeof pkcs7_oid + 1 || p[header] != 0x06 ||
      p[header + 1] != sizeof pkcs7_oid + 1 ||
      memcmp(p + header + 2, pkcs7_oid, sizeof pkcs7_oid))
    return NID_UNDEF;
  unsigned char last = p[header + 2 + sizeof pkcs7_oid];
  return last == 2 ? NID_PKCS7_SIGNED : last == 1 ? NID_PKCS7_DATA : NID_UNDEF;
}

static CMSDecoderRef decode_cms(const void *bytes, size_t length) {
  CMSDecoderRef decoder = NULL;
  if (CMSDecoderCreate(&decoder) != errSecSuccess)
    return NULL;
  if (CMSDecoderUpdateMessage(decoder, bytes, length) != errSecSuccess ||
      CMSDecoderFinalizeMessage(decoder) != errSecSuccess) {
    CFRelease(decoder);
    return NULL;
  }
  return decoder;
}

static void free_pkcs7(uint32_t p) {
  struct pkcs7_32 *p7 = (void *)(uintptr_t)p;
  if (!p7)
    return;
  struct pkcs7_signed32 *sign = (void *)(uintptr_t)p7->d;
  if (sign) {
    struct pkcs7_32 *contents = (void *)(uintptr_t)sign->contents;
    if (contents) {
      struct asn1_string32 *octets = (void *)(uintptr_t)contents->d;
      if (octets)
        free32(octets->data);
      free32(contents->d);
      free32(contents->type);
      free32(sign->contents);
    }
    free32(p7->d);
  }
  free32(p7->type);
  free32(p7->asn1);
  free32(p);
}

/* OpenSSL 0.9.8zh crypto/asn1/asn1_lib.c ASN1_get_object, with a 32-bit
 * long: 0x80 reports a malformed header, or a length past omax. */
static uint32_t asn1_get_object(uint32_t *pp, int32_t *plength, int32_t *ptag,
                                int32_t *pclass, int32_t omax) {
  const unsigned char *start = (void *)(uintptr_t)*pp, *p = start;
  int32_t max = omax;
  if (max <= 0)
    return 0x80;
  uint32_t constructed = *p & 0x20;
  int32_t xclass = *p & 0xc0, tag = *p & 0x1f;
  if (tag == 0x1f) {
    p++;
    if (--max == 0)
      return 0x80;
    int32_t l = 0;
    while (*p & 0x80) {
      l = (l << 7) | (*p++ & 0x7f);
      if (--max == 0 || l > (INT32_MAX >> 7))
        return 0x80;
    }
    tag = (l << 7) | (*p++ & 0x7f);
    if (--max == 0)
      return 0x80;
  } else {
    p++;
    if (--max == 0)
      return 0x80;
  }
  *ptag = tag;
  *pclass = xclass;
  uint32_t length = 0, indefinite = 0;
  if (max-- < 1)
    return 0x80;
  if (*p == 0x80) {
    indefinite = 1;
    p++;
  } else {
    uint32_t count = *p & 0x7f;
    if (*p++ & 0x80) {
      if (count > sizeof(int32_t) || max < (int32_t)count)
        return 0x80;
      while (count-- > 0)
        length = (length << 8) | *p++;
    } else
      length = count;
  }
  if (length > INT32_MAX)
    return 0x80;
  *plength = (int32_t)length;
  if (indefinite && !constructed)
    return 0x80;
  uint32_t result = constructed | indefinite;
  if (*plength > omax - (int32_t)(p - start))
    result |= 0x80;
  *pp = (uint32_t)(uintptr_t)p;
  return result;
}

static int receipt_dispatch(const char *name, const uint32_t *a,
                            uint64_t *out) {
  /* Every unbound import reaches here; skip the comparisons for others. */
  if (!name[0] || !name[1] || !strchr("ABEOPSXd", name[1]))
    return 0;
  if (strcmp(name, "_ERR_load_PKCS7_strings") == 0 ||
      strcmp(name, "_ERR_load_X509_strings") == 0 ||
      strcmp(name, "_OpenSSL_add_all_digests") == 0) {
    *out = 0;
    return 1;
  }
  /* The keychain search for the Apple root certificate is not bridged.
   * The game handles an unavailable keychain, and its certificate store is
   * only used by PKCS7_verify, whose result it does not check. */
  if (strcmp(name, "_SecKeychainOpen") == 0) {
    if (a[1])
      *(uint32_t *)(uintptr_t)a[1] = 0;
    *out = (uint32_t)errSecNoSuchKeychain;
    return 1;
  }
  if (strcmp(name, "_BIO_s_file") == 0 || strcmp(name, "_BIO_s_mem") == 0) {
    uint32_t *method = name[7] == 'f' ? &file_method : &memory_method;
    if (!*method)
      *method = compat_runtime32_allocate(4, 1);
    *out = *method;
    return 1;
  }
  if (strcmp(name, "_BIO_new") == 0) {
    *out = a[0] ? new_bio(a[0]) : 0;
    return 1;
  }
  if (strcmp(name, "_BIO_new_mem_buf") == 0) {
    int32_t length = (int32_t)a[1];
    if (!memory_method)
      memory_method = compat_runtime32_allocate(4, 1);
    if (!a[0] || length < -1) {
      *out = 0;
      return 1;
    }
    *out = new_bio(memory_method);
    struct bio32 *bio = (void *)(uintptr_t)*out;
    if (bio) {
      /* A read-only BIO over the caller's buffer. */
      bio->data = a[0];
      bio->length = length < 0 ? (uint32_t)strlen((char *)(uintptr_t)a[0])
                               : (uint32_t)length;
    }
    return 1;
  }
  if (strcmp(name, "_BIO_free") == 0) {
    struct bio32 *bio = (void *)(uintptr_t)a[0];
    if (bio && bio->capacity)
      free32(bio->data);
    free32(a[0]);
    *out = bio != NULL;
    return 1;
  }
  if (strcmp(name, "_BIO_ctrl") == 0) {
    /* BIO_read_filename: BIO_C_SET_FILENAME with BIO_CLOSE|BIO_FP_READ. */
    struct bio32 *bio = (void *)(uintptr_t)a[0];
    *out = 0;
    if (bio && bio->method == file_method && a[1] == 108 && (a[2] & 2) &&
        a[3]) {
      FILE *file = fopen((const char *)(uintptr_t)a[3], "rb");
      if (file) {
        unsigned char buffer[4096];
        size_t count;
        int ok = 1;
        while (ok && (count = fread(buffer, 1, sizeof buffer, file)) > 0)
          ok = bio_append(bio, buffer, (uint32_t)count);
        *out = ok && !ferror(file);
        fclose(file);
      }
    }
    return 1;
  }
  if (strcmp(name, "_X509_STORE_new") == 0) {
    *out = allocate32(4);
    return 1;
  }
  if (strcmp(name, "_X509_STORE_add_cert") == 0) {
    *out = a[0] && a[1];
    return 1;
  }
  if (strcmp(name, "_d2i_X509") == 0) {
    /* No certificate reaches this without the keychain search. */
    *out = 0;
    return 1;
  }
  if (strcmp(name, "_OBJ_obj2nid") == 0) {
    struct asn1_object32 *object = (void *)(uintptr_t)a[0];
    *out = (uint32_t)(object ? object->nid : NID_UNDEF);
    return 1;
  }
  if (strcmp(name, "_d2i_PKCS7_bio") == 0) {
    struct bio32 *bio = (void *)(uintptr_t)a[0];
    *out = 0;
    if (!bio || bio->position > bio->length)
      return 1;
    const unsigned char *bytes =
        (unsigned char *)(uintptr_t)bio->data + bio->position;
    size_t length = bio->length - bio->position;
    int32_t nid = content_info_nid(bytes, length);
    CMSDecoderRef decoder = nid == NID_PKCS7_SIGNED ? decode_cms(bytes, length) : NULL;
    CFDataRef content = NULL, type = NULL;
    if (decoder && CMSDecoderCopyContent(decoder, &content) == errSecSuccess &&
        CMSDecoderCopyEncapsulatedContentType(decoder, &type) == errSecSuccess &&
        content && type) {
      uint32_t p = allocate32(sizeof(struct pkcs7_32));
      uint32_t sign = allocate32(sizeof(struct pkcs7_signed32));
      uint32_t contents = allocate32(sizeof(struct pkcs7_32));
      uint32_t octets = allocate32(sizeof(struct asn1_string32));
      CFIndex size = CFDataGetLength(content);
      uint32_t data = allocate32(size ? (uint32_t)size : 1);
      uint32_t asn1 = allocate32((uint32_t)length);
      const UInt8 *oid = CFDataGetBytePtr(type);
      CFIndex oid_length = CFDataGetLength(type);
      int32_t content_nid =
          oid_length == sizeof pkcs7_oid + 1 && !memcmp(oid, pkcs7_oid, sizeof pkcs7_oid) &&
                  oid[sizeof pkcs7_oid] == 1 ? NID_PKCS7_DATA : NID_UNDEF;
      if (p) {
        struct pkcs7_32 *p7 = (void *)(uintptr_t)p;
        p7->type = new_object(NID_PKCS7_SIGNED);
        p7->d = sign;
        p7->asn1 = asn1;
        if (asn1) {
          memcpy((void *)(uintptr_t)asn1, bytes, length);
          p7->length = (int32_t)length;
        }
      }
      if (sign) {
        ((struct pkcs7_signed32 *)(uintptr_t)sign)->contents = contents;
      }
      if (contents) {
        struct pkcs7_32 *inner = (void *)(uintptr_t)contents;
        inner->type = new_object(content_nid);
        inner->d = octets;
      }
      if (octets && data) {
        struct asn1_string32 *string = (void *)(uintptr_t)octets;
        memcpy((void *)(uintptr_t)data, CFDataGetBytePtr(content), (size_t)size);
        string->length = (int32_t)size;
        string->type = 4; /* V_ASN1_OCTET_STRING */
        string->data = data;
      } else
        free32(data);
      struct pkcs7_32 *p7 = (void *)(uintptr_t)p;
      if (p && sign && contents && octets && data && asn1 && p7->type &&
          ((struct pkcs7_32 *)(uintptr_t)contents)->type) {
        *out = p;
        bio->position = bio->length;
        if (a[1])
          *(uint32_t *)(uintptr_t)a[1] = p;
      } else {
        if (!p) {
          free32(sign);
          free32(contents);
          free32(octets);
          free32(asn1);
        }
        free_pkcs7(p);
      }
    }
    if (content)
      CFRelease(content);
    if (type)
      CFRelease(type);
    if (decoder)
      CFRelease(decoder);
    if (getenv("LP32_TRACE_RECEIPT"))
      fprintf(stderr, "compat32: d2i_PKCS7_bio %zu bytes -> %s\n", length,
              *out ? "signed data" : "not decoded");
    return 1;
  }
  if (strcmp(name, "_PKCS7_verify") == 0) {
    /* PKCS7_verify(p7, certs, store, indata, out, flags): checks the
     * signature and the signer's chain to a trusted root, then writes the
     * content to out. */
    struct pkcs7_32 *p7 = (void *)(uintptr_t)a[0];
    *out = 0;
    CMSDecoderRef decoder =
        p7 && p7->asn1 ? decode_cms((void *)(uintptr_t)p7->asn1, (size_t)p7->length) : NULL;
    size_t signers = 0;
    CMSSignerStatus status = kCMSSignerUnsigned;
    SecPolicyRef policy = SecPolicyCreateBasicX509();
    if (decoder && CMSDecoderGetNumSigners(decoder, &signers) == errSecSuccess &&
        signers > 0 &&
        CMSDecoderCopySignerStatus(decoder, 0, policy, true, &status, NULL,
                                   NULL) == errSecSuccess &&
        status == kCMSSignerValid) {
      struct pkcs7_signed32 *sign = (void *)(uintptr_t)p7->d;
      struct pkcs7_32 *contents = sign ? (void *)(uintptr_t)sign->contents : NULL;
      struct asn1_string32 *octets = contents ? (void *)(uintptr_t)contents->d : NULL;
      struct bio32 *bio = (void *)(uintptr_t)a[4];
      *out = !bio || (octets && bio_append(bio, (void *)(uintptr_t)octets->data,
                                           (uint32_t)octets->length));
    }
    if (policy)
      CFRelease(policy);
    if (decoder)
      CFRelease(decoder);
    if (getenv("LP32_TRACE_RECEIPT"))
      fprintf(stderr, "compat32: PKCS7_verify signers=%zu status=%d -> %llu\n",
              signers, (int)status, (unsigned long long)*out);
    return 1;
  }
  if (strcmp(name, "_PKCS7_free") == 0) {
    free_pkcs7(a[0]);
    *out = 0;
    return 1;
  }
  if (strcmp(name, "_ASN1_get_object") == 0) {
    *out = asn1_get_object((uint32_t *)(uintptr_t)a[0], (int32_t *)(uintptr_t)a[1],
                           (int32_t *)(uintptr_t)a[2], (int32_t *)(uintptr_t)a[3],
                           (int32_t)a[4]);
    return 1;
  }
  return 0;
}

int crypto_bridge32_dispatch(const char *name, const uint32_t *a,
                             uint64_t *out) {
  if (receipt_dispatch(name, a, out))
    return 1;
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

/* LP32_RECEIPT_SELFTEST names a PKCS7 signed-data message whose content is
 * SET { SEQUENCE { INTEGER 2, INTEGER 1, OCTET STRING "A" } }, signed by an
 * untrusted certificate (make test-receipt-bridge). */
int crypto_bridge32_run_receipt_self_test(const char *path) {
  static const unsigned char payload[] = {0x31, 0x0b, 0x30, 0x09, 0x02,
                                          0x01, 0x02, 0x02, 0x01, 0x01,
                                          0x04, 0x01, 0x41};
  FILE *file = fopen(path, "rb");
  unsigned char message[16384];
  size_t size = file ? fread(message, 1, sizeof message, file) : 0;
  if (file)
    fclose(file);
  uint32_t guest = size ? compat_runtime32_allocate(size, 0) : 0;
  if (!guest) {
    fprintf(stderr, "receipt self-test: cannot read %s\n", path);
    return -1;
  }
  memcpy((void *)(uintptr_t)guest, message, size);
  uint64_t bio = 0, p7 = 0, nid = 0, inner_nid = 0, memory = 0, out_bio = 0,
           verified = 1, ignored = 0;
  uint32_t args[6] = {guest, (uint32_t)size};
  receipt_dispatch("_BIO_new_mem_buf", args, &bio);
  args[0] = (uint32_t)bio;
  args[1] = 0;
  receipt_dispatch("_d2i_PKCS7_bio", args, &p7);
  struct pkcs7_32 *outer = (void *)(uintptr_t)p7;
  struct pkcs7_signed32 *sign = outer ? (void *)(uintptr_t)outer->d : NULL;
  struct pkcs7_32 *inner = sign ? (void *)(uintptr_t)sign->contents : NULL;
  struct asn1_string32 *octets = inner ? (void *)(uintptr_t)inner->d : NULL;
  if (outer) {
    args[0] = outer->type;
    receipt_dispatch("_OBJ_obj2nid", args, &nid);
  }
  if (inner) {
    args[0] = inner->type;
    receipt_dispatch("_OBJ_obj2nid", args, &inner_nid);
  }
  int ok = nid == NID_PKCS7_SIGNED && inner_nid == NID_PKCS7_DATA && octets &&
           octets->length == sizeof payload &&
           !memcmp((void *)(uintptr_t)octets->data, payload, sizeof payload);
  /* Walk the content as the game does, then a header past omax. */
  uint32_t cursor_slot = compat_runtime32_allocate(16, 1);
  uint32_t *cursor = (void *)(uintptr_t)cursor_slot;
  int32_t *fields = (int32_t *)(cursor + 1);
  static const struct { int32_t omax; uint32_t result, tag, length, advance; } walk[] = {
      {13, 0x20, 17, 11, 2}, {11, 0x20, 16, 9, 2}, {9, 0, 2, 1, 2}};
  if (ok && cursor) {
    *cursor = octets->data;
    for (unsigned i = 0; ok && i < sizeof walk / sizeof walk[0]; ++i) {
      uint32_t before = *cursor;
      uint32_t call[5] = {cursor_slot, cursor_slot + 4, cursor_slot + 8,
                          cursor_slot + 12, (uint32_t)walk[i].omax};
      uint64_t result = 0;
      receipt_dispatch("_ASN1_get_object", call, &result);
      ok = result == walk[i].result && fields[1] == (int32_t)walk[i].tag &&
           fields[0] == (int32_t)walk[i].length && fields[2] == 0 &&
           *cursor - before == walk[i].advance;
    }
    uint64_t result = 0;
    *cursor = octets->data;
    uint32_t call[5] = {cursor_slot, cursor_slot + 4, cursor_slot + 8,
                        cursor_slot + 12, 5};
    receipt_dispatch("_ASN1_get_object", call, &result);
    ok = ok && (result & 0x80);
  }
  receipt_dispatch("_BIO_s_mem", args, &memory);
  args[0] = (uint32_t)memory;
  receipt_dispatch("_BIO_new", args, &out_bio);
  uint32_t verify[6] = {(uint32_t)p7, 0, 0, 0, (uint32_t)out_bio, 0};
  receipt_dispatch("_PKCS7_verify", verify, &verified);
  struct bio32 *written = (void *)(uintptr_t)out_bio;
  /* The signer is not trusted, so nothing is verified or written. */
  ok = ok && verified == 0 && written && written->length == 0;
  args[0] = (uint32_t)p7;
  receipt_dispatch("_PKCS7_free", args, &ignored);
  args[0] = (uint32_t)bio;
  receipt_dispatch("_BIO_free", args, &ignored);
  args[0] = (uint32_t)out_bio;
  receipt_dispatch("_BIO_free", args, &ignored);
  compat_runtime32_deallocate(cursor_slot);
  compat_runtime32_deallocate(guest);
  printf("Receipt bridge %s (PKCS7 signed data, receipt content, ASN1_get_object, untrusted signer)\n",
         ok ? "PASS" : "FAIL");
  return ok ? 0 : -1;
}
