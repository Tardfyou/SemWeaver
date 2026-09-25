import cpp

predicate inPatchFile(Element e) {
  e.getFile().getRelativePath().matches("%src/libjasper/jpc/jpc_dec.c")
}

from Element e
where
  inPatchFile(e) and
  (
    exists(Function f | e = f and f.getName() = "jpc_dec_cp_create")
    or
    exists(AssignExpr a | e = a and a.getLocation().getStartLine() >= 1220 and a.getLocation().getStartLine() <= 1245)
    or
    exists(FunctionCall c | e = c and c.getLocation().getStartLine() >= 1220 and c.getLocation().getStartLine() <= 1245)
  )
select e, e.toString(), e.getLocation().getStartLine(), e.getLocation().getStartColumn()
