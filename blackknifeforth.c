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

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <ctype.h>
#include <assert.h>
#include <string.h>

#define VERSION "0.1"

typedef int32_t i32;

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

// -----------------------------------------------------------------------------

typedef struct {
    char *text;
    int len;
} string_view;

#define SV_fmt(sv) (sv).len,(sv).text

string_view sv_init(char *str) {
    return (string_view){ .text = str, strlen(str) };
}

string_view sv_copy(string_view src) {
    string_view dest = { .len = src.len };
    dest.text = realloc_mem(NULL, src.len + 1);
    memcpy((char*)dest.text, src.text, src.len);
    return dest;
}

void sv_free(string_view sv) {
    realloc_mem((void*)sv.text, 0);
}

// Case insensitive string comparison is non-portable in C, unless you
// employ a healthy dose of preprocessor black magic
#ifdef _MSC_VER
# define strncasecmp _strnicmp
#else
# include <strings.h>
#endif // _MSC_VER

// -----------------------------------------------------------------------------

struct runtime;
typedef void (*native_fn)(struct runtime *);

typedef union cell {
    i32 num;
    struct word *xt;  // execution token for a word
    union cell *addr;
    native_fn native;
} cell;

cell cell_readnum(string_view sv, bool *ok) {
    int i = 0;
    i32 n = 0;
    *ok = true;
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
    return (cell){ .num = n };
}

// -----------------------------------------------------------------------------

typedef struct {
    cell *contents;
    int count, capacity;
} data_stack;

void ds_init(data_stack *ds) {
    ds->contents = NULL;
    ds->count = ds->capacity = 0;
}

void ds_push_with(data_stack *ds, cell c, int initial_cap) {
    if(ds->count + 1 > ds->capacity) {
        ds->capacity = (ds->capacity == 0) ?
            initial_cap : ds->capacity * 2;
        ds->contents = realloc_mem(ds->contents,
                ds->capacity * sizeof(*ds->contents));
    }
    ds->contents[ds->count++] = c;
}

void ds_push(data_stack *ds, cell c) {
    ds_push_with(ds, c, 8);
}

void ds_push_num(data_stack *ds, i32 num) {
    ds_push(ds, (cell){ .num = num });
}

void ds_push_xt(data_stack *ds, struct word *xt) {
    ds_push(ds, (cell){ .xt = xt });
}

void ds_push_addr(data_stack *ds, cell *addr) {
    if(addr == NULL) return;
    ds_push(ds, (cell){ .addr = addr });
}

int ds_pop(data_stack *ds, cell *out) {
    if(ds->count == 0) return -1; // underflow
    *out = ds->contents[--ds->count];
    return 0;
}

int ds_pop_addr(data_stack *ds, cell **ptr) {
    *ptr = (ds->count == 0) ? NULL : ds->contents[--ds->count].addr;
    return 0;
}

void ds_clear(data_stack *ds) {
    ds->count = 0;
}

cell *ds_top(const data_stack *ds) {
    if(ds->count == 0) return NULL;
    return &ds->contents[ds->count - 1];
}

void ds_free(data_stack *ds) {
    realloc_mem(ds->contents, 0);
    ds_init(ds);
}

// -----------------------------------------------------------------------------

typedef enum : uint8_t {
    FLAG_native    = (1 << 0),
    FLAG_immediate = (1 << 1),
    FLAG_hidden    = (1 << 2),
} word_flag;

#define check_flag(w, f) ((w)->flags & (f))

typedef struct word {
    struct word *prev;
    string_view name; // dynamically allocated
    data_stack body;
    uint8_t flags;
} word;

word *word_new(string_view name, uint8_t flags) {
    word *w = realloc_mem(NULL, sizeof(*w));
    w->prev = NULL;
    w->name = sv_copy(name);
    ds_init(&w->body);
    w->flags = flags;
    return w;
}

word *word_native_new(string_view name, native_fn body) {
    word *w = word_new(name, FLAG_native);
    ds_push_with(&w->body, (cell){ .native = body }, 1);
    return w;
}

word *word_find(word *latest, string_view name) {
    word *ptr = latest;
    while(ptr != NULL) {
        if(!check_flag(ptr, FLAG_hidden)
                && ptr->name.len == name.len
                && strncasecmp(name.text, ptr->name.text, name.len) == 0)
            return ptr;
        ptr = ptr->prev;
    }
    return NULL;
}

void word_free(word *word) {
    sv_free(word->name);
    ds_free(&word->body);
    word->prev = NULL;
}

// -----------------------------------------------------------------------------

typedef struct runtime {
    // Text processing fields, used for scanning
    string_view source;
    int offset;
    int line;
    int start_col, col;

    // Runtime related fields, for actually executing
    word *latest;
    cell *ip;
    word *current_word;
    data_stack ds;
    data_stack rets;
    bool panic;

    // Presentation config
    bool verbose;

    // Address of a couple significant words
    word *push_word;
} runtime;

// Loads predefined words of the language
void load_prelude(runtime *r);

void runtime_init(runtime *r) {
    r->source.len = 0;
    r->latest = NULL;
    r->ip = NULL;
    ds_init(&r->ds);
    ds_init(&r->rets);
    r->current_word = NULL;
    r->panic = false;

    r->verbose = false;
    r->push_word = NULL;
    load_prelude(r);
}

void runtime_free(runtime *r) {
    ds_free(&r->ds);
    ds_free(&r->rets);
    word *ptr = r->latest, *aux;
    while(ptr != NULL) {
        aux = ptr->prev;
        word_free(ptr);
        ptr = aux;
    }
    runtime_init(r);
}

void load_source(runtime *r, string_view source) {
    r->source = source;
    r->line = 1;
    r->start_col = r->col = 1;
    r->offset = 0;
}

// -----------------------------------------------------------------------------

void error(runtime *r, const char *err_msg) {
    if(r->verbose)
        fprintf(stderr, "(%d:%d) error: %s\n", r->line, r->start_col, err_msg);
    else fprintf(stderr, "%s\n", err_msg);
    r->panic = true;
}

void error_undef(runtime *r, string_view word) {
    if(r->verbose)
        fprintf(stderr, "(%d:%d) error: undefined word '%.*s'\n",
                r->line, r->start_col, SV_fmt(word));
    else fprintf(stderr, "%.*s?\n", SV_fmt(word));
    r->panic = true;
}

cell safe_pop(runtime *r) {
    cell c = { .num = 0 };
    int err = ds_pop(&r->ds, &c);
    if(err) error(r, "stack underflow");
    return c;
}

// -----------------------------------------------------------------------------

char scan_peek(const runtime *r) {
    return r->source.text[r->offset];
}

string_view scan_peek_text(const runtime *r) {
    return (string_view){ .text = r->source.text, .len = r->offset };
}

bool scan_end(const runtime *r) {
    return r->offset >= r->source.len;
}

void scan_advance(runtime *r) {
    if(scan_end(r)) return;
    r->offset += 1;
    r->col += 1;
}

void scan_sync(runtime *r) {
    while(isspace(scan_peek(r)) && !scan_end(r)) {
        if(scan_peek(r) == '\n') {
            r->line += 1;
            r->col = 0;
        }
        scan_advance(r);
    }
    r->source.text = &r->source.text[r->offset];
    r->source.len -= r->offset;
    r->offset = 0;
    r->start_col = r->col;
}

string_view scan_word(runtime *r) {
    scan_sync(r);
    while(!isspace(scan_peek(r)) && !scan_end(r))
        scan_advance(r);
    return scan_peek_text(r);
}

// -----------------------------------------------------------------------------

void run_word(runtime *r, word *w) {
    if(check_flag(w, FLAG_native)) {
        w->body.contents[0].native(r);
        return;
    }
    if(w->body.count == 0) return;
    data_stack *body = &w->body;

    // Save return address to the return stack
    ds_push_addr(&r->rets, r->ip);
    r->ip = body->contents;
    while(r->ip >= body->contents
            && r->ip < body->contents + body->count) {
        if(r->panic) break; // critical error
        cell operation = *r->ip;
        run_word(r, operation.xt);
        r->ip += 1;
    }
    // Restore return address
    ds_pop_addr(&r->rets, &r->ip);
}

void compile_word(runtime *r, word *w) {
    data_stack *body = &r->current_word->body;
    ds_push_xt(body, w);
}

void compile_number_to(runtime *r, cell c, word *w) {
    data_stack *body = &w->body;
    ds_push_xt(body, r->push_word);
    ds_push(body, c);
}

void next(runtime *r) {
    bool is_num = false;
    string_view name = scan_word(r);
    if(name.len == 0) return;
    word *w = word_find(r->latest, name);
    if(w == NULL) {
        cell operand = cell_readnum(name, &is_num);
        if(!is_num) {
            error_undef(r, name);
            return;
        }
        if(r->current_word != NULL)
            compile_number_to(r, operand, r->current_word);
        else ds_push(&r->ds, operand);
        return;
    }
    if(r->current_word != NULL) {
        if(check_flag(w, FLAG_immediate))
            run_word(r, w);
        else compile_word(r, w);
        return;
    }
    // if(check_flag(w, FLAG_immediate)) {
    //     error(r, "immediate word outside of definition");
    //     return;
    // }
    run_word(r, w);
}

// -----------------------------------------------------------------------------

void run_source(runtime *r, string_view source) {
    r->panic = false;
    load_source(r, source);
    while(!scan_end(r)) {
        if(r->panic) break; // critical error
        next(r);
    }
}

void repl(runtime *r) {
    printf("blackknifeforth " VERSION
            "  Copyright (C) 2025 Eduardo Antunes\n");
    char buf[2048];
    while(true) {
        printf("> ");
        if(fgets(buf, 2048, stdin) == NULL) break;
        string_view source = sv_init(buf);
        run_source(r, source);
        if(!r->panic) printf("ok\n");
    }
    printf("\n");
}

string_view read_file(const char *filename) {
    string_view source = { .text = NULL, .len = 0 }, err = source;
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

void run_file(runtime *r, const char *filename) {
    string_view source = read_file(filename);
    run_source(r, source);
    sv_free(source);
}

int main() {
    runtime bkf;
    runtime_init(&bkf);

    // run_file(&bkf, "prelude.f");
    repl(&bkf);

    runtime_free(&bkf);
    return 0;
}

// -----------------------------------------------------------------------------

void register_word(runtime *r, word *w) {
    w->prev = r->latest;
    r->latest = w;
}

word *register_nat(runtime *r, char *name, native_fn body, uint8_t flags) {
    string_view sv = { .text = name, .len = strlen(name) };
    word *w = word_native_new(sv, body);
    w->flags |= flags;
    register_word(r, w);
    return w;
}

void w_define(runtime *r) {
    string_view name = scan_word(r);
    word *w = word_new(name, FLAG_hidden);
    r->current_word = w;
    register_word(r, w);
}

void w_end(runtime *r) {
    r->current_word->flags &= ~FLAG_hidden;
    r->current_word = NULL;
}

void w_constant(runtime *r) {
    cell n = safe_pop(r);
    string_view name = scan_word(r);
    word *w = word_new(name, 0);
    compile_number_to(r, n, w);
    register_word(r, w);
}

void w_immediate(runtime *r) {
    if(r->current_word == NULL) return;
    r->current_word->flags |= FLAG_immediate;
}

void w_quote(runtime *r) {
    string_view name = scan_word(r);
    word *w = word_find(r->latest, name);
    ds_push_xt(&r->ds, w);
}

void w_compile(runtime *r) {
    cell n = safe_pop(r);
    if(r->current_word == NULL) return;
    ds_push(&r->current_word->body, n);
}

// -----------------------------------------------------------------------------

void w_dup(runtime *r) {
    cell n = safe_pop(r);
    ds_push(&r->ds, n);
    ds_push(&r->ds, n);
}

void w_swap(runtime *r) {
    cell n2 = safe_pop(r);
    cell n1 = safe_pop(r);
    ds_push(&r->ds, n2);
    ds_push(&r->ds, n1);
}

void w_over(runtime *r) {
    cell n2 = safe_pop(r);
    cell n1 = safe_pop(r);
    ds_push(&r->ds, n1);
    ds_push(&r->ds, n2);
    ds_push(&r->ds, n1);
}

void w_rot(runtime *r) {
    cell n3 = safe_pop(r);
    cell n2 = safe_pop(r);
    cell n1 = safe_pop(r);
    ds_push(&r->ds, n2);
    ds_push(&r->ds, n3);
    ds_push(&r->ds, n1);
}

void w_push(runtime *r) {
    r->ip += 1;
    cell n = *r->ip;
    ds_push(&r->ds, n);
}

void w_drop(runtime *r) {
    safe_pop(r);
}

// -----------------------------------------------------------------------------

void w_print(runtime *r) {
    cell n = safe_pop(r);
    printf("%" PRId32 "\n", n.num);
}

void w_print_u32(runtime *r) {
    cell n = safe_pop(r);
    printf("%" PRIX32 "\n", n.num);
}

void w_dump_ds(runtime *r) {
    bool first = true;
    for(int i = 0; i < r->ds.count; ++i) {
        if(!first) printf(" ");
        else first = false;
        printf("%" PRId32, r->ds.contents[i].num);
    }
    if(!first) printf("\n");
}

// -----------------------------------------------------------------------------

void w_add(runtime *r) {
    cell n2 = safe_pop(r);
    cell n1 = safe_pop(r);
    ds_push_num(&r->ds, n1.num + n2.num);
}

void w_sub(runtime *r) {
    cell n2 = safe_pop(r);
    cell n1 = safe_pop(r);
    ds_push_num(&r->ds, n1.num - n2.num);
}

void w_mul(runtime *r) {
    cell n2 = safe_pop(r);
    cell n1 = safe_pop(r);
    ds_push_num(&r->ds, n1.num * n2.num);
}

void w_div(runtime *r) {
    cell n2 = safe_pop(r);
    cell n1 = safe_pop(r);
    ds_push_num(&r->ds, n1.num * n2.num);
}

// -----------------------------------------------------------------------------

// In forth, it is traditional to represent true by -1 and false by 0
// This makes the bitwise operators behave like the standard logic ones
#define flag(cond) ((cond) ? -1 : 0)

void w_less(runtime *r) {
    cell n2 = safe_pop(r);
    cell n1 = safe_pop(r);
    ds_push_num(&r->ds, flag(n1.num < n2.num));
}

void w_less_eq(runtime *r) {
    cell n2 = safe_pop(r);
    cell n1 = safe_pop(r);
    ds_push_num(&r->ds, flag(n1.num <= n2.num));
}

void w_greater(runtime *r) {
    cell n2 = safe_pop(r);
    cell n1 = safe_pop(r);
    ds_push_num(&r->ds, flag(n1.num > n2.num));
}

void w_greater_eq(runtime *r) {
    cell n2 = safe_pop(r);
    cell n1 = safe_pop(r);
    ds_push_num(&r->ds, flag(n1.num >= n2.num));
}

void w_equals(runtime *r) {
    cell n2 = safe_pop(r);
    cell n1 = safe_pop(r);
    ds_push_num(&r->ds, flag(n1.num == n2.num));
}

void w_not_eq(runtime *r) {
    cell n2 = safe_pop(r);
    cell n1 = safe_pop(r);
    ds_push_num(&r->ds, flag(n1.num != n2.num));
}

// -----------------------------------------------------------------------------

void load_prelude(runtime *r) {
    register_nat(r, ":"        , w_define    , 0);
    register_nat(r, ";"        , w_end       , FLAG_immediate);
    register_nat(r, ","        , w_compile   , FLAG_immediate);
    register_nat(r, "'"        , w_quote     , FLAG_immediate);
    register_nat(r, "immediate", w_immediate , FLAG_immediate);
    register_nat(r, "constant" , w_constant  , 0);
    register_nat(r, "dup"      , w_dup       , 0);
    register_nat(r, "swap"     , w_swap      , 0);
    register_nat(r, "drop"     , w_drop      , 0);
    register_nat(r, "over"     , w_over      , 0);
    register_nat(r, "rot"      , w_rot       , 0);
    register_nat(r, "."        , w_print     , 0);
    register_nat(r, ".u"       , w_print_u32 , 0);
    register_nat(r, ".s"       , w_dump_ds   , 0);
    register_nat(r, "+"        , w_add       , 0);
    register_nat(r, "-"        , w_sub       , 0);
    register_nat(r, "*"        , w_mul       , 0);
    register_nat(r, "/"        , w_div       , 0);
    register_nat(r, "<"        , w_less      , 0);
    register_nat(r, "<="       , w_less_eq   , 0);
    register_nat(r, ">"        , w_greater   , 0);
    register_nat(r, ">="       , w_greater_eq, 0);
    register_nat(r, "="        , w_equals    , 0);
    register_nat(r, "<>"       , w_not_eq    , 0);

    r->push_word = register_nat(r, "_push", w_push, FLAG_hidden);
}
