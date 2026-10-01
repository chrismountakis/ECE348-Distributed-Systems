#SIMPLESCRIPT
SET @a 10
SET @b 3
ADD @sum $a $b
SUB @diff $a $b
MUL @prod $a $b
DIV @quot $a $b
MOD @rem $a $b
PRN "10 + 3 =" $sum
PRN "10 - 3 =" $diff
PRN "10 * 3 =" $prod
PRN "10 / 3 =" $quot
PRN "10 % 3 =" $rem
RET
