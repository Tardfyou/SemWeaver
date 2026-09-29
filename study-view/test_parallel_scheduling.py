"""Offline scheduler-only fixtures; no model/container/checkout execution."""
import ast
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parent


def transform_outcomes(source):
    tree=ast.parse(source)
    cls=next(n for n in ast.walk(tree) if isinstance(n,ast.ClassDef) and n.name=='Repeat3Native')
    namespace={'ast':ast}
    exec(compile(ast.fix_missing_locations(ast.Module(body=[cls],type_ignores=[])),
                 '<synthetic-scheduler-class>','exec'),namespace)
    fixture=ast.parse("for repeat in (2,3):\n for case in subjects:\n  for arm in ('native','no_internal'):\n   outputs.append((repeat,case,arm))")
    fixture=ast.fix_missing_locations(namespace['Repeat3Native']().visit(fixture))
    values={'subjects':['fixture-a','fixture-b'],'outputs':[]}
    exec(compile(fixture,'<synthetic-cells>','exec'),values)
    return values['outputs']


class SchedulerTests(unittest.TestCase):
    def test_native_only_original_third_decode(self):
        source=(ROOT/'launch_e3_parallel_repeat3_native.py').read_text()
        self.assertEqual(transform_outcomes(source),[(3,'fixture-a','native'),(3,'fixture-b','native')])

    def test_control_adapter_only_original_third_decode(self):
        adapter=ast.parse((ROOT/'launch_e3_parallel_repeat3_control.py').read_text())
        node=next(n for n in ast.walk(adapter) if isinstance(n,ast.Assign)
                  and any(isinstance(t,ast.Name) and t.id=='changes' for t in n.targets))
        changes=ast.literal_eval(node.value)
        source=(ROOT/'launch_e3_parallel_repeat3_native.py').read_text()
        for old,new in changes.items():
            self.assertEqual(source.count(old),1)
            source=source.replace(old,new)
        self.assertEqual(transform_outcomes(source),[(3,'fixture-a','no_internal'),(3,'fixture-b','no_internal')])

    def test_storage_guard_excludes_retired_full_matrices(self):
        tree=ast.parse((ROOT/'guard_focused_storage.py').read_text())
        node=next(n for n in tree.body if isinstance(n,ast.Assign)
                  and any(isinstance(t,ast.Name) and t.id=='QUEUES' for t in n.targets))
        names=ast.literal_eval(node.value)
        self.assertNotIn('launch_e4_parallel_luna.py',names)
        self.assertNotIn('launch_glm_high_matrix.py',names)
        self.assertIn('launch_focused_robustness.py',names)
        self.assertIn('launch_e3_parallel_repeat3_control.py',names)


if __name__=='__main__':unittest.main()
