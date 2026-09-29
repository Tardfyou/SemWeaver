"""Synthetic tree tests; no live evidence or package is modified."""
import tempfile
import unittest
from pathlib import Path
from artifact_tree_aliases import files_with_local_aliases


class AliasTests(unittest.TestCase):
    def test_local_probe_aliases_preserve_logical_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);cap=root/'cap-64000';cap.mkdir()
            (cap/'RESULT.json').write_text('{}')
            (root/'RESULT.json').symlink_to('cap-64000/RESULT.json')
            (root/'vulnerable').symlink_to('cap-64000',target_is_directory=True)
            files=dict(files_with_local_aliases(root))
            self.assertEqual(files[Path('RESULT.json')],cap/'RESULT.json')
            self.assertEqual(files[Path('vulnerable/RESULT.json')],cap/'RESULT.json')

    def test_external_alias_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)/'evidence';root.mkdir()
            outside=Path(directory)/'outside';outside.write_text('fixture')
            (root/'link').symlink_to(outside)
            with self.assertRaises(ValueError):list(files_with_local_aliases(root))

    def test_cycle_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);(root/'loop').symlink_to('.',target_is_directory=True)
            with self.assertRaises(ValueError):list(files_with_local_aliases(root))


if __name__=='__main__':unittest.main()
