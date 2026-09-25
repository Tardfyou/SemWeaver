import cpp
predicate inMat(Element e) { e.getFile().getRelativePath() = "coders/mat.c" }
from Stmt s
where inMat(s) and s.getLocation().getStartLine() >= 1338 and s.getLocation().getStartLine() <= 1346
select s, s.getLocation().getStartLine(), s.toString()
