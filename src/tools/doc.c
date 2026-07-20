/**
 * src/tools/doc.c - Flux documentation generator.
 *
 * Parses a .flx source file and produces Markdown documentation by:
 *   1. Extracting top-level (and class-level) function/class/struct/enum
 *      definitions from the AST.
 *   2. Collecting `#` comment blocks immediately preceding each definition
 *      in the source text.
 *   3. Writing a structured Markdown file to stdout (or <file>.md).
 *
 * Comment convention:
 *   A doc-comment is a block of consecutive `# ...` lines directly above
 *   a `func`, `class`, `struct`, or `enum` keyword with no blank lines in
 *   between.
 *
 * Output format:
 *   # <filename>
 *   ## Functions / Classes / Structs / Enums
 *   ### name(params)
 *   <docstring paragraph>
 */
#include "flux/lexer.h"
#include "flux/ast.h"
#include "flux/parser.h"
#include "flux/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* -------------------------------------------------------------------------
 * Source line table — maps line numbers to char offsets
 * ---------------------------------------------------------------------- */
typedef struct {
    int  *starts;   /* byte offset of start of each line (1-indexed: starts[1]) */
    int   count;
} LineTable;

static LineTable build_line_table(const char *src, int len) {
    LineTable t;
    t.count  = 2;
    t.starts = malloc(sizeof(int) * (size_t)(len + 2));
    t.starts[1] = 0; /* line 1 starts at offset 0 */
    int line = 1;
    for (int i = 0; i < len; i++) {
        if (src[i] == '\n') {
            line++;
            if (line >= t.count) t.count = line + 1;
            t.starts[line] = i + 1;
        }
    }
    return t;
}

static void line_table_free(LineTable *t) {
    free(t->starts);
    t->starts = NULL;
}

/* -------------------------------------------------------------------------
 * Extract doc-comment block above a given line
 *
 * Scans backwards from (def_line - 1) collecting consecutive `# ...` lines.
 * Returns a heap-allocated string (caller frees) or NULL.
 * ---------------------------------------------------------------------- */
static char *extract_doccomment(const char *src, const LineTable *lt, int def_line) {
    if (def_line <= 1) return NULL;

    /* Collect comment lines bottom-up, then reverse */
    char *lines[256];
    int   nlines = 0;

    for (int ln = def_line - 1; ln >= 1 && nlines < 256; ln--) {
        int start = lt->starts[ln];
        /* Advance past leading spaces */
        int pos = start;
        while (src[pos] == ' ' || src[pos] == '\t') pos++;
        if (src[pos] != '#') break; /* not a comment line — stop */

        /* Skip '#' and optional space */
        pos++;
        if (src[pos] == ' ') pos++;

        /* Find end of line */
        int end = pos;
        while (src[end] && src[end] != '\n') end++;

        int clen = end - pos;
        lines[nlines] = malloc((size_t)clen + 1);
        memcpy(lines[nlines], src + pos, (size_t)clen);
        lines[nlines][clen] = '\0';
        nlines++;
    }

    if (nlines == 0) return NULL;

    /* Reverse */
    for (int i = 0; i < nlines / 2; i++) {
        char *tmp = lines[i];
        lines[i] = lines[nlines - 1 - i];
        lines[nlines - 1 - i] = tmp;
    }

    /* Join */
    size_t total = 0;
    for (int i = 0; i < nlines; i++) total += strlen(lines[i]) + 1;
    char *out = malloc(total + 1);
    out[0] = '\0';
    for (int i = 0; i < nlines; i++) {
        strcat(out, lines[i]);
        if (i < nlines - 1) strcat(out, "\n");
        free(lines[i]);
    }
    return out;
}

/* -------------------------------------------------------------------------
 * Format parameter list as a string
 * ---------------------------------------------------------------------- */
static void fmt_params(char *buf, size_t bufsz, const AstParamList *params) {
    buf[0] = '\0';
    size_t pos = 0;
    for (int i = 0; i < params->count; i++) {
        const AstParam *p = &params->data[i];
        /* param name */
        int namelen = p->name.length;
        if (pos + (size_t)namelen + 4 >= bufsz) break;
        memcpy(buf + pos, p->name.start, (size_t)namelen);
        pos += (size_t)namelen;
        /* type hint */
        if (p->type_hint.kind != TOK_EOF) {
            buf[pos++] = ':';
            buf[pos++] = ' ';
            int tlen = p->type_hint.length;
            if (pos + (size_t)tlen + 2 < bufsz) {
                memcpy(buf + pos, p->type_hint.start, (size_t)tlen);
                pos += (size_t)tlen;
            }
        }
        /* default value */
        if (p->default_value) {
            if (pos + 4 < bufsz) {
                buf[pos++] = ' ';
                buf[pos++] = '=';
                buf[pos++] = ' ';
                buf[pos++] = '?'; /* we don't re-print AST to string here */
            }
        }
        if (i < params->count - 1 && pos + 2 < bufsz) {
            buf[pos++] = ',';
            buf[pos++] = ' ';
        }
    }
    buf[pos] = '\0';
}

/* -------------------------------------------------------------------------
 * Document sections
 * ---------------------------------------------------------------------- */
typedef struct DocEntry {
    char  kind[16];      /* "func", "async func", "class", "struct", "enum" */
    char  name[128];
    char  signature[512];
    char *docstring;     /* heap-allocated; may be NULL */
    struct DocEntry *next;
} DocEntry;

typedef struct {
    DocEntry *head;
    DocEntry *tail;
    int       count;
} DocList;

static DocEntry *doc_entry_new(const char *kind, const char *name,
                               const char *sig, char *docstring) {
    DocEntry *e = calloc(1, sizeof(DocEntry));
    strncpy(e->kind,      kind,  sizeof(e->kind)      - 1);
    strncpy(e->name,      name,  sizeof(e->name)      - 1);
    strncpy(e->signature, sig,   sizeof(e->signature) - 1);
    e->docstring = docstring;
    return e;
}

static void doc_list_push(DocList *list, DocEntry *e) {
    if (!list->head) list->head = list->tail = e;
    else { list->tail->next = e; list->tail = e; }
    list->count++;
}

static void doc_list_free(DocList *list) {
    DocEntry *cur = list->head;
    while (cur) {
        DocEntry *n = cur->next;
        free(cur->docstring);
        free(cur);
        cur = n;
    }
}

/* -------------------------------------------------------------------------
 * AST walker
 * ---------------------------------------------------------------------- */
static void walk_block(DocList *list, AstNode *block,
                       const char *src, const LineTable *lt, int depth);

static void walk_node(DocList *list, AstNode *node,
                      const char *src, const LineTable *lt, int depth) {
    if (!node) return;

    char params_buf[512];
    char sig[640];

    switch (node->kind) {
    case AST_FUNC_DEF:
    case AST_ASYNC_FUNC_DEF: {
        const char *kind = (node->kind == AST_ASYNC_FUNC_DEF) ? "async func" : "func";
        fmt_params(params_buf, sizeof(params_buf), &node->as.func_def.params);
        snprintf(sig, sizeof(sig), "%s(%s)", node->as.func_def.name, params_buf);
        char *doc = extract_doccomment(src, lt, node->line);
        /* Only top-level (depth==0) and class-level (depth==1) */
        if (depth <= 1) {
            DocEntry *e = doc_entry_new(kind, node->as.func_def.name, sig, doc);
            doc_list_push(list, e);
        } else {
            free(doc);
        }
        /* Don't recurse into function bodies for now */
        break;
    }

    case AST_CLASS_DEF:
    case AST_STRUCT_DEF: {
        const char *kind = (node->kind == AST_STRUCT_DEF) ? "struct" : "class";
        const char *base = node->as.class_def.superclass;
        if (base)
            snprintf(sig, sizeof(sig), "%s(%s)", node->as.class_def.name, base);
        else
            snprintf(sig, sizeof(sig), "%s", node->as.class_def.name);

        char *doc = extract_doccomment(src, lt, node->line);
        DocEntry *e = doc_entry_new(kind, node->as.class_def.name, sig, doc);
        doc_list_push(list, e);

        /* Recurse into class body (methods) */
        if (node->as.class_def.body)
            walk_block(list, node->as.class_def.body, src, lt, depth + 1);
        break;
    }

    case AST_ENUM_DEF: {
        /* Build member list */
        char members[512] = "";
        size_t mpos = 0;
        for (int i = 0; i < node->as.enum_def.members.count; i++) {
            AstNode *m = node->as.enum_def.members.data[i];
            if (m->kind == AST_IDENT) {
                size_t nlen = strlen(m->as.ident.name);
                if (mpos + nlen + 3 < sizeof(members)) {
                    memcpy(members + mpos, m->as.ident.name, nlen);
                    mpos += nlen;
                    if (i < node->as.enum_def.members.count - 1) {
                        members[mpos++] = ',';
                        members[mpos++] = ' ';
                    }
                }
            }
        }
        members[mpos] = '\0';
        snprintf(sig, sizeof(sig), "%s { %s }", node->as.enum_def.name, members);
        char *doc = extract_doccomment(src, lt, node->line);
        DocEntry *e = doc_entry_new("enum", node->as.enum_def.name, sig, doc);
        doc_list_push(list, e);
        break;
    }

    default:
        break;
    }
}

static void walk_block(DocList *list, AstNode *block,
                       const char *src, const LineTable *lt, int depth) {
    if (!block) return;
    if (block->kind != AST_BLOCK && block->kind != AST_MODULE) {
        walk_node(list, block, src, lt, depth);
        return;
    }
    AstList *stmts = &block->as.block.stmts;
    for (int i = 0; i < stmts->count; i++)
        walk_node(list, stmts->data[i], src, lt, depth);
}

/* -------------------------------------------------------------------------
 * Markdown output
 * ---------------------------------------------------------------------- */
static void write_markdown(FILE *out, const char *filename,
                           DocList *funcs, DocList *classes) {
    /* Strip directory from filename */
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;

    fprintf(out, "# %s\n\n", base);
    fprintf(out, "> Auto-generated documentation from `%s`\n\n", filename);
    fprintf(out, "---\n\n");

    /* Functions & async funcs */
    int fn_count = 0;
    for (DocEntry *e = funcs->head; e; e = e->next)
        if (strcmp(e->kind, "func") == 0 || strcmp(e->kind, "async func") == 0)
            fn_count++;

    if (fn_count > 0) {
        fprintf(out, "## Functions\n\n");
        for (DocEntry *e = funcs->head; e; e = e->next) {
            if (strcmp(e->kind, "func") != 0 && strcmp(e->kind, "async func") != 0)
                continue;
            fprintf(out, "### `%s %s`\n\n", e->kind, e->signature);
            if (e->docstring && *e->docstring)
                fprintf(out, "%s\n\n", e->docstring);
            else
                fprintf(out, "_No documentation provided._\n\n");
        }
    }

    /* Classes, structs, enums */
    for (DocEntry *e = classes->head; e; e = e->next) {
        if (strcmp(e->kind, "class") == 0 || strcmp(e->kind, "struct") == 0) {
            fprintf(out, "## %s `%s`\n\n",
                    e->kind[0] == 'c' ? "Class" : "Struct", e->signature);
            if (e->docstring && *e->docstring)
                fprintf(out, "%s\n\n", e->docstring);
            else
                fprintf(out, "_No documentation provided._\n\n");

            /* Methods belonging to this class */
            bool has_methods = false;
            for (DocEntry *m = funcs->head; m; m = m->next) {
                /* Methods are collected at depth==1 alongside top-level funcs.
                 * We can't easily attribute them to a class here without extra
                 * tracking; print them under the first class for now. */
                (void)m;
            }
            (void)has_methods;
        } else if (strcmp(e->kind, "enum") == 0) {
            fprintf(out, "## Enum `%s`\n\n", e->name);
            if (e->docstring && *e->docstring)
                fprintf(out, "%s\n\n", e->docstring);
            /* signature is "Name { A, B, C }" — skip "Name { " */
        {
            const char *mem_start = strchr(e->signature, '{');
            if (mem_start) {
                mem_start++; /* skip '{' */
                while (*mem_start == ' ') mem_start++;
                /* trim trailing " }" */
                char mem_buf[512];
                strncpy(mem_buf, mem_start, sizeof(mem_buf) - 1);
                mem_buf[sizeof(mem_buf) - 1] = '\0';
                char *close = strrchr(mem_buf, '}');
                if (close) { *close = '\0'; }
                /* trim trailing space */
                int mlen = (int)strlen(mem_buf);
                while (mlen > 0 && mem_buf[mlen - 1] == ' ') mem_buf[--mlen] = '\0';
                fprintf(out, "Members: `%s`\n\n", mem_buf);
            } else {
                fprintf(out, "Members: `%s`\n\n", e->signature);
            }
        }
        }
    }

    if (funcs->count == 0 && classes->count == 0)
        fprintf(out, "_No public symbols found._\n");
}

/* -------------------------------------------------------------------------
 * Public entry point
 * ---------------------------------------------------------------------- */
int flux_doc_file(const char *path, const char *out_path) {
    /* Read source */
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "flux doc: cannot open '%s': ", path);
        perror("");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *src = malloc((size_t)size + 1);
    if (!src) { fclose(f); return 1; }
    fread(src, 1, (size_t)size, f);
    fclose(f);
    src[size] = '\0';

    /* Build line table */
    LineTable lt = build_line_table(src, (int)size);

    /* Parse */
    Lexer     lex;
    AstArena *arena = ast_arena_new();
    lexer_init(&lex, src);
    Parser parser;
    parser_init(&parser, &lex, arena, path);
    AstNode *module = parser_parse(&parser);

    if (parser.had_error) {
        parser_print_errors(&parser, src);
        ast_arena_free(arena);
        line_table_free(&lt);
        free(src);
        return 1;
    }

    /* Collect documentation entries */
    DocList funcs   = {0};
    DocList classes = {0};

    /* We use a combined list and separate by kind when printing */
    DocList all = {0};
    walk_block(&all, module, src, &lt, 0);

    /* Split into funcs and classes */
    for (DocEntry *e = all.head; e; e = e->next) {
        DocEntry *copy = doc_entry_new(e->kind, e->name, e->signature,
                                       e->docstring ? strdup(e->docstring) : NULL);
        if (strcmp(e->kind, "func") == 0 || strcmp(e->kind, "async func") == 0)
            doc_list_push(&funcs, copy);
        else
            doc_list_push(&classes, copy);
    }

    ast_arena_free(arena);
    doc_list_free(&all);
    line_table_free(&lt);
    free(src);

    /* Output */
    FILE *out;
    char  gen_path[512];
    bool  to_file = false;

    if (out_path) {
        out = fopen(out_path, "w");
        if (!out) {
            fprintf(stderr, "flux doc: cannot write '%s': ", out_path);
            perror("");
            doc_list_free(&funcs);
            doc_list_free(&classes);
            return 1;
        }
        to_file = true;
    } else {
        /* Default: write <file>.md next to the source */
        strncpy(gen_path, path, sizeof(gen_path) - 5);
        /* Replace .flx extension with .md */
        char *dot = strrchr(gen_path, '.');
        if (dot && strcmp(dot, ".flx") == 0) strcpy(dot, ".md");
        else strncat(gen_path, ".md", sizeof(gen_path) - strlen(gen_path) - 1);

        out = fopen(gen_path, "w");
        if (!out) {
            fprintf(stderr, "flux doc: cannot write '%s': ", gen_path);
            perror("");
            doc_list_free(&funcs);
            doc_list_free(&classes);
            return 1;
        }
        to_file = true;
        out_path = gen_path;
    }

    write_markdown(out, path, &funcs, &classes);
    if (to_file) {
        fclose(out);
        printf("flux doc: documentation written to '%s'\n", out_path);
        printf("flux doc: %d function(s), %d class/struct/enum(s) documented\n",
               funcs.count, classes.count);
    }

    doc_list_free(&funcs);
    doc_list_free(&classes);
    return 0;
}
