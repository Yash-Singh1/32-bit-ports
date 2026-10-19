extern "C" {
#include "compat_runtime.h"
int libcpp_stream_bridge32_dispatch(const char *, const uint32_t *, uint64_t *);
}
#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <locale>
#include <istream>
#include <ostream>
#include <new>
#include <mutex>

// libc++ ABI v1 uses 32-bit pointers and streamsize on i386.
struct ios32 {
    uint32_t vptr, flags; int32_t precision, width;
    uint32_t state, exceptions, buffer, locale, callbacks, indices;
    uint32_t event_size, event_cap, integers, integer_size, integer_cap;
    uint32_t pointers, pointer_size, pointer_cap;
};
struct buffer32 { uint32_t vptr, locale, begin_get, next_get, end_get, begin_put, next_put, end_put; };
static_assert(sizeof(ios32)==72 && sizeof(buffer32)==32, "i386 stream layout");
static uint32_t *w(uint32_t p) { return reinterpret_cast<uint32_t *>(uintptr_t(p)); }
struct locale32 { std::locale host; uint32_t refs; locale32():refs(1){} };
static uint32_t new_locale() {
    uint32_t p=compat_runtime32_allocate(sizeof(locale32),0);
    if(p)new(reinterpret_cast<void *>(uintptr_t(p))) locale32;
    return p;
}
static void retain_locale(uint32_t p) { if(p)__atomic_add_fetch(&reinterpret_cast<locale32 *>(uintptr_t(p))->refs,1,__ATOMIC_RELAXED); }
static void release_locale(uint32_t p) { if(p && __atomic_sub_fetch(&reinterpret_cast<locale32 *>(uintptr_t(p))->refs,1,__ATOMIC_ACQ_REL)==0) { reinterpret_cast<locale32 *>(uintptr_t(p))->~locale32();compat_runtime32_deallocate(p); } }
static uint32_t invoke(uint32_t object,unsigned slot,const uint32_t *tail=nullptr,unsigned n=0) {
    uint32_t a[16]={object}; if(n)memcpy(a+1,tail,n*4);
    return compat_runtime32_call(w(w(object)[0])[slot],a,n+1);
}
static ios32 *ios(uint32_t p) {
    int32_t offset=reinterpret_cast<int32_t *>(uintptr_t(w(p)[0]))[-3];
    return reinterpret_cast<ios32 *>(uintptr_t(p+offset));
}
static uint32_t streambuf_vtable,codecvt_object;
static std::once_flag tables_once;
static void initialize_tables() {
    const char *names[]={"destroy","delete","imbue","setbuf","seekoff","seekpos","sync","showmanyc","xsgetn","underflow","uflow","pbackfail","xsputn","overflow"};
    streambuf_vtable=compat_runtime32_allocate(14*4,1);
    for(unsigned i=0;i<14;++i) { std::string name="_lp32_libcpp_buffer_";name+=names[i];w(streambuf_vtable)[i]=compat_runtime32_guest_callback(name.c_str()); }
    codecvt_object=compat_runtime32_allocate(48,1);w(codecvt_object)[0]=codecvt_object+8;
    const char *codec[]={"destroy","delete","zero","out","in","unshift","encoding","noconv","length","max"};
    for(unsigned i=0;i<10;++i) { std::string name="_lp32_libcpp_codec_";name+=codec[i];w(codecvt_object+8)[i]=compat_runtime32_guest_callback(name.c_str()); }
}
static int take(buffer32 *b,bool advance) {
    if(b->next_get<b->end_get) { int c=*reinterpret_cast<unsigned char *>(uintptr_t(b->next_get));if(advance)++b->next_get;return c; }
    return (int)invoke((uint32_t)(uintptr_t)b,advance?10:9);
}
static int put(buffer32 *b,int c) {
    if(b->next_put<b->end_put) { *reinterpret_cast<char *>(uintptr_t(b->next_put++))=(char)c;return (unsigned char)c; }
    uint32_t a=(uint32_t)c;return (int)invoke((uint32_t)(uintptr_t)b,13,&a,1);
}
// Native formatted I/O supplies numeric parsing/formatting; every byte still
// travels through the guest's streambuf and original file implementation.
class guest_buffer:public std::streambuf {
    buffer32 *b;
public:
    explicit guest_buffer(uint32_t p):b(reinterpret_cast<buffer32 *>(uintptr_t(p))){}
    int_type underflow() override { return take(b,false); }
    int_type uflow() override { return take(b,true); }
    int_type overflow(int_type c) override { return put(b,c); }
    int sync() override { return (int)invoke((uint32_t)(uintptr_t)b,6); }
};
static void configure(std::ios &host,ios32 *g) {
    host.flags((std::ios::fmtflags)g->flags);host.precision(g->precision);host.width(g->width);
    if(g->locale)host.imbue(reinterpret_cast<locale32 *>(uintptr_t(g->locale))->host);
    host.clear((std::ios::iostate)g->state);
}
static void finish(std::ios &host,ios32 *g) { g->state=host.rdstate();g->width=(int32_t)host.width(); }
extern "C" int libcpp_stream_bridge32_dispatch(const char *name,const uint32_t *a,uint64_t *out) {
#define IS(x) (!strcmp(name,x))
    *out=0;
    if(IS("__ZNSt3__18ios_base4initEPv")) {
        ios32 *g=reinterpret_cast<ios32 *>(uintptr_t(a[0]));memset((char *)g+4,0,sizeof(*g)-4);
        g->flags=std::ios::skipws|std::ios::dec;g->precision=6;g->buffer=a[1];g->state=a[1]?0:std::ios::badbit;g->locale=new_locale();return g->locale!=0;
    }
    if(IS("__ZNSt3__18ios_base5clearEj") || IS("__ZNSt3__18ios_base33__set_badbit_and_consider_rethrowEv")) {
        ios32 *g=reinterpret_cast<ios32 *>(uintptr_t(a[0]));
        g->state=IS("__ZNSt3__18ios_base5clearEj")?a[1]:g->state|std::ios::badbit;
        if(!g->buffer)g->state|=std::ios::badbit;
        return !(g->state & g->exceptions); // Guest exception unwinding is unsupported.
    }
    if(IS("__ZNSt3__16localeC1ERKS0_") || IS("__ZNSt3__16localeC2ERKS0_")) { *w(a[0])=*w(a[1]);retain_locale(*w(a[0]));return 1; }
    if(IS("__ZNSt3__16localeD1Ev") || IS("__ZNSt3__16localeD2Ev")) { release_locale(*w(a[0]));return 1; }
    if(IS("__ZNKSt3__18ios_base6getlocEv")) { *w(a[0])=reinterpret_cast<ios32 *>(uintptr_t(a[1]))->locale;retain_locale(*w(a[0]));*out=a[0];return 1; }
    if(IS("__ZNKSt3__16locale9has_facetERNS0_2idE") || IS("__ZNKSt3__16locale9use_facetERNS0_2idE")) {
        bool supported=compat_runtime32_pointer_import_matches(a[1],"__ZNSt3__17codecvtIcc11__mbstate_tE2idE");
        if(IS("__ZNKSt3__16locale9has_facetERNS0_2idE")){*out=supported;return 1;}
        if(!supported)return 0;std::call_once(tables_once,initialize_tables);*out=codecvt_object;return 1;
    }
    if(!strncmp(name,"_lp32_libcpp_codec_",19)) {
        const char *method=name+19;const auto &facet=std::use_facet<std::codecvt<char,char,mbstate_t>>(std::locale::classic());
        if(!strcmp(method,"noconv"))*out=facet.always_noconv();
        else if(!strcmp(method,"encoding"))*out=facet.encoding();
        else if(!strcmp(method,"max"))*out=facet.max_length();
        else if(!strcmp(method,"length"))*out=std::min(a[3]-a[2],a[4]);
        else if(!strcmp(method,"out") || !strcmp(method,"in")) { *w(a[4])=a[2];*w(a[7])=a[5];*out=std::codecvt_base::noconv; }
        else if(!strcmp(method,"unshift")){*w(a[4])=a[2];*out=std::codecvt_base::noconv;}
        else if(strcmp(method,"destroy")&&strcmp(method,"delete")&&strcmp(method,"zero"))return 0;
        return 1;
    }
    if(IS("__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEEC2Ev")) {
        std::call_once(tables_once,initialize_tables);buffer32 *b=reinterpret_cast<buffer32 *>(uintptr_t(a[0]));memset(b,0,sizeof(*b));b->vptr=streambuf_vtable;b->locale=new_locale();return b->locale!=0;
    }
    if(IS("__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEED2Ev")) { release_locale(w(a[0])[1]);return 1; }
    if(!strncmp(name,"_lp32_libcpp_buffer_",20)) {
        const char *method=name+20;buffer32 *b=reinterpret_cast<buffer32 *>(uintptr_t(a[0]));
        if(!strcmp(method,"underflow")||!strcmp(method,"overflow")||!strcmp(method,"pbackfail"))*out=UINT32_MAX;
        else if(!strcmp(method,"setbuf"))*out=a[0];
        else if(!strcmp(method,"uflow")){int c=(int)invoke(a[0],9);if(c!=-1)++b->next_get;*out=(uint32_t)c;}
        else if(!strcmp(method,"xsgetn")||!strcmp(method,"xsputn")) {
            uint32_t n=0;bool reading=!strcmp(method,"xsgetn");char *p=reinterpret_cast<char *>(uintptr_t(a[1]));
            while(n<a[2]){int c=reading?take(b,true):put(b,(unsigned char)p[n]);if(c==-1)break;if(reading)p[n]=(char)c;++n;}*out=n;
        } else if(strcmp(method,"sync")&&strcmp(method,"showmanyc")&&strcmp(method,"imbue")&&strcmp(method,"destroy")&&strcmp(method,"delete"))return 0;
        return 1;
    }
    if(IS("__ZNSt3__19basic_iosIcNS_11char_traitsIcEEED2Ev")) {
        ios32 *g=reinterpret_cast<ios32 *>(uintptr_t(a[0]));
        for(uint32_t i=g->event_size;i;--i) {uint32_t args[]={0,a[0],w(g->indices)[i-1]};compat_runtime32_call(w(g->callbacks)[i-1],args,3);}
        release_locale(g->locale);for(uint32_t p:{g->callbacks,g->indices,g->integers,g->pointers})if(p)compat_runtime32_deallocate(p);return 1;
    }
    if(IS("__ZNSt3__113basic_istreamIcNS_11char_traitsIcEEED2Ev") || IS("__ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEED2Ev") || IS("__ZNSt3__114basic_iostreamIcNS_11char_traitsIcEEED2Ev"))return 1; // Virtual ios base is destroyed by the most-derived destructor.
    const char *input="__ZNSt3__113basic_istreamIcNS_11char_traitsIcEEE";
    const char *output="__ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEE";
    bool reading=!strncmp(name,input,strlen(input)),writing=!strncmp(name,output,strlen(output));
    if(reading||writing) {
        const char *method=name+strlen(reading?input:output);
        if(!strncmp(method,"6sentry",7))return 0;
        ios32 *g=ios(a[0]);if(!g->buffer){g->state|=std::ios::badbit;*out=a[0];return 1;}
        guest_buffer buf(g->buffer);
        if(reading) {
            std::istream stream(&buf);configure(stream,g);
            if(!strcmp(method,"4readEPci"))stream.read(reinterpret_cast<char *>(uintptr_t(a[1])),(int32_t)a[2]);
            else if(!strcmp(method,"7getlineEPcic"))stream.getline(reinterpret_cast<char *>(uintptr_t(a[1])),(int32_t)a[2],(char)a[3]);
            else if(!strcmp(method,"rsERi")){int value=0;stream>>value;*w(a[1])=value;}
            else if(!strcmp(method,"rsERj")||!strcmp(method,"rsERm")){unsigned value=0;stream>>value;*w(a[1])=value;}
            else if(!strcmp(method,"rsERy")){unsigned long long value=0;stream>>value;memcpy(w(a[1]),&value,8);}
            else return 0;
            if(!strncmp(method,"4read",5)||!strncmp(method,"7getline",8))w(a[0])[1]=(uint32_t)stream.gcount();
            finish(stream,g);
        } else {
            std::ostream stream(&buf);configure(stream,g);
            if(!strcmp(method,"5writeEPKci"))stream.write(reinterpret_cast<char *>(uintptr_t(a[1])),(int32_t)a[2]);
            else if(!strcmp(method,"3putEc"))stream.put((char)a[1]);
            else if(!strcmp(method,"5flushEv"))stream.flush();
            else if(!strcmp(method,"lsEi"))stream<<(int32_t)a[1];
            else if(!strcmp(method,"lsEj")||!strcmp(method,"lsEm"))stream<<a[1];
            else if(!strcmp(method,"lsEb"))stream<<(a[1]!=0);
            else if(!strcmp(method,"lsEx")){int64_t v;memcpy(&v,a+1,8);stream<<v;}
            else if(!strcmp(method,"lsEy")){uint64_t v;memcpy(&v,a+1,8);stream<<v;}
            else if(!strcmp(method,"lsEd")){double v;memcpy(&v,a+1,8);stream<<v;}
            else return 0;
            finish(stream,g);
        }
        *out=a[0];return !(g->state&g->exceptions);
    }
    return 0;
}
