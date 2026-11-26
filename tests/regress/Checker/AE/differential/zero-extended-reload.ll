define i32 @main(i1 %choice) {
  %slot = alloca i8
  %a = alloca [201 x i32]
  %initial = select i1 %choice, i8 -56, i8 -55
  store i8 %initial, i8* %slot
  %old = load i8, i8* %slot
  %wide = zext i8 %old to i32
  %guard = icmp eq i32 %wide, 200
  br i1 %guard, label %body, label %exit
body:
  %reload = load i8, i8* %slot
  %index = zext i8 %reload to i32
  %ptr = getelementptr [201 x i32], [201 x i32]* %a, i32 0, i32 %index
  store i32 1, i32* %ptr
  ret i32 0
exit:
  ret i32 0
}
