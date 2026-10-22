/* Typed OpenGL ABI adapters. Signatures follow the macOS OpenGL headers.
 * Only used after the existing stateful GL bridge declines an entry point.
 * No native pointer or host-sized integer is returned to guest memory. */
#include "gl_core_bridge.h"
#include "objc_bridge.h"
#include "game_profile.h"
#include "asphalt_glsl.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl3.h>
static void *symbols[149];
static void gl_core_bridge32_shadow_compile_fallback(uint32_t shader);
static void gl_core_bridge32_forget_shadow_shader(uint32_t shader);
static float f32(uint32_t w){float f;memcpy(&f,&w,4);return f;}
static double f64(const uint32_t *w){double f;memcpy(&f,w,8);return f;}
static uint64_t call_glAttachShader(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t))symbols[0])((uint32_t)a[0], (uint32_t)a[1]);return 0;
}
static uint64_t call_glBeginTransformFeedback(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t))symbols[1])((uint32_t)a[0]);return 0;
}
static uint64_t call_glBindAttribLocation(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, const char *))symbols[2])((uint32_t)a[0], (uint32_t)a[1], (const char *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glBindBufferBase(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, uint32_t))symbols[3])((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2]);return 0;
}
static uint64_t call_glBindBufferRange(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, uint32_t, intptr_t, intptr_t))symbols[4])((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2], (intptr_t)(int32_t)a[3], (intptr_t)(int32_t)a[4]);return 0;
}
static uint64_t call_glBindFragDataLocation(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, const char *))symbols[5])((uint32_t)a[0], (uint32_t)a[1], (const char *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glBindProgramPipeline(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t))symbols[6])((uint32_t)a[0]);return 0;
}
static uint64_t call_glBindSampler(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t))symbols[7])((uint32_t)a[0], (uint32_t)a[1]);return 0;
}
static uint64_t call_glBindVertexArray(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t))symbols[8])((uint32_t)a[0]);return 0;
}
static uint64_t call_glBlendEquationSeparatei(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, uint32_t))symbols[9])((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2]);return 0;
}
static uint64_t call_glBlendFuncSeparateiARB(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))symbols[10])((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2], (uint32_t)a[3], (uint32_t)a[4]);return 0;
}
static uint64_t call_glClampColorARB(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t))symbols[11])((uint32_t)a[0], (uint32_t)a[1]);return 0;
}
static uint64_t call_glClearBufferfi(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, float, int32_t))symbols[12])((uint32_t)a[0], (int32_t)a[1], f32(a[2]), (int32_t)a[3]);return 0;
}
static uint64_t call_glClearBufferfv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, const float *))symbols[13])((uint32_t)a[0], (int32_t)a[1], (const float *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glClearBufferiv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, const int32_t *))symbols[14])((uint32_t)a[0], (int32_t)a[1], (const int32_t *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glClearBufferuiv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, const uint32_t *))symbols[15])((uint32_t)a[0], (int32_t)a[1], (const uint32_t *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glClipPlane(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, const double *))symbols[16])((uint32_t)a[0], (const double *)(uintptr_t)a[1]);return 0;
}
static uint64_t call_glColor4fv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(const float *))symbols[17])((const float *)(uintptr_t)a[0]);return 0;
}
static uint64_t call_glColorMaski(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint8_t, uint8_t, uint8_t, uint8_t))symbols[18])((uint32_t)a[0], (uint8_t)a[1], (uint8_t)a[2], (uint8_t)a[3], (uint8_t)a[4]);return 0;
}
static uint64_t call_glColorMaterial(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t))symbols[19])((uint32_t)a[0], (uint32_t)a[1]);return 0;
}
static uint64_t call_glColorPointer(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, uint32_t, int32_t, const void *))symbols[20])((int32_t)a[0], (uint32_t)a[1], (int32_t)a[2], (const void *)(uintptr_t)a[3]);return 0;
}
static uint64_t call_glCompileShader(const uint32_t *a,uint32_t site) {
    (void)site;
    uint32_t shader = a[0];
    ((void (*)(uint32_t))symbols[21])(shader);
    gl_core_bridge32_shadow_compile_fallback(shader);
    if (getenv("LP32_TRACE_SHADER_ERRORS")) { /* logs GLSL compile/link failures */
        GLint ok = 1; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[1024] = {0}; glGetShaderInfoLog(shader, sizeof log - 1, NULL, log); fprintf(stderr, "SHADERERR compile %u: %s\n", shader, log); }
    }
    return 0;
}
static uint64_t call_glCompressedTexImage1D(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, uint32_t, int32_t, int32_t, int32_t, const void *))symbols[22])((uint32_t)a[0], (int32_t)a[1], (uint32_t)a[2], (int32_t)a[3], (int32_t)a[4], (int32_t)a[5], (const void *)(uintptr_t)a[6]);return 0;
}
static uint64_t call_glCompressedTexImage3D(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, const void *))symbols[23])((uint32_t)a[0], (int32_t)a[1], (uint32_t)a[2], (int32_t)a[3], (int32_t)a[4], (int32_t)a[5], (int32_t)a[6], (int32_t)a[7], (const void *)(uintptr_t)a[8]);return 0;
}
static uint64_t call_glCompressedTexSubImage1D(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, int32_t, int32_t, uint32_t, int32_t, const void *))symbols[24])((uint32_t)a[0], (int32_t)a[1], (int32_t)a[2], (int32_t)a[3], (uint32_t)a[4], (int32_t)a[5], (const void *)(uintptr_t)a[6]);return 0;
}
static uint64_t call_glCompressedTexSubImage3D(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, int32_t, const void *))symbols[25])((uint32_t)a[0], (int32_t)a[1], (int32_t)a[2], (int32_t)a[3], (int32_t)a[4], (int32_t)a[5], (int32_t)a[6], (int32_t)a[7], (uint32_t)a[8], (int32_t)a[9], (const void *)(uintptr_t)a[10]);return 0;
}
static uint64_t call_glCopyBufferSubData(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, intptr_t, intptr_t, intptr_t))symbols[26])((uint32_t)a[0], (uint32_t)a[1], (intptr_t)(int32_t)a[2], (intptr_t)(int32_t)a[3], (intptr_t)(int32_t)a[4]);return 0;
}
static uint64_t call_glCreateProgram(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    return (uint64_t)((uint32_t (*)(void))symbols[27])();
}
static uint64_t call_glCreateShader(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    return (uint64_t)((uint32_t (*)(uint32_t))symbols[28])((uint32_t)a[0]);
}
static uint64_t call_glDeleteFencesAPPLE(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, const uint32_t *))symbols[29])((int32_t)a[0], (const uint32_t *)(uintptr_t)a[1]);return 0;
}
static uint64_t call_glDeleteFramebuffersEXT(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, const uint32_t *))symbols[30])((int32_t)a[0], (const uint32_t *)(uintptr_t)a[1]);return 0;
}
static uint64_t call_glDeleteProgram(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t))symbols[31])((uint32_t)a[0]);return 0;
}
static uint64_t call_glDeleteProgramPipelines(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, const uint32_t *))symbols[32])((int32_t)a[0], (const uint32_t *)(uintptr_t)a[1]);return 0;
}
static uint64_t call_glDeleteSamplers(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, const uint32_t *))symbols[33])((int32_t)a[0], (const uint32_t *)(uintptr_t)a[1]);return 0;
}
static uint64_t call_glDeleteShader(const uint32_t *a,uint32_t site) {
    (void)site;
    gl_core_bridge32_forget_shadow_shader(a[0]);
    ((void (*)(uint32_t))symbols[34])((uint32_t)a[0]);return 0;
}
static uint64_t call_glDeleteVertexArrays(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, const uint32_t *))symbols[35])((int32_t)a[0], (const uint32_t *)(uintptr_t)a[1]);return 0;
}
static uint64_t call_glDepthBoundsEXT(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(double, double))symbols[36])(f64(a+0), f64(a+2));return 0;
}
static uint64_t call_glDepthRange(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(double, double))symbols[37])(f64(a+0), f64(a+2));return 0;
}
static uint64_t call_glDepthRangeArrayv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, const double *))symbols[38])((uint32_t)a[0], (int32_t)a[1], (const double *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glDisableClientState(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t))symbols[39])((uint32_t)a[0]);return 0;
}
static uint64_t call_glDisablei(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t))symbols[40])((uint32_t)a[0], (uint32_t)a[1]);return 0;
}
static uint64_t call_glDrawArraysIndirect(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, const void *))symbols[41])((uint32_t)a[0], (const void *)(uintptr_t)a[1]);return 0;
}
static uint64_t call_glDrawArraysInstanced(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, int32_t, int32_t))symbols[42])((uint32_t)a[0], (int32_t)a[1], (int32_t)a[2], (int32_t)a[3]);return 0;
}
static uint64_t call_glDrawElements(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, uint32_t, const void *))symbols[43])((uint32_t)a[0], (int32_t)a[1], (uint32_t)a[2], (const void *)(uintptr_t)a[3]);return 0;
}
static uint64_t call_glDrawElementsBaseVertex(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, uint32_t, const void *, int32_t))symbols[44])((uint32_t)a[0], (int32_t)a[1], (uint32_t)a[2], (const void *)(uintptr_t)a[3], (int32_t)a[4]);return 0;
}
static uint64_t call_glDrawElementsIndirect(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, const void *))symbols[45])((uint32_t)a[0], (uint32_t)a[1], (const void *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glDrawElementsInstanced(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, uint32_t, const void *, int32_t))symbols[46])((uint32_t)a[0], (int32_t)a[1], (uint32_t)a[2], (const void *)(uintptr_t)a[3], (int32_t)a[4]);return 0;
}
static uint64_t call_glDrawElementsInstancedBaseVertex(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, uint32_t, const void *, int32_t, int32_t))symbols[47])((uint32_t)a[0], (int32_t)a[1], (uint32_t)a[2], (const void *)(uintptr_t)a[3], (int32_t)a[4], (int32_t)a[5]);return 0;
}
static uint64_t call_glEnableClientState(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t))symbols[48])((uint32_t)a[0]);return 0;
}
static uint64_t call_glEnablei(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t))symbols[49])((uint32_t)a[0], (uint32_t)a[1]);return 0;
}
static uint64_t call_glEndTransformFeedback(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(void))symbols[50])();return 0;
}
static uint64_t call_glFlushMappedBufferRange(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, intptr_t, intptr_t))symbols[51])((uint32_t)a[0], (intptr_t)(int32_t)a[1], (intptr_t)(int32_t)a[2]);return 0;
}
static uint64_t call_glFlushMappedBufferRangeAPPLE(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, intptr_t, intptr_t))symbols[52])((uint32_t)a[0], (intptr_t)(int32_t)a[1], (intptr_t)(int32_t)a[2]);return 0;
}
static uint64_t call_glFogCoordPointerEXT(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, const void *))symbols[53])((uint32_t)a[0], (int32_t)a[1], (const void *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glFogf(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, float))symbols[54])((uint32_t)a[0], f32(a[1]));return 0;
}
static uint64_t call_glFogfv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, const float *))symbols[55])((uint32_t)a[0], (const float *)(uintptr_t)a[1]);return 0;
}
static uint64_t call_glFogi(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t))symbols[56])((uint32_t)a[0], (int32_t)a[1]);return 0;
}
static uint64_t call_glFramebufferTexture(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, uint32_t, int32_t))symbols[57])((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2], (int32_t)a[3]);return 0;
}
static uint64_t call_glFramebufferTexture3D(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, uint32_t, uint32_t, int32_t, int32_t))symbols[58])((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2], (uint32_t)a[3], (int32_t)a[4], (int32_t)a[5]);return 0;
}
static uint64_t call_glFramebufferTextureLayer(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, uint32_t, int32_t, int32_t))symbols[59])((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2], (int32_t)a[3], (int32_t)a[4]);return 0;
}
static uint64_t call_glGenFencesAPPLE(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, uint32_t *))symbols[60])((int32_t)a[0], (uint32_t *)(uintptr_t)a[1]);return 0;
}
static uint64_t call_glGenProgramPipelines(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, uint32_t *))symbols[61])((int32_t)a[0], (uint32_t *)(uintptr_t)a[1]);return 0;
}
static uint64_t call_glGenSamplers(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, uint32_t *))symbols[62])((int32_t)a[0], (uint32_t *)(uintptr_t)a[1]);return 0;
}
static uint64_t call_glGenVertexArrays(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, uint32_t *))symbols[63])((int32_t)a[0], (uint32_t *)(uintptr_t)a[1]);return 0;
}
static uint64_t call_glGetActiveUniformBlockiv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, uint32_t, int32_t *))symbols[64])((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2], (int32_t *)(uintptr_t)a[3]);return 0;
}
static uint64_t call_glGetInteger64v(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int64_t *))symbols[65])((uint32_t)a[0], (int64_t *)(uintptr_t)a[1]);return 0;
}
static uint64_t call_glGetObjectParameterivAPPLE(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, uint32_t, int32_t *))symbols[66])((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2], (int32_t *)(uintptr_t)a[3]);return 0;
}
static uint64_t call_glGetProgramBinary(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, int32_t *, uint32_t *, void *))symbols[67])((uint32_t)a[0], (int32_t)a[1], (int32_t *)(uintptr_t)a[2], (uint32_t *)(uintptr_t)a[3], (void *)(uintptr_t)a[4]);return 0;
}
static uint64_t call_glGetProgramInfoLog(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, int32_t *, char *))symbols[68])((uint32_t)a[0], (int32_t)a[1], (int32_t *)(uintptr_t)a[2], (char *)(uintptr_t)a[3]);return 0;
}
static uint64_t call_glGetProgramiv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, int32_t *))symbols[69])((uint32_t)a[0], (uint32_t)a[1], (int32_t *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glGetQueryObjectui64v(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, uint64_t *))symbols[70])((uint32_t)a[0], (uint32_t)a[1], (uint64_t *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glGetRenderbufferParameteriv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, int32_t *))symbols[71])((uint32_t)a[0], (uint32_t)a[1], (int32_t *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glGetShaderInfoLog(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, int32_t *, char *))symbols[72])((uint32_t)a[0], (int32_t)a[1], (int32_t *)(uintptr_t)a[2], (char *)(uintptr_t)a[3]);return 0;
}
static uint64_t call_glGetShaderSource(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, int32_t *, char *))symbols[73])((uint32_t)a[0], (int32_t)a[1], (int32_t *)(uintptr_t)a[2], (char *)(uintptr_t)a[3]);return 0;
}
static uint64_t call_glGetShaderiv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, int32_t *))symbols[74])((uint32_t)a[0], (uint32_t)a[1], (int32_t *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glGetTexEnvfv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, float *))symbols[75])((uint32_t)a[0], (uint32_t)a[1], (float *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glGetTransformFeedbackVarying(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, int32_t, int32_t *, int32_t *, uint32_t *, char *))symbols[76])((uint32_t)a[0], (uint32_t)a[1], (int32_t)a[2], (int32_t *)(uintptr_t)a[3], (int32_t *)(uintptr_t)a[4], (uint32_t *)(uintptr_t)a[5], (char *)(uintptr_t)a[6]);return 0;
}
static uint64_t call_glGetUniformBlockIndex(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    return (uint64_t)((uint32_t (*)(uint32_t, const char *))symbols[77])((uint32_t)a[0], (const char *)(uintptr_t)a[1]);
}
static uint64_t call_glGetUniformLocation(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    return (uint64_t)((int32_t (*)(uint32_t, const char *))symbols[78])((uint32_t)a[0], (const char *)(uintptr_t)a[1]);
}
static uint64_t call_glHint(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t))symbols[79])((uint32_t)a[0], (uint32_t)a[1]);return 0;
}
static uint64_t call_glIsTexture(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    return (uint64_t)((uint8_t (*)(uint32_t))symbols[80])((uint32_t)a[0]);
}
static uint64_t call_glLightModelfv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, const float *))symbols[81])((uint32_t)a[0], (const float *)(uintptr_t)a[1]);return 0;
}
static uint64_t call_glLightModeli(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t))symbols[82])((uint32_t)a[0], (int32_t)a[1]);return 0;
}
static uint64_t call_glLightf(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, float))symbols[83])((uint32_t)a[0], (uint32_t)a[1], f32(a[2]));return 0;
}
static uint64_t call_glLightfv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, const float *))symbols[84])((uint32_t)a[0], (uint32_t)a[1], (const float *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glLinkProgram(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t))symbols[85])((uint32_t)a[0]);
    if (getenv("LP32_TRACE_SHADER_ERRORS")) { /* logs GLSL compile/link failures */
        GLint ok = 1; glGetProgramiv(a[0], GL_LINK_STATUS, &ok);
        if (!ok) { char log[1024] = {0}; glGetProgramInfoLog(a[0], sizeof log - 1, NULL, log); fprintf(stderr, "SHADERERR link %u: %s\n", a[0], log); }
    }
    return 0;
}
static uint64_t call_glLoadMatrixf(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(const float *))symbols[86])((const float *)(uintptr_t)a[0]);return 0;
}
static uint64_t call_glMaterialfv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, const float *))symbols[87])((uint32_t)a[0], (uint32_t)a[1], (const float *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glMateriali(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, int32_t))symbols[88])((uint32_t)a[0], (uint32_t)a[1], (int32_t)a[2]);return 0;
}
static uint64_t call_glMultMatrixf(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(const float *))symbols[89])((const float *)(uintptr_t)a[0]);return 0;
}
static uint64_t call_glNormalPointer(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, const void *))symbols[90])((uint32_t)a[0], (int32_t)a[1], (const void *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glObjectPurgeableAPPLE(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    return (uint64_t)((uint32_t (*)(uint32_t, uint32_t, uint32_t))symbols[91])((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2]);
}
static uint64_t call_glObjectUnpurgeableAPPLE(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    return (uint64_t)((uint32_t (*)(uint32_t, uint32_t, uint32_t))symbols[92])((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2]);
}
static uint64_t call_glPatchParameteri(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t))symbols[93])((uint32_t)a[0], (int32_t)a[1]);return 0;
}
static uint64_t call_glPolygonMode(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t))symbols[94])((uint32_t)a[0], (uint32_t)a[1]);return 0;
}
static uint64_t call_glPopGroupMarkerEXT(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(void))symbols[95])();return 0;
}
static uint64_t call_glPopMatrix(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(void))symbols[96])();return 0;
}
static uint64_t call_glPrioritizeTextures(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, const uint32_t *, const float *))symbols[97])((int32_t)a[0], (const uint32_t *)(uintptr_t)a[1], (const float *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glProgramBinary(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, const void *, int32_t))symbols[98])((uint32_t)a[0], (uint32_t)a[1], (const void *)(uintptr_t)a[2], (int32_t)a[3]);return 0;
}
static uint64_t call_glProgramParameteri(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, int32_t))symbols[99])((uint32_t)a[0], (uint32_t)a[1], (int32_t)a[2]);return 0;
}
static uint64_t call_glProgramUniform1i(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, int32_t))symbols[100])((uint32_t)a[0], (int32_t)a[1], (int32_t)a[2]);return 0;
}
static uint64_t call_glProgramUniform1iv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, int32_t, const int32_t *))symbols[101])((uint32_t)a[0], (int32_t)a[1], (int32_t)a[2], (const int32_t *)(uintptr_t)a[3]);return 0;
}
static uint64_t call_glProgramUniform1ui(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, uint32_t))symbols[102])((uint32_t)a[0], (int32_t)a[1], (uint32_t)a[2]);return 0;
}
static uint64_t call_glProgramUniform4fv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, int32_t, const float *))symbols[103])((uint32_t)a[0], (int32_t)a[1], (int32_t)a[2], (const float *)(uintptr_t)a[3]);return 0;
}
static uint64_t call_glProgramUniform4iv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, int32_t, const int32_t *))symbols[104])((uint32_t)a[0], (int32_t)a[1], (int32_t)a[2], (const int32_t *)(uintptr_t)a[3]);return 0;
}
static uint64_t call_glProvokingVertexEXT(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t))symbols[105])((uint32_t)a[0]);return 0;
}
static uint64_t call_glPushGroupMarkerEXT(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, const char *))symbols[106])((int32_t)a[0], (const char *)(uintptr_t)a[1]);return 0;
}
static uint64_t call_glPushMatrix(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(void))symbols[107])();return 0;
}
static uint64_t call_glQueryCounter(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t))symbols[108])((uint32_t)a[0], (uint32_t)a[1]);return 0;
}
static uint64_t call_glRenderbufferStorageMultisample(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, uint32_t, int32_t, int32_t))symbols[109])((uint32_t)a[0], (int32_t)a[1], (uint32_t)a[2], (int32_t)a[3], (int32_t)a[4]);return 0;
}
static uint64_t call_glSampleMaski(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t))symbols[110])((uint32_t)a[0], (uint32_t)a[1]);return 0;
}
static uint64_t call_glSamplerParameterf(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, float))symbols[111])((uint32_t)a[0], (uint32_t)a[1], f32(a[2]));return 0;
}
static uint64_t call_glSamplerParameterfv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, const float *))symbols[112])((uint32_t)a[0], (uint32_t)a[1], (const float *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glSamplerParameteri(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, int32_t))symbols[113])((uint32_t)a[0], (uint32_t)a[1], (int32_t)a[2]);return 0;
}
static uint64_t call_glScalef(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(float, float, float))symbols[114])(f32(a[0]), f32(a[1]), f32(a[2]));return 0;
}
static uint64_t call_glSecondaryColor3fvEXT(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(const float *))symbols[115])((const float *)(uintptr_t)a[0]);return 0;
}
static uint64_t call_glSecondaryColorPointerEXT(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, uint32_t, int32_t, const void *))symbols[116])((int32_t)a[0], (uint32_t)a[1], (int32_t)a[2], (const void *)(uintptr_t)a[3]);return 0;
}
static uint64_t call_glSetFenceAPPLE(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t))symbols[117])((uint32_t)a[0]);return 0;
}
static uint64_t call_glShadeModel(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t))symbols[118])((uint32_t)a[0]);return 0;
}
static uint64_t call_glTestFenceAPPLE(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    return (uint64_t)((uint8_t (*)(uint32_t))symbols[119])((uint32_t)a[0]);
}
static uint64_t call_glTexBuffer(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, uint32_t))symbols[120])((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2]);return 0;
}
static uint64_t call_glTexCoordPointer(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, uint32_t, int32_t, const void *))symbols[121])((int32_t)a[0], (uint32_t)a[1], (int32_t)a[2], (const void *)(uintptr_t)a[3]);return 0;
}
static uint64_t call_glTexEnvfv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, const float *))symbols[122])((uint32_t)a[0], (uint32_t)a[1], (const float *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glTexGenfv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, const float *))symbols[123])((uint32_t)a[0], (uint32_t)a[1], (const float *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glTexGeni(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, int32_t))symbols[124])((uint32_t)a[0], (uint32_t)a[1], (int32_t)a[2]);return 0;
}
static uint64_t call_glTexImage1D(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, const void *))symbols[125])((uint32_t)a[0], (int32_t)a[1], (int32_t)a[2], (int32_t)a[3], (int32_t)a[4], (uint32_t)a[5], (uint32_t)a[6], (const void *)(uintptr_t)a[7]);return 0;
}
static uint64_t call_glTexImage2DMultisample(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, int32_t, int32_t, int32_t, uint8_t))symbols[126])((uint32_t)a[0], (int32_t)a[1], (int32_t)a[2], (int32_t)a[3], (int32_t)a[4], (uint8_t)a[5]);return 0;
}
static uint64_t call_glTexImage3DMultisample(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint8_t))symbols[127])((uint32_t)a[0], (int32_t)a[1], (int32_t)a[2], (int32_t)a[3], (int32_t)a[4], (int32_t)a[5], (uint8_t)a[6]);return 0;
}
static uint64_t call_glTexStorage1D(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, uint32_t, int32_t))symbols[128])((uint32_t)a[0], (int32_t)a[1], (uint32_t)a[2], (int32_t)a[3]);return 0;
}
static uint64_t call_glTexStorage2D(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, uint32_t, int32_t, int32_t))symbols[129])((uint32_t)a[0], (int32_t)a[1], (uint32_t)a[2], (int32_t)a[3], (int32_t)a[4]);return 0;
}
static uint64_t call_glTexStorage3D(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, uint32_t, int32_t, int32_t, int32_t))symbols[130])((uint32_t)a[0], (int32_t)a[1], (uint32_t)a[2], (int32_t)a[3], (int32_t)a[4], (int32_t)a[5]);return 0;
}
static uint64_t call_glTexSubImage1D(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, const void *))symbols[131])((uint32_t)a[0], (int32_t)a[1], (int32_t)a[2], (int32_t)a[3], (uint32_t)a[4], (uint32_t)a[5], (const void *)(uintptr_t)a[6]);return 0;
}
static uint64_t call_glTexSubImage2D(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, const void *))symbols[132])((uint32_t)a[0], (int32_t)a[1], (int32_t)a[2], (int32_t)a[3], (int32_t)a[4], (int32_t)a[5], (uint32_t)a[6], (uint32_t)a[7], (const void *)(uintptr_t)a[8]);return 0;
}
static uint64_t call_glTextureBarrierNV(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(void))symbols[133])();return 0;
}
static uint64_t call_glTranslatef(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(float, float, float))symbols[134])(f32(a[0]), f32(a[1]), f32(a[2]));return 0;
}
static uint64_t call_glUniform1i(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, int32_t))symbols[135])((int32_t)a[0], (int32_t)a[1]);return 0;
}
static uint64_t call_glUniform1iv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, int32_t, const int32_t *))symbols[136])((int32_t)a[0], (int32_t)a[1], (const int32_t *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glUniform1ui(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, uint32_t))symbols[137])((int32_t)a[0], (uint32_t)a[1]);return 0;
}
static uint64_t call_glUniform4fv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, int32_t, const float *))symbols[138])((int32_t)a[0], (int32_t)a[1], (const float *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glUniform4iv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, int32_t, const int32_t *))symbols[139])((int32_t)a[0], (int32_t)a[1], (const int32_t *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glUniformBlockBinding(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, uint32_t))symbols[140])((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2]);return 0;
}
static uint64_t call_glUnmapBuffer(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    return (uint64_t)((uint8_t (*)(uint32_t))symbols[141])((uint32_t)a[0]);
}
static uint64_t call_glUseProgram(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t))symbols[142])((uint32_t)a[0]);return 0;
}
static uint64_t call_glUseProgramStages(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, uint32_t))symbols[143])((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2]);return 0;
}
static uint64_t call_glVertexAttribDivisorARB(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t))symbols[144])((uint32_t)a[0], (uint32_t)a[1]);return 0;
}
static uint64_t call_glVertexAttribIPointer(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, uint32_t, int32_t, const void *))symbols[145])((uint32_t)a[0], (int32_t)a[1], (uint32_t)a[2], (int32_t)a[3], (const void *)(uintptr_t)a[4]);return 0;
}
static uint64_t call_glVertexBlendARB(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t))symbols[146])((int32_t)a[0]);return 0;
}
static uint64_t call_glVertexPointer(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, uint32_t, int32_t, const void *))symbols[147])((int32_t)a[0], (uint32_t)a[1], (int32_t)a[2], (const void *)(uintptr_t)a[3]);return 0;
}
static uint64_t call_glViewportArrayv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, const float *))symbols[148])((uint32_t)a[0], (int32_t)a[1], (const float *)(uintptr_t)a[2]);return 0;
}
static const struct {const char *name; lp32_fast_import_fn call;} table[] = {
    {"glAttachShader",call_glAttachShader},
    {"glBeginTransformFeedback",call_glBeginTransformFeedback},
    {"glBindAttribLocation",call_glBindAttribLocation},
    {"glBindBufferBase",call_glBindBufferBase},
    {"glBindBufferRange",call_glBindBufferRange},
    {"glBindFragDataLocation",call_glBindFragDataLocation},
    {"glBindProgramPipeline",call_glBindProgramPipeline},
    {"glBindSampler",call_glBindSampler},
    {"glBindVertexArray",call_glBindVertexArray},
    {"glBlendEquationSeparatei",call_glBlendEquationSeparatei},
    {"glBlendFuncSeparateiARB",call_glBlendFuncSeparateiARB},
    {"glClampColorARB",call_glClampColorARB},
    {"glClearBufferfi",call_glClearBufferfi},
    {"glClearBufferfv",call_glClearBufferfv},
    {"glClearBufferiv",call_glClearBufferiv},
    {"glClearBufferuiv",call_glClearBufferuiv},
    {"glClipPlane",call_glClipPlane},
    {"glColor4fv",call_glColor4fv},
    {"glColorMaski",call_glColorMaski},
    {"glColorMaterial",call_glColorMaterial},
    {"glColorPointer",call_glColorPointer},
    {"glCompileShader",call_glCompileShader},
    {"glCompressedTexImage1D",call_glCompressedTexImage1D},
    {"glCompressedTexImage3D",call_glCompressedTexImage3D},
    {"glCompressedTexSubImage1D",call_glCompressedTexSubImage1D},
    {"glCompressedTexSubImage3D",call_glCompressedTexSubImage3D},
    {"glCopyBufferSubData",call_glCopyBufferSubData},
    {"glCreateProgram",call_glCreateProgram},
    {"glCreateShader",call_glCreateShader},
    {"glDeleteFencesAPPLE",call_glDeleteFencesAPPLE},
    {"glDeleteFramebuffersEXT",call_glDeleteFramebuffersEXT},
    {"glDeleteProgram",call_glDeleteProgram},
    {"glDeleteProgramPipelines",call_glDeleteProgramPipelines},
    {"glDeleteSamplers",call_glDeleteSamplers},
    {"glDeleteShader",call_glDeleteShader},
    {"glDeleteVertexArrays",call_glDeleteVertexArrays},
    {"glDepthBoundsEXT",call_glDepthBoundsEXT},
    {"glDepthRange",call_glDepthRange},
    {"glDepthRangeArrayv",call_glDepthRangeArrayv},
    {"glDisableClientState",call_glDisableClientState},
    {"glDisablei",call_glDisablei},
    {"glDrawArraysIndirect",call_glDrawArraysIndirect},
    {"glDrawArraysInstanced",call_glDrawArraysInstanced},
    {"glDrawElements",call_glDrawElements},
    {"glDrawElementsBaseVertex",call_glDrawElementsBaseVertex},
    {"glDrawElementsIndirect",call_glDrawElementsIndirect},
    {"glDrawElementsInstanced",call_glDrawElementsInstanced},
    {"glDrawElementsInstancedBaseVertex",call_glDrawElementsInstancedBaseVertex},
    {"glEnableClientState",call_glEnableClientState},
    {"glEnablei",call_glEnablei},
    {"glEndTransformFeedback",call_glEndTransformFeedback},
    {"glFlushMappedBufferRange",call_glFlushMappedBufferRange},
    {"glFlushMappedBufferRangeAPPLE",call_glFlushMappedBufferRangeAPPLE},
    {"glFogCoordPointerEXT",call_glFogCoordPointerEXT},
    {"glFogf",call_glFogf},
    {"glFogfv",call_glFogfv},
    {"glFogi",call_glFogi},
    {"glFramebufferTexture",call_glFramebufferTexture},
    {"glFramebufferTexture3D",call_glFramebufferTexture3D},
    {"glFramebufferTextureLayer",call_glFramebufferTextureLayer},
    {"glGenFencesAPPLE",call_glGenFencesAPPLE},
    {"glGenProgramPipelines",call_glGenProgramPipelines},
    {"glGenSamplers",call_glGenSamplers},
    {"glGenVertexArrays",call_glGenVertexArrays},
    {"glGetActiveUniformBlockiv",call_glGetActiveUniformBlockiv},
    {"glGetInteger64v",call_glGetInteger64v},
    {"glGetObjectParameterivAPPLE",call_glGetObjectParameterivAPPLE},
    {"glGetProgramBinary",call_glGetProgramBinary},
    {"glGetProgramInfoLog",call_glGetProgramInfoLog},
    {"glGetProgramiv",call_glGetProgramiv},
    {"glGetQueryObjectui64v",call_glGetQueryObjectui64v},
    {"glGetRenderbufferParameteriv",call_glGetRenderbufferParameteriv},
    {"glGetShaderInfoLog",call_glGetShaderInfoLog},
    {"glGetShaderSource",call_glGetShaderSource},
    {"glGetShaderiv",call_glGetShaderiv},
    {"glGetTexEnvfv",call_glGetTexEnvfv},
    {"glGetTransformFeedbackVarying",call_glGetTransformFeedbackVarying},
    {"glGetUniformBlockIndex",call_glGetUniformBlockIndex},
    {"glGetUniformLocation",call_glGetUniformLocation},
    {"glHint",call_glHint},
    {"glIsTexture",call_glIsTexture},
    {"glLightModelfv",call_glLightModelfv},
    {"glLightModeli",call_glLightModeli},
    {"glLightf",call_glLightf},
    {"glLightfv",call_glLightfv},
    {"glLinkProgram",call_glLinkProgram},
    {"glLoadMatrixf",call_glLoadMatrixf},
    {"glMaterialfv",call_glMaterialfv},
    {"glMateriali",call_glMateriali},
    {"glMultMatrixf",call_glMultMatrixf},
    {"glNormalPointer",call_glNormalPointer},
    {"glObjectPurgeableAPPLE",call_glObjectPurgeableAPPLE},
    {"glObjectUnpurgeableAPPLE",call_glObjectUnpurgeableAPPLE},
    {"glPatchParameteri",call_glPatchParameteri},
    {"glPolygonMode",call_glPolygonMode},
    {"glPopGroupMarkerEXT",call_glPopGroupMarkerEXT},
    {"glPopMatrix",call_glPopMatrix},
    {"glPrioritizeTextures",call_glPrioritizeTextures},
    {"glProgramBinary",call_glProgramBinary},
    {"glProgramParameteri",call_glProgramParameteri},
    {"glProgramUniform1i",call_glProgramUniform1i},
    {"glProgramUniform1iv",call_glProgramUniform1iv},
    {"glProgramUniform1ui",call_glProgramUniform1ui},
    {"glProgramUniform4fv",call_glProgramUniform4fv},
    {"glProgramUniform4iv",call_glProgramUniform4iv},
    {"glProvokingVertexEXT",call_glProvokingVertexEXT},
    {"glPushGroupMarkerEXT",call_glPushGroupMarkerEXT},
    {"glPushMatrix",call_glPushMatrix},
    {"glQueryCounter",call_glQueryCounter},
    {"glRenderbufferStorageMultisample",call_glRenderbufferStorageMultisample},
    {"glSampleMaski",call_glSampleMaski},
    {"glSamplerParameterf",call_glSamplerParameterf},
    {"glSamplerParameterfv",call_glSamplerParameterfv},
    {"glSamplerParameteri",call_glSamplerParameteri},
    {"glScalef",call_glScalef},
    {"glSecondaryColor3fvEXT",call_glSecondaryColor3fvEXT},
    {"glSecondaryColorPointerEXT",call_glSecondaryColorPointerEXT},
    {"glSetFenceAPPLE",call_glSetFenceAPPLE},
    {"glShadeModel",call_glShadeModel},
    {"glTestFenceAPPLE",call_glTestFenceAPPLE},
    {"glTexBuffer",call_glTexBuffer},
    {"glTexCoordPointer",call_glTexCoordPointer},
    {"glTexEnvfv",call_glTexEnvfv},
    {"glTexGenfv",call_glTexGenfv},
    {"glTexGeni",call_glTexGeni},
    {"glTexImage1D",call_glTexImage1D},
    {"glTexImage2DMultisample",call_glTexImage2DMultisample},
    {"glTexImage3DMultisample",call_glTexImage3DMultisample},
    {"glTexStorage1D",call_glTexStorage1D},
    {"glTexStorage2D",call_glTexStorage2D},
    {"glTexStorage3D",call_glTexStorage3D},
    {"glTexSubImage1D",call_glTexSubImage1D},
    {"glTexSubImage2D",call_glTexSubImage2D},
    {"glTextureBarrierNV",call_glTextureBarrierNV},
    {"glTranslatef",call_glTranslatef},
    {"glUniform1i",call_glUniform1i},
    {"glUniform1iv",call_glUniform1iv},
    {"glUniform1ui",call_glUniform1ui},
    {"glUniform4fv",call_glUniform4fv},
    {"glUniform4iv",call_glUniform4iv},
    {"glUniformBlockBinding",call_glUniformBlockBinding},
    {"glUnmapBuffer",call_glUnmapBuffer},
    {"glUseProgram",call_glUseProgram},
    {"glUseProgramStages",call_glUseProgramStages},
    {"glVertexAttribDivisorARB",call_glVertexAttribDivisorARB},
    {"glVertexAttribIPointer",call_glVertexAttribIPointer},
    {"glVertexBlendARB",call_glVertexBlendARB},
    {"glVertexPointer",call_glVertexPointer},
    {"glViewportArrayv",call_glViewportArrayv},
};
/* Compatibility-profile operations used by COD4. Keep these outside the
 * indexed generated table so its stable symbol indices remain unchanged. */
extern void glMaterialf(uint32_t, uint32_t, float);
extern void glActiveStencilFaceEXT(uint32_t);
extern void glDeleteVertexArraysAPPLE(int32_t, const uint32_t *);
extern void glPointParameterfARB(uint32_t, float);
extern void glPushClientAttrib(uint32_t);
extern void glPopClientAttrib(void);
static uint64_t extra_glMaterialf(const uint32_t *a, uint32_t site) {
    (void)site; glMaterialf(a[0], a[1], f32(a[2])); return 0;
}
static uint64_t extra_glActiveStencilFaceEXT(const uint32_t *a, uint32_t site) {
    (void)site; glActiveStencilFaceEXT(a[0]); return 0;
}
static uint64_t extra_glDeleteVertexArraysAPPLE(const uint32_t *a, uint32_t site) {
    (void)site; glDeleteVertexArraysAPPLE((int32_t)a[0], (const void *)(uintptr_t)a[1]); return 0;
}
static uint64_t extra_glPointParameterfARB(const uint32_t *a, uint32_t site) {
    (void)site; glPointParameterfARB(a[0], f32(a[1])); return 0;
}
static uint64_t extra_glPointSize(const uint32_t *a, uint32_t site) {
    (void)site; glPointSize(f32(a[0])); return 0;
}
static uint64_t extra_glPushClientAttrib(const uint32_t *a, uint32_t site) {
    (void)site; glPushClientAttrib(a[0]); return 0;
}
static uint64_t extra_glPopClientAttrib(const uint32_t *a, uint32_t site) {
    (void)site; (void)a; glPopClientAttrib(); return 0;
}
static uint64_t extra_glPixelStorei(const uint32_t *a,uint32_t site){
    (void)site;glPixelStorei(a[0],(int32_t)a[1]);return 0;
}
static uint64_t extra_glGetBooleanv(const uint32_t *a,uint32_t site){
    (void)site;glGetBooleanv(a[0],(void *)(uintptr_t)a[1]);return 0;
}
static uint64_t extra_glGetActiveAttrib(const uint32_t *a,uint32_t site){
    (void)site;
    glGetActiveAttrib(a[0],a[1],(GLsizei)a[2],(GLsizei *)(uintptr_t)a[3],
        (GLint *)(uintptr_t)a[4],(GLenum *)(uintptr_t)a[5],(GLchar *)(uintptr_t)a[6]);
    return 0;
}
static uint64_t extra_glGetActiveUniform(const uint32_t *a,uint32_t site){
    (void)site;
    glGetActiveUniform(a[0],a[1],(GLsizei)a[2],(GLsizei *)(uintptr_t)a[3],
        (GLint *)(uintptr_t)a[4],(GLenum *)(uintptr_t)a[5],(GLchar *)(uintptr_t)a[6]);
    return 0;
}
static uint64_t extra_glGetBufferSubData(const uint32_t *a,uint32_t site){
    (void)site;glGetBufferSubData(a[0],(int32_t)a[1],(int32_t)a[2],(void *)(uintptr_t)a[3]);return 0;
}
static uint64_t extra_glGetCompressedTexImage(const uint32_t *a,uint32_t site){
    (void)site;glGetCompressedTexImage(a[0],(int32_t)a[1],(void *)(uintptr_t)a[2]);return 0;
}
static uint64_t extra_glGetFramebufferAttachmentParameteriv(const uint32_t *a,uint32_t site){
    (void)site;glGetFramebufferAttachmentParameteriv(a[0],a[1],a[2],(void *)(uintptr_t)a[3]);return 0;
}
static uint64_t extra_glGetTexImage(const uint32_t *a,uint32_t site){
    (void)site;glGetTexImage(a[0],(int32_t)a[1],a[2],a[3],(void *)(uintptr_t)a[4]);return 0;
}
static uint64_t extra_glGetTexLevelParameteriv(const uint32_t *a,uint32_t site){
    (void)site;glGetTexLevelParameteriv(a[0],(int32_t)a[1],a[2],(void *)(uintptr_t)a[3]);return 0;
}
static uint64_t extra_glIsEnabled(const uint32_t *a,uint32_t site){
    (void)site;return glIsEnabled(a[0]);
}
static uint64_t extra_glReadPixels(const uint32_t *a,uint32_t site){
    (void)site;glReadPixels((int32_t)a[0],(int32_t)a[1],(int32_t)a[2],(int32_t)a[3],a[4],a[5],(void *)(uintptr_t)a[6]);return 0;
}
static uint64_t extra_glTransformFeedbackVaryings(const uint32_t *a,uint32_t site){
    (void)site;int32_t count=(int32_t)a[1];
    const uint32_t *guest=(void *)(uintptr_t)a[2];
    const char **names=count>0?calloc((size_t)count,sizeof(*names)):NULL;
    if(count>0&&!names)return 0;
    for(int32_t i=0;i<count;++i)names[i]=(void *)(uintptr_t)guest[i];
    glTransformFeedbackVaryings(a[0],count,names,a[3]);free(names);return 0;
}
/* MW2's legacy-context entry points, then COD2's fixed-function, NV combiner
 * and APPLE vertex-array/fence entry points. Integer and pointer arguments
 * widen losslessly into SysV integer slots, so one forwarder serves every
 * signature in mw2_integer_calls; float, string-array and sync-object
 * signatures have their own adapters. */
typedef uint64_t (*integer_gl_fn)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,
                                  uint64_t,uint64_t,uint64_t,uint64_t);
static const struct { const char *name; unsigned count; } mw2_integer_calls[] = {
    {"glBindBufferBaseEXT",3},{"glCompressedTexImage3DARB",9},
    {"glDrawArraysInstancedARB",4},{"glFinishObjectAPPLE",2},
    {"glGetAttribLocation",2},{"glGetCompressedTexImageARB",3},
    {"glGetUniformBufferSizeEXT",2},{"glRenderbufferStorageMultisampleEXT",5},
    {"glStencilFuncSeparateATI",4},{"glTexParameteriv",3},
    {"glUniform1fv",3},{"glUniform2fv",3},{"glUniform3fv",3},
    {"glUniform2iv",3},{"glUniform3iv",3},{"glUniformBufferEXT",3},
    {"glUniformMatrix2fv",4},{"glUniformMatrix3fv",4},{"glUniformMatrix4fv",4},
    {"glValidateProgram",1},
    {"glBindVertexArrayAPPLE",1},{"glCombinerParameterfvNV",2},
    {"glCombinerStageParameterfvNV",3},{"glFinishFenceAPPLE",1},
    {"glFlushVertexArrayRangeAPPLE",2},{"glGenVertexArraysAPPLE",2},
    {"glPointParameterfvARB",2},{"glTexEnvi",3},
    {"glVertexArrayParameteriAPPLE",2},{"glVertexArrayRangeAPPLE",2},
    {"glColor4ub",4},{"glPointParameterfv",2},
};
enum { kMw2IntegerCallCount = sizeof(mw2_integer_calls)/sizeof(mw2_integer_calls[0]) };
static integer_gl_fn mw2_integer_symbols[kMw2IntegerCallCount];
#define MW2_INTEGER_CALL(index) \
static uint64_t mw2_integer_##index(const uint32_t *a,uint32_t site){ \
    (void)site;uint64_t v[9]={0}; \
    /* index 2 is glDrawArraysInstancedARB. Other expansions constant-fold this out. */ \
    if(index==2) objc_bridge32_reconcile_fragment_shadows(); \
    for(unsigned i=0;i<mw2_integer_calls[index].count;++i)v[i]=a[i]; \
    return (uint32_t)mw2_integer_symbols[index](v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8]); \
}
MW2_INTEGER_CALL(0) MW2_INTEGER_CALL(1) MW2_INTEGER_CALL(2) MW2_INTEGER_CALL(3)
MW2_INTEGER_CALL(4) MW2_INTEGER_CALL(5) MW2_INTEGER_CALL(6) MW2_INTEGER_CALL(7)
MW2_INTEGER_CALL(8) MW2_INTEGER_CALL(9) MW2_INTEGER_CALL(10) MW2_INTEGER_CALL(11)
MW2_INTEGER_CALL(12) MW2_INTEGER_CALL(13) MW2_INTEGER_CALL(14) MW2_INTEGER_CALL(15)
MW2_INTEGER_CALL(16) MW2_INTEGER_CALL(17) MW2_INTEGER_CALL(18) MW2_INTEGER_CALL(19)
MW2_INTEGER_CALL(20) MW2_INTEGER_CALL(21) MW2_INTEGER_CALL(22) MW2_INTEGER_CALL(23)
MW2_INTEGER_CALL(24) MW2_INTEGER_CALL(25) MW2_INTEGER_CALL(26) MW2_INTEGER_CALL(27)
MW2_INTEGER_CALL(28) MW2_INTEGER_CALL(29) MW2_INTEGER_CALL(30) MW2_INTEGER_CALL(31)
static const lp32_fast_import_fn mw2_integer_handlers[kMw2IntegerCallCount] = {
    mw2_integer_0,mw2_integer_1,mw2_integer_2,mw2_integer_3,mw2_integer_4,mw2_integer_5,
    mw2_integer_6,mw2_integer_7,mw2_integer_8,mw2_integer_9,mw2_integer_10,mw2_integer_11,
    mw2_integer_12,mw2_integer_13,mw2_integer_14,mw2_integer_15,mw2_integer_16,
    mw2_integer_17,mw2_integer_18,mw2_integer_19,mw2_integer_20,mw2_integer_21,
    mw2_integer_22,mw2_integer_23,mw2_integer_24,mw2_integer_25,mw2_integer_26,
    mw2_integer_27,mw2_integer_28,mw2_integer_29,mw2_integer_30,mw2_integer_31,
};
_Static_assert(sizeof(mw2_integer_handlers)/sizeof(mw2_integer_handlers[0])==kMw2IntegerCallCount,
               "one handler per MW2 integer call");
/* Indexed draws read indices through objc_bridge's dynamic index buffer
   copies when one is bound (see objc_bridge32_index_shadow_acquire). */
static uint64_t extra_glDrawRangeElementsBaseVertex(const uint32_t *a,uint32_t site){
    (void)site;objc_bridge32_reconcile_fragment_shadows();
    const void *indices=(const void *)(uintptr_t)a[5];uint32_t shadowed=0;
    objc_bridge32_index_shadow_acquire(a[5],(int32_t)a[3],a[4],&indices,&shadowed);
    glDrawRangeElementsBaseVertex(a[0],a[1],a[2],(GLsizei)a[3],a[4],indices,(GLint)a[6]);
    if(shadowed)objc_bridge32_index_shadow_release(shadowed);
    return 0;
}
static uint64_t extra_glDrawElementsInstancedBaseVertex(const uint32_t *a,uint32_t site){
    (void)site;objc_bridge32_reconcile_fragment_shadows();
    const void *indices=(const void *)(uintptr_t)a[3];uint32_t shadowed=0;
    objc_bridge32_index_shadow_acquire(a[3],(int32_t)a[1],a[2],&indices,&shadowed);
    glDrawElementsInstancedBaseVertex(a[0],(GLsizei)a[1],a[2],indices,(GLsizei)a[4],(GLint)a[5]);
    if(shadowed)objc_bridge32_index_shadow_release(shadowed);
    return 0;
}
static uint64_t extra_glDrawElementsInstancedARB(const uint32_t *a,uint32_t site){
    (void)site;static void (*fn)(uint32_t,int32_t,uint32_t,const void *,int32_t);
    if(!fn)fn=(void (*)(uint32_t,int32_t,uint32_t,const void *,int32_t))dlsym(RTLD_DEFAULT,"glDrawElementsInstancedARB");
    if(!fn)return 0;
    objc_bridge32_reconcile_fragment_shadows();
    const void *indices=(const void *)(uintptr_t)a[3];uint32_t shadowed=0;
    objc_bridge32_index_shadow_acquire(a[3],(int32_t)a[1],a[2],&indices,&shadowed);
    fn(a[0],(int32_t)a[1],a[2],indices,(int32_t)a[4]);
    if(shadowed)objc_bridge32_index_shadow_release(shadowed);
    return 0;
}
extern void glTexCoord2f(float,float);
extern void glVertex2f(float,float);
extern void glOrtho(double,double,double,double,double,double);
static uint64_t extra_glTexCoord2f(const uint32_t *a,uint32_t site){
    (void)site;glTexCoord2f(f32(a[0]),f32(a[1]));return 0;
}
static uint64_t extra_glVertex2f(const uint32_t *a,uint32_t site){
    (void)site;glVertex2f(f32(a[0]),f32(a[1]));return 0;
}
static uint64_t extra_glVertexAttrib4f(const uint32_t *a,uint32_t site){
    (void)site;glVertexAttrib4f(a[0],f32(a[1]),f32(a[2]),f32(a[3]),f32(a[4]));return 0;
}
/* GLU defines gluOrtho2D as glOrtho with near -1 and far 1; issuing it
   here keeps it on the selected GL backend. */
/* Gangstar's fixed-function entry points with float/double arguments. */
extern void glLightModelf(uint32_t,float);
extern void glLineWidth(float);
extern void glMultiTexCoord4f(uint32_t,float,float,float,float);
extern void glNormal3f(float,float,float);
extern void glPointParameterf(uint32_t,float);
extern void glSampleCoverage(float,uint8_t);
static uint64_t extra_glLightModelf(const uint32_t *a,uint32_t site){
    (void)site;glLightModelf(a[0],f32(a[1]));return 0;
}
static uint64_t extra_glLineWidth(const uint32_t *a,uint32_t site){
    (void)site;glLineWidth(f32(a[0]));return 0;
}
static uint64_t extra_glMultiTexCoord4f(const uint32_t *a,uint32_t site){
    (void)site;glMultiTexCoord4f(a[0],f32(a[1]),f32(a[2]),f32(a[3]),f32(a[4]));return 0;
}
static uint64_t extra_glNormal3f(const uint32_t *a,uint32_t site){
    (void)site;glNormal3f(f32(a[0]),f32(a[1]),f32(a[2]));return 0;
}
static uint64_t extra_glOrtho(const uint32_t *a,uint32_t site){
    (void)site;glOrtho(f64(a),f64(a+2),f64(a+4),f64(a+6),f64(a+8),f64(a+10));return 0;
}
static uint64_t extra_glPointParameterf(const uint32_t *a,uint32_t site){
    (void)site;glPointParameterf(a[0],f32(a[1]));return 0;
}
static uint64_t extra_glSampleCoverage(const uint32_t *a,uint32_t site){
    (void)site;glSampleCoverage(f32(a[0]),(uint8_t)a[1]);return 0;
}
static uint64_t extra_gluOrtho2D(const uint32_t *a,uint32_t site){
    (void)site;glOrtho(f64(a),f64(a+2),f64(a+4),f64(a+6),-1.0,1.0);return 0;
}
static uint64_t extra_glTransformFeedbackVaryingsEXT(const uint32_t *a,uint32_t site){
    (void)site;static void (*fn)(uint32_t,int32_t,const char **,uint32_t);
    if(!fn)fn=(void (*)(uint32_t,int32_t,const char **,uint32_t))dlsym(RTLD_DEFAULT,"glTransformFeedbackVaryingsEXT");
    int32_t count=(int32_t)a[1];const uint32_t *guest=(void *)(uintptr_t)a[2];
    const char **names=count>0?calloc((size_t)count,sizeof(*names)):NULL;
    if(!fn||(count>0&&!names)){free(names);return 0;}
    for(int32_t i=0;i<count;++i)names[i]=(void *)(uintptr_t)guest[i];
    fn(a[0],count,names,a[3]);free(names);return 0;
}
/* GLsync is a native pointer; the guest holds a small nonzero index. */
static void *sync_objects[4096];
static pthread_mutex_t sync_lock=PTHREAD_MUTEX_INITIALIZER;
static void *host_sync(uint32_t handle){
    return handle&&handle<=4096?sync_objects[handle-1]:NULL;
}
static uint64_t extra_glFenceSync(const uint32_t *a,uint32_t site){
    (void)site;void *sync=glFenceSync(a[0],a[1]);if(!sync)return 0;
    pthread_mutex_lock(&sync_lock);
    for(uint32_t i=0;i<4096;++i)if(!sync_objects[i]){sync_objects[i]=sync;pthread_mutex_unlock(&sync_lock);return i+1;}
    pthread_mutex_unlock(&sync_lock);glDeleteSync(sync);return 0;
}
static uint64_t extra_glDeleteSync(const uint32_t *a,uint32_t site){
    (void)site;pthread_mutex_lock(&sync_lock);
    void *sync=host_sync(a[0]);if(sync)sync_objects[a[0]-1]=NULL;
    pthread_mutex_unlock(&sync_lock);if(sync)glDeleteSync(sync);return 0;
}
static uint64_t extra_glWaitSync(const uint32_t *a,uint32_t site){
    (void)site;void *sync=host_sync(a[0]);
    if(sync)glWaitSync(sync,a[1],(uint64_t)a[2]|((uint64_t)a[3]<<32));return 0;
}
static pthread_once_t once=PTHREAD_ONCE_INIT;
static void initialize(void){
    for(unsigned i=0;i<sizeof(table)/sizeof(table[0]);++i)symbols[i]=dlsym(RTLD_DEFAULT,table[i].name);
    for(unsigned i=0;i<kMw2IntegerCallCount;++i)
        mw2_integer_symbols[i]=(integer_gl_fn)dlsym(RTLD_DEFAULT,mw2_integer_calls[i].name);
}
lp32_fast_import_fn gl_core_bridge32_fast_import(const char *name){
    if(*name=='_')++name; if(strncmp(name,"gl",2))return NULL;
    if(!strcmp(name,"glMaterialf"))return extra_glMaterialf;
    if(!strcmp(name,"glActiveStencilFaceEXT"))return extra_glActiveStencilFaceEXT;
    if(!strcmp(name,"glDeleteVertexArraysAPPLE"))return extra_glDeleteVertexArraysAPPLE;
    if(!strcmp(name,"glPointParameterfARB"))return extra_glPointParameterfARB;
    if(!strcmp(name,"glPointSize"))return extra_glPointSize;
    if(!strcmp(name,"glPushClientAttrib"))return extra_glPushClientAttrib;
    if(!strcmp(name,"glPopClientAttrib"))return extra_glPopClientAttrib;
    if(!strcmp(name,"glPixelStorei"))return extra_glPixelStorei;
    if(!strcmp(name,"glGetBooleanv"))return extra_glGetBooleanv;
    if(!strcmp(name,"glGetActiveAttrib"))return extra_glGetActiveAttrib;
    if(!strcmp(name,"glGetActiveUniform"))return extra_glGetActiveUniform;
    if(!strcmp(name,"glGetBufferSubData"))return extra_glGetBufferSubData;
    if(!strcmp(name,"glGetCompressedTexImage"))return extra_glGetCompressedTexImage;
    if(!strcmp(name,"glGetFramebufferAttachmentParameteriv"))return extra_glGetFramebufferAttachmentParameteriv;
    if(!strcmp(name,"glGetTexImage"))return extra_glGetTexImage;
    if(!strcmp(name,"glGetTexLevelParameteriv"))return extra_glGetTexLevelParameteriv;
    if(!strcmp(name,"glIsEnabled"))return extra_glIsEnabled;
    if(!strcmp(name,"glReadPixels"))return extra_glReadPixels;
    if(!strcmp(name,"glTransformFeedbackVaryings"))return extra_glTransformFeedbackVaryings;
    if(!strcmp(name,"glDrawRangeElementsBaseVertex"))return extra_glDrawRangeElementsBaseVertex;
    if(!strcmp(name,"glDrawElementsInstancedBaseVertex"))return extra_glDrawElementsInstancedBaseVertex;
    if(!strcmp(name,"glDrawElementsInstancedARB"))return extra_glDrawElementsInstancedARB;
    if(!strcmp(name,"glTexCoord2f"))return extra_glTexCoord2f;
    if(!strcmp(name,"glVertex2f"))return extra_glVertex2f;
    if(!strcmp(name,"glVertexAttrib4f"))return extra_glVertexAttrib4f;
    if(!strcmp(name,"gluOrtho2D"))return extra_gluOrtho2D;
    if(!strcmp(name,"glLightModelf"))return extra_glLightModelf;
    if(!strcmp(name,"glLineWidth"))return extra_glLineWidth;
    if(!strcmp(name,"glMultiTexCoord4f"))return extra_glMultiTexCoord4f;
    if(!strcmp(name,"glNormal3f"))return extra_glNormal3f;
    if(!strcmp(name,"glOrtho"))return extra_glOrtho;
    if(!strcmp(name,"glPointParameterf"))return extra_glPointParameterf;
    if(!strcmp(name,"glSampleCoverage"))return extra_glSampleCoverage;
    if(!strcmp(name,"glTransformFeedbackVaryingsEXT"))return extra_glTransformFeedbackVaryingsEXT;
    if(!strcmp(name,"glFenceSync"))return extra_glFenceSync;
    if(!strcmp(name,"glDeleteSync"))return extra_glDeleteSync;
    if(!strcmp(name,"glWaitSync"))return extra_glWaitSync;
    pthread_once(&once,initialize);
    for(unsigned i=0;i<sizeof(table)/sizeof(table[0]);++i)if(!strcmp(name,table[i].name))return symbols[i]?table[i].call:NULL;
    for(unsigned i=0;i<kMw2IntegerCallCount;++i)
        if(!strcmp(name,mw2_integer_calls[i].name))return mw2_integer_symbols[i]?mw2_integer_handlers[i]:NULL;
    return NULL;
}
static int glsl_ident(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
static int glsl_token_at(const char *source, size_t length, size_t index, const char *token, size_t token_length) {
    if (index + token_length > length || memcmp(source + index, token, token_length) != 0) return 0;
    if (index && glsl_ident((unsigned char)source[index - 1])) return 0;
    if (index + token_length < length && glsl_ident((unsigned char)source[index + token_length])) return 0;
    return 1;
}
static int glsl_has_token(const char *source, size_t length, const char *token) {
    size_t token_length = strlen(token);
    if (!source || token_length > length) return 0;
    for (size_t index = 0; index + token_length <= length; ++index)
        if (glsl_token_at(source, length, index, token, token_length)) return 1;
    return 0;
}
/* #version and the other leading directives have to stay ahead of new code. */
static size_t glsl_prologue_end(const char *source, size_t length) {
    size_t index = 0;
    if (length >= 3 && (unsigned char)source[0] == 0xEF && (unsigned char)source[1] == 0xBB && (unsigned char)source[2] == 0xBF)
        index = 3;
    while (index < length) {
        size_t cursor = index;
        while (cursor < length && (source[cursor] == ' ' || source[cursor] == '\t' || source[cursor] == '\r')) ++cursor;
        if (cursor < length && source[cursor] == '\n') { index = cursor + 1; continue; }
        if (cursor >= length || source[cursor] != '#') return index;
        while (cursor < length && source[cursor] != '\n') {
            if (source[cursor] == '\\' && cursor + 1 < length && source[cursor + 1] == '\n') cursor += 2;
            else ++cursor;
        }
        index = cursor < length ? cursor + 1 : cursor;
    }
    return index;
}
/*
 * Apple's shadow2DProj divides correctly, but a sample whose shadow-space
 * coordinate falls outside the map compares as shadowed. The sun partitions
 * end on a plane, so that failure is a hard diagonal across the sky. Inside
 * the map the hardware compare is unchanged, so casters still occlude.
 * shadow2D returns vec4 on this driver; the helper matches that.
 */
char *gl_core_bridge32_rewrite_glsl_shadows(const char *source, size_t length) {
    static int keep = -1;
    if (keep < 0) {
        const char *env = getenv("LP32_KEEP_GLSL_SHADOWS");
        keep = env && env[0] && strcmp(env, "0") != 0;
    }
    if (keep || !source || !length) return NULL;
    int projective = glsl_has_token(source, length, "shadow2DProj");
    int rectangle = glsl_has_token(source, length, "shadow2DRectProj");
    if (!projective && !rectangle) return NULL;
    static const char projective_helper[] =
        "vec4 lp32_shadow2DProj(sampler2DShadow tex, vec4 coord) {\n"
        "    vec3 p = coord.xyz / coord.w;\n"
        "    if (p.x < 0.0 || p.y < 0.0 || p.z < 0.0 || p.x > 1.0 || p.y > 1.0 || p.z > 1.0)\n"
        "        return vec4(1.0);\n"
        "    return shadow2D(tex, p);\n"
        "}\n"
        "#define shadow2DProj(tex, coord) lp32_shadow2DProj(tex, coord)\n";
    static const char rectangle_helper[] =
        "vec4 lp32_shadow2DRectProj(sampler2DRectShadow tex, vec4 coord) {\n"
        "    vec3 p = coord.xyz / coord.w;\n"
        "    if (p.z < 0.0 || p.z > 1.0) return vec4(1.0);\n"
        "    return shadow2DRect(tex, p);\n"
        "}\n"
        "#define shadow2DRectProj(tex, coord) lp32_shadow2DRectProj(tex, coord)\n";
    size_t at = glsl_prologue_end(source, length);
    size_t extra = (projective ? sizeof projective_helper - 1 : 0) + (rectangle ? sizeof rectangle_helper - 1 : 0);
    char *rewritten = malloc(length + extra + 1);
    if (!rewritten) return NULL;
    memcpy(rewritten, source, at);
    char *cursor = rewritten + at;
    if (projective) {
        memcpy(cursor, projective_helper, sizeof projective_helper - 1);
        cursor += sizeof projective_helper - 1;
    }
    if (rectangle) {
        memcpy(cursor, rectangle_helper, sizeof rectangle_helper - 1);
        cursor += sizeof rectangle_helper - 1;
    }
    memcpy(cursor, source + at, length - at);
    cursor[length - at] = 0;
    return rewritten;
}

enum { kShadowShaderSlots = 256 };
struct glsl_shadow_original {
    uint32_t id;
    char *source;
    size_t length;
    int live;
};
static struct glsl_shadow_original shadow_originals[kShadowShaderSlots];
static int shadow_rewrite_logs;
static int shadow_reject_logs;

static void remember_shadow_original(uint32_t shader, const char *source, size_t length) {
    struct glsl_shadow_original *slot = NULL;
    for (int index = 0; index < kShadowShaderSlots; ++index) {
        if (shadow_originals[index].live && shadow_originals[index].id == shader) {
            slot = &shadow_originals[index];
            break;
        }
        if (!slot && !shadow_originals[index].live) slot = &shadow_originals[index];
    }
    if (!slot) return;
    char *copy = malloc(length + 1);
    if (!copy) return;
    memcpy(copy, source, length);
    copy[length] = 0;
    free(slot->source);
    slot->id = shader;
    slot->source = copy;
    slot->length = length;
    slot->live = 1;
}
static void gl_core_bridge32_forget_shadow_shader(uint32_t shader) {
    for (int index = 0; index < kShadowShaderSlots; ++index) {
        if (!shadow_originals[index].live || shadow_originals[index].id != shader) continue;
        free(shadow_originals[index].source);
        shadow_originals[index].source = NULL;
        shadow_originals[index].live = 0;
        return;
    }
}
static void gl_core_bridge32_shadow_compile_fallback(uint32_t shader) {
    struct glsl_shadow_original *slot = NULL;
    for (int index = 0; index < kShadowShaderSlots; ++index) {
        if (shadow_originals[index].live && shadow_originals[index].id == shader) {
            slot = &shadow_originals[index];
            break;
        }
    }
    if (!slot) return;
    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled) return;
    char log[480];
    GLsizei written = 0;
    glGetShaderInfoLog(shader, (GLsizei)sizeof log - 1, &written, log);
    if (written < 0) written = 0;
    if ((size_t)written >= sizeof log) written = (GLsizei)sizeof log - 1;
    log[written] = 0;
    if (shadow_reject_logs < 8) {
        ++shadow_reject_logs;
        fprintf(stderr, "compat32: GLSL shadow rewrite rejected shader %u: %s\n", shader, log);
    }
    const char *one = slot->source;
    GLint length = (GLint)slot->length;
    glShaderSource(shader, 1, &one, &length);
    ((void (*)(uint32_t))symbols[21])(shader);
    gl_core_bridge32_forget_shadow_shader(shader);
}
static int append_shader_bytes(char **buffer, size_t *length, size_t *capacity, const char *text, size_t count) {
    if (count > (4u << 20) || *length > (4u << 20) || *length + count > (4u << 20)) return 0;
    if (*length + count + 1 > *capacity) {
        size_t next = *capacity ? *capacity : 4096;
        while (next < *length + count + 1) {
            if (next > (4u << 20)) return 0;
            next *= 2;
        }
        char *grown = realloc(*buffer, next);
        if (!grown) return 0;
        *buffer = grown;
        *capacity = next;
    }
    if (count) memcpy(*buffer + *length, text, count);
    *length += count;
    (*buffer)[*length] = 0;
    return 1;
}
static uint64_t shader_source(const uint32_t *a,uint32_t site) {
    (void)site;
    extern void glShaderSource(uint32_t,int32_t,const char *const *,const int32_t *);
    int32_t count=(int32_t)a[1];
    if(count<=0){glShaderSource(a[0],count,NULL,(void *)(uintptr_t)a[3]);return 0;}
    const char **strings=calloc((size_t)count,sizeof(*strings));if(!strings)return 0;
    const uint32_t *guest=(void *)(uintptr_t)a[2];
    const int32_t *lengths = a[3] ? (const int32_t *)(uintptr_t)a[3] : NULL;
    char *joined = NULL;
    size_t joined_len = 0, joined_cap = 0;
    int can_rewrite = guest != NULL && count <= 64;
    for(int32_t i=0;i<count;++i){
        const char *text = guest ? (const char *)(uintptr_t)guest[i] : NULL;
        strings[i]=text;
        if (!can_rewrite || !text) { can_rewrite = 0; continue; }
        size_t piece = 0;
        if (lengths && lengths[i] >= 0) {
            piece = (size_t)lengths[i];
            if (piece > (1u << 20)) can_rewrite = 0;
        } else {
            while (piece < (1u << 20) && text[piece]) ++piece;
            if (text[piece]) can_rewrite = 0;
        }
        if (can_rewrite && !append_shader_bytes(&joined, &joined_len, &joined_cap, text, piece)) can_rewrite = 0;
    }
    /* Asphalt 6 and Fast Five ship the same Glitch engine shader preamble. */
    char *asphalt = can_rewrite && (lp32_profile()->title==LP32_TITLE_ASPHALT6 ||
                                    lp32_profile()->title==LP32_TITLE_FASTFIVE) ?
        lp32_asphalt_rewrite_glsl(joined,joined_len):NULL;
    const char *effective=asphalt?asphalt:joined;
    size_t effective_len=asphalt?strlen(asphalt):joined_len;
    char *rewritten = can_rewrite ? gl_core_bridge32_rewrite_glsl_shadows(effective, effective_len) : NULL;
    const char *dump = getenv("LP32_DUMP_GLSL");
    if (dump && can_rewrite) {
        char path[4096];
        snprintf(path, sizeof path, "%s/shader-%u.glsl", dump, a[0]);
        FILE *file = fopen(path, "wb");
        if (file) { fwrite(joined, 1, joined_len, file); fclose(file); }
    }
    if (rewritten) {
        const char *one = rewritten;
        glShaderSource(a[0], 1, &one, NULL);
        remember_shadow_original(a[0], effective, effective_len);
        if (shadow_rewrite_logs < 8) {
            ++shadow_rewrite_logs;
            fprintf(stderr, "compat32: GLSL shadow samples outside the map stay lit (shader %u, %zu bytes)\n", a[0], joined_len);
        }
        free(rewritten);
    } else if(asphalt) {
        const char *one=asphalt;
        glShaderSource(a[0],1,&one,NULL);
    } else {
        glShaderSource(a[0],count,strings,(void *)(uintptr_t)a[3]);
    }
    free(strings);
    free(asphalt);
    free(joined);
    return 0;
}
int gl_core_bridge32_dispatch(const char *name,const uint32_t *a,uint64_t *out){
    if(!strcmp(name,"_glShaderSource")||!strcmp(name,"glShaderSource")){*out=shader_source(a,0);return 1;}
    lp32_fast_import_fn fn=gl_core_bridge32_fast_import(name);if(!fn)return 0;*out=fn(a,0);return 1;
}
