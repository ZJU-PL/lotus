define i32 @main() {
  %a = alloca [4 x i32]
  %v = insertelement <2 x i32> zeroinitializer, i32 4, i32 0
  %index = extractelement <2 x i32> %v, i32 0
  %p = getelementptr [4 x i32], [4 x i32]* %a, i32 0, i32 %index
  store i32 1, i32* %p
  ret i32 0
}
