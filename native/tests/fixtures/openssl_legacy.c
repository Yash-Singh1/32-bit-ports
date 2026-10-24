#include "openssl_test_key.h"
extern void OPENSSL_add_all_algorithms_noconf(void), OpenSSL_add_all_digests(void);
extern void ERR_load_crypto_strings(void), ERR_load_X509_strings(void), EVP_cleanup(void);
extern void *BIO_s_mem(void), *BIO_new(void *), *BIO_new_mem_buf(const void *, int);
extern int BIO_free(void *);
extern void *PEM_read_bio_RSA_PUBKEY(void *, void **, void *, void *);
extern void RSA_free(void *);
extern void *EVP_PKEY_new(void);
extern int EVP_PKEY_assign(void *, int, void *);
/* OpenSSL 0.9.7, not the modern EVP_PKEY_CTX API. */
extern int EVP_PKEY_encrypt(unsigned char *, const unsigned char *, int, void *);
extern void EVP_PKEY_free(void *);
extern char *getenv(const char *);
extern void *fopen(const char *, const char *);
extern unsigned int fwrite(const void *, unsigned int, unsigned int, void *);
extern int fclose(void *);

int main(void) {
    unsigned char encrypted[1024], too_long[1024] = {0};
    const unsigned char plaintext[] = "COD4 legacy EVP encryption boundary";
    for (unsigned int iteration = 0; iteration < 64; ++iteration) {
        OPENSSL_add_all_algorithms_noconf();
        OpenSSL_add_all_digests(); ERR_load_crypto_strings(); ERR_load_X509_strings();
        void *method = BIO_s_mem();
        if (!method || method != BIO_s_mem()) return 1;
        void *empty = BIO_new(method);
        if (!empty || !BIO_free(empty)) return 2;
        void *bio = BIO_new_mem_buf(public_key, -1);
        if (!bio) return 3;
        void *rsa = 0;
        if (PEM_read_bio_RSA_PUBKEY(bio, &rsa, 0, 0) != rsa || !rsa) return 4;
        BIO_free(bio);
        bio = BIO_new_mem_buf(public_key, -1);
        if (PEM_read_bio_RSA_PUBKEY(bio, &rsa, 0, 0) != rsa) return 12;
        BIO_free(bio);
        void *key = EVP_PKEY_new();
        if (!key || EVP_PKEY_assign(key, 6, 0) || !EVP_PKEY_assign(key, 6, rsa)) return 5;
        int length = EVP_PKEY_encrypt(encrypted, plaintext, sizeof(plaintext) - 1, key);
        if (length != 128) return 6;
        if (EVP_PKEY_encrypt(encrypted, too_long, sizeof(too_long), key) > 0) return 7;
        length = EVP_PKEY_encrypt(encrypted, plaintext, sizeof(plaintext) - 1, key);
        if (length != 128) return 8;
        EVP_PKEY_free(key);
        if (iteration == 63) {
            void *file = fopen(getenv("LP32_CRYPTO_OUTPUT"), "wb");
            if (!file || fwrite(encrypted, 1, length, file) != (unsigned int)length || fclose(file)) return 9;
        }
        bio = BIO_new_mem_buf("invalid PEM", -1);
        if (PEM_read_bio_RSA_PUBKEY(bio, 0, 0, 0)) return 10;
        BIO_free(bio);
        bio = BIO_new_mem_buf(public_key, -1);
        rsa = PEM_read_bio_RSA_PUBKEY(bio, 0, 0, 0);
        if (!rsa) return 11;
        BIO_free(bio); RSA_free(rsa);
        EVP_cleanup();
    }
    return 0;
}
