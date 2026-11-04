/*******************************************************************************
 *
 * \file
 * \brief __ikos_* intrinsic implementations for the stub build
 *
 * The stub body is the only semantics the analyzer ever sees; when the same
 * body runs natively against this recorder, every intrinsic turns into an
 * observation the differential judge can reason about:
 *
 *   - __ikos_nondet_int/uint pop the next value from the DT_NONDET schedule
 *     (comma separated, consumed in call order, 0 when exhausted), so the
 *     harness can drive the stub towards a chosen return value;
 *   - __ikos_assert / __ikos_assume log whether the condition held (a
 *     violated assume marks the run as pruned: the analyzer would cut that
 *     path, so its observations do not count);
 *   - check_mem_access / check_string_access / assume_mem_size log the
 *     claimed region, resolved against the driver's __dt_buf registrations
 *     (the registration table is the bounds oracle);
 *   - forget_mem / abstract_mem log the havoced region the same way; the
 *     recorder never writes the havoced memory (the judge compares real
 *     write-sets with havoc *claims*, never stub-side contents);
 *   - a constructor logs one FRAME event with an early main-thread frame
 *     address, the landmark the judge uses to spot a stub returning the
 *     address of one of its own locals.
 *
 * Everything is logged to the file named by DT_LOG, one event per line, so
 * the contract on fd 4 stays untouched.
 *
 * Notices:
 *
 * Copyright (c) 2018-2023 United States Government as represented by the
 * Administrator of the National Aeronautics and Space Administration.
 * All Rights Reserved.
 *
 * Disclaimers:
 *
 * No Warranty: THE SUBJECT SOFTWARE IS PROVIDED "AS IS" WITHOUT WARRANTY OF
 * ANY KIND, EITHER EXPRESSED, IMPLIED, OR STATORY, INCLUDING, BUT NOT
 * LIMITED TO, ANY WARRANTY THAT THE SUBJECT SOFTWARE WILL CONFORM TO
 * SPECIFICATIONS, ANY IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
 * PARTICULAR PURPOSE, OR NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 ******************************************************************************/
#include "dt.h"
#include <ikos/analyzer/intrinsic.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the log file, opened lazily; without DT_LOG the recorder is silent (the
 * real build never links this file, but robustness costs nothing) */
static FILE* dt_log = NULL;

static FILE* dt_log_stream(void) {
    if (dt_log == NULL) {
        const char* path = getenv("DT_LOG");
        if (path == NULL) {
            path = "/dev/null";
        }
        dt_log = fopen(path, "a");
        if (dt_log == NULL) {
            dt_log = stderr;
        }
    }
    return dt_log;
}

/* target stack-region landmark, logged once per run.  The judge rejects a
 * stub whose return pointer lands inside the target's stack frame (e.g.
 * g_malloc's "return &p0": dead storage for the caller).  An early
 * main-thread frame is landmark enough: every frame between program start
 * and the target call sits within a few KB of the target's own frame, far
 * inside the judge's +-1MB window. */
__attribute__((constructor))
static void dt_log_frame(void) {
    fprintf(dt_log_stream(),
            "FRAME %lx\n",
            (unsigned long) (uintptr_t) __builtin_frame_address(0));
}

/* ---- nondet schedule ---- */

static long long dt_next_nondet(void) {
    static char* schedule = NULL;
    static char* cursor = NULL;
    if (schedule == NULL) {
        const char* env = getenv("DT_NONDET");
        if (env != NULL) {
            schedule = strdup(env);
        } else {
            schedule = strdup("");
        }
        cursor = schedule;
    }
    if (*cursor == '\0') {
        return 0; /* exhausted */
    }
    char* end = NULL;
    long long value = strtoll(cursor, &end, 10);
    if (end == cursor) {
        return 0; /* malformed entry */
    }
    cursor = (*end == ',') ? end + 1 : end;
    return value;
}

int __ikos_nondet_int(void) {
    int value = (int) dt_next_nondet();
    fprintf(dt_log_stream(), "NONDET %d\n", value);
    return value;
}

unsigned __ikos_nondet_uint(void) {
    unsigned value = (unsigned) dt_next_nondet();
    fprintf(dt_log_stream(), "NONDET %u\n", value);
    return value;
}

void* __ikos_malloc(size_t size) {
    void* value = malloc(size);
    fprintf(dt_log_stream(),
            "MALLOC %lx %zu\n",
            (unsigned long) (uintptr_t) value,
            size);
    return value;
}

/* ---- conditions ---- */

void __ikos_assert(int condition) {
    fprintf(dt_log_stream(),
            "ASSERT %s\n",
            condition ? "PASS" : "FAIL");
}

void __ikos_assume(int condition) {
    fprintf(dt_log_stream(),
            "ASSUME %s\n",
            condition ? "OK" : "VIOLATED");
}

/* ---- memory claims, resolved against the registration table ---- */

static void dt_resolve(const void* p, const char* what, size_t n) {
    const char* tag = NULL;
    size_t off = 0;
    if (p == NULL) {
        fprintf(dt_log_stream(), "%s NULL\n", what);
    } else if (__dt_lookup(p, &tag, &off)) {
        fprintf(dt_log_stream(),
                "%s %s %zu %zu\n",
                what,
                tag,
                off,
                n);
    } else {
        fprintf(dt_log_stream(),
                "%s UNREG %lx %zu\n",
                what,
                (unsigned long) (uintptr_t) p,
                n);
    }
}

static void dt_resolve_val(const void* p, const char* what, size_t n,
                           long lo, long hi) {
    const char* tag = NULL;
    size_t off = 0;
    if (p == NULL) {
        fprintf(dt_log_stream(), "%s NULL %ld %ld\n", what, lo, hi);
    } else if (__dt_lookup(p, &tag, &off)) {
        fprintf(dt_log_stream(),
                "%s %s %zu %zu %ld %ld\n",
                what,
                tag,
                off,
                n,
                lo,
                hi);
    } else {
        fprintf(dt_log_stream(),
                "%s UNREG %lx %zu %ld %ld\n",
                what,
                (unsigned long) (uintptr_t) p,
                n,
                lo,
                hi);
    }
}

void __ikos_check_mem_access(const void* ptr, size_t size) {
    dt_resolve(ptr, "CHECK_MEM", size);
}

void __ikos_check_string_access(const char* str) {
    const char* tag = NULL;
    size_t off = 0;
    if (str == NULL) {
        fprintf(dt_log_stream(), "CHECK_STR NULL\n");
        return;
    }
    if (!__dt_lookup(str, &tag, &off)) {
        fprintf(dt_log_stream(),
                "CHECK_STR UNREG %lx\n",
                (unsigned long) (uintptr_t) str);
        return;
    }
    /* scan the remaining extent of the registered buffer for a terminator */
    long remaining = __dt_remaining(str);
    size_t scanned = 0;
    while ((long) scanned < remaining && str[scanned] != '\0') {
        scanned++;
    }
    const char* verdict =
        ((long) scanned < remaining) ? "TERM" : "NOTERM";
    fprintf(dt_log_stream(),
            "CHECK_STR %s %zu %s\n",
            tag,
            off,
            verdict);
}

void __ikos_assume_mem_size(const void* ptr, size_t size) {
    dt_resolve(ptr, "ASSUME_SIZE", size);
}

void __ikos_forget_mem(void* ptr, size_t size) {
    dt_resolve(ptr, "HAVOC_FORGET", size);
}

void __ikos_abstract_mem(void* ptr, size_t size) {
    dt_resolve(ptr, "HAVOC_ABSTRACT", size);
}

void __ikos_assume_mem_value(void* ptr, size_t size, long lo, long hi) {
    dt_resolve_val(ptr, "ASSUME_VAL", size, lo, hi);
}

void __ikos_assume_mem_rel(const void* p1, size_t s1,
                           const void* p2, size_t s2, int kind) {
    (void) p1; (void) s1; (void) p2; (void) s2; (void) kind;
    // analyzer-side only: the relation is consumed by ikos, the native run
    // just needs a definition so bodies using it link
}

void __ikos_watch_mem(const void* ptr, size_t size) {
    dt_resolve(ptr, "WATCH", size);
}
