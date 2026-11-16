define i32 @main(i1 %choice) {
  %left = alloca i32
  %right = alloca i32
  %a = alloca [4 x i32]
  store i32 0, i32* %left
  store i32 4, i32* %right
  %selected = select i1 %choice, i32* %left, i32* %right
  %value = load i32, i32* %selected
  %guard = icmp eq i32 %value, 0
  br i1 %guard, label %body, label %exit
body:
  %reload = load i32, i32* %right
  %ptr = getelementptr [4 x i32], [4 x i32]* %a, i32 0, i32 %reload
  store i32 1, i32* %ptr
  ret i32 0
exit:
  ret i32 0
}
