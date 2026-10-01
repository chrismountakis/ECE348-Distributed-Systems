#SIMPLESCRIPT
SET @i 5000
#loop
SUB @i $i 1
MOD @m $i 5
BEQ $m 0 #show
BRA #loop
#show
PRN "Thread A remaining:" $i
BGT $i 0 #loop
PRN "Thread A done!"
RET
