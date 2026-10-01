#SIMPLESCRIPT
OUT "data" 1 "hello"
OUT "data" 2 "world"
RD "data" @n @s
PRN "RD all-wildcard (expect n=1 s=hello): n=" $n "s=" $s
IN "data" 1 @s2
PRN "IN first-exact (expect s2=hello): s2=" $s2
IN "data" @n2 "world"
PRN "IN second-exact (expect n2=2): n2=" $n2
PRN "PASS"
RET
