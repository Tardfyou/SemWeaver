import cpp

from Expr e
where e.getFile().getRelativePath() = "bfd/opncls.c" and e.getLocation().getStartLine() = 1206
select e, e.toString()
