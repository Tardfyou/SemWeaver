import cpp
predicate inMat(Element e) { e.getFile().getRelativePath().matches("%/coders/mat.c") }
from FunctionCall c, Expr arg, VariableAccess va
where inMat(c) and c.getTarget().getName() = "DeleteImageFromList" and arg = c.getArgument(0) and (va = arg or arg.getAChild*() = va)
select c, c.getLocation().getStartLine(), "call", arg.toString(), va.toString(), va.getTarget().getName()
