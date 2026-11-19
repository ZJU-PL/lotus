; Register aggregates need field definitions through insertvalue/select.
; The local SVF build models these; Lotus's pointer-only graph drops the
; aggregate definitions, leaving the pointer extract with no incoming flow.
@a = global i8 0
@b = global i8 0

define i32 @main(i32 %argc, i8** %argv) {
  %cond = icmp sgt i32 %argc, 1
  %left = insertvalue {i8*, i8*} undef, i8* @a, 0
  %right = insertvalue {i8*, i8*} undef, i8* @b, 0
  %selected = select i1 %cond, {i8*, i8*} %left, {i8*, i8*} %right
  %p = extractvalue {i8*, i8*} %selected, 0
  %expected = select i1 %cond, i8* @a, i8* @b
  %ok = icmp eq i8* %p, %expected
  %failed = xor i1 %ok, true
  %status = zext i1 %failed to i32
  ret i32 %status
}
