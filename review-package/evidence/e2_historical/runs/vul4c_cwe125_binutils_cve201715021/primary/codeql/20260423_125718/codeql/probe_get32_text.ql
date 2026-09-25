import cpp
from Expr e
where e.toString().matches("%bfd_get_32%")
select e, e.getFile().getRelativePath() + " :: " + e.toString()
