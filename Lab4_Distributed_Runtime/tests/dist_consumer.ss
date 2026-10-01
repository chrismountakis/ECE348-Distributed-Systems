#SIMPLESCRIPT
PRN "=== Test 1: RD blocks until remote OUT (NOTIFY path) ==="
RD "job" @val
PRN "RD got (expect 10):" $val

PRN "=== Test 2: RD again - tuple still in remote space (RD never removes) ==="
RD "job" @val2
PRN "RD got again (expect 10):" $val2

PRN "=== Test 3: IN removes tuple via QUERY ==="
IN "job" @val3
PRN "IN got (expect 10):" $val3

PRN "=== Test 4: IN blocks because tuple is gone, waits for second OUT ==="
IN "job" @val4
PRN "IN got (expect 20):" $val4

RET
