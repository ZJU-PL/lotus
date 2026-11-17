; k=0 merges the two calls; k=1 keeps the clean call distinct.
declare i32 @source()
declare void @sink(i32)

define i32 @identity(i32 %x) {
  ret i32 %x
}

define i32 @main() {
  %x = call i32 @source()
  %dirty = call i32 @identity(i32 %x)
  %clean = call i32 @identity(i32 0)
  call void @sink(i32 %dirty)
  call void @sink(i32 %clean)
  ret i32 0
}
