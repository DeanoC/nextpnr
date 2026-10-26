#!/usr/bin/env python3
"""Corruption checks exercise real retained Quartus/RBF references."""
import copy
import csv
from decimal import Decimal
import io
import json
from pathlib import Path
import re
import shutil
import tempfile
import unittest
from reference import ROOT, CORNERS, load, text, rows, validate_case, decode, routed_input


class PrimitiveReference(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.reference = load()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.case = "lab-l5"
        self.directory = Path(self.temporary.name)/self.case
        shutil.copytree(ROOT/"fixtures"/self.case,self.directory)

    def reject(self):
        with self.assertRaises((AssertionError,KeyError,ValueError,ArithmeticError,FileNotFoundError)):
            validate_case(self.directory,self.case)

    def edit_rows(self, name, action):
        original = text(self.directory,name)
        values = rows(original)
        action(values)
        result = io.StringIO()
        writer = csv.DictWriter(result,fieldnames=original.splitlines()[0].split('\t'),delimiter='\t',lineterminator='\n')
        writer.writeheader(); writer.writerows(values)
        (self.directory/name).write_text(result.getvalue())

    def test_complete_inventory(self):
        self.assertEqual(len(self.reference),3664)
        self.assertEqual({r['corner'] for r in self.reference},set(CORNERS))
        self.assertEqual({r['block'] for r in self.reference},{"LAB","MLAB"})
        self.assertEqual({r['alm'] for r in self.reference},set(range(10)))

    def test_zero_and_inactive_transitions_remain_distinct(self):
        carry = [r for r in self.reference if r['input'] == "cin" and r['output'] == "cout"]
        self.assertTrue(any(r['delays_ps']['rr'] == [0,0] for r in carry))
        self.assertTrue(any(r['delays_ps']['rr'][0] > 0 for r in carry))
        self.assertTrue(all(set(r['delays_ps']) == {'rr','ff'} for r in carry))

    def test_missing_arc(self):
        self.edit_rows('arcs/'+CORNERS[0]+'.tsv',lambda v:v.pop())
        self.reject()

    def test_duplicate_arc(self):
        self.edit_rows('arcs/'+CORNERS[0]+'.tsv',lambda v:v.append(copy.deepcopy(v[0])))
        self.reject()

    def test_reversed_interval(self):
        self.edit_rows('arcs/'+CORNERS[0]+'.tsv',lambda v:v[0].update(rr_min=str(Decimal(v[0]['rr_max'])+1)))
        self.reject()

    def test_nonfinite_interval(self):
        self.edit_rows('arcs/'+CORNERS[0]+'.tsv',lambda v:v[0].update(rr_min='NaN'))
        self.reject()

    def test_wrong_unateness(self):
        self.edit_rows('arcs/'+CORNERS[0]+'.tsv',lambda v:v[0].update(unate='negative'))
        self.reject()

    def test_wrong_physical_location(self):
        self.edit_rows('arcs/'+CORNERS[0]+'.tsv',lambda v:v[0].update(location='LABCELL_X1_Y1_N0'))
        self.reject()

    def test_spoofed_operating_corner(self):
        (self.directory/'arcs'/f'{CORNERS[0]}.tsv').write_text(text(self.directory,'arcs/'+CORNERS[2]+'.tsv'))
        self.reject()

    def test_missing_corner(self):
        (self.directory/'arcs'/f'{CORNERS[3]}.tsv').unlink()
        self.reject()

    def test_changed_source(self):
        p=self.directory/'source/top.sv'
        p.write_text(p.read_text().replace(".dataa(q[0])",".dataa(q[1])",1))
        self.reject()

    def test_wrong_launch_register(self):
        def change(values):
            values[0]['launch']='launch_99'
        self.edit_rows('bindings.tsv',change)
        self.reject()

    def test_wrong_launch_location(self):
        self.edit_rows('bindings.tsv',lambda v:v[0].update(launch_location='FF_X1_Y1_N1'))
        self.reject()

    def test_missing_route(self):
        data = text(self.directory,'fitted.bt')
        graph,_ = decode(data)
        binding = rows(text(self.directory,'bindings.tsv'))[0]
        spec = json.loads(text(self.directory,'fixture.json'))
        site = next(s for s in spec['sites'] if s['cell'] == binding['cell'])
        _,path = routed_input(graph,binding['launch_location'],site,'l5')
        route = f'r {path[-2]} {path[-1]}'
        lines = [line for line in data.splitlines() if line.split(' ;')[0] != route]
        self.assertLess(len(lines),len(data.splitlines()))
        (self.directory/'fitted.bt').write_text('\n'.join(lines)+'\n')
        self.reject()

    def test_feedback_mode_not_silently_accepted(self):
        data=text(self.directory,'fitted.bt')
        data,count=re.subn(r'(s LAB\.029\.077:TMODE\.0 )E_0',r'\1D_F',data)
        self.assertEqual(count,1)
        (self.directory/'fitted.bt').write_text(data)
        self.reject()

    def test_wrong_lut_truth(self):
        data=text(self.directory,'fitted.bt')
        def flip(m):
            value=int(m[2].replace('.',''),16)^1
            return m[1]+f'{value>>32:08x}.{value&0xffffffff:08x}'
        data,count=re.subn(r'(s LAB\.029\.077:LUT_MASK\.0 )([0-9a-f.]+)',flip,data)
        self.assertEqual(count,1)
        (self.directory/'fitted.bt').write_text(data)
        self.reject()

    def test_missing_independent_path_witness(self):
        name=f'paths/{CORNERS[0]}/setup-rise.rpt'
        (self.directory/name).write_text('Data Arrival Path\nData Required Path\n')
        self.reject()

    def test_path_increment_must_match_direct_edge_query(self):
        name=f'paths/{CORNERS[0]}/setup-rise.rpt'
        lines=text(self.directory,name).splitlines()
        for index,line in enumerate(lines):
            fields=[s.strip() for s in line.strip(';').split(';')]
            if len(fields)==7 and fields[3]=='CELL' and re.fullmatch(r'target_\d+\|combout',fields[6]):
                fields[1]=str(Decimal(fields[1])+Decimal('0.010'))
                lines[index]='; '+' ; '.join(fields)+' ;'
                break
        else: self.fail('no primitive path witness')
        (self.directory/name).write_text('\n'.join(lines)+'\n')
        self.reject()


if __name__ == '__main__':
    unittest.main()
