#define GL_SILENCE_DEPRECATION 1
#include "asphalt_scene.h"
#include "game_profile.h"
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
extern void glBegin(GLenum);
extern void glEnd(void);
extern void glVertex2f(GLfloat,GLfloat);
extern void glColor4f(GLfloat,GLfloat,GLfloat,GLfloat);
static struct lp32_game_profile profile={.title=LP32_TITLE_ASPHALT6};
const struct lp32_game_profile *lp32_profile(void){return &profile;}
static uint8_t *memory;
static uint64_t call(const char *name,const uint32_t *a){lp32_fast_import_fn f=asphalt_scene_fast_import(name);assert(f);return f(a,0);}
static void bind(GLenum target,GLuint fbo){uint32_t a[]={target,fbo};call("glBindFramebufferEXT",a);}
static void rect(const char *name,int x,int y,int w,int h){uint32_t a[]={x,y,w,h};call(name,a);}
static void image(int w,int h,GLuint data){uint32_t a[]={GL_TEXTURE_2D,0,GL_RGBA8,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,data};call("glTexImage2D",a);}
static GLuint shader(GLenum type,const char *text){GLuint s=glCreateShader(type);glShaderSource(s,1,&text,NULL);glCompileShader(s);GLint ok;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);assert(ok);return s;}
static void no_error(void){GLenum e=glGetError();if(e)fprintf(stderr,"GL ERROR %04x\n",e);assert(e==GL_NO_ERROR);}
int main(void){
 setenv("LP32_ASPHALT6_SCENE_SCALE","3",1);setenv("LP32_ASPHALT6_MSAA","8",1);
 memory=mmap((void *)0x10000000,16384,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE|MAP_FIXED,-1,0);assert(memory==(void *)0x10000000);
 profile.title=LP32_TITLE_GANGSTAR;assert(!asphalt_scene_fast_import("glViewport"));profile.title=LP32_TITLE_ASPHALT6;
 CGLPixelFormatAttribute attrs[]={kCGLPFAOpenGLProfile,(CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,kCGLPFAAccelerated,0};
 bool legacy=getenv("LP32_SCENE_TEST_LEGACY")!=NULL;
 CGLPixelFormatAttribute legacy_attrs[]={kCGLPFAAccelerated,0};
 CGLPixelFormatObj pf;GLint n;CGLContextObj ctx;assert(CGLChoosePixelFormat(legacy?legacy_attrs:attrs,&pf,&n)==kCGLNoError);assert(CGLCreateContext(pf,NULL,&ctx)==kCGLNoError);CGLDestroyPixelFormat(pf);CGLSetCurrentContext(ctx);
 /* Hundreds of ordinary assets cannot consume the scene-target registry. */
 GLuint assets[200];glGenTextures(200,assets);
 for(unsigned i=0;i<200;++i){glBindTexture(GL_TEXTURE_2D,assets[i]);image(8,8,0);}
 /* A zero pixel pointer with a PBO bound means offset zero, not allocation. */
 GLuint pbo,pbo_texture;glGenBuffers(1,&pbo);glBindBuffer(GL_PIXEL_UNPACK_BUFFER,pbo);
 glBufferData(GL_PIXEL_UNPACK_BUFFER,1280*720*4,NULL,GL_STATIC_DRAW);
 glGenTextures(1,&pbo_texture);glBindTexture(GL_TEXTURE_2D,pbo_texture);image(1280,720,0);
 GLint pbo_width=0;glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&pbo_width);assert(pbo_width==1280);
 glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);glDeleteBuffers(1,&pbo);no_error();
 GLuint tex,fbo,depth;glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);image(1280,720,0);
 GLint width=0;glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&width);assert(width==3840);
 uint32_t query[]={GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,0x10000000};call("glGetTexLevelParameteriv",query);assert(*(GLint *)memory==1280);
 glGenFramebuffers(1,&fbo);bind(GL_FRAMEBUFFER,fbo);
 uint32_t attach[]={GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0};call("glFramebufferTexture2DEXT",attach);
 glGenRenderbuffers(1,&depth);glBindRenderbuffer(GL_RENDERBUFFER,depth);
 uint32_t storage[]={GL_RENDERBUFFER,GL_DEPTH24_STENCIL8,1280,720};call("glRenderbufferStorageEXT",storage);
 uint32_t depth_attach[]={GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_RENDERBUFFER,depth};call("glFramebufferRenderbufferEXT",depth_attach);
 uint32_t target=GL_FRAMEBUFFER;assert(call("glCheckFramebufferStatusEXT",&target)==GL_FRAMEBUFFER_COMPLETE);
 uint32_t binding_query[]={GL_FRAMEBUFFER_BINDING,0x10000000};call("glGetIntegerv",binding_query);assert(*(GLuint *)memory==fbo);
 GLint actual;glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&actual);assert((GLuint)actual!=fbo);
 GLint color_rb;glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,&color_rb);glBindRenderbuffer(GL_RENDERBUFFER,color_rb);
 GLint samples;glGetRenderbufferParameteriv(GL_RENDERBUFFER,GL_RENDERBUFFER_SAMPLES,&samples);assert(samples>=2);glBindRenderbuffer(GL_RENDERBUFFER,depth);
 rect("glViewport",0,0,1280,720);rect("glScissor",0,0,1280,720);
 GLint vp[4];glGetIntegerv(GL_VIEWPORT,vp);assert(vp[2]==3840 && vp[3]==2160);
 uint32_t vpquery[]={GL_VIEWPORT,0x10000000};call("glGetIntegerv",vpquery);assert(((GLint *)memory)[2]==1280);
 glDisable(GL_SCISSOR_TEST);glClearColor(0,0,0,1);uint32_t mask=GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT;call("glClear",&mask);
 if(legacy){asphalt_scene_before_draw();glColor4f(1,1,1,1);glBegin(GL_TRIANGLES);glVertex2f(-1,-1);glVertex2f(1,-1);glVertex2f(1,1);glEnd();}
 else {
 GLuint vs=shader(GL_VERTEX_SHADER,"#version 150\nvoid main(){vec2 p[3]=vec2[3](vec2(-1,-1),vec2(1,-1),vec2(1,1));gl_Position=vec4(p[gl_VertexID],0,1);}");
 GLuint fs=shader(GL_FRAGMENT_SHADER,"#version 150\nout vec4 color;void main(){color=vec4(1.0);}");GLuint program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,fs);glLinkProgram(program);glUseProgram(program);GLuint vao;glGenVertexArrays(1,&vao);glBindVertexArray(vao);
 asphalt_scene_before_draw();glDrawArrays(GL_TRIANGLES,0,3);
 }
 no_error();
 /* This readback must resolve and return only one caller-sized pixel. */
 memset(memory,0xa5,32);uint32_t pixel[]={1000,100,1,1,GL_RGBA,GL_UNSIGNED_BYTE,0x10000000};call("glReadPixels",pixel);assert(memory[0]==255 && memory[4]==0xa5);no_error();
 bind(GL_READ_FRAMEBUFFER,fbo);
 unsigned char p[4];glReadPixels(3500,200,1,1,GL_RGBA,GL_UNSIGNED_BYTE,p);assert(p[0]==255); /* beyond the original 1280-pixel corner */
 /* A diagonal crossing sample locations must contain actual MSAA coverage. */
 unsigned char *row=malloc(3840*4);glReadPixels(0,1000,3840,1,GL_RGBA,GL_UNSIGNED_BYTE,row);unsigned partial=0;
 for(unsigned i=0;i<3840;++i)if(row[i*4]>0 && row[i*4]<255)++partial;assert(partial>0);free(row);
 /* Depth readback must not mutate the source framebuffer's read buffer. */
 uint32_t depth_pixel[]={1000,100,1,1,GL_DEPTH_COMPONENT,GL_FLOAT,0x10000000};call("glReadPixels",depth_pixel);no_error();
 glReadPixels(3500,200,1,1,GL_RGBA,GL_UNSIGNED_BYTE,p);assert(p[0]==255);
 rect("glScissor",1000,100,10,10);uint32_t scissor_cap=GL_SCISSOR_TEST;call("glEnable",&scissor_cap);
 glClearColor(1,0,0,1);uint32_t color_mask=GL_COLOR_BUFFER_BIT;call("glClear",&color_mask);
 bind(GL_READ_FRAMEBUFFER,fbo);glReadPixels(3005,305,1,1,GL_RGBA,GL_UNSIGNED_BYTE,p);assert(p[0]==255 && p[1]==0);
 glReadPixels(3040,305,1,1,GL_RGBA,GL_UNSIGNED_BYTE,p);assert(p[1]==255);call("glDisable",&scissor_cap);
 uint32_t ms_cap=GL_MULTISAMPLE;call("glDisable",&ms_cap);assert(glIsEnabled(GL_MULTISAMPLE));
 rect("glScissor",0,0,1280,720);
 /* Scene-to-normal texture copy must downsample rather than overflow. */
 GLuint copy;glGenTextures(1,&copy);glBindTexture(GL_TEXTURE_2D,copy);image(16,16,0);
 uint32_t copy_args[]={GL_TEXTURE_2D,0,0,0,1000,100,16,16};call("glCopyTexSubImage2D",copy_args);no_error();
 GLuint verify;glGenFramebuffers(1,&verify);glBindFramebuffer(GL_READ_FRAMEBUFFER,verify);glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,copy,0);glReadPixels(8,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,p);assert(p[0]==255);bind(GL_READ_FRAMEBUFFER,fbo);
 /* Switching to a menu/postprocess target restores logical rectangles. */
 bind(GL_FRAMEBUFFER,0);assert(!glIsEnabled(GL_MULTISAMPLE));glGetIntegerv(GL_VIEWPORT,vp);assert(vp[2]==1280 && vp[3]==720);glGetIntegerv(GL_SCISSOR_BOX,vp);assert(vp[2]==1280);
 /* Padded scene storage preserves the game's existing texture UV ratios. */
 GLuint padded;glGenTextures(1,&padded);glBindTexture(GL_TEXTURE_2D,padded);image(2048,1024,0);glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&width);assert(width==6144);
 /* Image data and shadow-sized resources never get mistaken for the scene. */
 GLuint shadow;glGenTextures(1,&shadow);glBindTexture(GL_TEXTURE_2D,shadow);image(1024,1024,0);glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&width);assert(width==1024);
 /* Reallocation destroys the old alias before attachment state is reused. */
 glBindTexture(GL_TEXTURE_2D,tex);image(64,64,0);bind(GL_FRAMEBUFFER,fbo);glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&actual);assert((GLuint)actual==fbo);
 *(GLuint *)memory=fbo;uint32_t del[]={1,0x10000000};call("glDeleteFramebuffersEXT",del);glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&actual);assert(actual==0);
 asphalt_scene_forget_context(ctx);CGLSetCurrentContext(NULL);CGLDestroyContext(ctx);
 printf("Asphalt scene: PASS (3840x2160 pixels, %d actual MSAA samples, fractional coverage, resolve, copy, logical state, realloc/delete, title isolation)\n",samples);
 return 0;
}
