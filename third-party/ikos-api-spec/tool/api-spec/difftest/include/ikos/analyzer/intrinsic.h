/*******************************************************************************
 *
 * \file
 * \brief Shadow copy of ikos/analyzer/intrinsic.h for differential testing
 *
 * The real header turns every __ikos_* helper into a macro under -D__IKOS__
 * (and __ikos_assume is a macro either way), so the intrinsics cannot be
 * intercepted at link time.  When the stub build compiles with
 * "-I <this directory>" ahead of the ikos include directory, the stub's
 * "#include <ikos/analyzer/intrinsic.h>" resolves HERE instead: every helper
 * -- including __ikos_assume -- is an extern function implemented by
 * recorder.c, which logs the abstract operations the stub performs.
 *
 * The signatures match analyzer/include/ikos/analyzer/intrinsic.h exactly.
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
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
#define IKOS_NOEXCEPT noexcept
#else
#define IKOS_NOEXCEPT
#endif

extern void __ikos_assert(int condition) IKOS_NOEXCEPT;

/* the real header defines this as a macro; here it is a function so the
 * recorder observes (and the judge can prune on) violated conditions */
extern void __ikos_assume(int condition) IKOS_NOEXCEPT;

extern int __ikos_nondet_int(void) IKOS_NOEXCEPT;

extern unsigned __ikos_nondet_uint(void) IKOS_NOEXCEPT;

extern void* __ikos_malloc(size_t size) IKOS_NOEXCEPT;

extern void __ikos_check_mem_access(const void* ptr, size_t size)
    IKOS_NOEXCEPT;

extern void __ikos_check_string_access(const char* str) IKOS_NOEXCEPT;

extern void __ikos_assume_mem_size(const void* ptr, size_t size)
    IKOS_NOEXCEPT;

extern void __ikos_forget_mem(void* ptr, size_t size) IKOS_NOEXCEPT;

extern void __ikos_abstract_mem(void* ptr, size_t size) IKOS_NOEXCEPT;

extern void __ikos_assume_mem_value(void* ptr, size_t size, long lo, long hi)
    IKOS_NOEXCEPT;

extern void __ikos_assume_mem_rel(const void* ptr1,
                                  size_t size1,
                                  const void* ptr2,
                                  size_t size2,
                                  int kind) IKOS_NOEXCEPT;

extern void __ikos_watch_mem(const void* ptr, size_t size) IKOS_NOEXCEPT;

#undef IKOS_NOEXCEPT

#ifdef __cplusplus
}
#endif
