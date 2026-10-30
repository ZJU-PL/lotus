@old = global i8 0
@new = global i8 0

declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)

define i32 @main() {
  %source = alloca i8*
  %destination = alloca i8*
  store i8* @new, i8** %source
  store i8* @old, i8** %destination
  %src = bitcast i8** %source to i8*
  %dst = bitcast i8** %destination to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* %dst, i8* %src, i64 8, i1 false)
  %r = load i8*, i8** %destination
  %oldaddr = getelementptr i8, i8* @old, i64 0
  %newaddr = getelementptr i8, i8* @new, i64 0
  %correct = icmp eq i8* %r, @new
  %failed = xor i1 %correct, true
  %status = zext i1 %failed to i32
  ret i32 %status
}
