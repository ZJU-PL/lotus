; Query-contract difference: SVF's alias() expands a BaseObjVar to all its
; fields even when the base memory object has not been collapsed to FI.
; Lotus only expands explicitly field-insensitive objects. Both load @b.
@a = global i8 0
@b = global i8 0

define i32 @main() {
  %record = alloca {i8*, i8*}
  %first = getelementptr {i8*, i8*}, {i8*, i8*}* %record, i32 0, i32 0
  %second = getelementptr {i8*, i8*}, {i8*, i8*}* %record, i32 0, i32 1
  store i8* @a, i8** %first
  store i8* @b, i8** %second
  %r = load i8*, i8** %second
  %ok = icmp eq i8* %r, @b
  %failed = xor i1 %ok, true
  %status = zext i1 %failed to i32
  ret i32 %status
}
