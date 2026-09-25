import cpp

from Expr e
where e.getFile().getRelativePath() = "bfd/opncls.c" and e.getLocation().getStartLine() = 1203
select e, e.toString()
