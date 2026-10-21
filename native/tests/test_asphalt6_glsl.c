#include "asphalt_glsl.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    const char *source="#ifndef GL_ES\n# define GL_ES 0\n#endif\n#if GL_ES\n#endif]\r\nfloat MY_GL_ES2=0.0;\n";
    const char *expected="#ifndef LP32_ES\n# define LP32_ES 0\n#endif\n#if LP32_ES\n#endif\r\nfloat MY_GL_ES2=0.0;\n";
    char *fixed=lp32_asphalt_rewrite_glsl(source,strlen(source));
    assert(fixed && !strcmp(fixed,expected));free(fixed);
    const char *unchanged="float GL_ES2; float MY_GL_ES; // keep normal shader bytes\n";
    assert(!lp32_asphalt_rewrite_glsl(unchanged,strlen(unchanged)));
    assert(!lp32_asphalt_rewrite_glsl(NULL,0));
    assert(!lp32_asphalt_rewrite_glsl("",0));
    fixed=lp32_asphalt_rewrite_glsl("GL_ES ignored",5);
    assert(fixed && !strcmp(fixed,"LP32_ES"));free(fixed);
    puts("Asphalt GLSL PASS (reserved macro, desktop conditions, shipped directive typo, token boundaries and explicit lengths)");
    return 0;
}
