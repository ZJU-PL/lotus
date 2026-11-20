IGNORE close
IGNORE fclose
IGNORE free
IGNORE _ZdlPv
IGNORE exit
IGNORE __assert_fail
PIPE puts Ret V Arg0 D
PIPE fputs Ret V Arg0 D
IGNORE srand
PIPE fmod Ret V Arg0 V
PIPE fmod Ret V Arg1 V
PIPE fabs Ret V Arg0 V
PIPE cos Ret V Arg0 V
PIPE cosh Ret V Arg0 V
PIPE sin Ret V Arg0 V
PIPE sinh Ret V Arg0 V
PIPE sqrt Ret V Arg0 V
PIPE exp Ret V Arg0 V
PIPE log Ret V Arg0 V
PIPE asin Ret V Arg0 V
PIPE acos Ret V Arg0 V
PIPE atan Ret V Arg0 V
PIPE atan2 Ret V Arg0 V
PIPE atan2 Ret V Arg1 V
PIPE floor Ret V Arg0 V
PIPE ceil Ret V Arg0 V
PIPE modf Ret V Arg0 V
PIPE modf Arg2 D Arg0 V
PIPE log10 Ret V Arg0 V
PIPE bcopy Arg1 R Arg0 R
PIPE fgets Ret V Arg0 V
PIPE htons Ret V Arg0 V
PIPE inet_addr Ret V Arg0 D
PIPE llvm.memcpy.p0i8.p0i8.i64 Arg0 R Arg1 R
PIPE llvm.memcpy.p0i8.p0i8.i64 Ret V Arg0 V
PIPE llvm.memset.p0i8.i64 Arg0 R Arg1 V
PIPE llvm.memset.p0i8.i64 Ret V Arg0 V
PIPE memcpy Arg0 R Arg1 R
PIPE memcpy Ret V Arg0 V
PIPE memcmp Ret V Arg0 R
PIPE memcmp Ret V Arg1 R
PIPE strcmp Ret V Arg0 R
PIPE strcmp Ret V Arg1 R
PIPE strncmp Ret V Arg0 R
PIPE strncmp Ret V Arg1 R
PIPE memmove Arg0 R Arg1 R
PIPE memmove Ret V Arg0 V
PIPE memset Arg0 R Arg1 V
PIPE memset Ret V Arg0 V
PIPE strchr Ret V Arg0 D
PIPE strrchr Ret V Arg0 D
PIPE strcpy Arg0 D Arg1 D
PIPE strcpy Ret V Arg0 V
PIPE strlen Ret V Arg0 D
PIPE strncat Arg0 D Arg1 D
PIPE strncat Ret V Arg0 V
PIPE strncpy Arg0 D Arg1 D
PIPE strncpy Ret V Arg0 V
PIPE strdup Ret D Arg0 D
PIPE strdup Ret V Arg0 V
PIPE strtok Ret V Arg0 D
PIPE putchar Ret V Arg0 V
PIPE sscanf Ret V Arg0 D
PIPE sscanf AfterArg2 D Arg0 D
PIPE __isoc99_sscanf Ret V Arg0 D
PIPE __isoc99_sscanf AfterArg2 D Arg0 D
PIPE fscanf Ret V Arg1 D
PIPE __isoc99_fscanf Ret V Arg1 D
PIPE scanf Ret V Arg0 D
PIPE __isoc99_scanf Ret V Arg0 D
PIPE atoi Ret V Arg0 D
PIPE atol Ret V Arg0 D
PIPE tan Ret V Arg0 V
PIPE tanh Ret V Arg0 V
PIPE system Ret V Arg0 D
PIPE strtod Ret V Arg0 D
PIPE strtod Arg1 D Arg0 V
PIPE toupper Ret V Arg0 V
PIPE tolower Ret V Arg0 V
SINK calloc Arg0 V
SINK fprintf Arg1 D
SINK malloc Arg0 V
SINK _Znwm Arg0 V
SINK _Znam Arg0 V
SINK printf Arg0 D
SINK read Arg0 V
SINK read Arg2 V
SINK realloc Arg1 V
SINK sprintf Arg1 D
SINK snprintf Arg2 D
SINK syslog Arg1 D
SINK vfprintf Arg1 D
SINK vprintf Arg0 D
SINK wprintf Arg0 D
SINK system Arg0 D
SOURCE __errno_location Ret V U
SOURCE __isoc99_scanf AfterArg0 D T
SOURCE _IO_getc Ret V T
SOURCE fgets Arg0 D T
SOURCE getchar Ret V T
SOURCE read Arg1 D T
SOURCE read Ret V T
SOURCE recv Arg1 D T
SOURCE scanf AfterArg0 D T
SOURCE fscanf AfterArg1 D T
SOURCE __isoc99_fscanf AfterArg1 D T
SOURCE clock_gettime Arg1 R U
SOURCE malloc Ret V U
SOURCE _Znwm Ret V U
SOURCE _Znam Ret V U
SOURCE calloc Ret V U
SOURCE calloc Ret D U
SOURCE sprintf Arg0 D U
SOURCE snprintf Arg0 D U
SOURCE vsnprintf Arg0 D U
# Format functions: values and pointer-memory inputs both reach the output.
# D ranges select pointer arguments; V ranges include scalar arguments.
PIPE sprintf Arg0 D AfterArg1 V
PIPE sprintf Arg0 D AfterArg1 D
PIPE snprintf Arg0 D AfterArg2 V
PIPE snprintf Arg0 D AfterArg2 D
PIPE vsnprintf Arg0 D AfterArg2 V
PIPE vsnprintf Arg0 D AfterArg2 D
PIPE vsprintf Arg0 D AfterArg1 V
PIPE vsprintf Arg0 D AfterArg1 D
SOURCE getenv Ret V U
SOURCE socket Ret V U
SOURCE connect Ret V U
SOURCE rand Ret V U
SOURCE time Ret V U
SOURCE time Arg0 R U
SOURCE fopen Ret V U
SOURCE fgetc Ret V T
PIPE fputc Ret V Arg0 V
SOURCE fread Arg0 D T
SOURCE fread Ret V U
SOURCE __ctype_b_loc Ret V U
PIPE ungetc Ret V Arg0 V
SOURCE fflush Ret V U
SOURCE clock Ret V U
SOURCE random Ret V U
SOURCE erand48 Ret V U
PIPE abs Ret V Arg0 V
SOURCE fileno Ret V U
PIPE pow Ret V Arg0 V
PIPE pow Ret V Arg1 V
SOURCE getpid Ret V U
SOURCE getrusage Ret V U
PIPE llvm.umul.with.overflow.i64 Ret V Arg0 V
PIPE llvm.umul.with.overflow.i64 Ret V Arg1 V
PIPE strtol Ret V Arg0 D
SOURCE fseek Ret V U
PIPE strcoll Ret V Arg0 D
PIPE strcoll Ret V Arg1 D
PIPE strcat Ret V Arg0 V
PIPE strspn Ret V Arg0 D
PIPE strspn Ret V Arg1 D
PIPE strcspn Ret V Arg0 D
PIPE strcspn Ret V Arg0 D
SOURCE strerror Ret V U
SOURCE strerror Ret D U
PIPE ldexp Ret V Arg0 V
PIPE ldexp Ret V Arg1 V
PIPE frexp Ret V Arg0 V
PIPE frexp Arg1 D Arg0 V
IGNORE clearerr
SOURCE feof Ret V U
SOURCE ferror Ret V U
SOURCE ftell Ret V U
SOURCE localeconv Ret V U
SOURCE localeconv Ret D U
PIPE strstr Ret V Arg0 V
PIPE strpbrk Ret V Arg0 V
PIPE memchr Ret V Arg0 V
SOURCE tmpnam Ret V U
SOURCE tmpnam Ret D U
PIPE mktime Ret V Arg0 R
SOURCE setlocale Ret V U
SOURCE rename Ret V U
SOURCE remove Ret V U
PIPE difftime Ret V Arg0 V
PIPE difftime Ret V Arg1 V
SOURCE gmtime Ret V U
SOURCE localtime Ret V U
PIPE strftime Ret V Arg2 D
PIPE strftime Arg0 D Arg2 D
PIPE strftime Arg0 D Arg3 R
SOURCE fwrite Ret V U
SOURCE tmpfile Ret V U
SOURCE setvbuf Ret V U
PIPE strtoul Ret V Arg0 D
PIPE strtoul Arg1 D Arg0 V
SOURCE _setjmp Ret V U
IGNORE longjmp

# Additional sources from hardcoded lists
SOURCE gets Arg0 D T
SOURCE recvfrom Arg1 D T
SOURCE fread Arg0 D T

# Additional sinks from hardcoded lists  
SINK exec Arg0 D
SINK execl Arg0 D
SINK execle Arg0 D
SINK execlp Arg0 D
SINK execv Arg0 D
SINK execve Arg0 D
SINK execvp Arg0 D
SINK popen Arg0 D
SINK strcpy Arg0 D
SINK strcat Arg0 D
SINK write Arg1 D
SINK send Arg1 D
SINK sendto Arg1 D

# C++ stdlib: std::basic_string / std::string
PIPE std::basic_string::c_str Ret V Arg0 V
PIPE std::basic_string::data Ret V Arg0 V
PIPE std::basic_string::operator[] Ret V Arg0 V
PIPE std::basic_string::at Ret V Arg0 V
PIPE std::basic_string::front Ret V Arg0 V
PIPE std::basic_string::back Ret V Arg0 V
PIPE std::basic_string::operator+= Arg0 D Arg0 V
PIPE std::basic_string::append Arg0 D Arg0 V
PIPE std::basic_string::append Arg0 D Arg1 D

# C++ stdlib: std::vector
PIPE std::vector::operator[] Ret V Arg0 V
PIPE std::vector::at Ret V Arg0 V
PIPE std::vector::front Ret V Arg0 V
PIPE std::vector::back Ret V Arg0 V
PIPE std::vector::data Ret V Arg0 V
PIPE std::vector::push_back Arg0 D Arg0 V
PIPE std::vector::insert Arg0 D Arg1 D
PIPE std::vector::emplace_back Ret V Arg0 V

# C++ stdlib: std::array
PIPE std::array::operator[] Ret V Arg0 V
PIPE std::array::at Ret V Arg0 V
PIPE std::array::data Ret V Arg0 V

# C++ stdlib: std::deque
PIPE std::deque::operator[] Ret V Arg0 V
PIPE std::deque::at Ret V Arg0 V
PIPE std::deque::front Ret V Arg0 V
PIPE std::deque::back Ret V Arg0 V
PIPE std::deque::push_back Arg0 D Arg0 V
PIPE std::deque::push_front Arg0 D Arg0 V

# C++ stdlib: std::map / std::unordered_map
PIPE std::map::operator[] Ret V Arg0 V
PIPE std::unordered_map::operator[] Ret V Arg0 V
PIPE std::map::at Ret V Arg0 V
PIPE std::unordered_map::at Ret V Arg0 V

# Sanitizer functions (for future extension)
# SANITIZE strlen Arg0 D
# SANITIZE strnlen Arg0 D
# SANITIZE strncpy Arg0 D
# SANITIZE strncat Arg0 D
# SANITIZE snprintf Arg0 D

# Additional dependencies adapted from PhASAR 88366fc69, LibCSummary.cpp.
# Copyright (c) PhASAR contributors. MIT: third-party/phasar/LICENSE.txt.
# Scalar math uses value dependencies. String conversions read input data;
# endptr receives an input pointer, not a copy of the input string.
# Deliberately excludes unreviewed varargs, callbacks and output structures.
PIPE acosf Ret V Arg0 V
PIPE acosh Ret V Arg0 V
PIPE acoshf Ret V Arg0 V
PIPE acoshl Ret V Arg0 V
PIPE acosl Ret V Arg0 V
PIPE asinf Ret V Arg0 V
PIPE asinh Ret V Arg0 V
PIPE asinhf Ret V Arg0 V
PIPE asinhl Ret V Arg0 V
PIPE asinl Ret V Arg0 V
PIPE atan2f Ret V Arg0 V
PIPE atan2f Ret V Arg1 V
PIPE atan2l Ret V Arg0 V
PIPE atan2l Ret V Arg1 V
PIPE atanf Ret V Arg0 V
PIPE atanh Ret V Arg0 V
PIPE atanhf Ret V Arg0 V
PIPE atanhl Ret V Arg0 V
PIPE atanl Ret V Arg0 V
PIPE cbrt Ret V Arg0 V
PIPE cbrtf Ret V Arg0 V
PIPE cbrtl Ret V Arg0 V
PIPE ceilf Ret V Arg0 V
PIPE ceill Ret V Arg0 V
PIPE copysign Ret V Arg0 V
PIPE copysign Ret V Arg1 V
PIPE copysignf Ret V Arg0 V
PIPE copysignl Ret V Arg0 V
PIPE copysignl Ret V Arg1 V
PIPE cosf Ret V Arg0 V
PIPE coshf Ret V Arg0 V
PIPE coshl Ret V Arg0 V
PIPE cosl Ret V Arg0 V
PIPE erf Ret V Arg0 V
PIPE erfc Ret V Arg0 V
PIPE erfcf Ret V Arg0 V
PIPE erff Ret V Arg0 V
PIPE erfl Ret V Arg0 V
PIPE exp10 Ret V Arg0 V
PIPE exp10f Ret V Arg0 V
PIPE exp10l Ret V Arg0 V
PIPE exp2 Ret V Arg0 V
PIPE exp2f Ret V Arg0 V
PIPE exp2l Ret V Arg0 V
PIPE expf Ret V Arg0 V
PIPE expl Ret V Arg0 V
PIPE expm1 Ret V Arg0 V
PIPE expm1f Ret V Arg0 V
PIPE expm1l Ret V Arg0 V
PIPE fabsf Ret V Arg0 V
PIPE fabsl Ret V Arg0 V
PIPE fdim Ret V Arg0 V
PIPE fdimf Ret V Arg0 V
PIPE fdiml Ret V Arg0 V
PIPE floorf Ret V Arg0 V
PIPE floorl Ret V Arg0 V
PIPE fma Ret V Arg0 V
PIPE fma Ret V Arg1 V
PIPE fma Ret V Arg2 V
PIPE fmaf Ret V Arg0 V
PIPE fmaf Ret V Arg1 V
PIPE fmaf Ret V Arg2 V
PIPE fmal Ret V Arg0 V
PIPE fmal Ret V Arg1 V
PIPE fmal Ret V Arg2 V
PIPE fmax Ret V Arg0 V
PIPE fmax Ret V Arg1 V
PIPE fmaxf Ret V Arg0 V
PIPE fmaxf Ret V Arg1 V
PIPE fmaxl Ret V Arg0 V
PIPE fmaxl Ret V Arg1 V
PIPE fmin Ret V Arg0 V
PIPE fmin Ret V Arg1 V
PIPE fminf Ret V Arg0 V
PIPE fminf Ret V Arg1 V
PIPE fminl Ret V Arg0 V
PIPE fminl Ret V Arg1 V
PIPE fmodf Ret V Arg0 V
PIPE fmodf Ret V Arg1 V
PIPE fmodl Ret V Arg0 V
PIPE fmodl Ret V Arg1 V
PIPE hypot Ret V Arg0 V
PIPE hypot Ret V Arg1 V
PIPE hypotf Ret V Arg0 V
PIPE hypotf Ret V Arg1 V
PIPE hypotl Ret V Arg0 V
PIPE hypotl Ret V Arg1 V
PIPE ilogb Ret V Arg0 V
PIPE ilogbf Ret V Arg0 V
PIPE ilogbl Ret V Arg0 V
PIPE labs Ret V Arg0 V
PIPE llabs Ret V Arg0 V
PIPE ldexpl Ret V Arg0 V
PIPE ldexpl Ret V Arg1 V
PIPE llrint Ret V Arg0 V
PIPE llrintf Ret V Arg0 V
PIPE llround Ret V Arg0 V
PIPE llroundf Ret V Arg0 V
PIPE llroundl Ret V Arg0 V
PIPE log10f Ret V Arg0 V
PIPE log10l Ret V Arg0 V
PIPE log1p Ret V Arg0 V
PIPE log1pf Ret V Arg0 V
PIPE log1pl Ret V Arg0 V
PIPE log2 Ret V Arg0 V
PIPE log2f Ret V Arg0 V
PIPE log2l Ret V Arg0 V
PIPE logb Ret V Arg0 V
PIPE logbf Ret V Arg0 V
PIPE logbl Ret V Arg0 V
PIPE logf Ret V Arg0 V
PIPE logl Ret V Arg0 V
PIPE lrint Ret V Arg0 V
PIPE lrintf Ret V Arg0 V
PIPE lrintl Ret V Arg0 V
PIPE lround Ret V Arg0 V
PIPE lroundf Ret V Arg0 V
PIPE lroundl Ret V Arg0 V
PIPE nearbyint Ret V Arg0 V
PIPE nearbyintf Ret V Arg0 V
PIPE nearbyintl Ret V Arg0 V
PIPE nextafter Ret V Arg0 V
PIPE nextafterl Ret V Arg0 V
PIPE nexttoward Ret V Arg0 V
PIPE nexttowardf Ret V Arg0 V
PIPE nexttowardl Ret V Arg0 V
PIPE powf Ret V Arg0 V
PIPE powf Ret V Arg1 V
PIPE powl Ret V Arg0 V
PIPE powl Ret V Arg1 V
PIPE remainder Ret V Arg0 V
PIPE remainder Ret V Arg1 V
PIPE remainderf Ret V Arg0 V
PIPE remainderf Ret V Arg1 V
PIPE remainderl Ret V Arg0 V
PIPE remainderl Ret V Arg1 V
PIPE rint Ret V Arg0 V
PIPE rintf Ret V Arg0 V
PIPE rintl Ret V Arg0 V
PIPE round Ret V Arg0 V
PIPE roundf Ret V Arg0 V
PIPE roundl Ret V Arg0 V
PIPE scalbln Ret V Arg0 V
PIPE scalbln Ret V Arg1 V
PIPE scalblnf Ret V Arg0 V
PIPE scalblnl Ret V Arg0 V
PIPE scalblnl Ret V Arg1 V
PIPE scalbn Ret V Arg0 V
PIPE scalbn Ret V Arg1 V
PIPE scalbnf Ret V Arg0 V
PIPE scalbnf Ret V Arg1 V
PIPE scalbnl Ret V Arg0 V
PIPE scalbnl Ret V Arg1 V
PIPE signbit Ret V Arg0 V
PIPE sinf Ret V Arg0 V
PIPE sinl Ret V Arg0 V
PIPE sinhf Ret V Arg0 V
PIPE sinhl Ret V Arg0 V
PIPE sqrtf Ret V Arg0 V
PIPE sqrtl Ret V Arg0 V
PIPE tanf Ret V Arg0 V
PIPE tanhf Ret V Arg0 V
PIPE tanhl Ret V Arg0 V
PIPE tanl Ret V Arg0 V
PIPE tgamma Ret V Arg0 V
PIPE tgammaf Ret V Arg0 V
PIPE tgammal Ret V Arg0 V
PIPE trunc Ret V Arg0 V
PIPE truncf Ret V Arg0 V
PIPE truncl Ret V Arg0 V
PIPE strtof Ret V Arg0 D
PIPE strtof Arg1 D Arg0 V
PIPE strtold Ret V Arg0 D
PIPE strtold Arg1 D Arg0 V
PIPE strtoll Ret V Arg0 D
PIPE strtoll Arg1 D Arg0 V
PIPE strtoull Ret V Arg0 D
PIPE strtoull Arg1 D Arg0 V
PIPE strtoimax Ret V Arg0 D
PIPE strtoimax Arg1 D Arg0 V
PIPE strtoumax Ret V Arg0 D
PIPE strtoumax Arg1 D Arg0 V
PIPE wcstod Ret V Arg0 D
PIPE wcstod Arg1 D Arg0 V
PIPE wcstof Ret V Arg0 D
PIPE wcstof Arg1 D Arg0 V
PIPE wcstold Ret V Arg0 D
PIPE wcstold Arg1 D Arg0 V
PIPE wcstol Ret V Arg0 D
PIPE wcstol Arg1 D Arg0 V
PIPE wcstoll Ret V Arg0 D
PIPE wcstoll Arg1 D Arg0 V
PIPE wcstoul Ret V Arg0 D
PIPE wcstoul Arg1 D Arg0 V
PIPE wcstoull Ret V Arg0 D
PIPE wcstoull Arg1 D Arg0 V
PIPE wcstoimax Ret V Arg0 D
PIPE wcstoimax Arg1 D Arg0 V
PIPE wcstoumax Ret V Arg0 D
PIPE wcstoumax Arg1 D Arg0 V
PIPE stpcpy Arg0 D Arg1 D
PIPE stpcpy Ret V Arg0 V
PIPE stpncpy Arg0 D Arg1 D
PIPE stpncpy Ret V Arg0 V
PIPE wcscpy Arg0 D Arg1 D
PIPE wcscpy Ret V Arg0 V
PIPE wcsncpy Arg0 D Arg1 D
PIPE wcsncpy Ret V Arg0 V
PIPE wcpcpy Arg0 D Arg1 D
PIPE wcpcpy Ret V Arg0 V
PIPE wcpncpy Arg0 D Arg1 D
PIPE wcpncpy Ret V Arg0 V
PIPE wcscat Arg0 D Arg1 D
PIPE wcscat Ret V Arg0 V
PIPE wcsncat Arg0 D Arg1 D
PIPE wcsncat Ret V Arg0 V
PIPE strndup Ret D Arg0 D
PIPE wcsdup Ret D Arg0 D
PIPE wmemcpy Arg0 R Arg1 R
PIPE wmemcpy Ret V Arg0 V
PIPE wmemmove Arg0 R Arg1 R
PIPE wmemmove Ret V Arg0 V
PIPE wmempcpy Arg0 R Arg1 R
PIPE wmempcpy Ret V Arg0 V
PIPE mempcpy Arg0 R Arg1 R
PIPE mempcpy Ret V Arg0 V
PIPE towlower Ret V Arg0 V
PIPE towupper Ret V Arg0 V
PIPE toascii Ret V Arg0 V
PIPE htonl Ret V Arg0 V
PIPE ntohl Ret V Arg0 V
PIPE ntohs Ret V Arg0 V
# Additional scalar operands omitted by the upstream table.
PIPE copysignf Ret V Arg1 V
PIPE fdim Ret V Arg1 V
PIPE fdimf Ret V Arg1 V
PIPE fdiml Ret V Arg1 V
PIPE nextafter Ret V Arg1 V
PIPE nextafterl Ret V Arg1 V
PIPE nexttoward Ret V Arg1 V
PIPE nexttowardf Ret V Arg1 V
PIPE nexttowardl Ret V Arg1 V
PIPE scalblnf Ret V Arg1 V
