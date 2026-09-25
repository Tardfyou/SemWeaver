import cpp

from Assignment a, FunctionCall src, VariableAccess lhs
where
  a.getFile().getRelativePath().matches("%bfd/elf64-x86-64.c") and
  a.getLocation().getStartLine() >= 6710 and
  a.getLocation().getStartLine() <= 6720 and
  lhs = a.getLValue() and
  (
    src = a.getRValue() or
    a.getRValue().getAChild*() = src
  )
select a, lhs.getTarget().getName(), a.getRValue().toString(), src.toString(), src.getTarget().getName(), src.getLocation().getStartLine()
