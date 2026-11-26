define i32 @main(i1 %choice) {
  %slot = alloca i32
  %a = alloca [4 x i32]
  %initial = select i1 %choice, i32 0, i32 1
  store i32 %initial, i32* %slot
  %old = load i32, i32* %slot
  store i32 4, i32* %slot
  %guard = icmp eq i32 %old, 0
  br i1 %guard, label %body, label %exit
body:
  %reload = load i32, i32* %slot
  %ptr = getelementptr [4 x i32], [4 x i32]* %a, i32 0, i32 %reload
  store i32 1, i32* %ptr
  ret i32 0
exit:
  ret i32 0
}
