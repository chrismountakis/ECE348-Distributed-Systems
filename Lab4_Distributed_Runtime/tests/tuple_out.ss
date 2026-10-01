#SIMPLESCRIPT
PRN "producer: sleeping 3s before first OUT"
SLP 3
OUT "result" 42
PRN "producer: added (result, 42)"
SLP 2
OUT "result" 99
PRN "producer: added (result, 99)"
RET
