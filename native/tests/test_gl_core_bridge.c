#define GL_SILENCE_DEPRECATION 1
#include "gl_core_bridge.h"
#include "game_profile.h"
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
/* The index-buffer copies live in objc_bridge.m; this test has none bound. */
int objc_bridge32_index_shadow_acquire(uint32_t offset,const void **pointer,uint32_t *buffer){(void)offset;(void)pointer;(void)buffer;return 0;}
void objc_bridge32_reconcile_fragment_shadows(void){}
void objc_bridge32_index_shadow_release(uint32_t buffer){(void)buffer;}
static uint64_t call(const char *name,const uint32_t *args){uint64_t out=0;assert(gl_core_bridge32_dispatch(name,args,&out));return out;}
const struct lp32_game_profile *lp32_profile(void){static const struct lp32_game_profile profile={0};return &profile;}
extern void glGetMaterialfv(uint32_t, uint32_t, float *);
extern void glBegin(uint32_t);
extern void glEnd(void);
extern void glVertex2f(float, float);
static void check_shadow_rewrite(void) {
    const char *plain = "#version 120\nvoid main(){gl_FragColor=vec4(1.0);}\n";
    assert(gl_core_bridge32_rewrite_glsl_shadows(plain, strlen(plain)) == NULL);
    const char *embedded = "#version 120\nvoid main(){float myshadow2DProj=1.0; gl_FragColor=vec4(myshadow2DProj);}\n";
    assert(gl_core_bridge32_rewrite_glsl_shadows(embedded, strlen(embedded)) == NULL);
    const char *projective =
        "#version 120\n"
        "uniform sampler2DShadow s;\n"
        "uniform vec4 c;\n"
        "void main(){gl_FragColor=shadow2DProj(s, c);}\n";
    char *rewritten = gl_core_bridge32_rewrite_glsl_shadows(projective, strlen(projective));
    assert(rewritten);
    assert(strncmp(rewritten, "#version 120\n", 13) == 0);
    assert(strstr(rewritten, "vec4 lp32_shadow2DProj"));
    assert(strstr(rewritten, "#define shadow2DProj"));
    assert(strstr(rewritten, "return vec4(1.0);"));
    free(rewritten);
    const char *rectangle = "#version 120\nshadow2DRectProj(s, c);\n";
    rewritten = gl_core_bridge32_rewrite_glsl_shadows(rectangle, strlen(rectangle));
    assert(rewritten);
    assert(strstr(rewritten, "#define shadow2DRectProj"));
    assert(!strstr(rewritten, "#define shadow2DProj("));
    free(rewritten);
}
int main(void){
    check_shadow_rewrite();
    uint8_t *memory=mmap((void *)0x10000000,4096,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE|MAP_FIXED,-1,0);
    assert(memory==(void *)0x10000000);
    CGLPixelFormatAttribute attrs[]={kCGLPFAOpenGLProfile,(CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,kCGLPFAAccelerated,0};
    CGLPixelFormatObj format;GLint count;CGLContextObj context;
    assert(CGLChoosePixelFormat(attrs,&format,&count)==kCGLNoError);
    assert(CGLCreateContext(format,NULL,&context)==kCGLNoError);CGLDestroyPixelFormat(format);
    assert(CGLSetCurrentContext(context)==kCGLNoError);
    const char *source[]={"#version 150\nvoid main(){vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));gl_Position=vec4(p[gl_VertexID],0,1);}",
        "#version 150\nuniform vec4 color;out vec4 pixel;void main(){pixel=color;}"};
    uint32_t shaders[2];
    for(unsigned i=0;i<2;++i){
        uint32_t a[]={i?GL_FRAGMENT_SHADER:GL_VERTEX_SHADER};shaders[i]=(uint32_t)call("_glCreateShader",a);
        strcpy((char *)memory+128,source[i]);*(uint32_t *)memory=0x10000080;
        uint32_t text[]={shaders[i],1,0x10000000,0};call("_glShaderSource",text);
        call("_glCompileShader",&shaders[i]);
        uint32_t status[]={shaders[i],GL_COMPILE_STATUS,0x10000300};
        *(uint32_t *)(memory+772)=0xabcdef01;call("_glGetShaderiv",status);
        assert(*(uint32_t *)(memory+768)==GL_TRUE&&*(uint32_t *)(memory+772)==0xabcdef01);
    }
    uint32_t program=(uint32_t)call("_glCreateProgram",NULL);
    for(unsigned i=0;i<2;++i){uint32_t a[]={program,shaders[i]};call("_glAttachShader",a);}
    call("_glLinkProgram",&program);uint32_t status[]={program,GL_LINK_STATUS,0x10000300};
    call("_glGetProgramiv",status);assert(*(uint32_t *)(memory+768)==GL_TRUE);call("_glUseProgram",&program);
    strcpy((char *)memory+128,"color");uint32_t uniform[]={program,0x10000080};
    uint32_t location=(uint32_t)call("_glGetUniformLocation",uniform);assert((int32_t)location>=0);
    float color[]={0.25,0.5,0.75,1};memcpy(memory+128,color,16);uint32_t set[]={location,1,0x10000080};call("_glUniform4fv",set);
    GLuint texture,fbo;glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,4,4,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
    uint32_t vaoargs[]={1,0x10000300};call("_glGenVertexArrays",vaoargs);GLuint vao=*(uint32_t *)(memory+768);call("_glBindVertexArray",&vao);
    glViewport(0,0,4,4);glDrawArrays(GL_TRIANGLES,0,3);
    uint32_t pack[]={GL_PACK_ALIGNMENT,1};call("_glPixelStorei",pack);
    memset(memory+1024,0xa5,8);
    uint32_t read[]={1,1,1,1,GL_RGBA,GL_UNSIGNED_BYTE,0x10000400};
    call("_glReadPixels",read);
    uint8_t *pixel=memory+1024;
    assert(*(uint32_t *)(pixel+4)==0xa5a5a5a5);
    assert(pixel[0]>=63&&pixel[0]<=64&&pixel[1]>=127&&pixel[1]<=128&&pixel[2]>=191&&pixel[2]<=192&&pixel[3]==255);
    assert(glGetError()==GL_NO_ERROR);
    call("_glDeleteProgram",&program);for(unsigned i=0;i<2;++i)call("_glDeleteShader",&shaders[i]);
    glDeleteVertexArrays(1,&vao);glDeleteFramebuffers(1,&fbo);glDeleteTextures(1,&texture);CGLSetCurrentContext(NULL);CGLDestroyContext(context);
    CGLPixelFormatAttribute legacy[] = {kCGLPFAAccelerated, 0};
    assert(CGLChoosePixelFormat(legacy, &format, &count) == kCGLNoError);
    assert(CGLCreateContext(format, NULL, &context) == kCGLNoError);
    CGLDestroyPixelFormat(format);
    assert(CGLSetCurrentContext(context) == kCGLNoError);
    float shininess = 12.5f, observed = 0;
    uint32_t material[] = {0x0408, 0x1601, 0};
    memcpy(material + 2, &shininess, 4);
    call("_glMaterialf", material);
    glGetMaterialfv(0x0404, 0x1601, &observed);
    assert(observed == shininess);
    uint32_t point_size; shininess = 3.5f; memcpy(&point_size, &shininess, 4);
    call("_glPointSize", &point_size);
    glGetFloatv(GL_POINT_SIZE, &observed); assert(observed == shininess);
    glPixelStorei(GL_PACK_ALIGNMENT, 8);
    uint32_t mask = 1; /* GL_CLIENT_PIXEL_STORE_BIT */
    call("_glPushClientAttrib", &mask);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    call("_glPopClientAttrib", NULL);
    GLint restored; glGetIntegerv(GL_PACK_ALIGNMENT, &restored); assert(restored == 8);
    assert(glGetError() == GL_NO_ERROR);
    GLuint depth;
    glGenTextures(1, &depth);
    glBindTexture(GL_TEXTURE_2D, depth);
    float texels[16];
    for (int i = 0; i < 16; ++i) texels[i] = 0.25f;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, 4, 4, 0, GL_DEPTH_COMPONENT, GL_FLOAT, texels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, 0x884E);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    float border[4] = {0, 0, 0, 1};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
    GLuint shadow_color, shadow_fbo;
    glGenTextures(1, &shadow_color);
    glBindTexture(GL_TEXTURE_2D, shadow_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &shadow_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, shadow_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, shadow_color, 0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glBindTexture(GL_TEXTURE_2D, depth);
    glViewport(0, 0, 4, 4);
    const char *shadow_source =
        "#version 120\n"
        "uniform sampler2DShadow s;\n"
        "uniform vec4 c;\n"
        "void main(){gl_FragColor=shadow2DProj(s, c);}\n";
    uint32_t shadow_shader = (uint32_t)call("_glCreateShader", (uint32_t[]){GL_FRAGMENT_SHADER});
    strcpy((char *)memory + 128, shadow_source);
    *(uint32_t *)memory = 0x10000080;
    uint32_t shadow_text[] = {shadow_shader, 1, 0x10000000, 0};
    call("_glShaderSource", shadow_text);
    call("_glCompileShader", &shadow_shader);
    GLint shadow_length = 0;
    glGetShaderiv(shadow_shader, GL_SHADER_SOURCE_LENGTH, &shadow_length);
    char *shadow_uploaded = calloc((size_t)shadow_length + 1, 1);
    glGetShaderSource(shadow_shader, shadow_length, NULL, shadow_uploaded);
    assert(strstr(shadow_uploaded, "lp32_shadow2DProj"));
    free(shadow_uploaded);
    uint32_t shadow_status[] = {shadow_shader, GL_COMPILE_STATUS, 0x10000300};
    *(uint32_t *)(memory + 772) = 0xabcdef01;
    call("_glGetShaderiv", shadow_status);
    assert(*(uint32_t *)(memory + 768) == GL_TRUE);
    const char *shadow_vertex = "void main(){gl_Position=gl_Vertex;}\n";
    uint32_t shadow_vs = (uint32_t)call("_glCreateShader", (uint32_t[]){GL_VERTEX_SHADER});
    strcpy((char *)memory + 128, shadow_vertex);
    *(uint32_t *)memory = 0x10000080;
    uint32_t shadow_vs_text[] = {shadow_vs, 1, 0x10000000, 0};
    call("_glShaderSource", shadow_vs_text);
    call("_glCompileShader", &shadow_vs);
    uint32_t shadow_program = (uint32_t)call("_glCreateProgram", NULL);
    uint32_t attach_fs[] = {shadow_program, shadow_shader};
    uint32_t attach_vs[] = {shadow_program, shadow_vs};
    call("_glAttachShader", attach_fs);
    call("_glAttachShader", attach_vs);
    call("_glLinkProgram", &shadow_program);
    call("_glUseProgram", &shadow_program);
    glUniform1i(glGetUniformLocation(shadow_program, "s"), 0);
    GLint coord = glGetUniformLocation(shadow_program, "c");
    unsigned char sample[4];
    glUniform4f(coord, 0.5f, 0.5f, 2.0f, 1.0f);
    glBegin(GL_QUADS);
    glVertex2f(-1, -1); glVertex2f(1, -1); glVertex2f(1, 1); glVertex2f(-1, 1);
    glEnd();
    glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, sample);
    assert(sample[0] == 255);
    glUniform4f(coord, 0.5f, 0.5f, 0.9f, 1.0f);
    glBegin(GL_QUADS);
    glVertex2f(-1, -1); glVertex2f(1, -1); glVertex2f(1, 1); glVertex2f(-1, 1);
    glEnd();
    glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, sample);
    assert(sample[0] == 0);
    glUniform4f(coord, 0.5f, 0.5f, 0.1f, 1.0f);
    glBegin(GL_QUADS);
    glVertex2f(-1, -1); glVertex2f(1, -1); glVertex2f(1, 1); glVertex2f(-1, 1);
    glEnd();
    glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, sample);
    assert(sample[0] == 255);
    call("_glDeleteProgram", &shadow_program);
    call("_glDeleteShader", &shadow_shader);
    call("_glDeleteShader", &shadow_vs);
    glDeleteFramebuffers(1, &shadow_fbo);
    glDeleteTextures(1, &shadow_color);
    glDeleteTextures(1, &depth);
    assert(glGetError() == GL_NO_ERROR);
    CGLSetCurrentContext(NULL); CGLDestroyContext(context);
    puts("Core GL bridge PASS (rendered pixels, shader ABI, legacy material/point floats, client state restoration)");
}
