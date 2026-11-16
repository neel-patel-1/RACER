; Functions annotated xmp_skip, the do_yield definition and naked functions
; get no probe points; the other function still does.
; RUN: %racer-opt -racer-probe-interval=0 -passes=racer-probe -S %s | FileCheck %s

@preempt_signal = thread_local global ptr null
@.str = private unnamed_addr constant [9 x i8] c"xmp_skip\00", section "llvm.metadata"
@.file = private unnamed_addr constant [7 x i8] c"test.c\00", section "llvm.metadata"
@llvm.global.annotations = appending global [1 x { ptr, ptr, ptr, i32, ptr }] [{ ptr, ptr, ptr, i32, ptr } { ptr @skipped, ptr @.str, ptr @.file, i32 1, ptr null }], section "llvm.metadata"

; The module's own definition of the signal is used, not a new declaration.
; CHECK:     @preempt_signal = thread_local global ptr null
; CHECK-NOT: @preempt_signal{{.*}} = external

define void @skipped(i64 %n) {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %i.next = add i64 %i, 1
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %exit, label %loop
exit:
  ret void
}
; CHECK-LABEL: define void @skipped(
; CHECK-NOT:   racer.
; CHECK:       ret void

define void @do_yield() {
entry:
  br label %loop
loop:
  br i1 true, label %exit, label %loop
exit:
  ret void
}
; CHECK-LABEL: define void @do_yield(
; CHECK-NOT:   racer.
; CHECK:       ret void

define void @naked_loop() naked {
entry:
  br label %loop
loop:
  br i1 true, label %exit, label %loop
exit:
  unreachable
}
; CHECK-LABEL: define void @naked_loop(
; CHECK-NOT:   racer.
; CHECK:       unreachable

define void @probed(i64 %n) {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %i.next = add i64 %i, 1
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %exit, label %loop
exit:
  ret void
}
; CHECK-LABEL: define void @probed(
; CHECK:       load volatile i8, ptr %racer.signal
; CHECK:       call void @do_yield()
