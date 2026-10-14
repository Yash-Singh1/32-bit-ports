#ifndef LP32_HITCH_RECORDER_H
#define LP32_HITCH_RECORDER_H
#include <stdint.h>
#include <stdbool.h>

enum hitch_kind { HITCH_UNKNOWN, HITCH_NONE, HITCH_COUNTED_RUNTIME, HITCH_DRAW, HITCH_UPLOAD,
    HITCH_SHADER, HITCH_IO, HITCH_WAIT, HITCH_AUDIO, HITCH_GL_STATE,
    HITCH_OBJC, HITCH_RUNTIME, HITCH_KIND_COUNT };
struct hitch_scope {
    uint64_t start, children, generation;
    struct hitch_scope *parent;
    uint64_t ownership_generation;
    bool counted;
};
extern int hitch_recorder_enabled;
extern int hitch_full_imports;
uint64_t hitch_now(void);
unsigned hitch_classify(const char *name);
int hitch_start(const char *path, double threshold_ms);
void hitch_stop(void);
void hitch_program(uint32_t target, uint32_t program);
/* Nested imports are measured exclusively. Calls spanning presentation are
   omitted, so swap/pacing cannot leak into the next frame's work totals. */
void hitch_scope_begin(struct hitch_scope *scope, uint64_t now);
void hitch_scope_end(struct hitch_scope *scope, unsigned kind, const char *name,
                     uint32_t caller, uint64_t now, uint32_t count);
void hitch_note(unsigned kind, const char *name, uint32_t caller,
                uint64_t start, uint64_t end, uint32_t draw_count);
/* Pure, frequent runtime calls still contribute to the frame's call count.
   Their cost remains in work time without taking two clocks per invocation. */
void hitch_count_runtime(void);
/* Count every GL state import, but time one of every 32 by default.
   Full-import mode times every call. Set scope.counted for sampled scopes. */
bool hitch_sample_gl_state(void);
/* Called once per presented frame, after pacing. Durations are CPU wall time. */
void hitch_frame(uint64_t swap, uint64_t work_end, uint64_t flush_end,
                 uint64_t present_end, uint64_t target_ns, bool active);
#endif
