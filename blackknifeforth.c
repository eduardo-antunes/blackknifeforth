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
    const char *text;
    int len;
} string_view;

#define SV_fmt(sv) (sv).len,(sv).text

string_view sv_init(const char *str) {
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
    uint32_t num;
    struct word *xt;  // execution token for a word
    union cell *addr;
    native_fn native;
} cell;

cell cell_readnum(string_view sv, bool *ok) {
    *ok = true;
    uint32_t n = 0;
    for(int i = 0; i < sv.len; ++i) {
        if(!isdigit(sv.text[i])) {
            *ok = false;
            break;
        }
        n = n * 10 + (sv.text[i] - '0');
    }
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

void ds_push_num(data_stack *ds, uint32_t num) {
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
    bool had_output;

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
    r->had_output = false;

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
    r->had_output = false;
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

void compile_number(runtime *r, cell c) {
    data_stack *body = &r->current_word->body;
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
            compile_number(r, operand);
        else ds_push(&r->ds, operand);
        return;
    }
    if(r->current_word != NULL) {
        if(check_flag(w, FLAG_immediate))
            run_word(r, w);
        else compile_word(r, w);
        return;
    }
    if(check_flag(w, FLAG_immediate)) {
        error(r, "immediate word outside of definition");
        return;
    }
    run_word(r, w);
}

// -----------------------------------------------------------------------------

void run_source(runtime *r, string_view source) {
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
        ds_clear(&r->ds);
        if(!r->had_output)
            printf("ok\n");
    }
    printf("\n");
}

int main() {
    runtime knight;
    runtime_init(&knight);
    repl(&knight);
    runtime_free(&knight);
    return 0;
}

// -----------------------------------------------------------------------------

void register_word(runtime *r, word *w) {
    w->prev = r->latest;
    r->latest = w;
}

word *register_nat(runtime *r, const char *name, native_fn body, uint8_t flags) {
    string_view sv = { .text = name, .len = strlen(name) };
    word *w = word_native_new(sv, body);
    w->flags |= flags;
    register_word(r, w);
    return w;
}

void w_dup(runtime *r) {
    cell val = safe_pop(r);
    ds_push(&r->ds, val);
    ds_push(&r->ds, val);
}

void w_swap(runtime *r) {
    cell val2 = safe_pop(r);
    cell val1 = safe_pop(r);
    ds_push(&r->ds, val2);
    ds_push(&r->ds, val1);
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

void w_push(runtime *r) {
    r->ip += 1;
    cell val = *r->ip;
    ds_push(&r->ds, val);
}

void w_drop(runtime *r) {
    safe_pop(r);
}

void w_dump_ds(runtime *r) {
    bool first = true;
    for(int i = 0; i < r->ds.count; ++i) {
        if(!first) printf(" ");
        else first = false;
        printf("%" PRId32, r->ds.contents[i].num);
    }
    printf("\n");
    r->had_output = true;
}

void w_print(runtime *r) {
    cell val = safe_pop(r);
    printf("%" PRId32 "\n", val.num);
    r->had_output = true;
}

void w_add(runtime *r) {
    cell val2 = safe_pop(r);
    cell val1 = safe_pop(r);
    ds_push_num(&r->ds, val1.num + val2.num);
}

void w_sub(runtime *r) {
    cell val2 = safe_pop(r);
    cell val1 = safe_pop(r);
    ds_push_num(&r->ds, val1.num - val2.num);
}

void w_mul(runtime *r) {
    cell val2 = safe_pop(r);
    cell val1 = safe_pop(r);
    ds_push_num(&r->ds, val1.num * val2.num);
}

void w_div(runtime *r) {
    cell val2 = safe_pop(r);
    cell val1 = safe_pop(r);
    ds_push_num(&r->ds, val1.num * val2.num);
}

void load_prelude(runtime *r) {
    register_nat(r, "dup"  , w_dup    , 0);
    register_nat(r, "swap" , w_swap   , 0);
    register_nat(r, "drop" , w_drop   , 0);
    register_nat(r, "."    , w_print  , 0);
    register_nat(r, ".s"   , w_dump_ds, 0);
    register_nat(r, "+"    , w_add    , 0);
    register_nat(r, "-"    , w_sub    , 0);
    register_nat(r, "*"    , w_mul    , 0);
    register_nat(r, "/"    , w_div    , 0);
    register_nat(r, ":"    , w_define , 0);
    register_nat(r, ";"    , w_end    , FLAG_immediate);

    r->push_word = register_nat(r, "_push", w_push, FLAG_hidden);
}
