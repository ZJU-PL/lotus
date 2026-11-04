/*******************************************************************************
 *
 * \file
 * \brief Differential testing contract helpers
 *
 * A driver program compiled for differential testing calls these helpers to
 * report the observable behavior of the tested function to the harness on
 * file descriptor 4 (never stdout, so a tested function that prints cannot
 * pollute the contract).
 *
 * The same helpers are linked into both builds: the real build (against the
 * real library, dt_real.c) and the stub build (stub + recorder.c + dt_real.c).
 *
 * This file is tool runtime support, not analyzed code.
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
#ifndef IKOS_DT_H
#define IKOS_DT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* contract sequence, exactly once per process:
 *
 *   __dt_begin(0);
 *   __dt_buf("b0", buf, sizeof buf);       buffers whose contents the driver
 *   __dt_buf_out("b1", out, sizeof out);   initialized (buf) or left raw for
 *                                          the function to fill (buf_out)
 *   __dt_call();
 *   <type> r = <tested function>(...);     exactly one call
 *   __dt_ret(&r, sizeof r);                or __dt_ret_void()
 *   __dt_end();
 */

/* "DT1 begin <id>" -- marks the contract as started */
void __dt_begin(int id);

/* register [ptr, ptr+len) under the given tag ("b0", "b1", ...);
 * emits "DT1 bufreg <tag> <base-hex> <len>" so the judge can classify
 * pointers against the registrations of the very same run */
void __dt_buf(const char* tag, void* ptr, size_t len);

/* like __dt_buf, but first fills the region with the canary byte
 * (DT_CANARY env, 0xAA or 0x55): a write of a byte equal to the fill value
 * is still caught by one of the two canary runs; use for pure output
 * buffers whose pre-call contents do not matter */
void __dt_buf_out(const char* tag, void* ptr, size_t len);

/* "DT1 call" -- the tested call starts here; snapshots every registered
 * buffer so __dt_end can diff the afterwards state */
void __dt_call(void);

/* "DT1 ret <hex bytes>" -- the return value of the tested call */
void __dt_ret(const void* ret, size_t size);

/* "DT1 ret void" */
void __dt_ret_void(void);

/* per registered tag: "DT1 wrote <tag> <changed-offset-ranges>" (or "-"),
 * then "DT1 ok" */
void __dt_end(void);

/* runtime support shared with recorder.c: resolve a pointer against the
 * registrations of this run; writes through tag/off (offset from the buffer
 * base) and returns 1, or returns 0 when the pointer is outside every
 * registered region */
int __dt_lookup(const void* p, const char** tag, size_t* off);

/* bytes from p up to the end of its registered buffer, or -1 when p is not
 * inside any registration (so string scans stay inside the oracle extent) */
long __dt_remaining(const void* p);

#ifdef __cplusplus
}
#endif

#endif /* IKOS_DT_H */
