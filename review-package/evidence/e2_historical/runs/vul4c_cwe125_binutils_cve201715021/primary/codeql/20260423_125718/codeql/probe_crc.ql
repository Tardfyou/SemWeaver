import cpp
from Expr e
where e.getFile().getRelativePath() = "bfd/opncls.c" and e.toString().matches("%crc_offset%")
select e, e.toString()
