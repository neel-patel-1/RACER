; With interval 0 each loop header gets one probe point and nothing unrolls.
; RUN: %racer-opt -racer-probe-interval=0 -passes=racer-probe -S %s | FileCheck %s

; CHECK: @preempt_signal = external thread_local(initialexec) global ptr

define void @fill(ptr %p, i64 %n) {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %addr = getelementptr i32, ptr %p, i64 %i
  store i32 0, ptr %addr
  %i.next = add i64 %i, 1
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %exit, label %loop
exit:
  ret void
}
; CHECK-LABEL: define void @fill(
; CHECK:      loop:
; CHECK-NEXT:   %i = phi i64
; CHECK-NEXT:   [[SLOT:%.*]] = call ptr @llvm.threadlocal.address.p0(ptr @preempt_signal)
; CHECK-NEXT:   %racer.signal = load ptr, ptr [[SLOT]]
; CHECK-NEXT:   %racer.status = load volatile i8, ptr %racer.signal
; CHECK-NEXT:   %racer.done = icmp ne i8 %racer.status, 0
; CHECK-NEXT:   br i1 %racer.done, label %racer.yield, label %[[BODY:.*]], !prof [[COLD:![0-9]+]]
; CHECK:      racer.yield:
; CHECK-NEXT:   call void @do_yield() [[NOUNWIND:#[0-9]+]]
; CHECK-NEXT:   br label %[[BODY]]
; CHECK:      [[BODY]]:
; CHECK-NEXT:   %addr = getelementptr
; CHECK-NOT:  do_yield
; CHECK:        ret void

; Two nested loops: two probe points, one at each header.
define void @nested(ptr %p, i64 %rows, i64 %cols) {
entry:
  br label %outer
outer:
  %r = phi i64 [ 0, %entry ], [ %r.next, %outer.latch ]
  br label %inner
inner:
  %c = phi i64 [ 0, %outer ], [ %c.next, %inner ]
  %index = add i64 %r, %c
  %addr = getelementptr i32, ptr %p, i64 %index
  store i32 1, ptr %addr
  %c.next = add i64 %c, 1
  %inner.done = icmp eq i64 %c.next, %cols
  br i1 %inner.done, label %outer.latch, label %inner
outer.latch:
  %r.next = add i64 %r, 1
  %outer.done = icmp eq i64 %r.next, %rows
  br i1 %outer.done, label %exit, label %outer
exit:
  ret void
}
; CHECK-LABEL: define void @nested(
; CHECK-COUNT-2: call void @do_yield()
; CHECK-NOT:     call void @do_yield()
; CHECK:         ret void

; No loop: no probe point.
define i32 @straight_line(i32 %x) {
  %y = add i32 %x, 1
  ret i32 %y
}
; CHECK-LABEL: define i32 @straight_line(
; CHECK-NOT:   do_yield
; CHECK:       ret i32

; CHECK: declare void @do_yield()
; CHECK: attributes [[NOUNWIND]] = { nounwind }
; CHECK: [[COLD]] = !{!"branch_weights", i32 1, i32 10000}
