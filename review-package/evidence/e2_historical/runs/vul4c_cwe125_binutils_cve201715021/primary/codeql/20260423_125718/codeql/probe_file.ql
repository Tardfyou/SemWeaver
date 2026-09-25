import cpp
from Element e
where e.getFile().getRelativePath() = "bfd/opncls.c"
select e, e.toString()
