; Model gap: SVF's extapi models posix_memalign's allocation through arg0.
; Lotus models pointer returns but does not materialize this pointer write.
; The program returns zero on allocation failure or successful observation.
declare i32 @posix_memalign(i8**, i64, i64)
declare void @free(i8*)

define i32 @main() {
entry:
  %slot = alloca i8*
  store i8* null, i8** %slot
  %rc = call i32 @posix_memalign(i8** %slot, i64 8, i64 8)
  %success = icmp eq i32 %rc, 0
  br i1 %success, label %allocated, label %exit
allocated:
  %p = load i8*, i8** %slot
  %q = load i8*, i8** %slot
  %nonnull = icmp ne i8* %p, null
  %failed = xor i1 %nonnull, true
  %status = zext i1 %failed to i32
  call void @free(i8* %p)
  ret i32 %status
exit:
  ret i32 0
}
