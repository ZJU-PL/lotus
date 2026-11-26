define i32 @main() {
  %slot = alloca i32
  %a = alloca [4 x i32]
  store i32 0, i32* %slot
  %old = atomicrmw xchg i32* %slot, i32 4 seq_cst
  %index = load i32, i32* %slot
  %p = getelementptr [4 x i32], [4 x i32]* %a, i32 0, i32 %index
  store i32 1, i32* %p
  ret i32 0
}
