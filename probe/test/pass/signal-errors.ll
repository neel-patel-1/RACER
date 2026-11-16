; A preempt_signal that is not thread_local is a user error, not a crash.
; RUN: not %racer-opt -passes=racer-probe -disable-output %s 2>&1 | FileCheck %s

; CHECK: LLVM ERROR: racer-probe: 'preempt_signal' exists but is not thread_local

@preempt_signal = global ptr null

define void @spin(i64 %n) {
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
