# File system syscalls
fopen MOD Ret D
fopen REF Arg0 D
fopen REF Arg1 D
fdopen MOD Ret D
fdopen REF Arg1 D
freopen REF AfterArg0 D
fclose REF Arg0 D
fread MOD Arg0 R
fread REF Arg3 D
feof REF Arg0 D
fileno REF Arg0 D
clearerr REF Arg0 D
ferror REF Arg0 D
fflush REF Arg0 D
fseek REF Arg0 D
fseeko REF Arg0 D
ftell REF Arg0 D
rewind REF Arg0 D
fwrite REF Arg0 D
fwrite REF Arg3 D
access REF Arg0 D
open REF Arg0 D
read MOD Arg1 R
write REF Arg1 R
remove REF Arg0 D
stat REF Arg0 D
stat MOD Arg1 R
getcwd MOD Arg0 D
getpwnam REF Arg0 D
rename REF Arg0 D
rename REF Arg1 D
setvbuf REF Arg0 D
setvbuf REF Arg1 D

getegid IGNORE
geteuid IGNORE
getgid IGNORE
getpgid IGNORE
getpid IGNORE
getppid IGNORE
getsid IGNORE
getuid IGNORE
getresgid IGNORE
getresuid IGNORE
close IGNORE
# Process syscalls
popen MOD Ret D
popen REF Arg0 D
popen REF Arg1 D
pclose REF Arg0 D
execl REF AfterArg0 D
execlp REF AfterArg0 D
alarm IGNORE
# Network syscalls
accept MOD Arg1 R
accept MOD Arg2 D
accept REF Arg2 D
send REF Arg1 R
recv MOD Arg1 R
inet_addr REF Arg0 D
connect REF Arg1 R
bind REF Arg1 R
htons IGNORE
htonl IGNORE
ntohs IGNORE
ntohl IGNORE
socket IGNORE
listen IGNORE
# Time
clock IGNORE
difftime IGNORE
time MOD Arg0 R
times MOD Arg0 R
clock_gettime MOD Arg1 R
localtime REF Arg0 R
gmtime REF Arg0 R

# I/O
__isoc99_fscanf MOD AfterArg2 D
__isoc99_fscanf REF Arg0 D
__isoc99_fscanf REF Arg1 D
fscanf MOD AfterArg2 D
fscanf REF Arg0 D
fscanf REF Arg1 D
__isoc99_scanf MOD AfterArg1 D
__isoc99_scanf REF Arg1 D
scanf MOD AfterArg1 D
scanf REF Arg0 D
__isoc99_sscanf MOD AfterArg2 D
__isoc99_sscanf REF Arg0 D
__isoc99_sscanf REF Arg1 D
sscanf MOD AfterArg2 D
__isoc99_swscanf MOD AfterArg2 D
__isoc99_swscanf REF Arg0 D
__isoc99_swscanf REF Arg1 D
sprintf MOD Arg0 D
sprintf REF AfterArg1 D
strftime REF Arg2 D
strftime REF Arg3 R
strftime MOD Arg0 D
vsprintf MOD Arg0 D
vsprintf REF AfterArg1 D
vsnprintf MOD Arg0 D
vsnprintf REF AfterArg2 D
snprintf MOD Arg0 D
snprintf REF AfterArg2 D
fgetc REF Arg0 D
fgets MOD Arg0 D
fgets REF Arg2 D
fgetws MOD Arg0 D
fgetws REF Arg2 D
gets MOD Arg0 D
fputc REF Arg1 D
fputs REF Arg0 D
fputs REF Arg1 D
getc REF Arg0 D
ungetc REF Arg1 D
_IO_getc REF Arg0 D
putc REF Arg1 D
_IO_putc REF Arg1 D
puts REF Arg0 D
printf REF Arg0 D
wprintf REF Arg0 D
fprintf REF Arg0 D
fprintf REF Arg1 D
fwprintf REF Arg0 D
fwprintf REF Arg1 D
getchar IGNORE
putchar IGNORE
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
tolower IGNORE
toupper IGNORE
towlower IGNORE
towupper IGNORE
# String
strdup MOD Ret D
strndup MOD Ret D
strerror MOD Ret D
strcat MOD Arg0 D
strcat REF Arg1 D
wcscat MOD Arg0 D
wcscat REF Arg1 D
strncat MOD Arg0 D
strncat REF Arg1 D
wcsncat MOD Arg0 D
wcsncat REF Arg1 D
strcpy MOD Arg0 D
strcpy REF Arg1 D
wcscpy MOD Arg0 D
wcscpy REF Arg1 D
strncpy MOD Arg0 D
strncpy REF Arg1 D
strtod MOD Arg1 D
strtod REF Arg0 D
strtof MOD Arg1 D
strtof REF Arg0 D
strtol MOD Arg1 D
strtol REF Arg0 D
strtold MOD Arg1 D
strtold REF Arg0 D
strtoll MOD Arg1 D
strtoll REF Arg0 D
strtoul MOD Arg1 D
strtoul REF Arg0 D
strcmp REF Arg0 D
strcmp REF Arg1 D
strcoll REF Arg0 D
strcoll REF Arg1 D
strcasecmp REF Arg0 D
strcasecmp REF Arg1 D
strncmp REF Arg0 D
strncmp REF Arg1 D
strncasecmp REF Arg0 D
strncasecmp REF Arg1 D
strlen REF Arg0 D
wcslen REF Arg0 D
strchr REF Arg0 D
strrchr REF Arg0 D
wcschr REF Arg0 D
strstr REF Arg0 D
strstr REF Arg1 D
strspn REF Arg0 D
strspn REF Arg1 D
strcspn REF Arg0 D
strcspn REF Arg1 D
strpbrk REF Arg0 D
strpbrk REF Arg1 D

# Math functions
frexp MOD Arg1 D
abs IGNORE
acos IGNORE
asin IGNORE
atan IGNORE
atan2 IGNORE
ceil IGNORE
cos IGNORE
cosf IGNORE
cosh IGNORE
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
sqrt IGNORE
sqrtf IGNORE
tan IGNORE
tanh IGNORE
# Conversions
atof REF Arg0 D
atoi REF Arg0 D
atol REF Arg0 D
btowc IGNORE
# Miscellaneous
mktime REF Arg0 R
mktime MOD Arg0 R
setlocale REF Arg1 D
setlocale MOD Ret D
localeconv MOD Ret R
qsort MOD Arg0 R
syslog REF AfterArg1 D
getenv REF Arg0 D
system REF Arg0 D
erand48 REF Arg0 R
getrusage MOD Arg1 R
tmpnam REF Arg0 D
tmpnam MOD Arg0 D
_setjmp MOD Arg0 R
longjmp REF Arg0 R
atexit IGNORE
srand IGNORE
srandom IGNORE
sysconf IGNORE
rand IGNORE
random IGNORE
# Memory management
malloc MOD Ret D
calloc MOD Ret D
valloc MOD Ret D
memalign MOD Ret D
realloc MOD Ret D
realloc REF Arg0 D
_Znwj MOD Ret D
_ZnwjRKSt9nothrow_t MOD Ret D
_Znwm MOD Ret D
_ZnwmRKSt9nothrow_t MOD Ret D
_Znaj MOD Ret D
_ZnajRKSt9nothrow_t MOD Ret D
_Znam MOD Ret D
_ZnamRKSt9nothrow_t MOD Ret D
bcopy MOD Arg0 R
bcopy REF Arg1 R
memccpy MOD Arg0 R
memccpy REF Arg1 R
memcpy MOD Arg0 R
memcpy REF Arg1 R
memmove MOD Arg0 R
memmove REF Arg1 R
wmemcpy MOD Arg0 R
wmemcpy REF Arg1 R
memset MOD Arg0 R
memcmp REF Arg0 R
memcmp REF Arg1 R
free REF Arg0 D
_ZdlPv REF Arg0 D
_ZdaPv REF Arg0 D
memchr REF Arg0 D

# LLVM intrinsics
llvm.memcpy.p0i8.p0i8.i32 MOD Arg0 R
llvm.memcpy.p0i8.p0i8.i32 REF Arg1 R
llvm.memcpy.p0i8.p0i8.i64 MOD Arg0 R
llvm.memcpy.p0i8.p0i8.i64 REF Arg1 R
llvm.memmove.p0i8.p0i8.i32 MOD Arg0 R
llvm.memmove.p0i8.p0i8.i32 REF Arg1 R
llvm.memmove.p0i8.p0i8.i64 MOD Arg0 R
llvm.memmove.p0i8.p0i8.i64 REF Arg1 R
llvm.memset.p0i8.i64 MOD Arg0 R
llvm.memset.p0i8.i32 MOD Arg0 R
llvm.bswap.i16 IGNORE
llvm.bswap.i32 IGNORE
llvm.ctlz.i64 IGNORE
llvm.dbg.declare IGNORE
llvm.dbg.value IGNORE
llvm.lifetime.end IGNORE
llvm.lifetime.start IGNORE
llvm.stackrestore IGNORE
llvm.trap IGNORE
llvm.umul.with.overflow.i64 IGNORE