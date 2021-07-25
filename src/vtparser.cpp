#include <memory>
#include "shared.h"
#include "vtparser.h"

inline bool is_cmd_char_w(wchar_t c) {
    return c <= L'\x17' || c == L'\x19' || c >= L'\x1c' && c <= L'\x1f';
}

inline bool is_cmd_char_a(char c) {
    return c <= '\x17' || c == '\x19' || c >= '\x1c' && c <= '\x1f';
}

inline void vt_parse_move_to_buffer(vt_parse_state* s, const void* pc) {
    if (!s->using_buffer) {
        Assert(s->params_start);
        // this function shouldn't be called if end is already known
        Assert(!s->end);
        size_t bytes = (char*)pc - (char*)s->params_start - 1;
        if (s->buffer_size < bytes) {
            s->buffer = (char*)realloc(s->buffer, bytes);
        }
        s->buffer = (char*)memcpy(s->buffer, s->params_start, bytes);
        if (s->intermediate_start) {
            s->intermediate_start = s->buffer + ((char*)s->intermediate_start - (char*)s->params_start);
        }
        s->params_start = s->buffer;
        s->using_buffer = true;
        s->buffer_idx = bytes;
    }
}

inline char* vt_parse_record_char_w(vt_parse_state* s, const wchar_t* pc, bool last_char) {
    if (last_char && !s->using_buffer) {
        vt_parse_move_to_buffer(s, pc + 1);
        return s->buffer + s->buffer_idx - 2;
    } else if (s->using_buffer) {
        if (s->buffer_idx + 1 >= s->buffer_size) {
            s->buffer_size *= 2;
            s->buffer = (char*)realloc(s->buffer, s->buffer_size);
        }
        char* result = s->buffer;
        wchar_t c = *pc;
        // do we really have to handle these as wchars?
        s->buffer[s->buffer_idx++] = c & 0xff;
        s->buffer[s->buffer_idx++] = (c >> 8) & 0xff;
        return result;
    } else {
        return (char*)pc;
    }
}

inline const char* vt_parse_record_char_a(vt_parse_state* s, const char* pc, bool last_char) {
    if (last_char && !s->using_buffer) {
        vt_parse_move_to_buffer(s, pc + 1);
        return s->buffer + s->buffer_idx - 1;
    } else if (s->using_buffer) {
        if (s->buffer_idx >= s->buffer_size) {
            s->buffer_size *= 2;
            s->buffer = (char*)realloc(s->buffer, s->buffer_size);
        }
        char* result = s->buffer + s->buffer_idx;
        s->buffer[s->buffer_idx++] = *pc;
        return result;
    } else {
        return pc;
    }
}

// based on https://vt100.net/emu/dec_ansi_parser
vt_action vt_process_char_w(vt_parse_state* s, const wchar_t* pc, bool last_char) {
    wchar_t c = *pc;
    // first check for anywhere->X transition chars
    if (c == L'\x18' || c == L'\x1a' || c >= L'\x80' && c <= L'\x8f' || c >= L'\x91' && c <= L'\x97' || c == L'\x99' || c == L'\x9a' || c == L'\x9c') {
        s->state = VtStateGround;
        return c == L'\x9c' ? Ignore : Execute;
    } else if (c == L'\x1b') {
        // todo: handle exit action of current state (if any)
        s->state = VtStateEscape;
        reset_vt_parse_state(s);
        return Ignore;
    } else if (c == L'\x90') {
        s->state = VtStateDcsEntry;
        reset_vt_parse_state(s);
        return Ignore;
    } else if (c == L'\x98' || c == L'\x9e' || c == L'\x9f') {
        s->state = VtStateSosPmApcString;
        reset_vt_parse_state(s);
        return Ignore;
    } else if (c == L'\x9b') {
        s->state = VtStateCsiEntry;
        reset_vt_parse_state(s);
        return Ignore;
    } else if (c == L'\x9d') {
        s->state = VtStateOscString;
        reset_vt_parse_state(s);
        return Ignore;
    }

    // nothing matched, now check based on current state
    switch (s->state) {
    case VtStateGround:
    {
        return is_cmd_char_w(c) ? Execute : Print;
    }
    case VtStateEscape:
    {
        if (is_cmd_char_w(c)) {
            return Execute;
        } else if (c >= L'\x20' && c <= L'\x2f') {
            if (last_char) {
                s->using_buffer = true;
            }
            s->params_start = vt_parse_record_char_w(s, pc, false);
            s->state = VtStateEscapeIntermediate;
            return Ignore;
        } else if (c == L'\x7f') {
            return Ignore;
        } else if (c == L'\x50') {
            s->state = VtStateDcsEntry;
            return Ignore;
        } else if (c == L'\x58' || c == L'\x5e' || c == L'\x5f') {
            s->state = VtStateSosPmApcString;
            return Ignore;
        } else if (c == L'\x5b') {
            s->state = VtStateCsiEntry;
            return Ignore;
        } else if (c == L'\x5d') {
            s->state = VtStateOscString;
            return Ignore;
        } else {
            s->params_start = pc;
            s->end = pc;
            return EscDispatch;
        }
    }
    case VtStateEscapeIntermediate: {
        if (is_cmd_char_w(c)) {
            vt_parse_move_to_buffer(s, pc);
            return Execute;
        } else if (c == L'\x7f') {
            return Ignore;
        } else if (c >= '\x20' && c <= '\x2f') {
            vt_parse_record_char_w(s, pc, last_char);
            return Ignore;
        } else {
            s->end = vt_parse_record_char_w(s, pc, false);
            s->state = VtStateGround;
            return EscDispatch;
        }
    }
    case VtStateCsiEntry:
    {
        // in this state we can avoid buffer check as nothing could've been buffered
        if (is_cmd_char_w(c)) {
            return Execute;
        } else if (c == L'\x7f') {
            return Ignore;
        } else if (c >= L'\x20' && c <= L'\x2f') {
            if (last_char) {
                s->using_buffer = true;
            }
            s->params_start = vt_parse_record_char_w(s, pc, false);
            s->intermediate_start = s->params_start;
            s->state = VtStateCsiIntermediate;
            return Ignore;
        } else if (c == L'\x3a') {
            s->state = VtStateCsiIgnore;
            return Ignore;
        } else if (c >= L'\x40' && c <= L'\x7e') {
            s->params_start = pc;
            s->intermediate_start = pc;
            s->end = pc;
            s->state = VtStateGround;
            return CsiDispatch;
        } else {
            if (last_char) {
                s->using_buffer = true;
            }
            s->params_start = vt_parse_record_char_w(s, pc, false);
            s->intermediate_start = 0;
            s->state = VtStateCsiParam;
            return Ignore;
        }
    }
    case VtStateCsiParam:
    {
        if (is_cmd_char_w(c)) {
            vt_parse_move_to_buffer(s, pc);
            return Execute;
        } else if (c == L'\x7f') {
            return Ignore;
        } else if (c >= L'\x30' && c <= L'\x39' || c == L'\x3b') {
            vt_parse_record_char_w(s, pc, last_char);
            return Ignore;
        } else if (c >= L'\x20' && c <= L'\x2f') {
            s->intermediate_start = vt_parse_record_char_w(s, pc, last_char);
            s->state = VtStateCsiIntermediate;
            return Ignore;
        } else if (c == '\x3a' || c >= L'\x3c' && c <= L'\x3f') {
            s->state = VtStateCsiIgnore;
            return Ignore;
        } else {
            s->end = vt_parse_record_char_w(s, pc, false);
            s->state = VtStateGround;
            return CsiDispatch;
        }
    }
    case VtStateCsiIntermediate:
    {
        if (is_cmd_char_w(c)) {
            vt_parse_move_to_buffer(s, pc);
            return Execute;
        } else if (c == L'\x7f') {
            return Ignore;
        } else if (c >= L'\x20' && c <= L'\x2f') {
            vt_parse_record_char_w(s, pc, last_char);
            return Ignore;
        } else if (c >= L'\x30' && c <= L'\x3f') {
            s->state = VtStateCsiIgnore;
            return Ignore;
        } else {
            s->end = vt_parse_record_char_w(s, pc, false);
            s->state = VtStateGround;
            return CsiDispatch;
        }
    }
    case VtStateCsiIgnore:
    {
        if (is_cmd_char_w(c)) {
            return Execute;
        } else if (c >= L'\x40' && c <= L'\x7e') {
            s->state = VtStateGround;
            return Ignore;
        } else {
            return Ignore;
        }
    }
    case VtStateOscString:
    {
        if (c == '\x9c' || c == '\x7') { // BELL is not in diagram, but used by bash
            if (s->using_buffer) {
                s->end = s->buffer + s->buffer_idx - 1;
            } else {
                s->end = pc - 1;
            }
            s->state = VtStateGround;
            return OscDispatch;
        } else if (is_cmd_char_w(c)) {
            vt_parse_move_to_buffer(s, pc);
            return Ignore;
        } else {
            vt_parse_record_char_w(s, pc, last_char);
            return Ignore;
        }
    }
    case VtStateDcsEntry:
    {
        if (is_cmd_char_w(c) || c == L'\x7f') {
            return Ignore;
        } else if (c >= L'\x20' && c <= L'\x2f') {
            if (last_char) {
                s->using_buffer = true;
            }
            s->params_start = vt_parse_record_char_w(s, pc, false);
            s->intermediate_start = s->params_start;
            s->state = VtStateDcsIntermediate;
            return Ignore;
        } else if (c >= L'\x30' && c <= L'\x39' || c == L'\x3B' || c >= L'\x3c' && c <= L'\x3f') {
            if (last_char) {
                s->using_buffer = true;
            }
            s->params_start = vt_parse_record_char_w(s, pc, false);
            s->intermediate_start = 0;
            s->state = VtStateDcsParam;
            return Ignore;
        } else if (c == '\x3a') {
            s->state = VtStateDcsIgnore;
            return Ignore;
        } else {
            if (last_char) {
                s->using_buffer = true;
            }
            s->params_start = vt_parse_record_char_w(s, pc, false);
            s->state = VtStateDcsPassthrough;
            return Ignore;
        }
    }
    case VtStateDcsParam:
    {
        if (is_cmd_char_w(c) || c == L'\x7f') {
            vt_parse_move_to_buffer(s, pc);
            return Ignore;
        } else if (c >= L'\x30' && c <= L'\x39' || c == L'\x3b') {
            vt_parse_record_char_w(s, pc, last_char);
            return Ignore;
        } else if (c >= L'\x20' && c <= L'\x2f') {
            s->intermediate_start = vt_parse_record_char_w(s, pc, last_char);
            s->state = VtStateDcsIntermediate;
            return Ignore;
        } else if (c == L'\x3a' || c >= L'\x3c' && c <= L'\x3f') {
            s->state = VtStateDcsIgnore;
            return Ignore;
        } else {
            vt_parse_record_char_w(s, pc, last_char);
            s->state = VtStateDcsPassthrough;
            return Ignore;
        }
    }
    case VtStateDcsIntermediate:
    {
        if (is_cmd_char_w(c) || c == L'\x7f') {
            vt_parse_move_to_buffer(s, pc);
            return Ignore;
        } else if (c >= L'\x20' && c <= L'\x2f') {
            vt_parse_record_char_w(s, pc, last_char);
            return Ignore;
        } else if (c >= L'\x30' && c <= L'\x3f') {
            s->state = VtStateDcsIgnore;
            return Ignore;
        } else {
            vt_parse_record_char_w(s, pc, last_char);
            s->state = VtStateDcsPassthrough;
            return Ignore;
        }
    }
    case VtStateDcsIgnore:
    {
        if (c == L'\x9c') {
            s->state = VtStateGround;
            return Ignore;
        } else {
            return Ignore;
        }
    }
    case VtStateDcsPassthrough:
    {
        if (c == L'\x7f') {
            vt_parse_move_to_buffer(s, pc);
            return Ignore;
        } else if (c == L'\x9c') {
            if (s->using_buffer) {
                s->end = s->buffer + s->buffer_idx - 1;
            } else {
                s->end = pc - 1;
            }
            s->state = VtStateGround;
            return DcsDispatch;
        } else {
            vt_parse_record_char_w(s, pc, last_char);
            return Ignore;
        }
    }
    case VtStateSosPmApcString:
    {
        if (c == L'\x9c') {
            s->state = VtStateGround;
            return Ignore;
        } else {
            return Ignore;
        }
    }
    }
    Assert(!"Unreachable");
}


vt_action vt_process_char_a(vt_parse_state* s, const char* pc, bool last_char) {
    char c = *pc;
    // first check for anywhere->X transition chars
    if (c == '\x18' || c == '\x1a' || c >= '\x80' && c <= '\x8f' || c >= '\x91' && c <= '\x97' || c == '\x99' || c == '\x9a' || c == '\x9c') {
        s->state = VtStateGround;
        return c == '\x9c' ? Ignore : Execute;
    } else if (c == '\x1b') {
        // todo: handle exit action of current state (if any)
        s->state = VtStateEscape;
        reset_vt_parse_state(s);
        return Ignore;
    } else if (c == '\x90') {
        s->state = VtStateDcsEntry;
        reset_vt_parse_state(s);
        return Ignore;
    } else if (c == '\x98' || c == '\x9e' || c == '\x9f') {
        s->state = VtStateSosPmApcString;
        reset_vt_parse_state(s);
        return Ignore;
    } else if (c == '\x9b') {
        s->state = VtStateCsiEntry;
        reset_vt_parse_state(s);
        return Ignore;
    } else if (c == '\x9d') {
        s->state = VtStateOscString;
        reset_vt_parse_state(s);
        return Ignore;
    }

    // nothing matched, now check based on current state
    switch (s->state) {
    case VtStateGround:
    {
        return is_cmd_char_a(c) ? Execute : Print;
    }
    case VtStateEscape:
    {
        if (is_cmd_char_a(c)) {
            return Execute;
        } else if (c >= '\x20' && c <= '\x2f') {
            if (last_char) {
                s->using_buffer = true;
            }
            s->params_start = vt_parse_record_char_a(s, pc, false);
            s->state = VtStateEscapeIntermediate;
            return Ignore;
        } else if (c == '\x7f') {
            return Ignore;
        } else if (c == '\x50') {
            s->state = VtStateDcsEntry;
            return Ignore;
        } else if (c == '\x58' || c == '\x5e' || c == '\x5f') {
            s->state = VtStateSosPmApcString;
            return Ignore;
        } else if (c == '\x5b') {
            s->state = VtStateCsiEntry;
            return Ignore;
        } else if (c == '\x5d') {
            s->state = VtStateOscString;
            return Ignore;
        } else {
            s->params_start = pc;
            s->end = pc;
            return EscDispatch;
        }
    }
    case VtStateEscapeIntermediate: {
        if (is_cmd_char_a(c)) {
            vt_parse_move_to_buffer(s, pc);
            return Execute;
        } else if (c == '\x7f') {
            return Ignore;
        } else if (c >= '\x20' && c <= '\x2f') {
            vt_parse_record_char_a(s, pc, last_char);
            return Ignore;
        } else {
            s->end = vt_parse_record_char_a(s, pc, false);
            s->state = VtStateGround;
            return EscDispatch;
        }
    }
    case VtStateCsiEntry:
    {
        if (is_cmd_char_a(c)) {
            return Execute;
        } else if (c == '\x7f') {
            return Ignore;
        } else if (c >= '\x20' && c <= '\x2f') {
            if (last_char) {
                s->using_buffer = true;
            }
            s->params_start = vt_parse_record_char_a(s, pc, false);
            s->intermediate_start = s->params_start;
            s->state = VtStateCsiIntermediate;
            return Ignore;
        } else if (c == '\x3a') {
            s->state = VtStateCsiIgnore;
            return Ignore;
        } else if (c >= '\x40' && c <= '\x7e') {
            s->params_start = pc;
            s->intermediate_start = pc;
            s->end = pc;
            s->state = VtStateGround;
            return CsiDispatch;
        } else {
            if (last_char) {
                s->using_buffer = true;
            }
            s->params_start = vt_parse_record_char_a(s, pc, false);
            s->intermediate_start = 0;
            s->state = VtStateCsiParam;
            return Ignore;
        }
    }
    case VtStateCsiParam:
    {
        if (is_cmd_char_a(c)) {
            vt_parse_move_to_buffer(s, pc);
            return Execute;
        } else if (c == '\x7f') {
            return Ignore;
        } else if (c >= '\x30' && c <= '\x39' || c == '\x3b') {
            vt_parse_record_char_a(s, pc, last_char);
            return Ignore;
        } else if (c >= '\x20' && c <= '\x2f') {
            s->intermediate_start = vt_parse_record_char_a(s, pc, last_char);
            s->state = VtStateCsiIntermediate;
            return Ignore;
        } else if (c == '\x3a' || c >= '\x3c' && c <= '\x3f') {
            s->state = VtStateCsiIgnore;
            return Ignore;
        } else {
            s->end = vt_parse_record_char_a(s, pc, false);
            s->state = VtStateGround;
            return CsiDispatch;
        }
    }
    case VtStateCsiIntermediate:
    {
        if (is_cmd_char_a(c)) {
            vt_parse_move_to_buffer(s, pc);
            return Execute;
        } else if (c == '\x7f') {
            return Ignore;
        } else if (c >= '\x20' && c <= '\x2f') {
            vt_parse_record_char_a(s, pc, last_char);
            return Ignore;
        } else if (c >= '\x30' && c <= '\x3f') {
            s->state = VtStateCsiIgnore;
            return Ignore;
        } else {
            s->end = vt_parse_record_char_a(s, pc, false);
            s->state = VtStateGround;
            return CsiDispatch;
        }
    }
    case VtStateCsiIgnore:
    {
        if (is_cmd_char_a(c)) {
            return Execute;
        } else if (c >= '\x40' && c <= '\x7e') {
            s->state = VtStateGround;
            return Ignore;
        } else {
            return Ignore;
        }
    }
    case VtStateOscString:
    {
        if (c == '\x9c' || c == '\x7') { // BELL is not in diagram, but used by bash
            if (s->using_buffer) {
                s->end = s->buffer + s->buffer_idx - 1;
            } else {
                s->end = pc - 1;
            }
            s->state = VtStateGround;
            return OscDispatch;
        } else if (is_cmd_char_a(c)) {
            vt_parse_move_to_buffer(s, pc);
            return Ignore;
        } else {
            vt_parse_record_char_a(s, pc, last_char);
            return Ignore;
        }
    }
    case VtStateDcsEntry:
    {
        if (is_cmd_char_a(c) || c == '\x7f') {
            return Ignore;
        } else if (c >= '\x20' && c <= '\x2f') {
            if (last_char) {
                s->using_buffer = true;
            }
            s->params_start = vt_parse_record_char_a(s, pc, false);
            s->intermediate_start = s->params_start;
            s->state = VtStateDcsIntermediate;
            return Ignore;
        } else if (c >= '\x30' && c <= '\x39' || c == '\x3B' || c >= '\x3c' && c <= '\x3f') {
            if (last_char) {
                s->using_buffer = true;
            }
            s->params_start = vt_parse_record_char_a(s, pc, false);
            s->intermediate_start = 0;
            s->state = VtStateDcsParam;
            return Ignore;
        } else if (c == '\x3a') {
            s->state = VtStateDcsIgnore;
            return Ignore;
        } else {
            if (last_char) {
                s->using_buffer = true;
            }
            s->params_start = vt_parse_record_char_a(s, pc, false);
            s->intermediate_start = pc;
            s->state = VtStateDcsPassthrough;
            return Ignore;
        }
    }
    case VtStateDcsParam:
    {
        if (is_cmd_char_a(c) || c == '\x7f') {
            vt_parse_move_to_buffer(s, pc);
            return Ignore;
        } else if (c >= '\x30' && c <= '\x39' || c == '\x3b') {
            vt_parse_record_char_a(s, pc, last_char);
            return Ignore;
        } else if (c >= '\x20' && c <= '\x2f') {
            s->intermediate_start = vt_parse_record_char_a(s, pc, last_char);
            s->state = VtStateDcsIntermediate;
            return Ignore;
        } else if (c == '\x3a' || c >= '\x3c' && c <= '\x3f') {
            s->state = VtStateDcsIgnore;
            return Ignore;
        } else {
            vt_parse_record_char_a(s, pc, last_char);
            s->state = VtStateDcsPassthrough;
            return Ignore;
        }
    }
    case VtStateDcsIntermediate:
    {
        if (is_cmd_char_a(c) || c == '\x7f') {
            vt_parse_move_to_buffer(s, pc);
            return Ignore;
        } else if (c >= '\x20' && c <= '\x2f') {
            vt_parse_record_char_a(s, pc, last_char);
            return Ignore;
        } else if (c >= '\x30' && c <= '\x3f') {
            s->state = VtStateDcsIgnore;
            return Ignore;
        } else {
            vt_parse_record_char_a(s, pc, last_char);
            s->state = VtStateDcsPassthrough;
            return Ignore;
        }
    }
    case VtStateDcsIgnore:
    {
        if (c == '\x9c') {
            s->state = VtStateGround;
            return Ignore;
        } else {
            return Ignore;
        }
    }
    case VtStateDcsPassthrough:
    {
        if (c == '\x7f') {
            vt_parse_move_to_buffer(s, pc);
            return Ignore;
        } else if (c == '\x9c') {
            if (s->using_buffer) {
                s->end = s->buffer + s->buffer_idx - 1;
            } else {
                s->end = pc - 1;
            }
            s->state = VtStateGround;
            return DcsDispatch;
        } else {
            vt_parse_record_char_a(s, pc, last_char);
            return Ignore;
        }
    }
    case VtStateSosPmApcString:
    {
        if (c == '\x9c') {
            s->state = VtStateGround;
            return Ignore;
        } else {
            return Ignore;
        }
    }
    }
    Assert(!"Unreachable");
}
