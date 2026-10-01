#SIMPLESCRIPT
PRN "consumer: waiting for first tuple..."
IN "result" @val
PRN "consumer: got" $val
PRN "consumer: waiting for second tuple..."
IN "result" @val2
PRN "consumer: got" $val2
RET
