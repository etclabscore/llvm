; This testcase shows a bug where an common subexpression exists, but there
; is no shared dominator block that the expression can be hoisted out to.
;
; RUN: as < %s | opt -load-vn -gcse | dis | not grep load

int %test(int* %P) {
	store int 5, int* %P
	%Z = load int* %P
        ret int %Z
}

