define i32 @main(i32 %argc, i8** %argv) {
  %first = getelementptr i8*, i8** %argv, i64 0
  %r = load i8*, i8** %first
  %failed = icmp eq i8* %r, null
  %status = zext i1 %failed to i32
  ret i32 %status
}
