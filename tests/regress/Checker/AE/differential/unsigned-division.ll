define i32 @main() {
  %a = alloca [2 x i32]
  %value = udiv i8 10, -2
  %index = zext i8 %value to i32
  %ptr = getelementptr [2 x i32], [2 x i32]* %a, i32 0, i32 %index
  store i32 1, i32* %ptr
  ret i32 0
}
