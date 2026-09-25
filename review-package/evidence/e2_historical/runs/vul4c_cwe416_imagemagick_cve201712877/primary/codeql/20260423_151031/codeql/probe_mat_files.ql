import cpp
from File f
where f.getRelativePath().matches("%mat.c%")
select f, f.getRelativePath()
