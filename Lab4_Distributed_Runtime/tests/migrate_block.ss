#SIMPLESCRIPT
PRN "blocking on RD — waiting for WAKE, value"
RD "WAKE" @value
PRN "unblocked — got value:" $value
RET
