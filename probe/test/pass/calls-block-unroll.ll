; A call can run for any length of time, so a loop with a call (or a memcpy of
; unknown length) keeps one probe point per iteration. A memcpy of constant
; length is cheap and does not block unrolling.
; RUN: %racer-opt -racer-probe-interval=200 -passes=racer-probe -pass-remarks=racer-probe -S %s 2>%t.remarks | FileCheck %s
; RUN: FileCheck --check-prefix=REMARK %s < %t.remarks

declare void @external_work(i64)
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)

define void @with_call(i64 %n) {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  call void @external_work(i64 %i)
  %i.next = add i64 %i, 1
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %exit, label %loop
exit:
  ret void
}
; CHECK-LABEL:   define void @with_call(
; CHECK:           call void @do_yield()
; CHECK-COUNT-1:   call void @external_work
; CHECK-NOT:       call void @external_work

define void @with_variable_memcpy(ptr %dst, ptr %src, i64 %bytes, i64 %n) {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  call void @llvm.memcpy.p0.p0.i64(ptr %dst, ptr %src, i64 %bytes, i1 false)
  %i.next = add i64 %i, 1
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %exit, label %loop
exit:
  ret void
}
; CHECK-LABEL:   define void @with_variable_memcpy(
; CHECK-COUNT-1:   call void @llvm.memcpy
; CHECK-NOT:       call void @llvm.memcpy

define void @with_constant_memcpy(ptr %dst, ptr %src, i64 %n) {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  call void @llvm.memcpy.p0.p0.i64(ptr %dst, ptr %src, i64 16, i1 false)
  %i.next = add i64 %i, 1
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %exit, label %loop
exit:
  ret void
}
; Body: call, add, icmp, br = 4 instructions; 200 / 4 = 50.
; REMARK-NOT: unrolled loop of {{.*}} in with_call
; REMARK:     remark: {{.*}} unrolled loop of 4 IR instructions by 50{{$}}
; REMARK-NOT: unrolled
; CHECK-LABEL:   define void @with_constant_memcpy(
; CHECK-COUNT-50:  call void @llvm.memcpy
