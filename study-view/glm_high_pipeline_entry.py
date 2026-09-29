"""Explicit GLM high effort, original wire/parser, adaptive diagnostic observer."""
import argparse
import ast
import runpy
import subprocess
import sys
from pathlib import Path

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--source-root',type=Path,required=True)
    parser.add_argument('--target-script',type=Path,required=True)
    parser.add_argument('arguments',nargs=argparse.REMAINDER)
    args=parser.parse_args();sys.path.insert(0,str(args.source_root))
    from src.llm import anthropic_chat_model as adapter
    tree=ast.parse(Path(adapter.__file__).read_text())
    cls=next(x for x in tree.body if isinstance(x,ast.ClassDef) and x.name=='AnthropicMessagesChatModel')
    function=next(x for x in cls.body if isinstance(x,ast.FunctionDef) and x.name=='_generate')
    class Effort(ast.NodeTransformer):
        count=0
        def visit_Assign(self,node):
            if any(isinstance(t,ast.Name) and t.id=='response' for t in node.targets):
                self.count+=1
                return [*ast.parse("request['extra_body'] = {'reasoning_effort': 'high'}").body,node]
            return node
    edit=Effort();function=edit.visit(function);assert edit.count==1
    exec(compile(ast.fix_missing_locations(ast.Module(body=[function],type_ignores=[])),
                 adapter.__file__+'[explicit-high]','exec'),adapter.__dict__)
    adapter.AnthropicMessagesChatModel._generate=adapter.__dict__.pop('_generate')
    original=subprocess.Popen
    def popen(command,*positional,**kwargs):
        if isinstance(command,list) and len(command)>1 and Path(command[1]).name=='run_checker_trace_probe.py':
            command=[command[0],str(Path(__file__).with_name('adaptive_trace_probe.py')),
                     '--source-root',str(args.source_root),'--',*command[2:]]
        return original(command,*positional,**kwargs)
    subprocess.Popen=popen
    arguments=args.arguments[1:] if args.arguments[:1]==['--'] else args.arguments
    sys.argv=[str(args.target_script),*arguments]
    runpy.run_path(str(args.target_script),run_name='__main__')
