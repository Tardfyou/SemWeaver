import cpp

from Assignment a
where
  a.getFile().getRelativePath().matches("%bfd/elf64-x86-64.c") and
  a.getLocation().getStartLine() >= 6708 and
  a.getLocation().getStartLine() <= 6724
select a, a.toString(), a.getLValue().toString(), a.getRValue().toString(), a.getLocation().getStartLine()
