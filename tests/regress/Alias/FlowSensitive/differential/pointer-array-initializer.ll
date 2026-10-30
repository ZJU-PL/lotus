@a = global i8 0
@b = global i8 0
@array = global [2 x i8*] [i8* @a, i8* @b]

define i32 @main() {
  %first = getelementptr [2 x i8*], [2 x i8*]* @array, i32 0, i32 0
  %r = load i8*, i8** %first
  %aaddr = getelementptr i8, i8* @a, i64 0
  %baddr = getelementptr i8, i8* @b, i64 0
  %correct = icmp eq i8* %r, @a
  %failed = xor i1 %correct, true
  %status = zext i1 %failed to i32
  ret i32 %status
}
