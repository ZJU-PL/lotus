define i32 @main() {
  %a = alloca [4 x i32]
  %bits = add i8 127, 1
  %same = icmp uge i8 %bits, -128
  br i1 %same, label %body, label %exit
body:
  %p = getelementptr [4 x i32], [4 x i32]* %a, i32 0, i32 4
  store i32 1, i32* %p
  ret i32 0
exit:
  ret i32 0
}
