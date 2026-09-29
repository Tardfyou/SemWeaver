"""CSV arithmetic against actual verified summary, no model or checker execution."""
import csv
import io
import json
import unittest
from export_main39_tables import ROOT,make_tables


class TableTests(unittest.TestCase):
    def test_actual_all_subjects_and_lossless_metrics(self):
        source=json.loads((ROOT/'FINAL39_SUMMARY.json').read_text())
        tables=make_tables(source)
        rows=list(csv.DictReader(io.StringIO(tables['CASE_TABLE.csv'])))
        self.assertEqual(len(rows),156)
        for method in ('unmodified','native','no_internal','knighter'):
            selected=[r for r in rows if r['method']==method]
            self.assertEqual(len({r['case_id'] for r in selected}),39)
        rows=list(csv.DictReader(io.StringIO(tables['SUMMARY.csv'])))
        for row in rows:
            expected=(source['starting'] if row['method']=='unmodified' else source['portfolios'][row['method']])['metrics']
            for key,value in expected.items():self.assertEqual(float(row[key]),value)

    def test_partial_summary_refused(self):
        with self.assertRaises(AssertionError):make_tables({'status':'partial'})


if __name__=='__main__':unittest.main()
