#ifndef LP32_OBJC_LEGACY_BRIDGE_H
#define LP32_OBJC_LEGACY_BRIDGE_H
#include <stdint.h>
uint32_t objc_legacy32_selector(const char *name);
void *objc_legacy32_class(const char *name);
void *objc_legacy32_object(uint32_t token);
uint32_t objc_legacy32_token(void *object);
void objc_legacy32_log_object(uint32_t token, uint32_t isa);
int objc_legacy32_message(const uint32_t *args,uint64_t *result);
int objc_legacy32_message_stret(const uint32_t *args,uint64_t *result);
int objc_legacy32_property(const char *,const uint32_t *,uint64_t *);
void objc_legacy32_register_classes(void);
void *objc_legacy32_application_class(void);
int objc_legacy32_super_message(void *, void *, const uint32_t *, uint64_t *);
int objc_legacy32_run_lifetime_self_test(void);
int objc_legacy32_run_factory_self_test(void);
int objc_legacy32_run_download_self_test(void);
#endif
