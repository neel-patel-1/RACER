; An innermost loop of B IR instructions is unrolled interval / B times, so a
; probe point covers about `interval` instructions. Each copy keeps its exit
; test: no remainder loop is made.
; RUN: %racer-opt -racer-probe-interval=20 -passes=racer-probe -pass-remarks=racer-probe -S %s 2>%t.remarks | FileCheck %s
; RUN: FileCheck --check-prefix=REMARK %s < %t.remarks
; RUN: %racer-opt -racer-probe-interval=0 -passes=racer-probe -S %s | FileCheck --check-prefix=NOUNROLL %s

; Body: getelementptr, store, add, icmp, br = 5 instructions; 20 / 5 = 4.
define void @unknown_trip_count(ptr %p, i64 %n) {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %addr = getelementptr i32, ptr %p, i64 %i
  store i32 7, ptr %addr
  %i.next = add i64 %i, 1
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %exit, label %loop
exit:
  ret void
}
; REMARK: remark: {{.*}} unrolled loop of 5 IR instructions by 4{{$}}
; CHECK-LABEL:   define void @unknown_trip_count(
; CHECK-COUNT-1:   call void @do_yield()
; CHECK-COUNT-4:   store i32 7
; CHECK-NOT:       store i32 7
; CHECK-NOT:       do_yield
; CHECK:           ret void
; NOUNROLL-LABEL:  define void @unknown_trip_count(
; NOUNROLL-COUNT-1: store i32 7
; NOUNROLL-NOT:    store i32 7

; A loop of 3 iterations is shorter than the interval: it is fully unrolled
; and needs no probe point.
define void @three_iterations(ptr %p) {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %addr = getelementptr i32, ptr %p, i64 %i
  store i32 3, ptr %addr
  %i.next = add i64 %i, 1
  %done = icmp eq i64 %i.next, 3
  br i1 %done, label %exit, label %loop
exit:
  ret void
}
; REMARK: remark: {{.*}} unrolled loop of 5 IR instructions by 3 (fully)
; CHECK-LABEL:   define void @three_iterations(
; CHECK-COUNT-3:   store i32 3
; CHECK-NOT:       do_yield
; CHECK:           ret void

; A body as large as the interval is not unrolled.
define void @large_body(ptr %p, i64 %n) {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %a0 = getelementptr i32, ptr %p, i64 %i
  store i32 0, ptr %a0
  store i32 1, ptr %a0
  store i32 2, ptr %a0
  store i32 3, ptr %a0
  store i32 4, ptr %a0
  store i32 5, ptr %a0
  store i32 6, ptr %a0
  store i32 8, ptr %a0
  store i32 9, ptr %a0
  store i32 10, ptr %a0
  store i32 11, ptr %a0
  store i32 12, ptr %a0
  store i32 13, ptr %a0
  store i32 14, ptr %a0
  store i32 15, ptr %a0
  store i32 16, ptr %a0
  %i.next = add i64 %i, 1
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %exit, label %loop
exit:
  ret void
}
; REMARK-NOT: unrolled loop of 20
; CHECK-LABEL:    define void @large_body(
; CHECK-COUNT-1:    store i32 16
; CHECK-NOT:        store i32 16
