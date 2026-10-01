#SIMPLESCRIPT
OUT "data" 42
RD "data" @v1
RD "data" @v2
IN "data" @v3
BEQ $v1 42 #chk2
PRN "FAIL: first RD expected 42 got" $v1
RET
#chk2
BEQ $v2 42 #chk3
PRN "FAIL: second RD expected 42 got" $v2
RET
#chk3
BEQ $v3 42 #ok
PRN "FAIL: IN expected 42 got" $v3
RET
#ok
PRN "PASS"
RET
