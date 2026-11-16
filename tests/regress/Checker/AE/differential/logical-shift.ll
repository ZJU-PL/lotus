define i32 @main() {
  %a = alloca [128 x i32]
  %value = lshr i8 -1, 1
  %index = zext i8 %value to i32
  %ptr = getelementptr [128 x i32], [128 x i32]* %a, i32 0, i32 %index
  store i32 1, i32* %ptr
  ret i32 0
}
