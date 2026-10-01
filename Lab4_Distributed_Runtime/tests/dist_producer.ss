#SIMPLESCRIPT
PRN "producer: sleeping 4s so consumer can block first..."
SLP 4
OUT "job" 10
PRN "producer: put (job, 10)"
SLP 4
OUT "job" 20
PRN "producer: put (job, 20)"
RET
