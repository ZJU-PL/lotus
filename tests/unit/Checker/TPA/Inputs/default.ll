declare i32 @system(i8*)

define i32 @main(i32 %argc, i8** %argv) {
  %slot = getelementptr i8*, i8** %argv, i64 1
  %command = load i8*, i8** %slot
  %result = call i32 @system(i8* %command)
  ret i32 0
}
