#!/usr/bin/env python3
"""Dependency closure and diagnostic-versus-retention boundaries."""
import copy
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import routed_placement_refine as refine
import routed_placement_displaced as displaced

f = refine.feedback


def plan(cycle=False):
    cells = {name:dict(type="MISTRAL_FF" if name == "x" else "MISTRAL_ALUT_ARITH" if name in "ab" else "MISTRAL_ALUT2",
                      cluster="a" if name in "ab" else "") for name in ("a", "b", "x", "y")}
    origin = dict(architecture=dict(group_moves=True), design=dict(cells=cells),
                  placement={name:["old-"+name, 1] for name in cells})
    targets = dict(a="new-a" if cycle else "old-x", b="new-b", x="old-y", y="old-x" if cycle else "new-y")
    return dict(version=3, origin=origin, parent=origin, evaluation="full_route_required", moves=[
        dict(cell=name, from_bel="old-"+name, to_bel=targets[name], strength=1) for name in cells])


def register_chain():
    origin = dict(architecture=dict(group_moves=True), design=dict(cells={
        name:dict(type="MISTRAL_FF", cluster="") for name in ("a", "b", "c")}),
        placement={name:["old-"+name, 1] for name in ("a", "b", "c")})
    return dict(version=3, origin=origin, parent=origin, evaluation="full_route_required", moves=[
        dict(cell=name, from_bel="old-"+name, to_bel=target, strength=1)
        for name, target in (("a", "old-b"), ("b", "old-c"), ("c", "empty"))])


class Refinement(unittest.TestCase):
    def test_carry_and_displacement_dependencies_are_closed_transitively(self):
        source = plan(); units = refine.movement_units(source)
        owner = {name:u["id"] for u in units for name in u["cells"]}
        self.assertEqual(owner["a"], owner["b"])
        self.assertEqual(refine.keep_closure(units, {owner["a"]}), frozenset(range(len(units))))
        self.assertNotIn(owner["a"], refine.remove_closure(units, {owner["x"]}))
        with self.assertRaises(f.Rejected): refine.subset_plan(source, units, {owner["a"]})
        reduced = refine.subset_plan(source, units, refine.remove_closure(units, {owner["a"]}))
        self.assertEqual({m["cell"] for m in reduced["moves"]}, {"x", "y"})
        self.assertEqual(len(source["moves"]), 4)

    def test_exchange_cycles_stay_atomic(self):
        source = plan(cycle=True); units = refine.movement_units(source)
        self.assertEqual({tuple(u["cells"]) for u in units}, {("a", "b"), ("x", "y")})
        exchange = next(u["id"] for u in units if "x" in u["cells"])
        reduced = refine.subset_plan(source, units, {exchange})
        self.assertEqual({m["cell"] for m in reduced["moves"]}, {"x", "y"})

    def test_partial_cluster_and_duplicate_targets_are_rejected(self):
        source = plan(); source["moves"] = [m for m in source["moves"] if m["cell"] != "b"]
        with self.assertRaisesRegex(f.Rejected, "split a carry"): refine.movement_units(source)
        source = plan(); source["moves"][1]["to_bel"] = source["moves"][0]["to_bel"]
        with self.assertRaisesRegex(f.Rejected, "duplicate target"): refine.movement_units(source)

    def test_path_diagnostics_require_exact_coverage_and_native_sums(self):
        key = ("capture", "DATAIN", "clock", 0, "clock", 0)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); (root/"native").mkdir()
            record = dict(cell="capture", port="DATAIN", launch=["clock", 0], capture=["clock", 0],
                setup_margin_ps=-100, path_sum_ps=1100,
                segments=[dict(type="routing", delay_ps=1100, **{"from":["launch", "Q"], "to":["capture", "DATAIN"]})])
            table = dict(version=1, diagnostic_only=True, clock_skew=True, final_analogue_model=True, paths=[record])
            observation = dict(directory=root, rows={key:dict(setup_timed=True, setup_margin_ps=-100)})
            def write(value):
                import json
                (root/"native/endpoint-paths.json").write_text(json.dumps(value))
            write(table); self.assertEqual(len(refine.path_inventory(observation)), 1)
            wrong = copy.deepcopy(table); wrong["paths"][0]["path_sum_ps"] = 1099; write(wrong)
            with self.assertRaisesRegex(f.Rejected, "sum mismatch"): refine.path_inventory(observation)
            wrong = copy.deepcopy(table); wrong["paths"] = []; write(wrong)
            with self.assertRaisesRegex(f.Rejected, "incomplete setup-path"): refine.path_inventory(observation)

    def test_diagnostic_progress_cannot_authorize_a_regressing_artifact(self):
        spec = importlib.util.spec_from_file_location("fixtures", Path(__file__).with_name("routed_placement.py"))
        fixtures = importlib.util.module_from_spec(spec); spec.loader.exec_module(fixtures)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            parent_path, code = fixtures.bundle(root/"parent", fixtures.fixture())
            parent = f.read_observation(parent_path, code)
            bad = fixtures.fixture(-70, "alternative")
            bad["native/endpoints.json"]["endpoints"][1]["rows"][0]["setup_margin_ps"] = 99
            path, code = fixtures.bundle(root/"bad", bad); candidate = f.read_observation(path, code)
            self.assertGreater(f.score(candidate), f.score(parent))
            self.assertGreater(refine.diagnostic_rank(parent, candidate, 1)[3], 0)
            with self.assertRaisesRegex(f.Rejected, "setup endpoint regression"): f.compare(parent, candidate)

    def test_passing_and_failing_endpoint_ablations_get_separate_slots(self):
        source = plan()
        for move in source["moves"]: move["to_bel"] = "new-"+move["cell"]
        cells = source["origin"]["design"]["cells"]
        for name in ("x", "y"):
            cells[name]["type"] = "MISTRAL_FF"
            cells[name]["ports"] = dict(DATAIN=[0, name+"-data", 0])
        source["origin"]["design"]["nets"] = {name+"-data":dict(driver=["a", "SO"]) for name in ("x", "y")}
        units = refine.movement_units(source)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); (root/"native").mkdir()
            keys = [(name, "DATAIN", "clock", 0, "clock", 0) for name in ("x", "y")]
            baseline = dict(rows={keys[0]:dict(setup_timed=True, setup_margin_ps=-100),
                                  keys[1]:dict(setup_timed=True, setup_margin_ps=1000)})
            control = dict(directory=root, rows={key:dict(setup_timed=True,
                setup_margin_ps=value["setup_margin_ps"]-10) for key,value in baseline["rows"].items()})
            records = [dict(cell=key[0], port=key[1], launch=["clock", 0], capture=["clock", 0],
                setup_margin_ps=control["rows"][key]["setup_margin_ps"], path_sum_ps=10,
                segments=[dict(type="routing", delay_ps=10, **{"from":["a", "SO"], "to":[key[0], key[1]]})]) for key in keys]
            f.save(root/"native/endpoint-paths.json", dict(version=1, diagnostic_only=True, clock_skew=True,
                                                         final_analogue_model=True, paths=records))
            choices = refine.candidates(source, units, baseline, control)
            self.assertEqual([p["family"] for p in choices[:2]],
                             ["regressed_failing_endpoint", "regressed_passing_endpoint"])
            self.assertNotEqual(choices[0]["keep"], choices[1]["keep"])

    def test_register_shortcuts_restore_occupants_without_deleting_the_predecessor(self):
        source = register_chain(); original = copy.deepcopy(source)
        value = refine.redirect_register_plan(source, {"b"})
        self.assertEqual({m["cell"]:m["to_bel"] for m in value["plan"]["moves"]}, {"a":"old-c", "c":"empty"})
        self.assertEqual(value["restored"], ["b"])
        value = refine.redirect_register_plan(source, {"b", "c"})
        self.assertEqual({m["cell"]:m["to_bel"] for m in value["plan"]["moves"]}, {"a":"empty"})
        self.assertEqual(source, original)
        self.assertEqual(value["plan"]["origin"], source["origin"])
        self.assertEqual(value["plan"]["parent"], source["parent"])

    def test_register_rotation_closes_the_chain_at_its_vacated_origin(self):
        source = register_chain()
        value = refine.redirect_register_plan(source, {"c"}, ("b", "old-a"))
        self.assertEqual({m["cell"]:m["to_bel"] for m in value["plan"]["moves"]}, {"a":"old-b", "b":"old-a"})
        self.assertEqual({tuple(u["cells"]) for u in refine.movement_units(value["plan"])}, {("a", "b")})

    def test_register_redirection_rejects_clusters_unknown_cells_and_invented_sites(self):
        source = register_chain()
        with self.assertRaisesRegex(f.Rejected, "unknown restored"): refine.redirect_register_plan(source, {"missing"})
        with self.assertRaisesRegex(f.Rejected, "native register origin"):
            refine.redirect_register_plan(source, {"c"}, ("b", "invented"))
        source["origin"]["design"]["cells"]["b"]["cluster"] = "b"
        with self.assertRaisesRegex(f.Rejected, "split a cluster"): refine.redirect_register_plan(source, {"b"})

    def test_register_exchange_collapse_is_not_a_new_destination(self):
        source = register_chain(); source["moves"] = source["moves"][:2]
        source["moves"][1]["to_bel"] = "old-a"
        self.assertIsNone(refine.redirect_register_plan(source, {"b"}))

    def test_register_candidate_families_preserve_native_sites_and_parent(self):
        source = register_chain()
        keys = [(name, "DATAIN", "clock", 0, "clock", 0) for name in ("b", "c")]
        baseline = dict(rows={keys[0]:dict(setup_timed=True, setup_margin_ps=-100),
                              keys[1]:dict(setup_timed=True, setup_margin_ps=1000)})
        control = dict(rows={key:dict(setup_timed=True, setup_margin_ps=value["setup_margin_ps"]-10)
                             for key, value in baseline["rows"].items()})
        choices = refine.register_candidates(source, baseline, control)
        self.assertEqual([p["family"] for p in choices], ["restore_failing_register", "restore_passing_register",
                                                       "close_register_chain", "restore_register_chain"])
        native_sites = {m[field] for m in source["moves"] for field in ("from_bel", "to_bel")}
        for choice in choices:
            self.assertTrue(all(m["to_bel"] in native_sites for m in choice["plan"]["moves"]))
            self.assertEqual(choice["plan"]["parent"], source["parent"])
            self.assertTrue(choice["redirected"])
        self.assertEqual(len({str(p["plan"]["moves"]) for p in choices}), len(choices))

    def test_register_search_with_no_displacement_emits_nothing(self):
        source = register_chain(); source["moves"] = source["moves"][:1]; source["moves"][0]["to_bel"] = "empty"
        key = ("a", "DATAIN", "clock", 0, "clock", 0)
        baseline = dict(rows={key:dict(setup_timed=True, setup_margin_ps=100)})
        control = dict(rows={key:dict(setup_timed=True, setup_margin_ps=90)})
        self.assertEqual(refine.register_candidates(source, baseline, control), [])

    def test_displaced_lut_protocol_preserves_original_parent_and_other_cells(self):
        source = register_chain()
        for cell in source["origin"]["design"]["cells"].values(): cell["type"] = "MISTRAL_ALUT3"
        current = f.group_target(source)
        candidate = copy.deepcopy(source); candidate["moves"][0]["to_bel"] = "free"
        self.assertEqual(displaced.validate_plan(source["origin"], current, candidate), "a")
        wrong = copy.deepcopy(candidate); wrong["parent"] = copy.deepcopy(current)
        with self.assertRaisesRegex(f.Rejected, "original baseline"):
            displaced.validate_plan(source["origin"], current, wrong)
        wrong = copy.deepcopy(candidate); wrong["moves"][1]["to_bel"] = "other-free"
        with self.assertRaisesRegex(f.Rejected, "exactly one"):
            displaced.validate_plan(source["origin"], current, wrong)
        wrong = copy.deepcopy(candidate); wrong["moves"][0]["to_bel"] = "old-c"
        with self.assertRaisesRegex(f.Rejected, "evicts another"):
            displaced.validate_plan(source["origin"], current, wrong)

    def test_displaced_lut_protocol_rejects_registers_and_clusters(self):
        for clustered in (False, True):
            source = register_chain()
            for cell in source["origin"]["design"]["cells"].values(): cell["type"] = "MISTRAL_ALUT3"
            source["origin"]["design"]["cells"]["a"].update(
                type="MISTRAL_ALUT3" if clustered else "MISTRAL_FF", cluster="a" if clustered else "")
            current = f.group_target(source)
            candidate = copy.deepcopy(source); candidate["moves"][0]["to_bel"] = "free"
            with self.assertRaisesRegex(f.Rejected, "register, cluster"):
                displaced.validate_plan(source["origin"], current, candidate)


if __name__ == "__main__": unittest.main()
