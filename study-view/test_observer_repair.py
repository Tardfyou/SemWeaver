"""Regression checks for observer-only statement name shadowing."""
import unittest
from pathlib import Path
from fixed_trace_probe import repaired_instrumenter

SOURCE=Path(__file__).resolve().parent.parent/'LLM-Native/SemWeaver-v43'


def fixture(statement_type='const Stmt *', statement_name='S'):
    source=f'void f({statement_type}{statement_name}, CheckerContext &C) {{ {{ SymbolRef {statement_name} = nullptr; return true; }} }}'.encode()
    begin=source.index(b'{'); end=len(source)-1; ret=source.index(b'return true;')
    def position(offset,length=1): return {'offset':offset,'tokLen':length}
    method={'kind':'CXXMethodDecl','name':'f','inner':[
        {'kind':'ParmVarDecl','name':statement_name,'type':{'qualType':statement_type}},
        {'kind':'ParmVarDecl','name':'C','type':{'qualType':'CheckerContext &'}},
        {'kind':'CompoundStmt','range':{'begin':position(begin),'end':position(end)},'inner':[
            {'kind':'ReturnStmt','range':{'begin':position(ret,6),'end':position(ret+7,4)}}]}]}
    return source,[method],begin


class RepairTest(unittest.TestCase):
    def test_shadowed_statement_is_captured_outside_local_scope(self):
        instrument,_=repaired_instrumenter(SOURCE)
        source,roots,begin=fixture()
        result,receipt=instrument(source,roots,SOURCE/'src/research/checker_trace_support.h')
        text=result.decode(); alias=f'__semweaver_statement_{begin}'
        self.assertIn(f'const clang::Stmt *{alias} = S;',text)
        self.assertIn(f'"pre_return", {alias},',text)
        self.assertNotIn('"pre_return", S,',text)
        self.assertEqual([x['event'] for x in receipt['sites']],['enter','pre_return'])
        self.assertIn('SymbolRef S = nullptr; return',text.replace('{semweaver_trace::emit(C, "f", 1, "pre_return", '+alias+', [&]() { return llvm::json::Array{}; });',''))

    def test_non_statement_pointer_has_null_statement_capture(self):
        instrument,_=repaired_instrumenter(SOURCE)
        source,roots,begin=fixture('SymbolRef ','V')
        result,_=instrument(source,roots,SOURCE/'src/research/checker_trace_support.h')
        self.assertIn(f'const clang::Stmt *__semweaver_statement_{begin} = nullptr;',result.decode())


if __name__=='__main__':
    unittest.main()
