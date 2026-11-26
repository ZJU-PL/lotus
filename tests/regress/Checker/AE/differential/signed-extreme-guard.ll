define i32 @main(i64 %n) {
  %a = alloca [4 x i32]
  %impossible = icmp sgt i64 %n, 9223372036854775807
  br i1 %impossible, label %body, label %exit
body:
  %p = getelementptr [4 x i32], [4 x i32]* %a, i32 0, i32 4
  store i32 1, i32* %p
  ret i32 0
exit:
  ret i32 0
}
