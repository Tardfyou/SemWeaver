import cpp

from FunctionCall call
where call.getTarget().hasName("bfd_get_32")
select call, call.getFile().getRelativePath() + " :: " + call.getEnclosingFunction().getName() + " :: " + call.getArgument(1).toString()
