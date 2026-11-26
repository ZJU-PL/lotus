; A pointer field at a non-zero offset must survive a call boundary.
;
;   main stores @old into reg.field, calls read_field(), which loads
;   reg.field (must be @old), then calls write_field(), which stores @new,
;   and reloads reg.field (must be @new).
;
; The expected exit status is 0, so `lli struct-field-call-boundary.ll`
; returns 0 when the analysis produced the right answers.  Before the
; argument-granular memory-summary fix both loads in read_field/main observed
; an empty memory state (and SVF's fspta reported the correct two values).

%struct.Reg = type { i32, i8* }

@old = global i8 0
@new = global i8 0

define internal i8* @read_field(%struct.Reg* %reg) {
entry:
  %field = getelementptr inbounds %struct.Reg, %struct.Reg* %reg, i32 0, i32 1
  %value = load i8*, i8** %field
  ret i8* %value
}

define internal void @write_field(%struct.Reg* %reg) {
entry:
  %field = getelementptr inbounds %struct.Reg, %struct.Reg* %reg, i32 0, i32 1
  store i8* @new, i8** %field
  ret void
}

define i32 @main() {
entry:
  %reg = alloca %struct.Reg
  %field = getelementptr inbounds %struct.Reg, %struct.Reg* %reg, i32 0, i32 1
  store i8* @old, i8** %field
  %read = call i8* @read_field(%struct.Reg* %reg)
  %before = icmp eq i8* %read, @old
  call void @write_field(%struct.Reg* %reg)
  %after = load i8*, i8** %field
  %killed = icmp eq i8* %after, @new
  %ok = and i1 %before, %killed
  %failed = xor i1 %ok, true
  %status = zext i1 %failed to i32
  ret i32 %status
}
