; NOTE: Assertions have been autogenerated by utils/update_test_checks.py
; RUN: opt < %s -instcombine -S | FileCheck %s

; InstCombine should mark null-checked argument as nonnull at callsite
declare void @dummy(i32*, i32)

define void @test(i32* %a, i32 %b) {
; CHECK-LABEL: @test(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    [[COND1:%.*]] = icmp eq i32* %a, null
; CHECK-NEXT:    br i1 [[COND1]], label %dead, label %not_null
; CHECK:       not_null:
; CHECK-NEXT:    [[COND2:%.*]] = icmp eq i32 %b, 0
; CHECK-NEXT:    br i1 [[COND2]], label %dead, label %not_zero
; CHECK:       not_zero:
; CHECK-NEXT:    call void @dummy(i32* nonnull %a, i32 %b)
; CHECK-NEXT:    ret void
; CHECK:       dead:
; CHECK-NEXT:    unreachable
;
entry:
  %cond1 = icmp eq i32* %a, null
  br i1 %cond1, label %dead, label %not_null
not_null:
  %cond2 = icmp eq i32 %b, 0
  br i1 %cond2, label %dead, label %not_zero
not_zero:
  call void @dummy(i32* %a, i32 %b)
  ret void
dead:
  unreachable
}

