; Positive, negative and unknown cases for the semantic Cypher predicates.
; No debug metadata is needed: assertions use function identity, not lines.
target datalayout = "e-p:64:64-i64:64"
@two = private constant [5 x i8] c"%d%d\00"
@scan = private constant [3 x i8] c"%s\00"
@bounded = private constant [4 x i8] c"%3s\00"
@name = private constant [2 x i8] c"X\00"
@clean = private constant [3 x i8] c"ok\00"
declare i8* @strncpy(i8*, i8*, i64)
declare i32 @printf(i8*, ...)
declare i32 @scanf(i8*, ...)
declare i8* @getenv(i8*)
declare i32 @system(i8*)
declare i8* @malloc(i64)
declare void @free(i8*)

define void @bad_bounds(i8* %source) {
  %buffer = alloca [4 x i8]
  %p = getelementptr [4 x i8], [4 x i8]* %buffer, i64 0, i64 0
  %copy = call i8* @strncpy(i8* %p, i8* %source, i64 5)
  ret void
}
define void @good_bounds(i8* %source) {
  %buffer = alloca [4 x i8]
  %p = getelementptr [4 x i8], [4 x i8]* %buffer, i64 0, i64 0
  %copy = call i8* @strncpy(i8* %p, i8* %source, i64 4)
  ret void
}
define void @unknown_bounds(i8* %destination, i8* %source, i64 %size) {
  %copy = call i8* @strncpy(i8* %destination, i8* %source, i64 %size)
  ret void
}
define void @bad_format() {
  %p = getelementptr [5 x i8], [5 x i8]* @two, i64 0, i64 0
  %call = call i32 (i8*, ...) @printf(i8* %p, i32 1)
  ret void
}
define void @extra_format() {
  %p = getelementptr [5 x i8], [5 x i8]* @two, i64 0, i64 0
  %call = call i32 (i8*, ...) @printf(i8* %p, i32 1, i32 2, i32 3)
  ret void
}
define void @good_format() {
  %p = getelementptr [5 x i8], [5 x i8]* @two, i64 0, i64 0
  %call = call i32 (i8*, ...) @printf(i8* %p, i32 1, i32 2)
  ret void
}
define void @scan_formats() {
  %buffer = alloca [4 x i8]
  %destination = getelementptr [4 x i8], [4 x i8]* %buffer, i64 0, i64 0
  %unbounded = getelementptr [3 x i8], [3 x i8]* @scan, i64 0, i64 0
  %bounded = getelementptr [4 x i8], [4 x i8]* @bounded, i64 0, i64 0
  %bad = call i32 (i8*, ...) @scanf(i8* %unbounded, i8* %destination)
  %good = call i32 (i8*, ...) @scanf(i8* %bounded, i8* %destination)
  ret void
}
define i32 @bad_null() {
  %value = load i32, i32* null
  ret i32 %value
}
define i32 @bad_initialization() {
  %slot = alloca i32
  %value = load i32, i32* %slot
  ret i32 %value
}
define i32 @good_initialization() {
  %slot = alloca i32
  store i32 1, i32* %slot
  %value = load i32, i32* %slot
  ret i32 %value
}
define i32 @bad_taint() {
  %name = getelementptr [2 x i8], [2 x i8]* @name, i64 0, i64 0
  %input = call i8* @getenv(i8* %name)
  %bad = call i32 @system(i8* %input)
  ret i32 %bad
}
define i32 @good_taint() {
  %literal = getelementptr [3 x i8], [3 x i8]* @clean, i64 0, i64 0
  %good = call i32 @system(i8* %literal)
  ret i32 %good
}
define i8 @bad_lifetime() {
  %p = call i8* @malloc(i64 8)
  call void @free(i8* %p)
  %bad = load i8, i8* %p
  call void @free(i8* %p)
  ret i8 %bad
}
define void @good_lifetime() {
  %p = call i8* @malloc(i64 8)
  store i8 0, i8* %p
  call void @free(i8* %p)
  ret void
}
