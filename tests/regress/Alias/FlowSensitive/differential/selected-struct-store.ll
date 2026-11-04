@a = global i8 0
@b = global i8 0
@record = global {i8*, i8*} {i8* @a, i8* @b}

define i32 @main(i32 %argc, i8** %argv) {
  %condition = icmp sgt i32 %argc, 1
  %first = getelementptr {i8*, i8*}, {i8*, i8*}* @record, i32 0, i32 0
  %second = getelementptr {i8*, i8*}, {i8*, i8*}* @record, i32 0, i32 1
  %destination = select i1 %condition, i8** %first, i8** %second
  store i8* @a, i8** %destination
  %r = load i8*, i8** %second
  %aaddr = getelementptr i8, i8* @a, i64 0
  %baddr = getelementptr i8, i8* @b, i64 0
  %expected = select i1 %condition, i8* @b, i8* @a
  %correct = icmp eq i8* %r, %expected
  %failed = xor i1 %correct, true
  %status = zext i1 %failed to i32
  ret i32 %status
}
