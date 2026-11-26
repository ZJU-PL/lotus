define i32 @main() {
entry:
  %a = alloca [4 x i32]
  br i1 false, label %body, label %body
body:
  %index = phi i32 [4, %entry], [4, %entry]
  %ptr = getelementptr [4 x i32], [4 x i32]* %a, i32 0, i32 %index
  store i32 1, i32* %ptr
  ret i32 0
}
