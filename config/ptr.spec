# File system syscalls
fopen ALLOC
fopen64 ALLOC
fdopen ALLOC
tmpfile ALLOC
tmpfile64 ALLOC
getcwd COPY Ret V Arg0 V
tmpnam COPY Ret V Arg0 V
freopen COPY Ret V Arg2 V
freopen64 COPY Ret V Arg2 V
get_current_dir_name ALLOC
getpwuid COPY Ret V STATIC V
getpwnam COPY Ret V STATIC V
getpwent COPY Ret V STATIC V
readdir COPY Ret V STATIC V
getgrgid COPY Ret V STATIC V
opendir ALLOC
fopendir ALLOC
setpwent IGNORE
endpwent IGNORE
fstat IGNORE
lstat IGNORE
chdir IGNORE
fchdir IGNORE
chmod IGNORE
chown IGNORE
chroot IGNORE
close IGNORE
closedir IGNORE
creat IGNORE
dup IGNORE
dup2 IGNORE
fchmod IGNORE
fchown IGNORE
fcntl IGNORE
fdatasync IGNORE
fgetpos IGNORE
flockfile IGNORE
fnmatch IGNORE
fsetpos IGNORE
fsync IGNORE
ftello IGNORE
ftruncate IGNORE
ftrylockfile IGNORE
funlockfile IGNORE
getegid IGNORE
geteuid IGNORE
getgid IGNORE
getgroups IGNORE
getpid IGNORE
getppid IGNORE
getresgid IGNORE
getresuid IGNORE
getsid IGNORE
getuid IGNORE
lchown IGNORE
link IGNORE
lseek IGNORE
mkdir IGNORE
mkfifo IGNORE
mknod IGNORE
mkostemp IGNORE
mkostemps IGNORE
mkstemp IGNORE
mkstemps IGNORE
poll IGNORE
readlink IGNORE
rmdir IGNORE
select IGNORE
setbuf IGNORE
setbuffer IGNORE
setegid IGNORE
seteuid IGNORE
setgid IGNORE
setgroups IGNORE
setlinebuf IGNORE
setsid IGNORE
setuid IGNORE
symlink IGNORE
truncate IGNORE
umask IGNORE
unlink IGNORE
getwd IGNORE
isatty IGNORE
# Process syscalls
popen ALLOC
signal COPY Ret V Arg1 V
__sysv_signal COPY Ret V Arg1 V
getpass COPY Ret V STATIC V
getpgid IGNORE
setpgid IGNORE
getpgrp IGNORE
setpgrp IGNORE
alarm IGNORE
execle IGNORE
execv IGNORE
execve IGNORE
execvp IGNORE
fork IGNORE
kill IGNORE
nice IGNORE
pause IGNORE
perror IGNORE
pipe IGNORE
sleep IGNORE
wait IGNORE
waitpid IGNORE
usleep IGNORE
sigemptyset IGNORE
sigfillset IGNORE
sigaddset IGNORE
sigprocmask IGNORE
sigaction IGNORE
setitimer IGNORE
# Network syscalls
gethostname IGNORE
getpeername IGNORE
getsockname IGNORE
getsockopt IGNORE
htonl IGNORE
htons IGNORE
inet_aton IGNORE
inet_network IGNORE
listen IGNORE
ntohl IGNORE
ntohs IGNORE
recvfrom IGNORE
recvmsg IGNORE
sendmsg IGNORE
sendto IGNORE
sethostname IGNORE
setsockopt IGNORE
socket IGNORE
socketpair IGNORE
# Time
ctime COPY Ret V STATIC V
strptime COPY Ret V Arg0 V
gettimeofday IGNORE
clock IGNORE
difftime IGNORE
timegm IGNORE
timelocal IGNORE
utime IGNORE
utimes IGNORE
tzset IGNORE
# I/O
# fgets copies data from the return value to the first argument.
fgets COPY Ret V Arg0 V
fgetws COPY Ret V Arg0 V
gets COPY Ret V Arg0 V
asprintf ALLOC
getchar IGNORE
putchar IGNORE
vfprintf IGNORE
vfwprintf IGNORE
vprintf IGNORE
vsnprintf IGNORE
vsprintf IGNORE
vwprintf IGNORE
# Char
isalnum IGNORE
isalpha IGNORE
isascii IGNORE
isblank IGNORE
iscntrl IGNORE
isdigit IGNORE
isgraph IGNORE
islower IGNORE
isprint IGNORE
ispunct IGNORE
isspace IGNORE
isupper IGNORE
iswalnum IGNORE
iswalpha IGNORE
iswctype IGNORE
iswdigit IGNORE
iswlower IGNORE
iswprint IGNORE
iswspace IGNORE
iswupper IGNORE
isxdigit IGNORE
iswxdigit IGNORE
mblen IGNORE
mbrlen IGNORE
mbrtowc IGNORE
mbtowc IGNORE
tolower IGNORE
toupper IGNORE
towlower IGNORE
towupper IGNORE
# String
strdup ALLOC
strndup ALLOC
strerror ALLOC
strcat COPY Ret V Arg0 V
wcscat COPY Ret V Arg0 V
strchr COPY Ret V Arg0 V
wcschr COPY Ret V Arg0 V
strcpy COPY Ret V Arg0 V
wcscpy COPY Ret V Arg0 V
strncat COPY Ret V Arg0 V
wcsncat COPY Ret V Arg0 V
strncpy COPY Ret V Arg0 V
strrchr COPY Ret V Arg0 V
strstr COPY Ret V Arg0 V
strtod COPY Arg1 D Arg0 V
strtof COPY Arg1 D Arg0 V
strtol COPY Arg1 D Arg0 V
strtoul COPY Arg1 D Arg0 V
strtok COPY Ret V STATIC V
strpbrk COPY Ret V NULL V
strpbrk COPY Ret V Arg0 V
index COPY Ret V Arg0 V
rindex COPY Ret V Arg0 V
# Math functions
abs IGNORE
acos IGNORE
asin IGNORE
atan IGNORE
atan2 IGNORE
ceil IGNORE
cos IGNORE
cosf IGNORE
exp IGNORE
exp10 IGNORE
exp2 IGNORE
fabs IGNORE
fabsf IGNORE
floor IGNORE
floorf IGNORE
hypot IGNORE
ldexp IGNORE
ldexpf IGNORE
ldexpl IGNORE
log IGNORE
log10 IGNORE
lrand48 IGNORE
modf IGNORE
fmod IGNORE
fmodf IGNORE
pow IGNORE
seed48 IGNORE
sin IGNORE
sinf IGNORE
sinh IGNORE
cosh IGNORE
tanh IGNORE
sqrt IGNORE
sqrtf IGNORE
tan IGNORE
# Conversions
btowc IGNORE
frexpf IGNORE
frexpl IGNORE
# Miscellaneous
setlocale COPY Ret V STATIC V
localeconv COPY Ret V STATIC V
getenv COPY Ret V STATIC V
__ctype_b_loc COPY Ret V STATIC V
localtime COPY Ret V STATIC V
gmtime COPY Ret V STATIC V
__assert_fail EXIT
_exit EXIT
abort EXIT
longjmp EXIT
putenv IGNORE
atexit IGNORE
closelog IGNORE
exit EXIT
__cxa_pure_virtual EXIT
openlog IGNORE
rand IGNORE
random IGNORE
srand IGNORE
srandom IGNORE
sysconf IGNORE
getopt IGNORE
# Memory management
__errno_location COPY Ret V STATIC V
malloc ALLOC Arg0
calloc ALLOC Arg1
calloc COPY Ret R NULL V
valloc ALLOC Arg0
posix_memalign ALLOC Arg2 OUT Arg0
realloc ALLOC Arg1
realloc COPY Ret V Arg0 V
memalign ALLOC Arg1
_Znwj ALLOC Arg0
_ZnwjRKSt9nothrow_t ALLOC Arg0
_Znwm ALLOC Arg0
_ZnwmRKSt9nothrow_t ALLOC Arg0
_Znaj ALLOC Arg0
_ZnajRKSt9nothrow_t ALLOC Arg0
_Znam ALLOC Arg0
_ZnamRKSt9nothrow_t ALLOC Arg0
memchr COPY Ret V Arg0 V
bcopy COPY Arg0 R Arg1 R
bcopy COPY Ret V Arg0 V
memccpy COPY Arg0 R Arg1 R
memccpy COPY Ret V Arg0 V
memcpy COPY Arg0 R Arg1 R
memcpy COPY Ret V Arg0 V
memmove COPY Arg0 R Arg1 R
memmove COPY Ret V Arg0 V
wmemcpy COPY Arg0 R Arg1 R
wmemcpy COPY Ret V Arg0 V
memset COPY Arg0 R NULL V
memset COPY Ret V Arg0 V

free DEALLOC
cfree DEALLOC
_ZdlPv DEALLOC
_ZdaPv DEALLOC
_ZdlPvj DEALLOC
_ZdlPvm DEALLOC
_ZdlPvRKSt9nothrow_t DEALLOC
_ZdaPvj DEALLOC
_ZdaPvm DEALLOC
_ZdaPvRKSt9nothrow_t DEALLOC
mprotect IGNORE
munmap IGNORE
# LLVM intrinsics
llvm.memcpy.p0i8.p0i8.i32 COPY Arg0 R Arg1 R
llvm.memcpy.p0i8.p0i8.i32 COPY Ret V Arg0 V
llvm.memcpy.p0i8.p0i8.i64 COPY Arg0 R Arg1 R
llvm.memcpy.p0i8.p0i8.i64 COPY Ret V Arg0 V
llvm.memmove.p0i8.p0i8.i32 COPY Arg0 R Arg1 R
llvm.memmove.p0i8.p0i8.i32 COPY Ret V Arg0 V
llvm.memmove.p0i8.p0i8.i64 COPY Arg0 R Arg1 R
llvm.memmove.p0i8.p0i8.i64 COPY Ret V Arg0 V
llvm.memset.p0i8.i64 COPY Arg0 R NULL V
llvm.memset.p0i8.i64 COPY Ret V Arg0 V
llvm.memset.p0i8.i32 COPY Arg0 R NULL V
llvm.memset.p0i8.i32 COPY Ret V Arg0 V
llvm.bswap.i16 IGNORE
llvm.bswap.i32 IGNORE
llvm.ctlz.i64 IGNORE
llvm.dbg.declare IGNORE
llvm.dbg.value IGNORE
llvm.lifetime.end IGNORE
llvm.lifetime.start IGNORE
llvm.stackrestore IGNORE
llvm.va_start IGNORE
llvm.va_end IGNORE
llvm.va_copy IGNORE
llvm.trap IGNORE
llvm.umul.with.overflow.i64 IGNORE
# C++ placement new operators (these don't allocate memory)
_ZnwmPv IGNORE
_ZnamPv IGNORE
_ZnwjPv IGNORE
_ZnajPv IGNORE
# C++ Standard Library - Smart Pointers and Containers (TBD)
_ZnwmSt11align_val_t ALLOC Arg0
_ZnamSt11align_val_t ALLOC Arg0
_ZnwmSt11align_val_tRKSt9nothrow_t ALLOC Arg0
_ZnamSt11align_val_tRKSt9nothrow_t ALLOC Arg0
_ZnwjSt11align_val_t ALLOC Arg0
_ZnajSt11align_val_t ALLOC Arg0
_ZnwjSt11align_val_tRKSt9nothrow_t ALLOC Arg0
_ZnajSt11align_val_tRKSt9nothrow_t ALLOC Arg0

# Additional allocation functions
reallocf ALLOC Arg1
reallocf COPY Ret V Arg0 V
aligned_alloc ALLOC Arg1
pvalloc ALLOC Arg0
kmalloc ALLOC Arg0
mmap ALLOC
mmap64 ALLOC
getline ALLOC
getwline ALLOC
getdelim ALLOC
getwdelim ALLOC
