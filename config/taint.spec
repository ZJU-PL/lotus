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
SOURCE __isoc99_scanf AfterArg1 D T
SOURCE _IO_getc Ret V T
SOURCE fgets Arg0 D T
SOURCE getchar Ret V T
SOURCE read Arg1 D T
SOURCE read Ret V T
SOURCE recv Arg1 D T
SOURCE scanf AfterArg1 D T
SOURCE fscanf AfterArg2 D T
SOURCE __isoc99_fscanf AfterArg2 D T
SOURCE clock_gettime Arg1 R U
SOURCE malloc Ret V U
SOURCE _Znwm Ret V U
SOURCE _Znam Ret V U
SOURCE calloc Ret V U
SOURCE calloc Ret D U
SOURCE sprintf Arg0 D U
SOURCE snprintf Arg0 D U
SOURCE vsnprintf Arg0 D U
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

# Sanitizer functions (for future extension)
# SANITIZE strlen Arg0 D
# SANITIZE strnlen Arg0 D
# SANITIZE strncpy Arg0 D
# SANITIZE strncat Arg0 D
# SANITIZE snprintf Arg0 D