
implementation   ; Functions:

declare void %__main()

int %foo(int %X, int %Y, double %A) {
bb0:		; No predecessors!
	%cond212 = setne double %A, 1.000000e+00		; <bool> [#uses=1]
	%cast110 = cast bool %cond212 to int		; <int> [#uses=1]
	ret int %cast110
}

int %main() {
bb0:		; No predecessors!
	call void %__main( )
	%reg212 = call int %foo( int 0, int 1, double 1.000000e+00 )		; <int> [#uses=1]
	ret int %reg212
}
