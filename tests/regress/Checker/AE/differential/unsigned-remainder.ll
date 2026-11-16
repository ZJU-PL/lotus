define i32 @main() {
  %a = alloca [4 x i32]
  %value = urem i8 10, -2
  %index = zext i8 %value to i32
  %ptr = getelementptr [4 x i32], [4 x i32]* %a, i32 0, i32 %index
  store i32 1, i32* %ptr
  ret i32 0
}
