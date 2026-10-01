#SIMPLESCRIPT
PRN "producer: waiting 4s so both consumers can block first..."
SLP 4
OUT "job" 1
PRN "producer: put (job, 1) — both consumers should race for it"
SLP 5
OUT "job" 2
PRN "producer: put (job, 2) — losing consumer should get this"
RET
