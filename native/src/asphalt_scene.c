/* Asphalt's offscreen scene uses a 1280x720 viewport within 2048x1024
 * allocations. Scale both storage and coordinates, retaining normalized UVs.
 * Private MSAA FBOs resolve into the game's original, sampleable textures.
 * All GL calls here go through the selected backend's native trampolines. */
#define GL_SILENCE_DEPRECATION 1
#include "asphalt_scene.h"
#include "game_profile.h"
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CAP = 128 };
struct storage { GLuint id; GLint width, height, format; bool scaled; };
struct framebuffer {
    GLuint id, color, depth, stencil, ms, ms_color, ms_depth, ms_stencil;
    GLint width, height, samples;
    bool dirty, failed, seed_pending;
    unsigned unsupported;
};
struct scene {
    CGLContextObj context;
    struct scene *next;
    struct storage textures[CAP], buffers[CAP];
    struct framebuffer frames[CAP];
    GLuint draw, read;
    GLint viewport[4], scissor[4];
    int scale, samples;
    bool multisample, multisample_saved;
};
static struct scene *contexts;
static pthread_mutex_t context_lock = PTHREAD_MUTEX_INITIALIZER;

static int setting(const char *name, int fallback, int maximum)
{
    const char *text = getenv(name);
    if (!text) return fallback;
    char *end; long value = strtol(text, &end, 10);
    return *end || value < 0 || value > maximum ? fallback : (int)value;
}
static bool enabled(void)
{
    return lp32_profile()->title == LP32_TITLE_ASPHALT6 &&
        (setting("LP32_ASPHALT6_SCENE_SCALE", 1, 4) > 1 ||
         setting("LP32_ASPHALT6_MSAA", 0, 16) > 1);
}
static struct scene *state(void)
{
    CGLContextObj context = CGLGetCurrentContext();
    if (!context || !enabled()) return NULL;
    pthread_mutex_lock(&context_lock);
    struct scene *s = contexts;
    while (s && s->context != context) s = s->next;
    if (!s) {
        s = calloc(1, sizeof(*s));
        if (s) {
            s->context = context;
            s->scale = setting("LP32_ASPHALT6_SCENE_SCALE", 1, 4);
            if (s->scale < 1) s->scale = 1;
            s->samples = setting("LP32_ASPHALT6_MSAA", 0, 16);
            GLint limit = 0, rb_limit = 0; glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
            glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &rb_limit);
            if(rb_limit < limit)limit=rb_limit;
            while (s->scale > 1 && 2048 * s->scale > limit) --s->scale;
            glGetIntegerv(GL_MAX_SAMPLES, &limit);
            if (s->samples > limit) s->samples = limit;
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, (GLint *)&s->draw);
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, (GLint *)&s->read);
            glGetIntegerv(GL_VIEWPORT, s->viewport);
            glGetIntegerv(GL_SCISSOR_BOX, s->scissor);
            s->next = contexts; contexts = s;
            fprintf(stderr, "compat32: Asphalt scene %dx%d, MSAA request=%d device-limit=%d\n",
                1280*s->scale, 720*s->scale,
                setting("LP32_ASPHALT6_MSAA", 0, 16), s->samples);
        }
    }
    pthread_mutex_unlock(&context_lock);
    return s;
}
static struct storage *storage(struct storage *array, GLuint id, bool create)
{
    if (!id) return NULL;
    struct storage *free_slot = NULL;
    for (unsigned i = 0; i < CAP; ++i) {
        if (array[i].id == id) return &array[i];
        if (!array[i].id && !free_slot) free_slot = &array[i];
    }
    if (create && free_slot) { free_slot->id = id; return free_slot; }
    return NULL;
}
static struct framebuffer *frame(struct scene *s, GLuint id, bool create)
{
    if (!id) return NULL;
    struct framebuffer *free_slot = NULL;
    for (unsigned i = 0; i < CAP; ++i) {
        if (s->frames[i].id == id) return &s->frames[i];
        if (!s->frames[i].id && !free_slot) free_slot = &s->frames[i];
    }
    if (create && free_slot) { free_slot->id = id; return free_slot; }
    return NULL;
}
static bool scaled(struct scene *s, GLuint id)
{
    struct framebuffer *f = frame(s, id, false);
    struct storage *t = f ? storage(s->textures, f->color, false) : NULL;
    return t && t->scaled;
}
static void drop_ms(struct framebuffer *f)
{
    if (f->ms) glDeleteFramebuffers(1, &f->ms);
    GLuint buffers[] = {f->ms_color, f->ms_depth, f->ms_stencil};
    glDeleteRenderbuffers(3, buffers);
    f->ms = f->ms_color = f->ms_depth = f->ms_stencil = 0;
    f->dirty = f->failed = false;
}
static void resolve(struct framebuffer *f)
{
    if (!f || !f->ms || !f->dirty) return;
    GLint draw, read; glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    if (scissor) glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, f->ms);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, f->id);
    GLbitfield mask = GL_COLOR_BUFFER_BIT;
    if (f->depth) mask |= GL_DEPTH_BUFFER_BIT;
    if (f->stencil) mask |= GL_STENCIL_BUFFER_BIT;
    glBlitFramebuffer(0,0,f->width,f->height,0,0,f->width,f->height,mask,GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    f->dirty = false;
}
static GLuint make_buffer(GLenum attachment, GLint format, int w, int h, int samples)
{
    GLuint rb; glGenRenderbuffers(1, &rb); glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, format, w, h);
    glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, attachment, GL_RENDERBUFFER, rb);
    return rb;
}
static void build_ms(struct scene *s, struct framebuffer *f)
{
    if (!f || f->ms || f->failed || f->seed_pending || f->unsupported || s->samples < 2 || !scaled(s,f->id)) return;
    struct storage *t = storage(s->textures, f->color, false);
    struct storage *depth = storage(s->buffers, f->depth, false);
    struct storage *stencil = storage(s->buffers, f->stencil, false);
    /* Never redirect a partially constructed framebuffer or invent missing
       depth attachments. Texture depth attachments retain their normal path. */
    GLint draw, read, rb;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &rb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, f->id);
    if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw); return;
    }
    f->width = t->width*s->scale; f->height = t->height*s->scale;
    for (int samples = s->samples; samples >= 2; samples /= 2) {
        glGenFramebuffers(1, &f->ms); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, f->ms);
        GLint format = t->format == GL_RGB ? GL_RGB8 : t->format == GL_RGBA ? GL_RGBA8 : t->format;
        f->ms_color = make_buffer(GL_COLOR_ATTACHMENT0, format, f->width, f->height, samples);
        if (depth) f->ms_depth = make_buffer(GL_DEPTH_ATTACHMENT, depth->format, f->width, f->height, samples);
        if (stencil && stencil == depth)
            glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, f->ms_depth);
        else if (stencil) f->ms_stencil = make_buffer(GL_STENCIL_ATTACHMENT, stencil->format, f->width, f->height, samples);
        GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        glBindRenderbuffer(GL_RENDERBUFFER, f->ms_color);
        GLint actual = 0; glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_SAMPLES, &actual);
        if (status == GL_FRAMEBUFFER_COMPLETE && actual >= 2) {
            f->samples = actual;
            fprintf(stderr, "compat32: Asphalt scene FBO %u: %dx%d, %d actual samples\n", f->id,f->width,f->height,actual);
            break;
        }
        drop_ms(f);
    }
    if (!f->ms) {
        f->failed = true;
        fprintf(stderr,"compat32: Asphalt MSAA allocation failed; retaining high-resolution scene\n");
    }
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
}
static void route(struct scene *s)
{
    struct framebuffer *f = frame(s,s->draw,false);
    build_ms(s,f);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, f && f->ms ? f->ms : s->draw);
    if (f && f->ms) {
        if (!s->multisample_saved) {
            s->multisample = glIsEnabled(GL_MULTISAMPLE); s->multisample_saved = true;
        }
        glEnable(GL_MULTISAMPLE);
    } else if (s->multisample_saved) {
        if (!s->multisample) glDisable(GL_MULTISAMPLE);
        s->multisample_saved = false;
    }
    int k = scaled(s,s->draw) ? s->scale : 1;
    glViewport(s->viewport[0]*k,s->viewport[1]*k,s->viewport[2]*k,s->viewport[3]*k);
    glScissor(s->scissor[0]*k,s->scissor[1]*k,s->scissor[2]*k,s->scissor[3]*k);
}
void asphalt_scene_before_draw(void)
{
    if (!enabled()) return;
    struct scene *s = state(); if (!s) return;
    route(s);
    struct framebuffer *f = frame(s,s->draw,false);
    if (f && f->ms) f->dirty = true;
}
void asphalt_scene_resolve_texture(unsigned texture)
{
    if (!enabled()) return;
    struct scene *s = state(); if (!s) return;
    for (unsigned i=0;i<CAP;++i) if (s->frames[i].color==texture) resolve(&s->frames[i]);
}
static void invalidate(struct scene *s, GLuint id, bool texture)
{
    for (unsigned i=0;i<CAP;++i) {
        struct framebuffer *f=&s->frames[i];
        if ((texture && f->color==id) || (!texture && (f->depth==id || f->stencil==id))) {
            resolve(f); drop_ms(f);
        }
    }
}
void asphalt_scene_delete_textures(int count,const unsigned *textures)
{
    if (!enabled()) return;
    struct scene *s=state(); if(!s)return;
    for(int i=0;i<count;++i) {
        invalidate(s,textures[i],true);
        struct storage *t=storage(s->textures,textures[i],false); if(t)memset(t,0,sizeof(*t));
        for(unsigned j=0;j<CAP;++j)if(s->frames[j].color==textures[i])s->frames[j].color=0;
    }
    route(s);
}
void asphalt_scene_forget_context(void *context)
{
    pthread_mutex_lock(&context_lock);
    struct scene **p=&contexts;
    while(*p && (*p)->context!=context)p=&(*p)->next;
    if(*p){struct scene *old=*p;*p=old->next;free(old);}
    pthread_mutex_unlock(&context_lock);
    /* GL itself releases the context's objects; no other context is touched. */
}
static bool base_size(int w,int h)
{
    return (w==2048 && h==1024) || (w==1280 && h==720);
}
static uint64_t bind_frame(const uint32_t *a,uint32_t site)
{
    (void)site; struct scene *s=state(); if(!s){glBindFramebuffer(a[0],a[1]);return 0;}
    GLenum target=a[0];
    if(target!=GL_FRAMEBUFFER && target!=GL_DRAW_FRAMEBUFFER && target!=GL_READ_FRAMEBUFFER){glBindFramebuffer(target,a[1]);return 0;}
    if(target!=GL_READ_FRAMEBUFFER){resolve(frame(s,s->draw,false));s->draw=a[1];}
    if(target!=GL_DRAW_FRAMEBUFFER){resolve(frame(s,a[1],false));s->read=a[1];glBindFramebuffer(GL_READ_FRAMEBUFFER,a[1]);}
    frame(s,a[1],true); route(s); return 0;
}
static uint64_t tex_image(const uint32_t *a,uint32_t site)
{
    (void)site; struct scene *s=state();GLuint id=0;
    if(s && a[0]==GL_TEXTURE_2D && a[1]==0) glGetIntegerv(GL_TEXTURE_BINDING_2D,(GLint *)&id);
    int w=a[3],h=a[4];
    GLint unpack=0;glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING,&unpack);
    bool candidate=id && !unpack && !a[8] && !a[5] && base_size(w,h) &&
        (a[2]==GL_RGB || a[2]==GL_RGBA || a[2]==GL_RGB8 || a[2]==GL_RGBA8);
    /* Asset uploads must not exhaust the scene target table before the first
       race. Offset zero in a pixel-unpack buffer is data, not a null upload. */
    struct storage *t=s?storage(s->textures,id,candidate):NULL;
    if(t){invalidate(s,id,true);t->width=w;t->height=h;t->format=a[2];
        t->scaled=candidate;
        if(t->scaled){w*=s->scale;h*=s->scale;}}
    glTexImage2D(a[0],a[1],a[2],w,h,a[5],a[6],a[7],(void *)(uintptr_t)a[8]);
    if(s)route(s);return 0;
}
static uint64_t rb_storage(const uint32_t *a,uint32_t site)
{
    (void)site;struct scene *s=state();GLuint id=0;glGetIntegerv(GL_RENDERBUFFER_BINDING,(GLint *)&id);
    struct storage *r=s?storage(s->buffers,id,true):NULL;int w=a[2],h=a[3];
    if(r){invalidate(s,id,false);r->width=w;r->height=h;r->format=a[1];r->scaled=base_size(w,h);if(r->scaled){w*=s->scale;h*=s->scale;}}
    glRenderbufferStorage(a[0],a[1],w,h);
    if(r)glGetRenderbufferParameteriv(GL_RENDERBUFFER,GL_RENDERBUFFER_INTERNAL_FORMAT,&r->format);
    if(s)route(s);return 0;
}
static uint64_t attach_texture(const uint32_t *a,uint32_t site)
{
    (void)site;struct scene *s=state();if(!s){glFramebufferTexture2D(a[0],a[1],a[2],a[3],a[4]);return 0;}
    GLuint id=a[0]==GL_READ_FRAMEBUFFER?s->read:s->draw;
    struct framebuffer *f=frame(s,id,true);if(f){resolve(f);drop_ms(f);if(a[1]==GL_COLOR_ATTACHMENT0)f->color=a[2]==GL_TEXTURE_2D && !a[4]?a[3]:0;
        unsigned bit = a[1]==GL_DEPTH_ATTACHMENT ? 1u<<16 : a[1]==GL_STENCIL_ATTACHMENT ? 1u<<17 :
            a[1]>GL_COLOR_ATTACHMENT0 && a[1]<=GL_COLOR_ATTACHMENT15 ? 1u<<(a[1]-GL_COLOR_ATTACHMENT0) : 0;
        if(a[3])f->unsupported|=bit;else f->unsupported&=~bit;}
    glBindFramebuffer(a[0],id);glFramebufferTexture2D(a[0],a[1],a[2],a[3],a[4]);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,s->read);route(s);return 0;
}
static uint64_t attach_buffer(const uint32_t *a,uint32_t site)
{
    (void)site;struct scene *s=state();if(!s){glFramebufferRenderbuffer(a[0],a[1],a[2],a[3]);return 0;}
    GLuint id=a[0]==GL_READ_FRAMEBUFFER?s->read:s->draw;
    struct framebuffer *f=frame(s,id,true);if(f){resolve(f);drop_ms(f);
        if(a[1]==GL_DEPTH_ATTACHMENT || a[1]==GL_DEPTH_STENCIL_ATTACHMENT){f->depth=a[3];f->unsupported&=~(1u<<16);}
        if(a[1]==GL_STENCIL_ATTACHMENT || a[1]==GL_DEPTH_STENCIL_ATTACHMENT){f->stencil=a[3];f->unsupported&=~(1u<<17);}}
    glBindFramebuffer(a[0],id);glFramebufferRenderbuffer(a[0],a[1],a[2],a[3]);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,s->read);route(s);return 0;
}
static uint64_t viewport(const uint32_t *a,uint32_t site)
{
    (void)site;struct scene *s=state();if(!s){glViewport(a[0],a[1],a[2],a[3]);return 0;}
    memcpy(s->viewport,a,16);int k=scaled(s,s->draw)?s->scale:1;glViewport((int)a[0]*k,(int)a[1]*k,a[2]*k,a[3]*k);return 0;
}
static uint64_t scissor(const uint32_t *a,uint32_t site)
{
    (void)site;struct scene *s=state();if(!s){glScissor(a[0],a[1],a[2],a[3]);return 0;}
    memcpy(s->scissor,a,16);int k=scaled(s,s->draw)?s->scale:1;glScissor((int)a[0]*k,(int)a[1]*k,a[2]*k,a[3]*k);return 0;
}
static uint64_t integers(const uint32_t *a,uint32_t site)
{
    (void)site;struct scene *s=state();GLint *out=(void *)(uintptr_t)a[1];
    if(s && a[0]==GL_DRAW_FRAMEBUFFER_BINDING){*out=s->draw;return 0;}
    if(s && a[0]==GL_READ_FRAMEBUFFER_BINDING){*out=s->read;return 0;}
    if(s && a[0]==GL_VIEWPORT){memcpy(out,s->viewport,16);return 0;}
    if(s && a[0]==GL_SCISSOR_BOX){memcpy(out,s->scissor,16);return 0;}
    glGetIntegerv(a[0],out);return 0;
}
static uint64_t floats(const uint32_t *a,uint32_t site)
{
    (void)site;struct scene *s=state();GLfloat *out=(void *)(uintptr_t)a[1];
    const GLint *v=s && a[0]==GL_VIEWPORT?s->viewport:s && a[0]==GL_SCISSOR_BOX?s->scissor:NULL;
    if(v){for(unsigned i=0;i<4;++i)out[i]=v[i];return 0;}glGetFloatv(a[0],out);return 0;
}
static uint64_t tex_level(const uint32_t *a,uint32_t site)
{
    (void)site;struct scene *s=state();GLuint id=0;glGetIntegerv(GL_TEXTURE_BINDING_2D,(GLint *)&id);
    struct storage *t=s && a[0]==GL_TEXTURE_2D && !a[1]?storage(s->textures,id,false):NULL;
    GLint *out=(void *)(uintptr_t)a[3];
    if(t && t->scaled && (a[2]==GL_TEXTURE_WIDTH || a[2]==GL_TEXTURE_HEIGHT)) {*out=a[2]==GL_TEXTURE_WIDTH?t->width:t->height;return 0;}
    glGetTexLevelParameteriv(a[0],a[1],a[2],out);return 0;
}
static uint64_t rb_parameter(const uint32_t *a,uint32_t site)
{
    (void)site;struct scene *s=state();GLuint id=0;glGetIntegerv(GL_RENDERBUFFER_BINDING,(GLint *)&id);
    struct storage *r=s?storage(s->buffers,id,false):NULL;GLint *out=(void *)(uintptr_t)a[2];
    if(r && r->scaled && (a[1]==GL_RENDERBUFFER_WIDTH || a[1]==GL_RENDERBUFFER_HEIGHT)){*out=a[1]==GL_RENDERBUFFER_WIDTH?r->width:r->height;return 0;}
    glGetRenderbufferParameteriv(a[0],a[1],out);return 0;
}
static uint64_t status(const uint32_t *a,uint32_t site)
{
    (void)site;struct scene *s=state();if(!s)return glCheckFramebufferStatus(a[0]);
    GLuint id=a[0]==GL_READ_FRAMEBUFFER?s->read:s->draw;
    glBindFramebuffer(a[0],id);GLenum result=glCheckFramebufferStatus(a[0]);glBindFramebuffer(GL_READ_FRAMEBUFFER,s->read);route(s);return result;
}
static uint64_t attachment(const uint32_t *a,uint32_t site)
{
    (void)site;struct scene *s=state();if(s)glBindFramebuffer(a[0],a[0]==GL_READ_FRAMEBUFFER?s->read:s->draw);
    glGetFramebufferAttachmentParameteriv(a[0],a[1],a[2],(void *)(uintptr_t)a[3]);
    if(s){glBindFramebuffer(GL_READ_FRAMEBUFFER,s->read);route(s);}return 0;
}
static uint64_t clear(const uint32_t *a,uint32_t site)
{
    (void)site;struct scene *s=state();
    struct framebuffer *f=s?frame(s,s->draw,false):NULL;
    if(f && f->seed_pending && (a[0]&GL_COLOR_BUFFER_BIT) && !glIsEnabled(GL_SCISSOR_TEST)) {
        GLboolean masks[4];glGetBooleanv(GL_COLOR_WRITEMASK,masks);
        GLboolean depth_write=GL_TRUE;
        if(f->depth)glGetBooleanv(GL_DEPTH_WRITEMASK,&depth_write);
        GLint stencil_write=-1;
        if(f->stencil)glGetIntegerv(GL_STENCIL_WRITEMASK,&stencil_write);
        bool depth_clear=!f->depth || ((a[0]&GL_DEPTH_BUFFER_BIT) && depth_write);
        bool stencil_clear=!f->stencil || ((a[0]&GL_STENCIL_BUFFER_BIT) && (stencil_write&255)==255);
        if(masks[0] && masks[1] && masks[2] && masks[3] && depth_clear && stencil_clear)f->seed_pending=false;
    }
    asphalt_scene_before_draw();glClear(a[0]);return 0;
}
static uint64_t delete_frames(const uint32_t *a,uint32_t site)
{
    (void)site;struct scene *s=state();const GLuint *ids=(void *)(uintptr_t)a[1];
    if(s){for(unsigned i=0;i<a[0];++i){struct framebuffer *f=frame(s,ids[i],false);if(f){drop_ms(f);memset(f,0,sizeof(*f));}if(s->draw==ids[i])s->draw=0;if(s->read==ids[i])s->read=0;}}
    glDeleteFramebuffers(a[0],ids);if(s){glBindFramebuffer(GL_READ_FRAMEBUFFER,s->read);route(s);}return 0;
}
static uint64_t delete_buffers(const uint32_t *a,uint32_t site)
{
    (void)site;struct scene *s=state();const GLuint *ids=(void *)(uintptr_t)a[1];
    if(s){for(unsigned i=0;i<a[0];++i){invalidate(s,ids[i],false);struct storage *r=storage(s->buffers,ids[i],false);if(r)memset(r,0,sizeof(*r));for(unsigned j=0;j<CAP;++j){if(s->frames[j].depth==ids[i])s->frames[j].depth=0;if(s->frames[j].stencil==ids[i])s->frames[j].stencil=0;}}}
    glDeleteRenderbuffers(a[0],ids);if(s)route(s);return 0;
}
static uint64_t blit(const uint32_t *a,uint32_t site)
{
    (void)site;struct scene *s=state();uint32_t b[10];memcpy(b,a,sizeof(b));
    if(s){resolve(frame(s,s->read,false));int r=scaled(s,s->read)?s->scale:1,d=scaled(s,s->draw)?s->scale:1;
        for(unsigned i=0;i<4;++i){b[i]=(int)a[i]*r;b[i+4]=(int)a[i+4]*d;}
        /* A guest blit targets the resolved image, not a multisample alias. */
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,s->draw);glBindFramebuffer(GL_READ_FRAMEBUFFER,s->read);}
    glBlitFramebuffer(b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9]);
    if(s){struct framebuffer *f=frame(s,s->draw,false);if(f){drop_ms(f);f->seed_pending=true;}route(s);}return 0;
}
static uint64_t copy_texture(const uint32_t *a,uint32_t site)
{
    (void)site;struct scene *s=state();GLuint texture=0;glGetIntegerv(GL_TEXTURE_BINDING_2D,(GLint *)&texture);
    struct storage *t=s?storage(s->textures,texture,false):NULL;
    int src=s && scaled(s,s->read)?s->scale:1,dst=t && t->scaled?s->scale:1;
    if(s){resolve(frame(s,s->read,false));invalidate(s,texture,true);
        for(unsigned i=0;i<CAP;++i)if(s->frames[i].color==texture)s->frames[i].seed_pending=true;
    }
    if(src==dst){glCopyTexSubImage2D(a[0],a[1],(int)a[2]*dst,(int)a[3]*dst,(int)a[4]*src,(int)a[5]*src,a[6]*src,a[7]*src);return 0;}
    if(a[0]!=GL_TEXTURE_2D || a[1]){glCopyTexSubImage2D(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]);return 0;}
    GLuint tmp;GLint draw,read;glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&draw);glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read);
    glGenFramebuffers(1,&tmp);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,tmp);glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    GLboolean clip=glIsEnabled(GL_SCISSOR_TEST);if(clip)glDisable(GL_SCISSOR_TEST);
    if(s)glBindFramebuffer(GL_READ_FRAMEBUFFER,s->read);
    glBlitFramebuffer((int)a[4]*src,(int)a[5]*src,((int)a[4]+(int)a[6])*src,((int)a[5]+(int)a[7])*src,
        (int)a[2]*dst,(int)a[3]*dst,((int)a[2]+(int)a[6])*dst,((int)a[3]+(int)a[7])*dst,GL_COLOR_BUFFER_BIT,GL_LINEAR);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,draw);glBindFramebuffer(GL_READ_FRAMEBUFFER,read);glDeleteFramebuffers(1,&tmp);if(clip)glEnable(GL_SCISSOR_TEST);return 0;
}
/* Readback keeps the caller's requested byte count. Downsample into a
 * temporary single-sample buffer before glReadPixels, never enlarge its write. */
static uint64_t read_pixels(const uint32_t *a,uint32_t site)
{
    (void)site;struct scene *s=state();
    if(!s || !scaled(s,s->read) || (int)a[2]<=0 || (int)a[3]<=0){
        glReadPixels(a[0],a[1],a[2],a[3],a[4],a[5],(void *)(uintptr_t)a[6]);return 0;
    }
    resolve(frame(s,s->read,false));
    GLint draw,read,rb;glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&draw);glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read);glGetIntegerv(GL_RENDERBUFFER_BINDING,&rb);
    GLuint temp,buffer;glGenFramebuffers(1,&temp);glBindFramebuffer(GL_FRAMEBUFFER,temp);
    glGenRenderbuffers(1,&buffer);glBindRenderbuffer(GL_RENDERBUFFER,buffer);
    bool depth=a[4]==GL_DEPTH_COMPONENT || a[4]==GL_DEPTH_STENCIL || a[4]==GL_STENCIL_INDEX;
    glRenderbufferStorage(GL_RENDERBUFFER,depth?GL_DEPTH24_STENCIL8:GL_RGBA8,a[2],a[3]);
    glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER,depth?GL_DEPTH_STENCIL_ATTACHMENT:GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,buffer);
    if(depth){glDrawBuffer(GL_NONE);glReadBuffer(GL_NONE);}
    glBindFramebuffer(GL_READ_FRAMEBUFFER,s->read);
    GLboolean clip=glIsEnabled(GL_SCISSOR_TEST);if(clip)glDisable(GL_SCISSOR_TEST);
    GLbitfield mask=a[4]==GL_DEPTH_COMPONENT?GL_DEPTH_BUFFER_BIT:a[4]==GL_STENCIL_INDEX?GL_STENCIL_BUFFER_BIT:a[4]==GL_DEPTH_STENCIL?GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT:GL_COLOR_BUFFER_BIT;
    glBlitFramebuffer((int)a[0]*s->scale,(int)a[1]*s->scale,((int)a[0]+(int)a[2])*s->scale,((int)a[1]+(int)a[3])*s->scale,
        0,0,a[2],a[3],mask,depth?GL_NEAREST:GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,temp);
    glReadPixels(0,0,a[2],a[3],a[4],a[5],(void *)(uintptr_t)a[6]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,draw);glBindFramebuffer(GL_READ_FRAMEBUFFER,read);glBindRenderbuffer(GL_RENDERBUFFER,rb);
    glDeleteFramebuffers(1,&temp);glDeleteRenderbuffers(1,&buffer);if(clip)glEnable(GL_SCISSOR_TEST);return 0;
}
static uint64_t enable_cap(const uint32_t *a,uint32_t site)
{
    (void)site;struct scene *s=state();if(s && a[0]==GL_MULTISAMPLE && s->multisample_saved)s->multisample=true;
    glEnable(a[0]);return 0;
}
static uint64_t disable_cap(const uint32_t *a,uint32_t site)
{
    (void)site;struct scene *s=state();if(s && a[0]==GL_MULTISAMPLE && s->multisample_saved){s->multisample=false;return 0;}
    glDisable(a[0]);return 0;
}
lp32_fast_import_fn asphalt_scene_fast_import(const char *name)
{
    if(!enabled())return NULL;if(*name=='_')++name;
    static const struct {const char *name;lp32_fast_import_fn fn;} handlers[]={
        {"glBindFramebuffer",bind_frame},{"glBindFramebufferEXT",bind_frame},
        {"glTexImage2D",tex_image},{"glRenderbufferStorage",rb_storage},{"glRenderbufferStorageEXT",rb_storage},
        {"glFramebufferTexture2D",attach_texture},{"glFramebufferTexture2DEXT",attach_texture},
        {"glFramebufferRenderbuffer",attach_buffer},{"glFramebufferRenderbufferEXT",attach_buffer},
        {"glViewport",viewport},{"glScissor",scissor},{"glGetIntegerv",integers},{"glGetFloatv",floats},
        {"glGetTexLevelParameteriv",tex_level},{"glGetRenderbufferParameteriv",rb_parameter},{"glGetRenderbufferParameterivEXT",rb_parameter},
        {"glCheckFramebufferStatus",status},{"glCheckFramebufferStatusEXT",status},
        {"glGetFramebufferAttachmentParameteriv",attachment},{"glGetFramebufferAttachmentParameterivEXT",attachment},
        {"glClear",clear},{"glReadPixels",read_pixels},{"glEnable",enable_cap},{"glDisable",disable_cap},{"glDeleteFramebuffers",delete_frames},{"glDeleteFramebuffersEXT",delete_frames},
        {"glDeleteRenderbuffers",delete_buffers},{"glDeleteRenderbuffersEXT",delete_buffers},
        {"glBlitFramebuffer",blit},{"glBlitFramebufferEXT",blit},{"glCopyTexSubImage2D",copy_texture}
    };
    for(unsigned i=0;i<sizeof(handlers)/sizeof(*handlers);++i)if(!strcmp(name,handlers[i].name))return handlers[i].fn;
    return NULL;
}
