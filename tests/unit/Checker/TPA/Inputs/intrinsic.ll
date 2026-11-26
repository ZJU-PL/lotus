declare void @llvm.memcpy.p0i8.p0i8.i64(i8* nocapture writeonly, i8* nocapture readonly, i64, i1 immarg)
declare i32 @system(i8*)

define i32 @main(i32 %argc) {
  %src = alloca i8
  %dst = alloca i8
  %input = trunc i32 %argc to i8
  store i8 %input, i8* %src
  store i8 0, i8* %dst
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* %dst, i8* %src, i64 1, i1 false)
  %result = call i32 @system(i8* %dst)
  ret i32 0
}
