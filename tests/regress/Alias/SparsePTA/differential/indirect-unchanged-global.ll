@old = global i8 0
@new = global i8 0
@slot = global i8* @old
@fp = global void ()* @write

define void @write() {
  store i8* @new, i8** @slot
  ret void
}

define void @noop() {
  ret void
}

define i32 @main() {
  store void ()* @noop, void ()** @fp
  %f = load void ()*, void ()** @fp
  call void %f()
  %r = load i8*, i8** @slot
  %oldaddr = getelementptr i8, i8* @old, i64 0
  %newaddr = getelementptr i8, i8* @new, i64 0
  %correct = icmp eq i8* %r, @old
  %failed = xor i1 %correct, true
  %status = zext i1 %failed to i32
  ret i32 %status
}
