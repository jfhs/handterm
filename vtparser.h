#pragma once

typedef enum vt_state {
    VtStateGround,
    VtStateEscape,
    VtStateEscapeIntermediate,
    VtStateCsiEntry,
    VtStateCsiParam,
    VtStateCsiIntermediate,
    VtStateCsiIgnore,
    VtStateOscString,
    VtStateSosPmApcString,
    VtStateDcsEntry,
    VtStateDcsParam,
    VtStateDcsIntermediate,
    VtStateDcsPassthrough,
    VtStateDcsIgnore
} vt_state;

typedef enum vt_action {
    Ignore,
    Execute,
    Print,
    CsiDispatch,
    EscDispatch,
    OscDispatch,
    DcsDispatch,
} vt_action;

typedef struct vt_parse_state {
    vt_state state;
    const void* params_start;
    const void* intermediate_start;
    const void* end;
    bool using_buffer;
    char* buffer;
    size_t buffer_idx;
    size_t buffer_size;
} vt_parse_state;

inline void reset_vt_parse_state(vt_parse_state* s) {
    s->using_buffer = false;
    s->buffer_idx = 0;
    s->params_start = 0;
    s->intermediate_start = 0;
    s->end = 0;
}

inline bool is_cmd_char_w(wchar_t c);
inline bool is_cmd_char_a(char c);
vt_action vt_process_char_w(vt_parse_state* s, const wchar_t* pc, bool last_char);
vt_action vt_process_char_a(vt_parse_state* s, const char* pc, bool last_char);