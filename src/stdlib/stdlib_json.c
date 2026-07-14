/**
 * src/stdlib/stdlib_json.c
 * json module: encode(value) → string, decode(string) → value
 *
 * encode: Flux value → JSON string.
 * decode: JSON string → Flux value (recursive descent parser).
 */
#include "stdlib_internal.h"

/* =========================================================================
 * JSON encode
 * ====================================================================== */

/* Forward declaration */
static void encode_value(FluxVM *vm, Value v, char **buf, int *pos, int *cap);

static void encode_ensure(char **buf, int *pos, int need, int *cap) {
    if (*pos + need + 1 <= *cap) return;
    while (*pos + need + 1 > *cap) *cap *= 2;
    *buf = realloc(*buf, (size_t)*cap);
}

static void encode_char(char **buf, int *pos, int *cap, char c) {
    encode_ensure(buf, pos, 1, cap);
    (*buf)[(*pos)++] = c;
}

static void encode_str_raw(char **buf, int *pos, int *cap, const char *s, int len) {
    encode_ensure(buf, pos, len + 2, cap);
    (*buf)[(*pos)++] = '"';
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"')       { encode_ensure(buf,pos,2,cap); (*buf)[(*pos)++]='\\'; (*buf)[(*pos)++]='"'; }
        else if (c == '\\') { encode_ensure(buf,pos,2,cap); (*buf)[(*pos)++]='\\'; (*buf)[(*pos)++]='\\'; }
        else if (c == '\n') { encode_ensure(buf,pos,2,cap); (*buf)[(*pos)++]='\\'; (*buf)[(*pos)++]='n'; }
        else if (c == '\r') { encode_ensure(buf,pos,2,cap); (*buf)[(*pos)++]='\\'; (*buf)[(*pos)++]='r'; }
        else if (c == '\t') { encode_ensure(buf,pos,2,cap); (*buf)[(*pos)++]='\\'; (*buf)[(*pos)++]='t'; }
        else if (c < 0x20) {
            encode_ensure(buf,pos,6,cap);
            *pos += snprintf(*buf+*pos, 7, "\\u%04x", c);
        } else {
            encode_ensure(buf,pos,1,cap);
            (*buf)[(*pos)++] = (char)c;
        }
    }
    (*buf)[(*pos)++] = '"';
}

static void encode_value(FluxVM *vm, Value v, char **buf, int *pos, int *cap) {
    char tmp[64];
    int  tlen;
    switch (v.type) {
        case VAL_NULL:
            encode_ensure(buf, pos, 4, cap);
            memcpy(*buf + *pos, "null", 4); *pos += 4;
            break;
        case VAL_BOOL:
            if (v.as.boolean) { encode_ensure(buf,pos,4,cap); memcpy(*buf+*pos,"true",4);  *pos+=4; }
            else              { encode_ensure(buf,pos,5,cap); memcpy(*buf+*pos,"false",5); *pos+=5; }
            break;
        case VAL_INT:
            tlen = snprintf(tmp, sizeof(tmp), "%lld", (long long)v.as.integer);
            encode_ensure(buf,pos,tlen,cap);
            memcpy(*buf+*pos, tmp, (size_t)tlen); *pos += tlen;
            break;
        case VAL_FLOAT:
            if (isnan(v.as.floating) || isinf(v.as.floating)) {
                encode_ensure(buf,pos,4,cap); memcpy(*buf+*pos,"null",4); *pos+=4;
            } else {
                tlen = snprintf(tmp, sizeof(tmp), "%g", v.as.floating);
                encode_ensure(buf,pos,tlen,cap);
                memcpy(*buf+*pos, tmp, (size_t)tlen); *pos+=tlen;
            }
            break;
        case VAL_OBJECT:
            if (IS_STRING(v)) {
                FluxString *s = AS_STRING(v);
                encode_str_raw(buf, pos, cap, s->chars, s->length);
            } else if (IS_LIST(v)) {
                FluxList *list = AS_LIST(v);
                encode_char(buf,pos,cap,'[');
                for (int i = 0; i < list->elements.count; i++) {
                    if (i) encode_char(buf,pos,cap,',');
                    encode_value(vm, list->elements.data[i], buf, pos, cap);
                }
                encode_char(buf,pos,cap,']');
            } else if (IS_DICT(v)) {
                FluxDict *dict = AS_DICT(v);
                encode_char(buf,pos,cap,'{');
                bool first = true;
                for (int i = 0; i < dict->capacity; i++) {
                    DictEntry *e = &dict->entries[i];
                    if (!e->key) continue;
                    if (!first) encode_char(buf,pos,cap,',');
                    first = false;
                    encode_str_raw(buf,pos,cap, e->key->chars, e->key->length);
                    encode_char(buf,pos,cap,':');
                    encode_value(vm, e->value, buf, pos, cap);
                }
                encode_char(buf,pos,cap,'}');
            } else {
                /* fallback: serialize as string representation */
                FluxString *s = value_to_string(vm, v);
                encode_str_raw(buf,pos,cap, s->chars, s->length);
            }
            break;
    }
}

static Value json_encode(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    int cap = 256, pos = 0;
    char *buf = malloc((size_t)cap);
    if (!buf) return value_null();
    encode_value(vm, argv[0], &buf, &pos, &cap);
    buf[pos] = '\0';
    /* Transfer ownership to a Flux string (object_string_take frees buf) */
    FluxString *result = object_string_take(vm, buf, pos);
    return value_object((FluxObject *)result);
}

/* =========================================================================
 * JSON decode
 * ====================================================================== */

typedef struct {
    const char *src;
    int         pos;
    int         len;
    FluxVM     *vm;
    bool        error;
    char        errmsg[128];
} JsonParser;

static void jp_error(JsonParser *jp, const char *msg) {
    if (!jp->error) {
        jp->error = true;
        snprintf(jp->errmsg, sizeof(jp->errmsg), "json.decode: %s (at offset %d)", msg, jp->pos);
    }
}

static void jp_skip_ws(JsonParser *jp) {
    while (jp->pos < jp->len) {
        char c = jp->src[jp->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') jp->pos++;
        else break;
    }
}

static Value jp_parse_value(JsonParser *jp);

static Value jp_parse_string(JsonParser *jp) {
    if (jp->pos >= jp->len || jp->src[jp->pos] != '"') {
        jp_error(jp, "expected '\"'"); return value_null();
    }
    jp->pos++; /* skip opening " */
    char buf[4096];
    int  out = 0;
    while (jp->pos < jp->len && jp->src[jp->pos] != '"') {
        if (out >= 4090) { jp_error(jp, "string too long"); return value_null(); }
        char c = jp->src[jp->pos++];
        if (c == '\\') {
            if (jp->pos >= jp->len) { jp_error(jp, "unexpected end in escape"); return value_null(); }
            char esc = jp->src[jp->pos++];
            switch (esc) {
                case '"':  buf[out++] = '"';  break;
                case '\\': buf[out++] = '\\'; break;
                case '/':  buf[out++] = '/';  break;
                case 'n':  buf[out++] = '\n'; break;
                case 'r':  buf[out++] = '\r'; break;
                case 't':  buf[out++] = '\t'; break;
                case 'b':  buf[out++] = '\b'; break;
                case 'f':  buf[out++] = '\f'; break;
                case 'u': {
                    /* Basic: read 4 hex digits, store as ASCII if < 128 */
                    if (jp->pos + 4 > jp->len) { jp_error(jp, "bad \\u escape"); return value_null(); }
                    unsigned code = 0;
                    for (int i = 0; i < 4; i++) {
                        char h = jp->src[jp->pos++];
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') code |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code |= (unsigned)(h - 'A' + 10);
                    }
                    if (code < 128) buf[out++] = (char)code;
                    else buf[out++] = '?'; /* simplified: skip non-ASCII */
                    break;
                }
                default: buf[out++] = esc; break;
            }
        } else {
            buf[out++] = c;
        }
    }
    if (jp->pos >= jp->len) { jp_error(jp, "unterminated string"); return value_null(); }
    jp->pos++; /* skip closing " */
    return value_object((FluxObject *)object_string_copy(jp->vm, buf, out));
}

static Value jp_parse_array(JsonParser *jp) {
    jp->pos++; /* skip '[' */
    FluxList *list = object_list_new(jp->vm);
    vm_push(jp->vm, value_object((FluxObject *)list));
    jp_skip_ws(jp);
    if (jp->pos < jp->len && jp->src[jp->pos] == ']') {
        jp->pos++;
        vm_pop(jp->vm);
        return value_object((FluxObject *)list);
    }
    while (!jp->error) {
        jp_skip_ws(jp);
        Value elem = jp_parse_value(jp);
        if (jp->error) break;
        value_array_write(&list->elements, elem);
        jp_skip_ws(jp);
        if (jp->pos >= jp->len) { jp_error(jp, "unterminated array"); break; }
        if (jp->src[jp->pos] == ']') { jp->pos++; break; }
        if (jp->src[jp->pos] != ',') { jp_error(jp, "expected ',' or ']' in array"); break; }
        jp->pos++;
    }
    vm_pop(jp->vm);
    return jp->error ? value_null() : value_object((FluxObject *)list);
}

static Value jp_parse_object(JsonParser *jp) {
    jp->pos++; /* skip '{' */
    FluxDict *dict = object_dict_new(jp->vm);
    vm_push(jp->vm, value_object((FluxObject *)dict));
    jp_skip_ws(jp);
    if (jp->pos < jp->len && jp->src[jp->pos] == '}') {
        jp->pos++;
        vm_pop(jp->vm);
        return value_object((FluxObject *)dict);
    }
    while (!jp->error) {
        jp_skip_ws(jp);
        if (jp->pos >= jp->len || jp->src[jp->pos] != '"') { jp_error(jp, "expected string key"); break; }
        Value key_val = jp_parse_string(jp);
        if (jp->error) break;
        jp_skip_ws(jp);
        if (jp->pos >= jp->len || jp->src[jp->pos] != ':') { jp_error(jp, "expected ':'"); break; }
        jp->pos++;
        jp_skip_ws(jp);
        Value val = jp_parse_value(jp);
        if (jp->error) break;
        FluxString *key = AS_STRING(key_val);
        dict_set(jp->vm, dict, key, val);
        jp_skip_ws(jp);
        if (jp->pos >= jp->len) { jp_error(jp, "unterminated object"); break; }
        if (jp->src[jp->pos] == '}') { jp->pos++; break; }
        if (jp->src[jp->pos] != ',') { jp_error(jp, "expected ',' or '}' in object"); break; }
        jp->pos++;
    }
    vm_pop(jp->vm);
    return jp->error ? value_null() : value_object((FluxObject *)dict);
}

static Value jp_parse_value(JsonParser *jp) {
    jp_skip_ws(jp);
    if (jp->pos >= jp->len) { jp_error(jp, "unexpected end of input"); return value_null(); }
    char c = jp->src[jp->pos];

    if (c == '"') return jp_parse_string(jp);
    if (c == '[') return jp_parse_array(jp);
    if (c == '{') return jp_parse_object(jp);

    if (c == 't') {
        if (jp->pos+4 <= jp->len && memcmp(jp->src+jp->pos, "true", 4)==0) {
            jp->pos += 4; return value_bool(true);
        }
        jp_error(jp, "expected 'true'"); return value_null();
    }
    if (c == 'f') {
        if (jp->pos+5 <= jp->len && memcmp(jp->src+jp->pos,"false",5)==0) {
            jp->pos += 5; return value_bool(false);
        }
        jp_error(jp, "expected 'false'"); return value_null();
    }
    if (c == 'n') {
        if (jp->pos+4 <= jp->len && memcmp(jp->src+jp->pos,"null",4)==0) {
            jp->pos += 4; return value_null();
        }
        jp_error(jp, "expected 'null'"); return value_null();
    }

    /* number */
    if (c == '-' || (c >= '0' && c <= '9')) {
        const char *start = jp->src + jp->pos;
        bool is_float = false;
        if (jp->src[jp->pos] == '-') jp->pos++;
        while (jp->pos < jp->len && jp->src[jp->pos] >= '0' && jp->src[jp->pos] <= '9') jp->pos++;
        if (jp->pos < jp->len && jp->src[jp->pos] == '.') { is_float = true; jp->pos++;
            while (jp->pos < jp->len && jp->src[jp->pos] >= '0' && jp->src[jp->pos] <= '9') jp->pos++;
        }
        if (jp->pos < jp->len && (jp->src[jp->pos]=='e'||jp->src[jp->pos]=='E')) {
            is_float = true; jp->pos++;
            if (jp->pos < jp->len && (jp->src[jp->pos]=='+'||jp->src[jp->pos]=='-')) jp->pos++;
            while (jp->pos < jp->len && jp->src[jp->pos] >= '0' && jp->src[jp->pos] <= '9') jp->pos++;
        }
        char tmp[64];
        int tlen = (int)(jp->src + jp->pos - start);
        if (tlen >= (int)sizeof(tmp)) { jp_error(jp, "number too long"); return value_null(); }
        memcpy(tmp, start, (size_t)tlen); tmp[tlen] = '\0';
        if (is_float) return value_float(strtod(tmp, NULL));
        return value_int(strtoll(tmp, NULL, 10));
    }

    jp_error(jp, "unexpected character"); return value_null();
}

static Value json_decode(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0])) { vm_runtime_error(vm, "json.decode: expected string"); return value_null(); }
    FluxString *s = AS_STRING(argv[0]);
    JsonParser jp = { s->chars, 0, s->length, vm, false, {0} };
    Value result = jp_parse_value(&jp);
    if (jp.error) { vm_runtime_error(vm, "%s", jp.errmsg); return value_null(); }
    return result;
}

/* =========================================================================
 * flux_stdlib_load_json
 * ====================================================================== */

void flux_stdlib_load_json(FluxVM *vm) {
    static const char *names[] = { "encode", "decode" };
    static NativeFn fns[]      = { json_encode, json_decode };
    static int arities[]       = { 1, 1 };
    register_module(vm, "json", names, fns, arities, 2);
}
