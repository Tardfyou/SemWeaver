import cpp

from FunctionCall c
where c.getFile().getRelativePath().matches("%bfd/elf64-x86-64.c")
select c, c.toString(), c.getTarget().getName(), c.getLocation().getStartLine()
