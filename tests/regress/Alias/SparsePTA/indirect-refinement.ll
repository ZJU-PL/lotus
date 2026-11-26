@x = global i8 0
@y = global i8 0
@fp = global i8* (i8*)* @old

declare void @__aser_alias__(i8*, i8*)
declare void @__aser_no_alias__(i8*, i8*)

define i8* @old(i8* %a) {
  ret i8* @y
}

define i8* @new(i8* %a) {
  ret i8* %a
}

define i8* @main() {
  store i8* (i8*)* @new, i8* (i8*)** @fp
  %f = load i8* (i8*)*, i8* (i8*)** @fp
  %r = call i8* %f(i8* @x)
  call void @__aser_alias__(i8* %r, i8* @x)
  call void @__aser_no_alias__(i8* %r, i8* @y)
  ret i8* %r
}
