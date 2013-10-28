; RUN: %lli_mcjit -extra-module=%p/multi-module-eh-b.ir %s
; XFAIL: arm, cygwin, win32, mingw
declare i8* @__cxa_allocate_exception(i64)
declare void @__cxa_throw(i8*, i8*, i8*)
declare i32 @__gxx_personality_v0(...)
declare void @__cxa_end_catch()
declare i8* @__cxa_begin_catch(i8*)

@_ZTIi = external constant i8*

declare i32 @FB()

define void @throwException() {
  %exception = tail call i8* @__cxa_allocate_exception(i64 4)
  call void @__cxa_throw(i8* %exception, i8* bitcast (i8** @_ZTIi to i8*), i8* null)
  unreachable
}

define i32 @main() {
entry:
  invoke void @throwException()
          to label %try.cont unwind label %lpad

lpad:
  %p = landingpad { i8*, i32 } personality i8* bitcast (i32 (...)* @__gxx_personality_v0 to i8*)
          catch i8* bitcast (i8** @_ZTIi to i8*)
  %e = extractvalue { i8*, i32 } %p, 0
  call i8* @__cxa_begin_catch(i8* %e)
  call void @__cxa_end_catch()
  br label %try.cont

try.cont:
  %r = call i32 @FB( )
  ret i32 %r
}
