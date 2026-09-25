import cpp
predicate inMat(Element e) { e.getFile().getRelativePath().matches("%/coders/mat.c") }
from FunctionCall c
where inMat(c) and c.getLocation().getStartLine() >= 1330 and c.getLocation().getStartLine() <= 1375
select c, c.getLocation().getStartLine(), c.toString(), c.getTarget().getName(), c.getArgument(0).toString()
