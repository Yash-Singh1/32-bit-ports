#include "objc_legacy_bridge.h"
#include "objc_bridge.h"
#include "objc_diagnostics.h"
#include "focus_policy.h"
#include "compat_runtime.h"
#include "game_profile.h"
#include "gl_backend.h"
#define GL_SILENCE_DEPRECATION 1
#import <AppKit/AppKit.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <objc/objc-sync.h>
#include <string.h>
#include <pthread.h>

/* Fragile ObjC objects keep guest ivars in a separate 32-bit allocation.
 * Native superclasses keep their own 64-bit storage; messages cross this
 * boundary, never raw copies of NSWindow/NSApplication object layouts. */
struct class32 { uint32_t isa,super,name,version,info,size,ivars,methods,cache,protocols,layout,extension; };
struct method32 {uint32_t name,types,imp;};
struct category32 {uint32_t name,klass,instance_methods,class_methods,protocols,size,properties;};
static struct category32 *categories[512];
static unsigned category_count;
struct class_entry {struct class32 *guest;Class native;};
struct instance_entry {id native;uint32_t guest;struct class_entry *klass;};
static struct class_entry classes[512];static unsigned class_count;
static struct instance_entry instances[4096];static unsigned instance_count;
static pthread_mutex_t instance_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Keep only scalar diagnostics, never references that could extend an
   object's lifetime. Heap reuse can give a later allocation the same token. */
static struct { uint32_t token, klass, caller; } retired_instances[64];
static unsigned retired_cursor;
static bool scanned;
static struct { uint32_t *values; uint32_t count; } selector_sections[16];
static unsigned selector_section_count;
static const char *str(uint32_t p){return (void *)(uintptr_t)p;}
static struct class_entry *entry_for_native(Class c){
    for(;c;c=class_getSuperclass(c))for(unsigned i=0;i<class_count;++i)if(classes[i].native==c)return &classes[i];return NULL;
}
static void scan(void){
    if(scanned)return;scanned=true;
    const struct macho_image32 *image=compat_runtime32_image();if(!image||!image->header)return;
    const uint8_t *p=(const void *)(image->header+1);
    for(unsigned i=0;i<image->header->ncmds;++i){
        const struct load_command *lc=(const void *)p;
        if(lc->cmd==LC_SEGMENT){const struct segment_command *seg=(const void *)p;const struct section *s=(const void *)(seg+1);
            for(unsigned j=0;j<seg->nsects;++j) {
              if((!strncmp(s[j].sectname,"__message_refs",16)||!strncmp(s[j].sectname,"__objc_selrefs",16)) && selector_section_count<16){
                selector_sections[selector_section_count].values=(void *)(uintptr_t)s[j].addr;
                selector_sections[selector_section_count++].count=s[j].size/4;
              }
              if(!strncmp(s[j].sectname,"__category",16)&&!strncmp(seg->segname,"__OBJC",16)) {
                for(uint32_t k=0;k+sizeof(struct category32)<=s[j].size;k+=sizeof(struct category32))
                    if(category_count<512)categories[category_count++]=(void *)(uintptr_t)(s[j].addr+k);
              }
              if(!strncmp(s[j].sectname,"__class",16)&&!strncmp(seg->segname,"__OBJC",16)){
                for(uint32_t k=0;k+sizeof(struct class32)<=s[j].size;k+=sizeof(struct class32)){
                    struct class32 *c=(void *)(uintptr_t)(s[j].addr+k);if(class_count<512)classes[class_count++].guest=c;
                }
              }
            }
        }p+=lc->cmdsize;
    }
}
uint32_t objc_legacy32_selector(const char *name){
    if(!name)return 0;
    scan();
    for(unsigned j=0;j<selector_section_count;++j)
        for(uint32_t i=0;i<selector_sections[j].count;++i){
            uint32_t value=selector_sections[j].values[i];
            if(value && !strcmp(str(value),name))return value;
        }
    return 0;
}
static struct method32 *method_in_list(uint32_t address,const char *selector){
    if(!address)return NULL;
    uint32_t *list=(void *)(uintptr_t)address;
    struct method32 *m=(void *)(list+2);
    for(uint32_t i=0;i<list[1];++i)
        if(!strcmp(str(m[i].name),selector))return &m[i];
    return NULL;
}
static struct method32 *method(Class native,bool meta,const char *selector){
    for(Class cls=native;cls;cls=class_getSuperclass(cls)){
        for(unsigned i=category_count;i>0;--i){
            struct category32 *cat=categories[i-1];
            if(strcmp(str(cat->klass),class_getName(cls)))continue;
            struct method32 *m=method_in_list(meta?cat->class_methods:cat->instance_methods,selector);
            if(m)return m;
        }
        for(unsigned i=0;i<class_count;++i)if(classes[i].native==cls){
            struct class32 *c=meta?(void *)(uintptr_t)classes[i].guest->isa:classes[i].guest;
            struct method32 *m=method_in_list(c->methods,selector);
            if(m)return m;
        }
    }
    return NULL;
}
void *objc_legacy32_object(uint32_t token){
    pthread_mutex_lock(&instance_mutex);
    id object = nil;
    uint32_t klass = 0;
    for (unsigned i = 0; i < instance_count; ++i)
        if (instances[i].guest == token) {
            object = instances[i].native;
            klass = (uint32_t)(uintptr_t)instances[i].klass->guest;
            break;
        }
    pthread_mutex_unlock(&instance_mutex);
    if (object) { objc_diag_resolved(token, object, klass); return object; }
    for(unsigned i=0;i<class_count;++i)if((uint32_t)(uintptr_t)classes[i].guest==token)return classes[i].native;
    return NULL;
}
uint32_t objc_legacy32_token(void *raw){
    id object=(id)raw;if(!object)return 0;
    if(class_isMetaClass(object_getClass(object))){struct class_entry *c=entry_for_native((Class)object);return c?(uint32_t)(uintptr_t)c->guest:0;}
    struct class_entry *c=entry_for_native(object_getClass(object));if(!c)return 0;
    pthread_mutex_lock(&instance_mutex);
    uint32_t token = 0;
    for (unsigned i = 0; i < instance_count; ++i)
        if (instances[i].native == object) { token = instances[i].guest; break; }
    if (!token && instance_count < 4096) {
        token = compat_runtime32_allocate(c->guest->size < 4 ? 4 : c->guest->size, 1);
        if (token) {
            *(uint32_t *)(uintptr_t)token = (uint32_t)(uintptr_t)c->guest;
            instances[instance_count++] = (struct instance_entry){object, token, c};
            objc_diag_note(OBJC_DIAG_CREATE, token, object, (uint32_t)(uintptr_t)c->guest,
                           str(c->guest->name), UINT32_MAX);
        }
    }
    pthread_mutex_unlock(&instance_mutex);
    return token;
}
void objc_legacy32_log_object(uint32_t token, uint32_t isa) {
    scan();
    for (unsigned i = 0; i < class_count; ++i)
        if ((uint32_t)(uintptr_t)classes[i].guest == isa) {
            fprintf(stderr, "compat32: Objective-C receiver guest class=%s "
                    "instance_size=%u native_registered=%d\n",
                    str(classes[i].guest->name), classes[i].guest->size, classes[i].native != Nil);
            break;
        }
    pthread_mutex_lock(&instance_mutex);
    bool live = false;
    for (unsigned i = 0; i < instance_count; ++i)
        if (instances[i].guest == token) { live = true; break; }
    fprintf(stderr, "compat32: Objective-C receiver identity=%s live_instances=%u\n",
            live ? "registered" : "absent", instance_count);
    for (unsigned i = 0; i < 64; ++i) {
        unsigned slot = (retired_cursor - 1 - i) % 64;
        if (token && retired_instances[slot].token == token) {
            fprintf(stderr, "compat32: Objective-C previous mapping retired "
                    "class=0x%08x caller=0x%08x (token may have been reused)\n",
                    retired_instances[slot].klass, retired_instances[slot].caller);
            break;
        }
    }
    pthread_mutex_unlock(&instance_mutex);
}
static size_t guest_size(const char *t){
    switch(*t){case 'v':return 0;case 'd':case 'q':case 'Q':return 8;case '{':
        if(strstr(t,"CGRect=")||strstr(t,"_NSRect="))return 16;
        if(strstr(t,"CGPoint=")||strstr(t,"CGSize=")||strstr(t,"_NSPoint=")||strstr(t,"_NSSize=")||strstr(t,"_NSRange="))return 8;
        return 0;
        default:return 4;}
}
static const char *type(const char *t){while(strchr("rnNoORV",*t)&&*t)++t;return t;}
static void forward(id self,SEL command,NSInvocation *inv){
    (void)command;
    bool meta=class_isMetaClass(object_getClass(self));
    const char *selector=sel_getName(inv.selector);struct method32 *m=method(meta?(Class)self:object_getClass(self),meta,selector);
    if(!m){[self doesNotRecognizeSelector:inv.selector];return;}
    if (lp32_ignore_guest_focus_loss() &&
        (!strcmp(selector, "windowDidResignKey:") ||
         !strcmp(selector, "windowDidResignMain:") ||
         !strcmp(selector, "applicationWillResignActive:") ||
         !strcmp(selector, "applicationDidResignActive:"))) return;
    if(getenv("LP32_TRACE_OBJC_SELECTORS"))fprintf(stderr,"compat32: native callback %s on %s\n",selector,class_getName(object_getClass(self)));
    uint32_t a[128]={objc_bridge32_guest_object(self),m->name};unsigned n=2;
    NSMethodSignature *sig=inv.methodSignature;
    for(NSUInteger i=2;i<sig.numberOfArguments;++i){
        const char *t=type([sig getArgumentTypeAtIndex:i]);uint64_t value=0;
        if(*t=='@'||*t=='#'){id o=nil;[inv getArgument:&o atIndex:i];a[n++]=objc_bridge32_guest_object(o);}
        else if(*t==':'){SEL s;[inv getArgument:&s atIndex:i];a[n++]=objc_bridge32_guest_selector(sel_getName(s));}
        else if(*t=='^'||*t=='*'){void *p=NULL;[inv getArgument:&p atIndex:i];a[n++]=(uintptr_t)p<=UINT32_MAX?(uint32_t)(uintptr_t)p:objc_bridge32_guest_pointer(p);}
        else if(*t=='{'){
            if(strstr(t,"Rect=")){NSRect r;[inv getArgument:&r atIndex:i];float f[]={r.origin.x,r.origin.y,r.size.width,r.size.height};memcpy(a+n,f,16);n+=4;}
            else if(strstr(t,"Point=")||strstr(t,"Size=")){NSSize s;[inv getArgument:&s atIndex:i];float f[]={s.width,s.height};memcpy(a+n,f,8);n+=2;}
            else {fprintf(stderr,"compat32: unsupported guest ObjC callback struct %s\n",selector);return;}
        } else {
            [inv getArgument:&value atIndex:i];const char *guest_type=type([[NSMethodSignature signatureWithObjCTypes:str(m->types)] getArgumentTypeAtIndex:i]);
            size_t bytes=guest_size(guest_type);if(!bytes||n+bytes/4>128)return;memcpy(a+n,&value,bytes);n+=bytes/4;
        }
    }
    if (lp32_profile()->title == LP32_TITLE_COD4_MP &&
        !strcmp(class_getName(object_getClass(self)), "DownloadDelegate") &&
        !strcmp(selector, "download:didFailWithError:")) {
        NSError *error = objc_bridge32_host_object(a[3]);
        fprintf(stderr, "compat32: mod download failed: %s\n",
                error.description.UTF8String);
    }
    if(getenv("LP32_TRACE_OBJC_SELECTORS"))fprintf(stderr,"CALLBACK ENTER %s tid=%u imp=%08x self=%08x\n",selector,pthread_mach_thread_np(pthread_self()),m->imp,a[0]);
    if(getenv("LP32_TRACE_EVENTS") && !strcmp(selector,"sendEvent:")) {
        NSEvent *e=objc_bridge32_host_object(a[2]);
        if(e.type==NSEventTypeApplicationDefined)fprintf(stderr,"RECEIVE tid=%u token=%08x data=%lx,%lx subtype=%d\n",pthread_mach_thread_np(pthread_self()),a[2],(long)e.data1,(long)e.data2,(int)e.subtype);
    }
    for(unsigned i=2;i<n;++i)objc_bridge32_pin_event(a[i],1);
    uint32_t result=compat_runtime32_call(m->imp,a,n);
    for(unsigned i=2;i<n;++i)objc_bridge32_pin_event(a[i],0);
    if(getenv("LP32_TRACE_OBJC_SELECTORS"))fprintf(stderr,"CALLBACK EXIT %s tid=%u\n",selector,pthread_mach_thread_np(pthread_self()));
    if(getenv("LP32_TRACE_JS_PROPERTIES") && !strcmp(selector,"getValueOfProperty:withType:")) {
        fprintf(stderr,"compat32: JS property %s type=%s -> token=%08x class=%s\n",
            [[(id)objc_bridge32_host_object(a[2]) description] UTF8String],
            [[(id)objc_bridge32_host_object(a[3]) description] UTF8String],result,
            class_getName(object_getClass((id)objc_bridge32_host_object(result))));
    }
    const char *ret=type(sig.methodReturnType);
    if(*ret=='@'||*ret=='#'){id o=objc_bridge32_host_object(result);[inv setReturnValue:&o];}
    else if(*ret==':'){SEL s=result?sel_registerName(str(result)):NULL;[inv setReturnValue:&s];}
    else if(*ret!='v'){uint64_t r=(int64_t)(int32_t)result;[inv setReturnValue:&r];}
}
/* The identity map must not keep guest objects alive. Their dealloc methods
 * unregister observers and invalidate callbacks before their owners disappear. */
static void legacy_dealloc(id self, SEL command) {
    uint32_t token=0;
    struct guest_resume_context frame;
    uint32_t caller = compat_runtime32_import_frame(&frame) ? frame.eip : 0;
    pthread_mutex_lock(&instance_mutex);
    for(unsigned i=0;i<instance_count;++i)if(instances[i].native==self){token=instances[i].guest;break;}
    pthread_mutex_unlock(&instance_mutex);
    Class cls=object_getClass(self);
    struct class_entry *entry=entry_for_native(cls);
    if(entry)cls=entry->native;
    objc_diag_note(OBJC_DIAG_DEALLOC, token, self,
                   entry ? (uint32_t)(uintptr_t)entry->guest : 0,
                   entry ? str(entry->guest->name) : NULL, UINT32_MAX);
    struct method32 *m=method(cls,false,"dealloc");
    if(m && token){uint32_t args[]={token,m->name};compat_runtime32_call(m->imp,args,2);}
    else {struct objc_super super={self,class_getSuperclass(cls)};((void(*)(struct objc_super *,SEL))objc_msgSendSuper)(&super,command);}
    pthread_mutex_lock(&instance_mutex);
    for(unsigned i=0;i<instance_count;++i)if(instances[i].guest==token && token){
        unsigned slot = retired_cursor++ % 64;
        retired_instances[slot].token = token;
        retired_instances[slot].klass = (uint32_t)(uintptr_t)instances[i].klass->guest;
        retired_instances[slot].caller = caller;
        objc_diag_note(OBJC_DIAG_RETIRE, token, self, retired_instances[slot].klass,
                       str(instances[i].klass->guest->name), UINT32_MAX);
        instances[i]=instances[--instance_count];break;
    }
    pthread_mutex_unlock(&instance_mutex);
    if (token) compat_runtime32_deallocate(token);
}
static void add_methods(Class native,struct class_entry *c,bool meta){
    struct class32 *g=meta?(void *)(uintptr_t)c->guest->isa:c->guest;if(!g->methods)return;
    uint32_t *list=(void *)(uintptr_t)g->methods;struct method32 *m=(void *)(list+2);
    for(uint32_t i=0;i<list[1];++i){
        const char *name=str(m[i].name),*types=str(m[i].types);
        /* Class initialization must not re-enter guest code before its imports
         * and C++ globals have been initialized. These images have no +load. */
        if(!strcmp(name,"load")||!strcmp(name,"initialize")||!strcmp(name,"dealloc")||*types=='{')continue;
        Method inherited=class_getInstanceMethod(class_getSuperclass(native),sel_registerName(name));
        const char *encoding=inherited?method_getTypeEncoding(inherited):types;
        class_addMethod(native,sel_registerName(name),(IMP)_objc_msgForward,encoding);
    }
    for(unsigned j=0;j<category_count;++j){
        struct category32 *cat=categories[j];
        if(strcmp(str(cat->klass),str(c->guest->name)))continue;
        uint32_t address=meta?cat->class_methods:cat->instance_methods;
        if(!address)continue;
        uint32_t *list=(void *)(uintptr_t)address;
        struct method32 *methods=(void *)(list+2);
        for(uint32_t i=0;i<list[1];++i){
            const char *name=str(methods[i].name),*types=str(methods[i].types);
            if(!strcmp(name,"load")||!strcmp(name,"initialize")||*types=='{')continue;
            Method inherited=class_getInstanceMethod(class_getSuperclass(native),sel_registerName(name));
            class_replaceMethod(native,sel_registerName(name),(IMP)_objc_msgForward,
                                inherited?method_getTypeEncoding(inherited):types);
        }
    }
    class_addMethod(native,@selector(forwardInvocation:),(IMP)forward,"v@:@");
    if(!meta)class_addMethod(native,sel_registerName("dealloc"),(IMP)legacy_dealloc,"v@:");
}
void *objc_legacy32_class(const char *name){
    scan();Class existing=objc_getClass(name);if(existing)return existing;
    struct class_entry *c=NULL;for(unsigned i=0;i<class_count;++i)if(!strcmp(str(classes[i].guest->name),name)){c=&classes[i];break;}
    if(!c)return NULL;
    Class parent=objc_legacy32_class(str(c->guest->super));if(!parent)return NULL;
    if (lp32_gl_backend_is_replacement() && parent == [NSOpenGLContext class]) {
        Class replacement = objc_getClass(lp32_gl_backend_context_class());
        if (replacement) parent = replacement;
    }
    Class native=objc_allocateClassPair(parent,name,0);if(!native)return objc_getClass(name);
    c->native=native;add_methods(native,c,false);add_methods(object_getClass(native),c,true);
    objc_registerClassPair(native);return native;
}
void objc_legacy32_register_classes(void){
    scan();for(unsigned i=0;i<class_count;++i)objc_legacy32_class(str(classes[i].guest->name));
}
void *objc_legacy32_application_class(void){
    scan();for(unsigned i=0;i<class_count;++i)
        if(!strcmp(str(classes[i].guest->super),"NSApplication"))
            return objc_legacy32_class(str(classes[i].guest->name));
    return [NSApplication class];
}
int objc_legacy32_message(const uint32_t *a,uint64_t *out){
    id object=objc_bridge32_host_object(a[0]);if(!object)return 0;
    bool meta=class_isMetaClass(object_getClass(object));
    const char *sel=str(a[1]);struct method32 *m=method(meta?(Class)object:object_getClass(object),meta,sel);if(!m)return 0;
    objc_diag_method(a[0], m->imp, str(m->types));
    if (!meta && lp32_profile()->title == LP32_TITLE_COD4_MP &&
        !strcmp(sel, "progress:") &&
        !strcmp(class_getName(object_getClass(object)), "DownloadDelegate")) {
        /* NSURLDownload schedules its delegate in the creating thread's
         * default run loop. COD4 polls progress from a Carbon game loop,
         * which can starve both byte-count and terminal callbacks. Service
         * ready callbacks here without waiting or consuming game input. */
        static _Thread_local bool pumping_download;
        if (!pumping_download) {
            pumping_download = true;
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);
            pumping_download = false;
        }
    }
    NSMethodSignature *sig=[NSMethodSignature signatureWithObjCTypes:str(m->types)];unsigned n=2;
    for(NSUInteger i=2;i<sig.numberOfArguments;++i){size_t bytes=guest_size(type([sig getArgumentTypeAtIndex:i]));if(!bytes)return 0;n+=(unsigned)(bytes/4);}
    uint32_t args[128];if(n>128)return 0;memcpy(args,a,n*4);args[0]=objc_bridge32_guest_object(object);
    if(getenv("LP32_TRACE_OBJC_SELECTORS"))fprintf(stderr,"LEGACY %s self=%08x tid=%u ra=%08x\n",sel,args[0],pthread_mach_thread_np(pthread_self()),a[-1]);
    for(unsigned i=2;i<n;++i)objc_bridge32_pin_event(args[i],1);
    *out=compat_runtime32_call(m->imp,args,n);
    for(unsigned i=2;i<n;++i)objc_bridge32_pin_event(args[i],0);
    return 1;
}
int objc_legacy32_message_stret(const uint32_t *a,uint64_t *out){
    id object=objc_bridge32_host_object(a[1]);
    if(!object || !a[0])return 0;
    bool meta=class_isMetaClass(object_getClass(object));
    struct method32 *m=method(meta?(Class)object:object_getClass(object),meta,str(a[2]));
    if(!m)return 0;
    NSMethodSignature *sig=[NSMethodSignature signatureWithObjCTypes:str(m->types)];
    if(*type(sig.methodReturnType)!='{')return 0;
    unsigned n=3;
    for(NSUInteger i=2;i<sig.numberOfArguments;++i){
        size_t bytes=guest_size(type([sig getArgumentTypeAtIndex:i]));
        if(!bytes)return 0;
        n+=(unsigned)(bytes/4);
    }
    if(n>128)return 0;
    uint32_t args[128];memcpy(args,a,n*4);
    args[1]=objc_bridge32_guest_object(object);
    *out=compat_runtime32_call(m->imp,args,n);
    return 1;
}
int objc_legacy32_property(const char *name,const uint32_t *a,uint64_t *out){
    bool set=!strcmp(name,"_objc_setProperty");
    if(!set&&strcmp(name,"_objc_getProperty"))return 0;
    id native=objc_legacy32_object(a[0]);if(!native){if(!a[0]){*out=0;return 1;}return 0;}
    struct class_entry *c=entry_for_native(object_getClass(native));
    if(!c||a[2]<4||a[2]>c->guest->size-4)return 0;
    uint32_t *slot=(void *)(uintptr_t)(a[0]+a[2]);
    bool atomic=a[set?4:3]!=0;
    if(atomic)objc_sync_enter(native);
    if(set){
        id value=objc_bridge32_host_object(a[3]);
        if(a[5]==1)value=[value copy];else if(a[5]==2)value=[value mutableCopy];else [value retain];
        id old=objc_bridge32_host_object(*slot);
        *slot=objc_bridge32_guest_object(value);
        if(atomic)objc_sync_exit(native);
        [old release];*out=0;
    }else{
        /* The proxy owns the native result across the per-call autorelease
           pool, including the retain/autorelease guarantee of atomic reads. */
        *out=*slot;
        if(atomic)objc_sync_exit(native);
    }
    return 1;
}

int objc_legacy32_super_message(void *receiver, void *parent, const uint32_t *a, uint64_t *out) {
    struct method32 *m=method((Class)parent,false,str(a[1]));
    if(!m)return 0;
    NSMethodSignature *sig=[NSMethodSignature signatureWithObjCTypes:str(m->types)];unsigned n=2;
    for(NSUInteger i=2;i<sig.numberOfArguments;++i){size_t bytes=guest_size(type([sig getArgumentTypeAtIndex:i]));if(!bytes)return 0;n+=(unsigned)(bytes/4);}
    if(n>128)return 0;uint32_t args[128];memcpy(args,a,n*4);args[0]=objc_bridge32_guest_object(receiver);
    *out=compat_runtime32_call(m->imp,args,n);return 1;
}

int objc_legacy32_run_lifetime_self_test(void) {
    scan();
    if(class_count>=512)return -1;
    uint32_t metadata=compat_runtime32_allocate(sizeof(struct class32),1);
    if(!metadata)return -1;
    struct class32 *guest=(void *)(uintptr_t)metadata;
    guest->name=compat_runtime32_copy_cstring("LP32LifetimeTest");guest->size=4;
    Class cls=objc_allocateClassPair([NSObject class],"LP32LifetimeTest",0);
    if(!cls)return -1;
    class_addMethod(cls,sel_registerName("dealloc"),(IMP)legacy_dealloc,"v@:");
    objc_registerClassPair(cls);
    classes[class_count++]=(struct class_entry){guest,cls};
    id object=[[cls alloc] init];uint32_t token=objc_legacy32_token(object);
    bool valid=token && objc_legacy32_object(token)==object;
    [object release];
    valid=valid && objc_legacy32_object(token)==NULL;
    uint64_t pool = 0, result = 0;
    uint32_t pool_alloc[] = {objc_bridge32_guest_object([NSAutoreleasePool class]),
                            objc_bridge32_guest_selector("alloc")};
    valid = valid && objc_bridge32_dispatch("_objc_msgSend", pool_alloc, &pool);
    object = [[cls alloc] init];
    token = objc_legacy32_token(object);
    uint32_t autorelease[] = {token, objc_bridge32_guest_selector("autorelease")};
    valid = valid && objc_bridge32_dispatch("_objc_msgSend", autorelease, &result);
    valid = valid && result == token && objc_legacy32_object(token) == object;
    uint32_t drain[] = {(uint32_t)pool, objc_bridge32_guest_selector("drain")};
    valid = valid && objc_bridge32_dispatch("_objc_msgSend", drain, &result);
    valid = valid && objc_legacy32_object(token) == NULL;
    --class_count;
    compat_runtime32_deallocate(guest->name);compat_runtime32_deallocate(metadata);
    if(valid)puts("Legacy Objective-C lifetime self-test: PASS (identity map permits deallocation; guest autorelease scope preserves subclasses)");
    return valid?0:-1;
}

/* Construct the image's own request class without starting a network request.
   Run after guest constructors, since ASIHTTPRequest initializes shared state.
   This checks the factory/retain boundary from issue #13 without the game UI. */
int objc_legacy32_run_factory_self_test(void) {
    const char *name = getenv("LP32_OBJC_FACTORY_TEST_CLASS");
    Class cls = name ? objc_legacy32_class(name) : Nil;
    if (!cls) return -1;
    uint64_t pool = 0, result = 0;
    uint32_t allocate[] = {objc_bridge32_guest_object([NSAutoreleasePool class]),
                           objc_bridge32_guest_selector("alloc")};
    bool explicit_pool = getenv("LP32_OBJC_FACTORY_TEST_POOL") != NULL;
    if (explicit_pool && !objc_bridge32_dispatch("_objc_msgSend", allocate, &pool)) return -1;
    uint32_t arguments[4] = {objc_bridge32_guest_object(cls),
        objc_bridge32_guest_selector("requestWithURL:"),
        objc_bridge32_guest_object([NSURL URLWithString:@"http://127.0.0.1/issue13-not-started"])};
    if (!objc_bridge32_dispatch("_objc_msgSend", arguments, &result) ||
        !result || compat_runtime32_last_call_trapped()) return -1;
    uint32_t token = (uint32_t)result;
    arguments[0] = token;
    arguments[1] = objc_bridge32_guest_selector("retain");
    if (!objc_bridge32_dispatch("_objc_msgSend", arguments, &result) || result != token ||
        compat_runtime32_last_call_trapped() || !objc_legacy32_object(token)) return -1;
    const char *data_function = getenv("LP32_OBJC_FACTORY_TEST_DATA_FUNCTION");
    if (data_function) {
        char *end = NULL;
        unsigned long address = strtoul(data_function, &end, 0);
        const struct macho_image32 *image = compat_runtime32_image();
        if (!end || *end || address < image->min_address || address >= image->max_address) return -1;
        uint32_t data = compat_runtime32_call((uint32_t)address, NULL, 0);
        if (!data || compat_runtime32_last_call_trapped()) return -1;
        uint32_t message[4] = {data, objc_bridge32_guest_selector("autorelease"), 0, 0};
        if (!objc_bridge32_dispatch("_objc_msgSend", message, &result)) return -1;
        message[1] = objc_bridge32_guest_selector("getBytes:length:");
        message[2] = compat_runtime32_allocate(8, 1); message[3] = 6;
        if (!message[2] || !objc_bridge32_dispatch("_objc_msgSend", message, &result) ||
            compat_runtime32_last_call_trapped()) return -1;
        compat_runtime32_deallocate(message[2]);
        puts("Guest IOKit data self-test: PASS (CreateCFProperty/autorelease/getBytes)");
    }
    const char *function = getenv("LP32_OBJC_FACTORY_TEST_FUNCTION");
    if (function) {
        char *end = NULL;
        unsigned long address = strtoul(function, &end, 0);
        const struct macho_image32 *image = compat_runtime32_image();
        if (!end || *end || address < image->min_address || address >= image->max_address) return -1;
        compat_runtime32_call((uint32_t)address, NULL, 0);
        if (compat_runtime32_last_call_trapped()) return -1;
    }
    if (explicit_pool) {
        uint32_t drain[] = {(uint32_t)pool, objc_bridge32_guest_selector("drain")};
        if (!objc_bridge32_dispatch("_objc_msgSend", drain, &result) ||
            !objc_legacy32_object(token)) return -1;
    }
    arguments[1] = objc_bridge32_guest_selector("release");
    if (!objc_bridge32_dispatch("_objc_msgSend", arguments, &result) ||
        compat_runtime32_last_call_trapped()) return -1;
    if (explicit_pool && objc_legacy32_object(token)) return -1;
    printf("Guest request factory self-test: PASS (class=%s, pool=%s, factory/retain/release; no request started)\n",
        name, explicit_pool ? "explicit" : "fallback");
    return 0;
}

/* Exercise the shipped i386 DownloadDelegate, including its native callbacks
 * and progress pointer, against the local HTTP fixture in the test runner. */
int objc_legacy32_run_download_self_test(void) {
    const char *url = getenv("LP32_COD4_DOWNLOAD_TEST_URL");
    const char *path = getenv("LP32_COD4_DOWNLOAD_TEST_PATH");
    Class cls = objc_legacy32_class("DownloadDelegate");
    if (!url || !path || !cls) return -1;
    id delegate = [[cls alloc] init];
    uint32_t token = objc_bridge32_guest_object(delegate);
    uint32_t guest_url = compat_runtime32_copy_cstring(url);
    uint32_t guest_path = compat_runtime32_copy_cstring(path);
    uint32_t bytes = compat_runtime32_allocate(4, 1);
    uint32_t start[] = {token, objc_legacy32_selector("start:path:"), guest_url, guest_path};
    uint64_t result = 0;
    int status = -1;
    uint32_t pending_bytes = 0;
    bool started = guest_url && guest_path && bytes &&
        objc_legacy32_message(start, &result) && result;
    if (started) {
        uint32_t progress[] = {token, objc_legacy32_selector("progress:"), bytes};
        NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:10];
        status = 0;
        while (!status && deadline.timeIntervalSinceNow > 0) {
            if (!objc_legacy32_message(progress, &result)) break;
            status = (int32_t)result;
            if (!status) pending_bytes = *(uint32_t *)(uintptr_t)bytes;
            /* Deliberately no Cocoa run-loop pump: use the game's poll. */
            if (!status) [NSThread sleepForTimeInterval:0.001];
        }
    }
    printf("cod4-download-selftest: status=%d bytes=%u pendingBytes=%u\n", status,
           bytes ? *(uint32_t *)(uintptr_t)bytes : 0, pending_bytes);
    uint32_t stop[] = {token, objc_legacy32_selector("stop")};
    objc_legacy32_message(stop, &result);
    [delegate release];
    compat_runtime32_deallocate(guest_url);
    compat_runtime32_deallocate(guest_path);
    compat_runtime32_deallocate(bytes);
    return started && status ? 0 : -1;
}
