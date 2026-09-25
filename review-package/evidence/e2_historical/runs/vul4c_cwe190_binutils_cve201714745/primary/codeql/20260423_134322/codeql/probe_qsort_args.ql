import cpp

from FunctionCall c, Expr arg
where
  c.getFile().getRelativePath().matches("%bfd/elf64-x86-64.c") and
  c.getTarget().hasName("qsort") and
  arg = c.getAnArgument()
select c, arg, arg.toString(), arg.getLocation().getStartLine(), arg.getLocation().getStartColumn()
