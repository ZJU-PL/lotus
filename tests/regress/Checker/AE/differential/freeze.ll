; LLVM 14 supports freeze in IR; Clang 14 has no __builtin_freeze C builtin.
define i32 @main() {
entry:
  %a = alloca [4 x i32], align 4
  %i = freeze i64 3
  %p = getelementptr [4 x i32], [4 x i32]* %a, i64 0, i64 %i
  store i32 0, i32* %p
  ret i32 0
}
