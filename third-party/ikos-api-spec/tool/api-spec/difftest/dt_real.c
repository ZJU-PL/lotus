/*******************************************************************************
 *
 * \file
 * \brief __dt_* contract helpers, real-build side (also linked into the stub
 * build next to recorder.c)
 *
 * Implements the DT1 contract on file descriptor 4: buffer registration,
 * canary fills, pre-call snapshots, return dumps and post-call write-set
 * diffs.  Registration lines carry a fourth in/out column
 * ("DT1 bufreg <tag> <base-hex> <len> in|out"): __dt_buf marks caller-
 * initialized input storage, __dt_buf_out marks pure output storage, and
 * the judge uses the distinction to tell a legal strcpy-style result
 * (pointer into the output buffer) from an input-aliasing stub.  The stub
 * build links this file too, so the driver observes the same contract
 * shape against the stub; the judge only ever compares real-run
 * observations with stub-run *havoc claims* (never buffer contents
 * across builds).
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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DT_MAX_BUFFERS 32
#define DT_MAX_SNAPSHOT (1 << 20)

typedef struct {
    char tag[16];
    unsigned char* base;
    size_t len;
    unsigned char* snapshot; /* taken at __dt_call, owned */
} dt_buffer;

static dt_buffer dt_buffers[DT_MAX_BUFFERS];
static int dt_buffer_count = 0;

static int dt_fd(void) {
    return 4; /* the harness dups the contract pipe here */
}

static unsigned char dt_canary(void) {
    const char* env = getenv("DT_CANARY");
    if (env != NULL && strcmp(env, "55") == 0) {
        return 0x55;
    }
    return 0xaa;
}

static void dt_register(const char* tag, void* ptr, size_t len, int fill) {
    dt_buffer* buf = NULL;
    for (int i = 0; i < dt_buffer_count; i++) {
        if (strcmp(dt_buffers[i].tag, tag) == 0) {
            buf = &dt_buffers[i]; /* re-registration: replace */
            free(buf->snapshot);
            buf->snapshot = NULL;
            break;
        }
    }
    if (buf == NULL && dt_buffer_count < DT_MAX_BUFFERS) {
        buf = &dt_buffers[dt_buffer_count++];
    }
    if (buf == NULL) {
        return; /* registry full: the driver registers far too much */
    }
    snprintf(buf->tag, sizeof(buf->tag), "%s", tag);
    buf->base = (unsigned char*) ptr;
    buf->len = len;
    buf->snapshot = NULL;
    if (fill && len > 0 && len <= DT_MAX_SNAPSHOT) {
        memset(ptr, dt_canary(), len);
    }
    dprintf(dt_fd(),
            "DT1 bufreg %s %lx %zu %s\n",
            buf->tag,
            (unsigned long) (uintptr_t) buf->base,
            buf->len,
            fill ? "out" : "in");
}

void __dt_buf(const char* tag, void* ptr, size_t len) {
    dt_register(tag, ptr, len, 0);
}

void __dt_buf_out(const char* tag, void* ptr, size_t len) {
    dt_register(tag, ptr, len, 1);
}

void __dt_begin(int id) {
    dprintf(dt_fd(), "DT1 begin %d\n", id);
}

void __dt_call(void) {
    for (int i = 0; i < dt_buffer_count; i++) {
        dt_buffer* buf = &dt_buffers[i];
        if (buf->len == 0 || buf->len > DT_MAX_SNAPSHOT) {
            continue;
        }
        buf->snapshot = (unsigned char*) malloc(buf->len);
        if (buf->snapshot != NULL) {
            memcpy(buf->snapshot, buf->base, buf->len);
        }
    }
    dprintf(dt_fd(), "DT1 call\n");
}

void __dt_ret(const void* ret, size_t size) {
    dprintf(dt_fd(), "DT1 ret ");
    if (ret == NULL) {
        dprintf(dt_fd(), "null");
    } else {
        const unsigned char* bytes = (const unsigned char*) ret;
        size_t n = size < 16 ? size : 16;
        for (size_t i = 0; i < n; i++) {
            dprintf(dt_fd(), "%02x", bytes[i]);
        }
        if (size > 16) {
            dprintf(dt_fd(), "...");
        }
    }
    dprintf(dt_fd(), "\n");
}

void __dt_ret_void(void) {
    dprintf(dt_fd(), "DT1 ret void\n");
}

int __dt_lookup(const void* p, const char** tag, size_t* off) {
    for (int i = 0; i < dt_buffer_count; i++) {
        dt_buffer* buf = &dt_buffers[i];
        if ((const unsigned char*) p >= buf->base &&
            (const unsigned char*) p < buf->base + buf->len) {
            *tag = buf->tag;
            *off = (size_t)((const unsigned char*) p - buf->base);
            return 1;
        }
    }
    return 0;
}

long __dt_remaining(const void* p) {
    const char* tag = NULL;
    size_t off = 0;
    for (int i = 0; i < dt_buffer_count; i++) {
        dt_buffer* buf = &dt_buffers[i];
        if ((const unsigned char*) p >= buf->base &&
            (const unsigned char*) p < buf->base + buf->len) {
            return (long) (buf->len - ((const unsigned char*) p - buf->base));
        }
    }
    (void) tag;
    (void) off;
    return -1;
}

/* "3-6,8,10-12" (inclusive byte offsets), "-" when nothing changed */
static void dt_write_ranges(dt_buffer* buf) {
    int first = 1;
    size_t run_start = 0;
    int in_run = 0;
    for (size_t i = 0; i < buf->len; i++) {
        int changed = buf->snapshot != NULL &&
                      buf->base[i] != buf->snapshot[i];
        if (changed && !in_run) {
            run_start = i;
            in_run = 1;
        } else if (!changed && in_run) {
            dprintf(dt_fd(),
                    first ? "%zu-%zu" : ",%zu-%zu",
                    run_start,
                    i - 1);
            first = 0;
            in_run = 0;
        }
    }
    if (in_run) {
        dprintf(dt_fd(),
                first ? "%zu-%zu" : ",%zu-%zu",
                run_start,
                buf->len - 1);
        first = 0;
    }
    if (first) {
        dprintf(dt_fd(), "-");
    }
}

/* "DT1 data <tag> <hex>" -- post-call contents, capped so a large buffer
 * cannot flood the contract pipe (the harness reads fd 4 only after the
 * child exits); the judge scans it for pointer-valued writes (8-byte
 * windows equal to a known pointer), which sit at pointer-slot offsets,
 * near the start of output buffers */
static void dt_write_data(dt_buffer* buf) {
    size_t cap = buf->len;
    if (cap > 512) {
        cap = 512;
    }
    dprintf(dt_fd(), "DT1 data %s ", buf->tag);
    for (size_t i = 0; i < cap; i++) {
        dprintf(dt_fd(), "%02x", buf->base[i]);
    }
    dprintf(dt_fd(), "\n");
}

void __dt_end(void) {
    for (int i = 0; i < dt_buffer_count; i++) {
        dt_buffer* buf = &dt_buffers[i];
        dprintf(dt_fd(), "DT1 wrote %s ", buf->tag);
        if (buf->len == 0 || buf->len > DT_MAX_SNAPSHOT ||
            buf->snapshot == NULL) {
            dprintf(dt_fd(), "?"); /* never snapshotted: driver shape bug */
        } else {
            dt_write_ranges(buf);
        }
        dprintf(dt_fd(), "\n");
        dt_write_data(buf);
    }
    dprintf(dt_fd(), "DT1 ok\n");
    for (int i = 0; i < dt_buffer_count; i++) {
        free(dt_buffers[i].snapshot);
        dt_buffers[i].snapshot = NULL;
    }
}
