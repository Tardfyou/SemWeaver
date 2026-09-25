import cpp
predicate inMat(Element e) { e.getFile().getRelativePath() = "coders/mat.c" }
from Expr e
where inMat(e) and e.getLocation().getStartLine() >= 1338 and e.getLocation().getStartLine() <= 1346
select e, e.getLocation().getStartLine(), e.toString(), e.getClass().getName()
