/*
 * Copyright 2025 Eduardo Antunes dos Santos Vieira
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <stddef.h>
#include <stdbool.h>
#include <inttypes.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>

#include <ctype.h>
#include <assert.h>
#include <string.h>

#define VERSION "0.1"

typedef int32_t i32;

#ifdef __GNUC__
# define UNUSED __attribute__((unused))
#else
# define UNUSED
#endif // __GNUC__

void *realloc_mem(void *ptr, size_t size) {
    if(size == 0) {
        free(ptr);
        return NULL;
    }
    void *new_ptr = realloc(ptr, size);
    if(new_ptr == NULL) {
        fprintf(stderr, "error: out of memory\n");
        exit(64);
    }
    return new_ptr;
}

#define get_mem(size) realloc_mem(NULL, size)
#define free_mem(ptr) realloc_mem(ptr, 0)

// -----------------------------------------------------------------------------

typedef struct {
    char *text;
    int len;
} StringView;

#define SV_fmt(sv) (sv).len,(sv).text

StringView sv_init(char *str) {
    return (StringView){ .text = str, strlen(str) };
}

StringView sv_copy(StringView src) {
    StringView dest = { .len = src.len };
    dest.text = get_mem(src.len + 1);
    memcpy(dest.text, src.text, src.len);
    return dest;
}

void sv_free(StringView sv) {
    free_mem(sv.text);
}

// Case insensitive string comparison is non-portable in C, unless you
// employ a healthy dose of preprocessor black magic
#ifdef _MSC_VER
# define strncasecmp _strnicmp
#else
# include <strings.h>
#endif // _MSC_VER

// -----------------------------------------------------------------------------

struct processor;
typedef void (*CodeWordFn)(struct processor *);

typedef union value {
    i32 num;
    char ch;
    struct word *xt;  // execution token for a word
    union value *addr; // address of a variable or instruction
} Value;

Value value_read_num(StringView sv, bool *ok) {
    int i = 0;
    i32 n = 0;
    bool neg = false;
    if(sv.text[0] == '-') {
        neg = true;
        i += 1;
    }
    for(; i < sv.len; ++i) {
        if(!isdigit(sv.text[i])) {
            *ok = false;
            break;
        }
        n = n * 10 + (sv.text[i] - '0');
    }
    n = neg ? -n : n;
    return (Value){ .num = n };
}

Value value_read_ch(StringView sv, bool *ok) {
    if(sv.len > 3 || sv.text[2] != '\'') {
        *ok = false;
        return (Value){ .ch = 0 };
    }
    return (Value){ .ch = sv.text[1] };
}

Value value_read(StringView sv, bool *ok) {
    *ok = true;
    if(sv.text[0] == '-' || isdigit(sv.text[0]))
        return value_read_num(sv, ok);
    if(sv.text[0] == '\'')
        return value_read_ch(sv, ok);
    *ok = false;
    return (Value){ .num = 0 };
}

// -----------------------------------------------------------------------------

typedef struct {
    Value *contents;
    int count, capacity;
} DataStack;

void ds_init(DataStack *ds) {
    ds->contents = NULL;
    ds->count = ds->capacity = 0;
}

void ds_push(DataStack *ds, Value c) {
    if(ds->count + 1 > ds->capacity) {
        ds->capacity = (ds->capacity == 0) ?
            8 : ds->capacity * 2;
        ds->contents = realloc_mem(ds->contents,
                ds->capacity * sizeof(*ds->contents));
    }
    ds->contents[ds->count++] = c;
}

void ds_push_num(DataStack *ds, i32 num) {
    Value v = { .num = num };
    ds_push(ds, v);
}

void ds_push_xt(DataStack *ds, struct word *word) {
    Value v = { .xt = word };
    ds_push(ds, v);
}

void ds_push_addr(DataStack *ds, Value *addr) {
    if(addr == NULL) return;
    Value v = { .addr = addr };
    ds_push(ds, v);
}

int ds_pop(DataStack *ds, Value *out) {
    if(ds->count == 0) return -1; // underflow
    *out = ds->contents[--ds->count];
    return 0;
}

Value *ds_pop_addr(DataStack *ds) {
    if(ds->count == 0) return NULL;
    return ds->contents[--ds->count].addr;
}

Value *ds_top(const DataStack *ds) {
    if(ds->count == 0) return NULL;
    return &ds->contents[ds->count - 1];
}

void ds_clear(DataStack *ds) {
    ds->count = 0;
}

void ds_free(DataStack *ds) {
    realloc_mem(ds->contents, 0);
    ds_init(ds);
}

// -----------------------------------------------------------------------------

typedef enum : uint8_t {
    FLAG_code      = (1 << 0), // code or colon word?
    FLAG_immediate = (1 << 1), // executed on compile time?
    FLAG_hidden    = (1 << 2), // hidden to the user?
    FLAG_comp_only = (1 << 3), // only valid in definitions?
} WordFlag;

#define check_flag(flags, f) ((flags) & (f))

typedef struct word {
    struct word *prev;
    StringView name;
    uint8_t flags;

    union {
        CodeWordFn code; // valid if flags & FLAG_code
        DataStack colon; // valid otherwise
    } as;
} Word;

Word *word_new(StringView name, uint8_t flags) {
    Word *w = get_mem(sizeof(*w));
    w->prev = NULL;
    w->flags = flags;
    w->name = sv_copy(name);
    if(!check_flag(flags, FLAG_code))
        ds_init(&w->as.colon);
    return w;
}

Word *word_code_new(StringView name, CodeWordFn body) {
    Word *w = word_new(name, FLAG_code);
    w->as.code = body;
    return w;
}

void word_free(Word *w) {
    sv_free(w->name);
    if(!check_flag(w->flags, FLAG_code))
        ds_free(&w->as.colon);
    free_mem(w);
}

void word_list_add(Word **last, Word *w) {
    w->prev = *last;
    *last = w;
}

Word *word_list_find(Word *last, StringView name) {
    Word *w = last;
    while(w != NULL) {
        if(!check_flag(w->flags, FLAG_hidden)
                && w->name.len == name.len
                && strncasecmp(name.text, w->name.text, name.len) == 0)
            return w;
        w = w->prev;
    }
    return NULL;
}

void word_list_free(Word *last) {
    Word *w = last, *aux;
    while(w != NULL) {
        aux = w->prev;
        word_free(w);
        w = aux;
    }
}

// -----------------------------------------------------------------------------

typedef struct {
    StringView source;
    int offset;
    int line;
    int start_col, col;
} Scanner;

void scan_init(Scanner *s, StringView source) {
    s->source = source;
    s->offset = 0;
    s->line = 1;
    s->start_col = s->col = 1;
}

char scan_peek(const Scanner *s) {
    return s->source.text[s->offset];
}

StringView scan_peek_text(const Scanner *s) {
    return (StringView){ .text = s->source.text, .len = s->offset };
}

bool scan_end(const Scanner *s) {
    return s->offset >= s->source.len;
}

void scan_advance(Scanner *s) {
    if(scan_end(s)) return;
    s->offset += 1;
    s->col += 1;
}

void scan_sync(Scanner *s) {
    while(isspace(scan_peek(s)) && !scan_end(s)) {
        if(scan_peek(s) == '\n') {
            s->line += 1;
            s->col = 0;
        }
        scan_advance(s);
    }
    s->source.text = &s->source.text[s->offset];
    s->source.len -= s->offset;
    s->offset = 0;
    s->start_col = s->col;
}

StringView scan_word(Scanner *s) {
    scan_sync(s);
    while(!isspace(scan_peek(s)) && !scan_end(s))
        scan_advance(s);
    return scan_peek_text(s);
}

// -----------------------------------------------------------------------------

typedef struct processor {
    Scanner scan;
    Value *ip;    // instruction pointer
    DataStack ds; // parameter stack, for general use data
    DataStack rs; // R stack, for auxiliary data
    bool panic;   // critical error?
    bool verbose; // verbose error messages?

    Word *dict;      // word list
    Word *comp_word; // word currently being compiled

    // Address of a couple significant words
    Word *w_exit;
    Word *w_push;
} Processor;

void load_builtin(Processor *p);

void proc_init(Processor *p) {
    p->ip = NULL;
    ds_init(&p->ds);
    ds_init(&p->rs);
    p->panic = false;
    p->verbose = false;

    p->dict = NULL;
    p->comp_word = NULL;
    load_builtin(p);
}

bool proc_compile_mode(const Processor *p) {
    return p->comp_word != NULL;
}

void proc_free(Processor *p) {
    ds_free(&p->ds);
    ds_free(&p->rs);
    word_list_free(p->dict);
    proc_init(p);
}

void load_source(Processor *p, StringView source) {
    scan_init(&p->scan, source);
}

void error(Processor *p, const char *err_msg) {
    if(p->verbose)
        fprintf(stderr, "(%d:%d) error: %s\n",
                p->scan.line, p->scan.start_col, err_msg);
    else fprintf(stderr, "%s\n", err_msg);
    p->panic = true;
}

void error_undef(Processor *p, StringView word) {
    if(p->verbose)
        fprintf(stderr, "(%d:%d) error: undefined word '%.*s'\n",
                p->scan.line, p->scan.start_col, SV_fmt(word));
    else fprintf(stderr, "%.*s?\n", SV_fmt(word));
    p->panic = true;
}

void error_comp_only(Processor *p, StringView word) {
    if(p->verbose)
        fprintf(stderr, "(%d:%d) error: word '%.*s' is only valid in definitions\n",
                p->scan.line, p->scan.start_col, SV_fmt(word));
    else fprintf(stderr, "%.*s?\n", SV_fmt(word));
    p->panic = true;
}

Value proc_pop(Processor *p) {
    Value c = { .num = 0 };
    int err = ds_pop(&p->ds, &c);
    if(!p->panic && err) error(p, "stack underflow");
    return c;
}

// -----------------------------------------------------------------------------

void execute_word(Processor *p, Word *w) {
    if(check_flag(w->flags, FLAG_code)) {
        w->as.code(p);
        return;
    }
    DataStack *word_body = &w->as.colon;
    if(word_body->count == 0) return; // empty word

    Value *old_ip = p->ip;
    p->ip = word_body->contents;
    while(p->ip >= word_body->contents
            && p->ip < word_body->contents + word_body->count) {
        Word *operation = p->ip->xt;
        execute_word(p, operation);
        if(p->ip == NULL) break;
        p->ip += 1;
    }
    p->ip = old_ip;
    ds_clear(&p->rs);
}

void proc_comp_push(Processor *p, Word *w, Value val) {
    ds_push_xt(&w->as.colon, p->w_push);
    ds_push(&w->as.colon, val);
}

void proc_next(Processor *p) {
    bool is_val = false;
    StringView name = scan_word(&p->scan);
    if(name.len == 0) return;
    Word *w = word_list_find(p->dict, name);
    if(w == NULL) {
        Value operand = value_read(name, &is_val);
        if(!is_val) {
            error_undef(p, name);
            return;
        }
        if(proc_compile_mode(p))
            proc_comp_push(p, p->comp_word, operand);
        else ds_push(&p->ds, operand);
        return;
    }
    if(proc_compile_mode(p)) {
        if(check_flag(w->flags, FLAG_immediate))
            execute_word(p, w);
        else ds_push_xt(&p->comp_word->as.colon, w);
        return;
    }
    if(check_flag(w->flags, FLAG_comp_only))
        error_comp_only(p, name);
    else execute_word(p, w);
}

// -----------------------------------------------------------------------------

void run_source(Processor *p, StringView source) {
    p->panic = false;
    load_source(p, source);
    while(!scan_end(&p->scan)) {
        if(p->panic) break; // critical error
        proc_next(p);
    }
}

void repl(Processor *p) {
    printf("blackknifeforth " VERSION
            "  Copyright (C) 2025 Eduardo Antunes\n");
    char buf[2048];
    while(true) {
        printf("> ");
        if(fgets(buf, 2048, stdin) == NULL) break;
        StringView source = sv_init(buf);
        run_source(p, source);
        if(!p->panic) printf(" ok\n");
    }
    printf("\n");
}

StringView read_file(const char *filename) {
    StringView source = { .text = NULL, .len = 0 }, err = source;
    FILE *fp = fopen(filename, "rb");
    if(fp == NULL) return err;
    fseek(fp, 0, SEEK_END);
    source.len = ftell(fp);
    rewind(fp);

    source.text = realloc_mem(NULL, source.len + 1);
    int n = fread((char*)source.text, sizeof(char),
            source.len, fp);
    if(n < source.len) return err;
    source.text[n] = '\0'; // terminando com 0 manualmente
    fclose(fp);
    return source;
}

void run_file(Processor *p, const char *filename) {
    bool v = p->verbose;
    p->verbose = true;
    StringView source = read_file(filename);
    run_source(p, source);
    sv_free(source);
    p->verbose = v;
}

int main() {
    Processor bkf;
    proc_init(&bkf);

    run_file(&bkf, "prelude.f");
    repl(&bkf);

    proc_free(&bkf);
    return 0;
}

// -----------------------------------------------------------------------------

Word *code_word(Processor *p, char *name, CodeWordFn body, uint8_t flags) {
    StringView sv = { .text = name, .len = strlen(name) };
    Word *w = word_code_new(sv, body);
    w->flags |= flags;
    word_list_add(&p->dict, w);
    return w;
}

void w_define(Processor *p) {
    StringView name = scan_word(&p->scan);
    Word *w = word_new(name, FLAG_hidden);
    p->comp_word = w;
    word_list_add(&p->dict, w);
}

void w_end(Processor *p) {
    p->comp_word->flags &= ~FLAG_hidden;
    p->comp_word = NULL;
}

void w_immediate(Processor *p) {
    p->comp_word->flags |= FLAG_immediate;
}

void w_quote(Processor *p) {
    StringView name = scan_word(&p->scan);
    Word *w = word_list_find(p->dict, name);
    ds_push_xt(&p->ds, w);
}

void w_compile(Processor *p) {
    Value value = proc_pop(p);
    ds_push(&p->comp_word->as.colon, value);
}

void w_exit(Processor *p) {
    p->ip = NULL;
}

// -----------------------------------------------------------------------------

void w_constant(Processor *p) {
    Value val = proc_pop(p);
    StringView name = scan_word(&p->scan);
    Word *w = word_new(name, 0);
    proc_comp_push(p, w, val);
    word_list_add(&p->dict, w);
}

void w_variable(Processor *p) {
    Value tmp = { .num = 0 };
    StringView name = scan_word(&p->scan);
    Word *w = word_new(name, 0);

    proc_comp_push(p, w, tmp);
    Value *operand = ds_top(&w->as.colon);
    ds_push_xt(&w->as.colon, p->w_exit);
    ds_push(&w->as.colon, tmp);
    operand->addr = ds_top(&w->as.colon);

    word_list_add(&p->dict, w);
}

void w_fetch(Processor *p) {
    Value addr = proc_pop(p);
    if(p->panic) return;
    Value val = *addr.addr;
    ds_push(&p->ds, val);
}

void w_store(Processor *p) {
    Value addr = proc_pop(p);
    if(p->panic) return;
    Value val = proc_pop(p);
    *addr.addr = val;
}

// -----------------------------------------------------------------------------

void w_dup(Processor *p) {
    Value n = proc_pop(p);
    ds_push(&p->ds, n);
    ds_push(&p->ds, n);
}

void w_swap(Processor *p) {
    Value n2 = proc_pop(p);
    Value n1 = proc_pop(p);
    ds_push(&p->ds, n2);
    ds_push(&p->ds, n1);
}

void w_over(Processor *p) {
    Value n2 = proc_pop(p);
    Value n1 = proc_pop(p);
    ds_push(&p->ds, n1);
    ds_push(&p->ds, n2);
    ds_push(&p->ds, n1);
}

void w_rot(Processor *p) {
    Value n3 = proc_pop(p);
    Value n2 = proc_pop(p);
    Value n1 = proc_pop(p);
    ds_push(&p->ds, n2);
    ds_push(&p->ds, n3);
    ds_push(&p->ds, n1);
}

void w_push(Processor *p) {
    p->ip += 1;
    Value n = *p->ip;
    ds_push(&p->ds, n);
}

void w_drop(Processor *p) {
    proc_pop(p);
}

// -----------------------------------------------------------------------------

void w_print(Processor *p) {
    Value val = proc_pop(p);
    if(p->panic) return;
    printf("%" PRId32, val.num);
}

void w_print_u32(Processor *p) {
    Value val = proc_pop(p);
    if(p->panic) return;
    printf("%" PRIX32, val.num);
}

void w_print_ch(Processor *p) {
    Value val = proc_pop(p);
    if(p->panic) return;
    printf("%c", val.ch);
}

void w_endline(UNUSED Processor *p) {
    printf("\n");
}

void w_dump_ds(Processor *p) {
    bool first = true;
    for(int i = 0; i < p->ds.count; ++i) {
        if(!first) printf(" ");
        else first = false;
        printf("%" PRId32, p->ds.contents[i].num);
    }
    if(!first) printf("\n");
}

// -----------------------------------------------------------------------------

void w_add(Processor *p) {
    Value n2 = proc_pop(p);
    Value n1 = proc_pop(p);
    ds_push_num(&p->ds, n1.num + n2.num);
}

void w_sub(Processor *p) {
    Value n2 = proc_pop(p);
    Value n1 = proc_pop(p);
    ds_push_num(&p->ds, n1.num - n2.num);
}

void w_mul(Processor *p) {
    Value n2 = proc_pop(p);
    Value n1 = proc_pop(p);
    ds_push_num(&p->ds, n1.num * n2.num);
}

void w_div(Processor *p) {
    Value n2 = proc_pop(p);
    Value n1 = proc_pop(p);
    ds_push_num(&p->ds, n1.num * n2.num);
}

// -----------------------------------------------------------------------------

// In forth, it is traditional to represent true by -1 and false by 0
// This makes the bitwise operators behave like the standard logic ones
#define flag(cond) ((cond) ? -1 : 0)

void w_less(Processor *p) {
    Value n2 = proc_pop(p);
    Value n1 = proc_pop(p);
    ds_push_num(&p->ds, flag(n1.num < n2.num));
}

void w_less_eq(Processor *p) {
    Value n2 = proc_pop(p);
    Value n1 = proc_pop(p);
    ds_push_num(&p->ds, flag(n1.num <= n2.num));
}

void w_greater(Processor *p) {
    Value n2 = proc_pop(p);
    Value n1 = proc_pop(p);
    ds_push_num(&p->ds, flag(n1.num > n2.num));
}

void w_greater_eq(Processor *p) {
    Value n2 = proc_pop(p);
    Value n1 = proc_pop(p);
    ds_push_num(&p->ds, flag(n1.num >= n2.num));
}

void w_equals(Processor *p) {
    Value n2 = proc_pop(p);
    Value n1 = proc_pop(p);
    ds_push_num(&p->ds, flag(n1.num == n2.num));
}

void w_not_eq(Processor *p) {
    Value n2 = proc_pop(p);
    Value n1 = proc_pop(p);
    ds_push_num(&p->ds, flag(n1.num != n2.num));
}

void w_and(Processor *p) {
    Value f2 = proc_pop(p);
    Value f1 = proc_pop(p);
    ds_push_num(&p->ds, f1.num & f2.num);
}

void w_or(Processor *p) {
    Value f2 = proc_pop(p);
    Value f1 = proc_pop(p);
    ds_push_num(&p->ds, f1.num | f2.num);
}

void w_xor(Processor *p) {
    Value f2 = proc_pop(p);
    Value f1 = proc_pop(p);
    ds_push_num(&p->ds, f1.num ^ f2.num);
}

// -----------------------------------------------------------------------------

void load_builtin(Processor *p) {
    p->w_exit = code_word(p, "exit" , w_exit, 0);
    p->w_push = code_word(p, "_push", w_push, FLAG_hidden);

    code_word(p, ":"        , w_define    , 0);
    code_word(p, "'"        , w_quote     , FLAG_immediate);
    code_word(p, ";"        , w_end       , FLAG_immediate | FLAG_comp_only);
    code_word(p, ","        , w_compile   , FLAG_immediate | FLAG_comp_only);
    code_word(p, "immediate", w_immediate , FLAG_immediate | FLAG_comp_only);
    code_word(p, "constant" , w_constant  , FLAG_immediate);
    code_word(p, "variable" , w_variable  , 0);
    code_word(p, "@"        , w_fetch     , 0);
    code_word(p, "!"        , w_store     , 0);
    code_word(p, "dup"      , w_dup       , 0);
    code_word(p, "swap"     , w_swap      , 0);
    code_word(p, "drop"     , w_drop      , 0);
    code_word(p, "over"     , w_over      , 0);
    code_word(p, "rot"      , w_rot       , 0);
    code_word(p, "."        , w_print     , 0);
    code_word(p, ".u"       , w_print_u32 , 0);
    code_word(p, ".c"       , w_print_ch  , 0);
    code_word(p, "cr"       , w_endline   , 0);
    code_word(p, ".s"       , w_dump_ds   , 0);
    code_word(p, "+"        , w_add       , 0);
    code_word(p, "-"        , w_sub       , 0);
    code_word(p, "*"        , w_mul       , 0);
    code_word(p, "/"        , w_div       , 0);
    code_word(p, "<"        , w_less      , 0);
    code_word(p, "<="       , w_less_eq   , 0);
    code_word(p, ">"        , w_greater   , 0);
    code_word(p, ">="       , w_greater_eq, 0);
    code_word(p, "="        , w_equals    , 0);
    code_word(p, "<>"       , w_not_eq    , 0);
    code_word(p, "and"      , w_and       , 0);
    code_word(p, "or"       , w_or        , 0);
    code_word(p, "xor"      , w_xor       , 0);
}
