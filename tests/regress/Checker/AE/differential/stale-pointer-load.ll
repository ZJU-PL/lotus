define i32 @main(i1 %choice) {
  %slot = alloca i32*
  %x = alloca i32
  %a = alloca [4 x i32]
  %initial = select i1 %choice, i32* null, i32* %x
  store i32* %initial, i32** %slot
  %old = load i32*, i32** %slot
  store i32* %x, i32** %slot
  %guard = icmp eq i32* %old, null
  br i1 %guard, label %body, label %exit
body:
  %p = getelementptr [4 x i32], [4 x i32]* %a, i32 0, i32 4
  store i32 1, i32* %p
  ret i32 0
exit:
  ret i32 0
}
