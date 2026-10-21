#include "asphalt_glsl.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static bool identifier(unsigned char c) {
    return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_';
}

char *lp32_asphalt_rewrite_glsl(const char *source,size_t length) {
    if(!source || length>(4u<<20))return NULL;
    char *result=malloc(length*2+1);
    if(!result)return NULL;
    size_t used=0;
    bool changed=false;
    for(size_t i=0;i<length;) {
        /* The shipped texture shader has a stray bracket on this directive.
           Older Apple drivers accepted it; current GLSL compilers reject it. */
        if(i+7<=length && (i==0 || source[i-1]=='\n' || source[i-1]=='\r') &&
           !memcmp(source+i,"#endif]",7)) {
            memcpy(result+used,"#endif",6);used+=6;i+=7;changed=true;
        } else if(i+5<=length && !memcmp(source+i,"GL_ES",5) &&
           (!i || !identifier((unsigned char)source[i-1])) &&
           (i+5==length || !identifier((unsigned char)source[i+5]))) {
            /* Keep the engine's desktop value and every conditional, while
               avoiding a definition of the reserved built-in macro. */
            memcpy(result+used,"LP32_ES",7);used+=7;i+=5;changed=true;
        } else result[used++]=source[i++];
    }
    result[used]=0;
    if(!changed){free(result);return NULL;}
    return result;
}
