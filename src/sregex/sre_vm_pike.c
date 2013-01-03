
/*
 * Copyright 2012 Yichun "agentzh" Zhang
 * Copyright 2007-2009 Russ Cox.  All Rights Reserved.
 * Use of this source code is governed by a BSD-style
 * license that can be found in the LICENSE file.
 */


#ifndef DDEBUG
#define DDEBUG 0
#endif
#include <sregex/ddebug.h>


#include <sregex/sre_vm_pike.h>
#include <sregex/sre_capture.h>


#define sre_vm_pike_free_thread(ctx, t)                                     \
    (t)->next = (ctx)->free_threads;                                        \
    (ctx)->free_threads = t;


enum {
    SRE_VM_PIKE_SEEN_WORD = 1
};


typedef struct sre_vm_pike_thread_s  sre_vm_pike_thread_t;

struct sre_vm_pike_thread_s {
    sre_instruction_t       *pc;
    sre_capture_t           *capture;
    sre_vm_pike_thread_t    *next;
    unsigned                 seen_word; /* :1 */
};


typedef struct {
    sre_vm_pike_thread_t     *head;
    sre_vm_pike_thread_t    **next;
} sre_vm_pike_thread_list_t;


struct sre_vm_pike_ctx_s {
    unsigned                 tag;
    unsigned                 processed_bytes;
    u_char                  *buffer;
    sre_pool_t              *pool;
    sre_program_t           *program;
    sre_capture_t           *matched;
    sre_capture_t           *free_capture;
    sre_vm_pike_thread_t    *free_threads;

    int                     *ovector;
    unsigned                 ovecsize;

    sre_vm_pike_thread_list_t       *current_threads;
    sre_vm_pike_thread_list_t       *next_threads;

    int                      last_matched_pos; /* the pos for the last
                                                  (partial) match */

    unsigned                 first_buf:1;
    unsigned                 eof:1;
    unsigned                 empty_capture:1;
    unsigned                 seen_newline:1;
    unsigned                 seen_word:1;
} ;


static sre_vm_pike_thread_list_t *
    sre_vm_pike_thread_list_create(sre_pool_t *pool);
static int sre_vm_pike_add_thread(sre_vm_pike_ctx_t *ctx,
    sre_vm_pike_thread_list_t *l, sre_instruction_t *pc, sre_capture_t *capture,
    int pos, sre_capture_t **pcap);
static void sre_vm_pike_prepare_temp_captures(sre_vm_pike_ctx_t *ctx);


sre_vm_pike_ctx_t *
sre_vm_pike_create_ctx(sre_pool_t *pool, sre_program_t *prog, int *ovector,
    unsigned ovecsize)
{
    sre_vm_pike_ctx_t               *ctx;
    sre_vm_pike_thread_list_t       *clist, *nlist;

    ctx = sre_palloc(pool, sizeof(sre_vm_pike_ctx_t));
    if (ctx == NULL) {
        return NULL;
    }

    ctx->pool = pool;
    ctx->program = prog;
    ctx->processed_bytes = 0;

    clist = sre_vm_pike_thread_list_create(pool);
    if (clist == NULL) {
        return NULL;
    }

    ctx->current_threads = clist;

    nlist = sre_vm_pike_thread_list_create(pool);
    if (nlist == NULL) {
        return NULL;
    }

    ctx->next_threads = nlist;

    ctx->program = prog;
    ctx->pool = pool;
    ctx->free_capture = NULL;
    ctx->free_threads = NULL;
    ctx->matched = NULL;

    ctx->ovecsize = ovecsize;
    ctx->ovector = ovector;

    ctx->first_buf = 1;
    ctx->eof = 0;
    ctx->empty_capture = 0;
    ctx->seen_newline = 0;
    ctx->seen_word = 0;

    return ctx;
}


int
sre_vm_pike_exec(sre_vm_pike_ctx_t *ctx, u_char *input, size_t size,
    unsigned eof)
{
    int                        rc, seen_word;
    u_char                    *sp, *last, *p;
    unsigned                   i, in;
    sre_pool_t                *pool;
    sre_program_t             *prog;
    sre_capture_t             *cap, *matched;
    sre_vm_range_t            *range;
    sre_instruction_t         *pc;
    sre_vm_pike_thread_t      *t;
    sre_vm_pike_thread_list_t *clist, *nlist, *tmp;
    sre_vm_pike_thread_list_t  list;

    if (ctx->eof) {
        dd("eof found");
        return SRE_ERROR;
    }

    pool = ctx->pool;
    prog = ctx->program;
    clist = ctx->current_threads;
    nlist = ctx->next_threads;
    matched = ctx->matched;

    ctx->buffer = input;
    ctx->last_matched_pos = -1;

    if (ctx->empty_capture) {
        dd("found empty capture");
        ctx->empty_capture = 0;

        if (size == 0) {
            if (eof) {
                ctx->eof = 1;
                return SRE_DECLINED;
            }

            return SRE_AGAIN;
        }

        sp = input + 1;

    } else {
        sp = input;
    }

    if (ctx->first_buf) {
        ctx->first_buf = 0;

        cap = sre_capture_create(pool, ctx->ovecsize, 1);
        if (cap == NULL) {
            return SRE_ERROR;
        }

        ctx->tag = prog->tag + 1;
        rc = sre_vm_pike_add_thread(ctx, clist, prog->start, cap,
                                   (int)(sp - input), NULL);
        if (rc != SRE_OK) {
            prog->tag = ctx->tag;
            return SRE_ERROR;
        }

    } else {
        ctx->tag = prog->tag;
    }

    last = input + size;

    for (; sp < last || (eof && sp == last); sp++) {
        dd("=== pos %d (char '%c' (%d)).\n", (int)(sp - input),
           sp < last ? *sp : '?', sp < last ? *sp : 0);

        if (clist->head == NULL) {
            dd("clist empty. abort.");
            break;
        }

#if (DDEBUG)
        fprintf(stderr, "cur list:");
        for (t = clist->head; t; t = t->next) {
            fprintf(stderr, " %d", (int) (t->pc - prog->start));
        }
        fprintf(stderr, "\n");
#endif

        ctx->tag++;

        while (clist->head) {
            t = clist->head;
            clist->head = t->next;

            pc = t->pc;
            cap = t->capture;

            dd("--- #%u: pc %d: opcode %d\n", ctx->tag, (int)(pc - prog->start),
               pc->opcode);

            switch (pc->opcode) {
            case SRE_OPCODE_IN:
                if (sp == last) {
                    sre_capture_decr_ref(ctx, cap);
                    break;
                }

                in = 0;
                for (i = 0; i < pc->v.ranges->count; i++) {
                    range = &pc->v.ranges->head[i];

                    dd("testing %d for [%d, %d] (%u)", *sp, range->from,
                       range->to, i);

                    if (*sp >= range->from && *sp <= range->to) {
                        in = 1;
                        break;
                    }
                }

                if (!in) {
                    sre_capture_decr_ref(ctx, cap);
                    break;
                }

                rc = sre_vm_pike_add_thread(ctx, nlist, pc + 1, cap,
                                           (int) (sp - input + 1), &cap);

                if (rc == SRE_DONE) {
                    goto matched;
                }

                if (rc != SRE_OK) {
                    prog->tag = ctx->tag;
                    return SRE_ERROR;
                }

                break;

            case SRE_OPCODE_NOTIN:
                if (sp == last) {
                    sre_capture_decr_ref(ctx, cap);
                    break;
                }

                in = 0;
                for (i = 0; i < pc->v.ranges->count; i++) {
                    range = &pc->v.ranges->head[i];

                    dd("testing %d for [%d, %d] (%u)", *sp, range->from,
                       range->to, i);

                    if (*sp >= range->from && *sp <= range->to) {
                        in = 1;
                        break;
                    }
                }

                if (in) {
                    sre_capture_decr_ref(ctx, cap);
                    break;
                }

                rc = sre_vm_pike_add_thread(ctx, nlist, pc + 1, cap,
                                           (int) (sp - input + 1), &cap);

                if (rc == SRE_DONE) {
                    goto matched;
                }

                if (rc != SRE_OK) {
                    prog->tag = ctx->tag;
                    return SRE_ERROR;
                }

                break;

            case SRE_OPCODE_CHAR:

                dd("matching char '%c' (%d) against %d",
                   sp != last ? *sp : '?', sp != last ? *sp : 0, pc->v.ch);

                if (sp == last || *sp != pc->v.ch) {
                    sre_capture_decr_ref(ctx, cap);
                    break;
                }

                rc = sre_vm_pike_add_thread(ctx, nlist, pc + 1, cap,
                                            (int) (sp - input + 1), &cap);

                if (rc == SRE_DONE) {
                    goto matched;
                }

                if (rc != SRE_OK) {
                    prog->tag = ctx->tag;
                    return SRE_ERROR;
                }

                break;

            case SRE_OPCODE_ANY:

                if (sp == last) {
                    sre_capture_decr_ref(ctx, cap);
                    break;
                }

                rc = sre_vm_pike_add_thread(ctx, nlist, pc + 1, cap,
                                           (int) (sp - input + 1), &cap);

                if (rc == SRE_DONE) {
                    goto matched;
                }

                if (rc != SRE_OK) {
                    prog->tag = ctx->tag;
                    return SRE_ERROR;
                }

                break;

            case SRE_OPCODE_ASSERT:
                switch (pc->v.assertion_type) {
                case SRE_REGEX_ASSERTION_SMALL_Z:
                    if (sp != last) {
                        break;
                    }

                    goto assertion_hold;

                case SRE_REGEX_ASSERTION_DOLLAR:

                    if (sp != last && *sp != '\n') {
                        break;
                    }

                    dd("dollar $ assertion hold: pos=%d",
                       (int)(sp - input + ctx->processed_bytes));

                    goto assertion_hold;

                case SRE_REGEX_ASSERTION_BIG_B:

                    seen_word = (t->seen_word
                                 || (sp == input && ctx->seen_word));
                    if (seen_word ^ (sp != last && sre_isword(*sp))) {
                        break;
                    }

                    dd("\\B assertion passed: %u %c", t->seen_word, *sp);

                    goto assertion_hold;

                case SRE_REGEX_ASSERTION_SMALL_B:

                    seen_word = (t->seen_word
                                 || (sp == input && ctx->seen_word));
                    if ((seen_word ^ (sp != last && sre_isword(*sp))) == 0) {
                        break;
                    }

#if (DDEBUG)
                    if (sp) {
                        dd("\\b assertion passed: t:%u, ctx:%u, %c",
                           t->seen_word, ctx->seen_word, *sp);
                    }
#endif

                    goto assertion_hold;

                default:
                    /* impossible to reach here */
                    break;
                }

                break;

assertion_hold:
                ctx->tag--;

                list.head = NULL;
                rc = sre_vm_pike_add_thread(ctx, &list, pc + 1, cap,
                                           (int)(sp - input), NULL);

                if (rc != SRE_OK) {
                    prog->tag = ctx->tag + 1;
                    return SRE_ERROR;
                }

                if (list.head) {
                    *list.next = clist->head;
                    clist->head = list.head;
                }

                ctx->tag++;

                dd("sp + 1 == last: %d, eof: %u", sp + 1 == last, eof);
                break;

            case SRE_OPCODE_MATCH:

                ctx->last_matched_pos = cap->vector[1];

matched:
                if (matched) {

                    dd("discarding match: ");
#if (DDEBUG)
                    sre_capture_dump(matched);
#endif

                    sre_capture_decr_ref(ctx, matched);
                }

                dd("set matched");

                matched = cap;

                sre_vm_pike_free_thread(ctx, t);

                while (clist->head) {
                    t = clist->head;
                    clist->head = t->next;

                    sre_capture_decr_ref(ctx, t->capture);
                    sre_vm_pike_free_thread(ctx, t);
                }

                goto step_done;

                /*
                 * Jmp, Split, Save handled in addthread, so that
                 * machine execution matches what a backtracker would do.
                 * This is discussed (but not shown as code) in
                 * Regular Expression Matching: the Virtual Machine Approach.
                 */
            default:
                /* impossible to reach here */
                break;
            }

            sre_vm_pike_free_thread(ctx, t);
        } /* while */

step_done:
        tmp = clist;
        clist = nlist;
        nlist = tmp;

        if (nlist->head) {
            *nlist->next = ctx->free_threads;
            ctx->free_threads = nlist->head;
            nlist->head = NULL;
        }

        if (sp == last) {
            break;
        }
    } /* for */

    dd("matched: %p, clist: %p, pos: %d", matched, clist->head,
       (int) (ctx->processed_bytes + (sp - input)));

    if (ctx->last_matched_pos >= 0) {
        p = input + ctx->last_matched_pos - ctx->processed_bytes;
        if (p > input) {
            dd("diff: %d", ctx->last_matched_pos - ctx->processed_bytes);
            dd("p=%p, input=%p", p, ctx->buffer);

            ctx->seen_newline = (p[-1] == '\n');
            ctx->seen_word = sre_isword(p[-1]);

            dd("set seen newline: %u", ctx->seen_newline);
            dd("set seen word: %u", ctx->seen_word);
        }

        ctx->last_matched_pos = -1;
    }

    prog->tag = ctx->tag;
    ctx->current_threads = clist;
    ctx->next_threads = nlist;

    if (matched && (clist->head == NULL || eof)) {
        memcpy(ctx->ovector, matched->vector, ctx->ovecsize);
        /* sre_capture_decr_ref(matched, freecap); */

        if (clist->head) {
            *clist->next = ctx->free_threads;
            ctx->free_threads = clist->head;
            clist->head = NULL;
            ctx->eof = 1;
        }

        ctx->processed_bytes = ctx->ovector[1];
        ctx->empty_capture = (ctx->ovector[0] == ctx->ovector[1]);

        dd("set empty capture: %u", ctx->empty_capture);
        ctx->matched = NULL;
        ctx->first_buf = 1;

        return SRE_OK;
    }

    if (eof) {
        ctx->eof = 1;
        return SRE_DECLINED;
    }

    ctx->processed_bytes += sp - input;

    dd("processed bytes: %u", ctx->processed_bytes);

    ctx->matched = matched;

    sre_vm_pike_prepare_temp_captures(ctx);

#if 0
    if (sp > input) {
        ctx->seen_newline = (sp[-1] == '\n');
        ctx->seen_word = sre_isword(sp[-1]);
    }
#endif

    return SRE_AGAIN;
}


static void
sre_vm_pike_prepare_temp_captures(sre_vm_pike_ctx_t *ctx)
{
    int                      i, a, b;
    int                      ngroups;
    sre_capture_t           *cap;
    sre_vm_pike_thread_t    *t;

    t = ctx->current_threads->head;
    if (t) {

        ngroups = ctx->ovecsize / sizeof(int);

        dd("ngroups: %d", ngroups);

        memcpy(ctx->ovector, t->capture->vector, ctx->ovecsize);

        for (t = t->next; t; t = t->next) {
            cap = t->capture;

            for (i = 0; i < ngroups; i += 2) {
                a = ctx->ovector[i];
                b = cap->vector[i];

                if (b != -1 && (a == -1 || b < a)) {
                    dd("setting group %d to %d", i, cap->vector[i]);
                    ctx->ovector[i] = b;
                }

                a = ctx->ovector[i + 1];
                b = cap->vector[i + 1];

                if (b != -1 && (a == -1 || b > a)) {
                    dd("setting group %d to %d", i + 1, cap->vector[i + 1]);
                    ctx->ovector[i + 1] = b;
                }
            }
        }
    }
}


static sre_vm_pike_thread_list_t *
sre_vm_pike_thread_list_create(sre_pool_t *pool)
{
    sre_vm_pike_thread_list_t       *l;

    l = sre_palloc(pool, sizeof(sre_vm_pike_thread_list_t));
    if (l == NULL) {
        return NULL;
    }

    l->head = NULL;
    l->next = &l->head;

    return l;
}


static int
sre_vm_pike_add_thread(sre_vm_pike_ctx_t *ctx, sre_vm_pike_thread_list_t *l,
    sre_instruction_t *pc, sre_capture_t *capture, int pos,
    sre_capture_t **pcap)
{
    int                          rc;
    sre_vm_pike_thread_t        *t;
    sre_capture_t               *cap;
    unsigned                     seen_word = 0;

#if 0
    dd("pc tag: %u, ctx tag: %u", pc->tag, ctx->tag);
#endif

    if (pc->tag == ctx->tag) {
        dd("pc %d: already on list: %d", (int) (pc - ctx->program->start),
           pc->tag);

        if (pc->opcode == SRE_OPCODE_SPLIT) {
            if (pc->y->tag != ctx->tag) {
                return sre_vm_pike_add_thread(ctx, l, pc->y, capture, pos,
                                              pcap);
            }
        }

        return SRE_OK;
    }

    dd("adding thread: pc %d, bytecode %d", (int) (pc - ctx->program->start),
       pc->opcode);

    pc->tag = ctx->tag;

    switch (pc->opcode) {
    case SRE_OPCODE_JMP:
        return sre_vm_pike_add_thread(ctx, l, pc->x, capture, pos, pcap);

    case SRE_OPCODE_SPLIT:
        capture->ref++;

        rc = sre_vm_pike_add_thread(ctx, l, pc->x, capture, pos, pcap);
        if (rc != SRE_OK) {
            capture->ref--;
            return rc;
        }

        return sre_vm_pike_add_thread(ctx, l, pc->y, capture, pos, pcap);

    case SRE_OPCODE_SAVE:

#if 0
        dd("pc %d: cap %p: save %d as group %d",
           (int) (pc - ctx->program->start), capture, pos,
           pc->v.group);
#endif

        dd("processed bytes: %u, pos: %u", ctx->processed_bytes, pos);

        cap = sre_capture_update(ctx->pool, capture, pc->v.group,
                                 ctx->processed_bytes + pos,
                                 &ctx->free_capture);
        if (cap == NULL) {
            return SRE_ERROR;
        }

#if 0
        dd("new cap: %p", cap);
#endif

        return sre_vm_pike_add_thread(ctx, l, pc + 1, cap, pos, pcap);


    case SRE_OPCODE_ASSERT:
        switch (pc->v.assertion_type) {
        case SRE_REGEX_ASSERTION_BIG_A:
            if (pos || ctx->processed_bytes) {
                break;
            }

            return sre_vm_pike_add_thread(ctx, l, pc + 1, capture, pos, pcap);

        case SRE_REGEX_ASSERTION_CARET:
            dd("seen newline: %u", ctx->seen_newline);

            if (pos == 0) {
                if (ctx->processed_bytes && !ctx->seen_newline) {
                    break;
                }

            } else {
                if (ctx->buffer[pos - 1] != '\n') {
                    break;
                }
            }

            dd("newline assertion hold");

            return sre_vm_pike_add_thread(ctx, l, pc + 1, capture, pos, pcap);

        case SRE_REGEX_ASSERTION_SMALL_B:
        case SRE_REGEX_ASSERTION_BIG_B:
            {
                u_char c;

                if (pos == 0) {
                    seen_word = 0;

                } else {
                    c = ctx->buffer[pos - 1];
                    seen_word = sre_isword(c);
                }

                goto add;
            }

        default:
            /* postpone look-ahead assertions */
            goto add;
        }

        break;

    case SRE_OPCODE_MATCH:

        ctx->last_matched_pos = capture->vector[1];

        if (pcap && l->head == NULL) {
            *pcap = capture;
            return SRE_DONE;
        }

    default:

add:
        if (ctx->free_threads) {
            /* fprintf(stderr, "reusing free thread\n"); */

            t = ctx->free_threads;
            ctx->free_threads = t->next;
            t->next = NULL;

        } else {
            /* fprintf(stderr, "creating new thread\n"); */

            t = sre_palloc(ctx->pool, sizeof(sre_vm_pike_thread_t));
            if (t == NULL) {
                return SRE_ERROR;
            }
        }

        t->pc = pc;
        t->capture = capture;
        t->next = NULL;
        t->seen_word = seen_word;

        if (l->head == NULL) {
            l->head = t;

        } else {
            *l->next = t;
        }

        l->next = &t->next;

        dd("added thread: pc %d, bytecode %d", (int) (pc - ctx->program->start),
           pc->opcode);

        break;
    }

    return SRE_OK;
}
