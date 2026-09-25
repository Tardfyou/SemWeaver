import cpp

from IfStmt ifs
where
  ifs.getFile().getBaseName() = "jpgfile.c" and
  ifs.getEnclosingFunction().getName() = "process_COM"
select ifs.getCondition(), ifs.getCondition().toString()
