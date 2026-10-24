#ifndef LP32_CRYPTO_BRIDGE_H
#define LP32_CRYPTO_BRIDGE_H
#include <stdint.h>
int crypto_bridge32_dispatch(const char *, const uint32_t *, uint64_t *);
int crypto_bridge32_run_receipt_self_test(const char *path);
#endif
