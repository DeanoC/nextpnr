#!/usr/bin/env python3
"""Fault injection for native observation parsing and whole-bundle retention."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import sys

sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location("feedback", Path(__file__).resolve().parents[1] / "routed_placement.py")
feedback = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(feedback)


def fixture(margin=-100, placement="initial"):
    """Synthetic protocol data; native extraction is covered by C++/route tests."""
    row = dict(launch=["clk", 0], capture=["clk", 0], setup_timed=True, hold_related=True,
               setup_window_ps=2000, setup_margin_ps=margin, hold_margin_ps=30, max_path_ps=2100, min_path_ps=30)
    endpoint = dict(cell="capture", port="DATAIN", **{"class": 2}, rows=[row])
    other = copy.deepcopy(endpoint)
    other["cell"] = "other"
    other["rows"][0]["setup_margin_ps"] = 100
    origin = dict(design={"cells": ["launch", "capture", "other"], "ports": {}}, placement={"launch": ["initial", 1]},
                  clocks={"clk": 2000}, timing_settings={}, architecture={}, ordering={}, rng="123")
    current = copy.deepcopy(origin)
    current["placement"]["launch"][0] = placement
    passed = margin >= 0
    return {"native/placement.json": dict(version=1, origin=origin, current=current),
            "native/routed-identity.json": current,
            "native/completion.json": dict(version=2, process_complete=True, route_complete=True,
                analysis_complete=True, graph_matches=True, coverage_complete=True, timing_pass=passed,
                rbf_bytes=len((json.dumps({"whole": placement}, indent=2) + "\n").encode()),
                errors_before_signoff=0, errors_after_signoff=0 if passed else 1,
                errors_at_completion=0 if passed else 1),
            "native/endpoints.json": dict(version=2, analysis_complete=True, final_analogue_model=True,
                complete=True, expected_endpoint_count=2, endpoints=[endpoint, other], static_endpoints=[],
                inactive_endpoints=[], clock_inputs=[], unsupported=[], primitive_clocks=[
                    dict(cell="capture", port="CLK", clock="clk", period_ps=[2000, 2000], high_ps=[1000, 1000],
                         low_ps=[1000, 1000], route_ps=[0, 100], required_ps=[1000, 400, 400], actual_ps=[2000, 900, 900])]),
            "timing.json": dict(timing_summary=dict(final_analogue_model=True),
                                fmax=dict(clk=dict(constraint=500, achieved=450))),
            "native/model.json": dict(timing_summary=dict(final_analogue_model=True),
                fmax=dict(clk=dict(constraint=500, achieved=450)),
                cohort_route_model=dict(version=1, identity={"device": "fixture"}, arcs=[{}])),
            "routed.json": {"whole": placement}, "core.rbf": {"whole": placement}}


def preserved(data):
    current = data["native/placement.json"]["current"]
    current["architecture"]["preserve_routes"] = True
    data["native/placement.json"]["origin"]["architecture"]["preserve_routes"] = True
    data["native/routes.json"] = dict(version=1, parent=copy.deepcopy(current), nets={})
    data["native/model.json"]["cohort_route_model"]["route_costs"] = dict(
        version=3, routes=copy.deepcopy(data["native/routes.json"]), calibrated=True, background={}, reservations={},
        prior=1.0, observed={}, types=[[0, 0, 0] for _ in range(256)])
    data["native/completion.json"]["preserved_routes_match"] = True
    return data


def bundle(path, data, code=None):
    path.mkdir()
    for name, value in data.items():
        target = path / name
        target.parent.mkdir(exist_ok=True)
        feedback.save(target, value)
    if code is None:
        code = 0 if data["native/completion.json"]["timing_pass"] else 1
    feedback.save(path / "result.json", dict(exit_code=code, sha256={
        str(p.relative_to(path)): feedback.digest(p) for p in path.rglob("*") if p.is_file()}))
    return path, code


def grouped(margin=-100, placement="initial", plan=None):
    data = fixture(margin, placement)
    for key in ("origin", "current"):
        data["native/placement.json"][key]["architecture"]["group_moves"] = True
    preserved(data)
    data["native/completion.json"]["group_placement_match"] = True
    if plan is not None:
        data["native/placement.json"]["group_plan"] = plan
    return data


def group_plan(parent, target):
    return dict(version=3, origin=copy.deepcopy(parent["origin"]), parent=copy.deepcopy(parent["current"]),
        evaluation="full_route_required", moves=[dict(cell="launch", from_bel="initial", to_bel=target, strength=1)])


class Retention(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.serial = 0

    def observation(self, data):
        self.serial += 1
        path, code = bundle(self.root / str(self.serial), data)
        return feedback.read_observation(path, code)

    def test_complete_observation_and_20ps_gain(self):
        parent = self.observation(fixture())
        candidate = self.observation(fixture(-80, "alternative"))
        self.assertEqual(feedback.compare(parent, candidate), (-80, -80))
        self.assertFalse(candidate["status"]["timing_pass"])

    def test_group_plan_has_no_timing_claim_and_requires_exact_final_bindings(self):
        parent = grouped()["native/placement.json"]
        plan = group_plan(parent, "alternative")
        data = grouped(-70, "alternative", plan)
        self.assertEqual(feedback.group_target(plan), data["native/placement.json"]["current"])
        self.assertEqual(feedback.compare(self.observation(grouped()), self.observation(data)), (-70, -70))
        mutations = [
            lambda d: d["native/completion.json"].pop("group_placement_match"),
            lambda d: d["native/routed-identity.json"]["placement"].update(launch=["other", 1]),
            lambda d: d["native/placement.json"].pop("group_plan"),
            lambda d: d["native/placement.json"]["group_plan"].update(predicted={}),
            lambda d: d["native/placement.json"]["group_plan"]["moves"][0].update(from_bel="stale"),
            lambda d: d["native/placement.json"].update(group_plan="invalid"),
        ]
        for mutation in mutations:
            changed = copy.deepcopy(data); mutation(changed)
            with self.assertRaises(feedback.Rejected): self.observation(changed)

    def test_group_mode_routes_legal_proposals_and_keeps_all_measured_guards(self):
        for regress in (False, True):
            with self.subTest(regress=regress):
                root = self.root / ("regression" if regress else "gain"); root.mkdir()
                class Workers:
                    def __init__(worker): worker.root = root
                    def run(worker, label, plan=None, model=None, routes=None):
                        if label == "baseline": return bundle(root / label, grouped())
                        if label.endswith("proposals"):
                            parent = feedback.load(root / "baseline/native/placement.json")
                            return bundle(root / label, {"native/proposals.json": dict(version=2, kind="group_moves",
                                restored=True, parent=parent["current"], plans=[group_plan(parent, "alternative")])}, 0)
                        self.assertEqual(routes, root / "baseline/native/routes.json")
                        data = grouped(-70, "alternative", feedback.load(plan))
                        if regress: data["native/endpoints.json"]["endpoints"][1]["rows"][0]["setup_margin_ps"] = 99
                        return bundle(root / label, data)
                self.assertFalse(feedback.optimize(Workers(), 1, 4, preserve_routes=True, group_moves=True))
                best = feedback.load(root / "best.json")
                self.assertEqual(best["bundle"], "baseline" if regress else "round-1-candidate-0")
                self.assertFalse(best["timing_pass"])

    def test_group_mode_cannot_use_an_ordinary_proposal_set(self):
        class Workers:
            root = self.root
            def run(worker, label, plan=None, model=None, routes=None):
                if label == "baseline": return bundle(worker.root / label, grouped())
                parent = feedback.load(worker.root / "baseline/native/placement.json")
                return bundle(worker.root / label, {"native/proposals.json": dict(version=1, restored=True,
                    parent=parent["current"], plans=[])}, 0)
        with self.assertRaisesRegex(feedback.Rejected, "invalid or stale"):
            feedback.optimize(Workers(), 1, 4, preserve_routes=True, group_moves=True)
        self.assertEqual(feedback.load(self.root / "best.json")["bundle"], "baseline")

    def test_full_reroute_groups_need_no_parent_tree_or_reservation_replay(self):
        def full(margin=-100, target="initial", plan=None):
            data = grouped(margin, target, plan)
            for key in ("origin", "current"):
                data["native/placement.json"][key]["architecture"].pop("preserve_routes")
            data.pop("native/routes.json")
            data["native/model.json"]["cohort_route_model"].pop("route_costs")
            data["native/completion.json"].pop("preserved_routes_match")
            return data
        parent_data = full()
        plan = group_plan(parent_data["native/placement.json"], "alternative")
        parent = self.observation(parent_data)
        candidate = self.observation(full(-70, "alternative", plan))
        self.assertEqual(feedback.compare(parent, candidate), (-70, -70))
        with self.assertRaisesRegex(feedback.Rejected, "different fresh placement/recipe"):
            feedback.compare(self.observation(grouped()), candidate)

    def test_rejects_missing_or_forged_observations(self):
        mutations = [
            lambda d: d["native/completion.json"].update(version=1),
            lambda d: d["native/endpoints.json"].update(version=1),
            lambda d: d["native/endpoints.json"].update(unsupported=[{"cell": "unknown"}]),
            lambda d: d["native/endpoints.json"].update(expected_endpoint_count=3),
            lambda d: d["native/endpoints.json"]["endpoints"][0].update(rows=[]),
            lambda d: d["native/endpoints.json"]["endpoints"].append(d["native/endpoints.json"]["endpoints"][0]),
            lambda d: d["native/completion.json"].update(errors_at_completion=2),
            lambda d: d["native/completion.json"].update(rbf_bytes=0),
            lambda d: d["native/completion.json"].update(process_complete=False),
            lambda d: d["native/endpoints.json"].update(final_analogue_model=False),
            lambda d: d["native/model.json"].update(cohort_route_model={}),
            lambda d: d["native/routed-identity.json"].update(design={}),
            lambda d: d["native/endpoints.json"]["endpoints"][0]["rows"][0].update(setup_margin_ps=-80.5),
        ]
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                data = fixture(-80)
                mutation(data)
                with self.assertRaises(feedback.Rejected):
                    self.observation(data)

    def test_tampered_artifact_cannot_authorize_retention(self):
        path, code = bundle(self.root / "tamper", fixture(-80))
        (path / "core.rbf").write_bytes(b"different bitstream")
        with self.assertRaisesRegex(feedback.Rejected, "artifact changed"):
            feedback.read_observation(path, code)

    def test_mutated_cumulative_plan_cannot_be_replayed_from_retained_descriptor(self):
        path, code = bundle(self.root / "dependency", fixture(-80))
        plan = self.root / "plan.json"
        plan.write_text("original plan")
        manifest = feedback.load(path / "result.json")
        manifest["dependencies_sha256"] = {str(plan): feedback.digest(plan)}
        (path / "result.json").write_text(json.dumps(manifest))
        feedback.verify_bundle(path, code)
        plan.write_text("changed plan")
        with self.assertRaisesRegex(feedback.Rejected, "dependency changed"):
            feedback.verify_bundle(path, code)

    def test_gain_cannot_hide_endpoint_hold_clock_or_contract_regression(self):
        parent = self.observation(fixture())
        mutations = [
            lambda d: d["native/endpoints.json"]["endpoints"][1]["rows"][0].update(setup_margin_ps=99),
            lambda d: d["native/endpoints.json"]["endpoints"][0]["rows"][0].update(hold_margin_ps=0),
            lambda d: d["native/endpoints.json"]["endpoints"][0]["rows"][0].update(capture=["clk", 1]),
            lambda d: d["native/endpoints.json"]["primitive_clocks"][0].update(actual_ps=[2000, 899, 900]),
            lambda d: d["native/placement.json"]["current"].update(rng="different"),
        ]
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                data = fixture(-70, "alternative")
                mutation(data)
                candidate = self.observation(data)
                with self.assertRaises(feedback.Rejected):
                    feedback.compare(parent, candidate)
        data = fixture(-70)
        for file in ("timing.json", "native/model.json"):
            data[file]["fmax"]["clk"]["achieved"] = 449.99
        with self.assertRaisesRegex(feedback.Rejected, "Fmax"):
            feedback.compare(parent, self.observation(data))

    def test_small_gain_and_worse_unrelated_extrema_are_rejected(self):
        with self.assertRaisesRegex(feedback.Rejected, "20 ps"):
            feedback.compare(self.observation(fixture()), self.observation(fixture(-81)))
        a, b = fixture(), fixture(-70)
        for data in (a, b):
            row = data["native/endpoints.json"]["endpoints"][1]["rows"][0]
            row.update(setup_timed=False, hold_related=False, setup_window_ps=None, setup_margin_ps=None,
                       hold_margin_ps=None)
        b["native/endpoints.json"]["endpoints"][1]["rows"][0]["min_path_ps"] = 29
        with self.assertRaisesRegex(feedback.Rejected, "unrelated"):
            feedback.compare(self.observation(a), self.observation(b))

    def test_saved_recipe_cannot_override_controller_or_timing(self):
        for flag in ("--force", "--timing-allow-fail", "--no-tmdriv", "--gpu-device=0", "--set", "--pre-route"):
            with self.subTest(flag=flag), self.assertRaises(feedback.Rejected):
                feedback.validate_recipe([flag])
        feedback.validate_recipe(["--arrival-pin-assignment", "--placer-heap-timingweight", "300",
                                  "--gpu-opt=analogue_attempts=4"])
        for value in ("device=0", "cpu=1", "telemetryPath=7", "cost=NaN"):
            with self.subTest(value=value), self.assertRaises(feedback.Rejected):
                feedback.validate_recipe(["--gpu-opt=" + value])

    def test_strict_json_rejects_duplicates_and_nonfinite_numbers(self):
        path = self.root / "invalid.json"
        for text in ('{"version": 1, "version": 2}', '{"delay": NaN}'):
            path.write_text(text)
            with self.assertRaises(feedback.Rejected):
                feedback.load(path)

    def test_preserved_routes_require_matching_parent_and_native_completion(self):
        data = preserved(fixture())
        self.observation(data)
        wrong = copy.deepcopy(data)
        wrong["native/routes.json"]["parent"]["rng"] = "changed"
        with self.assertRaisesRegex(feedback.Rejected, "preserved-route"):
            self.observation(wrong)
        wrong = copy.deepcopy(data)
        wrong["native/completion.json"]["preserved_routes_match"] = False
        with self.assertRaisesRegex(feedback.Rejected, "preserved-route"):
            self.observation(wrong)

    def test_branch_mode_requires_explicit_matching_completion_and_target(self):
        data = grouped()
        data["command.json"] = ["nextpnr", "--routed-placement-preserve-branches"]
        data["native/completion.json"]["preserved_branches_match"] = True
        current = data["native/placement.json"]["current"]
        data["native/route-branches.json"] = dict(version=1, parent=copy.deepcopy(current),
                                                 target=copy.deepcopy(current), nets={})
        self.observation(data)
        for mutation in (lambda d:d["native/completion.json"].pop("preserved_branches_match"),
                         lambda d:d["native/route-branches.json"]["target"].update(rng="stale"),
                         lambda d:d["command.json"].clear()):
            wrong = copy.deepcopy(data); mutation(wrong)
            with self.assertRaises(feedback.Rejected): self.observation(wrong)

    def test_branch_prefix_requires_sealed_source_and_complete_final_edges(self):
        data = grouped(); current = data["native/placement.json"]["current"]
        source_physical = dict(source="source", sinks=["stationary", "old"])
        target_physical = dict(source="source", sinks=["stationary", "new"])
        prefix = [["source", ""], ["trunk", "source-trunk"], ["stationary", "trunk-stationary"]]
        source_tree = dict(physical=source_physical, wires=copy.deepcopy(prefix+[["old", "trunk-old"]]))
        target_tree = dict(physical=target_physical, wires=copy.deepcopy(prefix+[["new", "trunk-new"]]))
        imported = dict(version=1, parent=copy.deepcopy(current), nets=dict(data=source_tree))
        source_path = self.root/"branch-source.json"; feedback.save(source_path, imported)
        data["command.json"] = ["nextpnr", "--routed-placement-preserve-branches", "--routed-placement-routes", str(source_path)]
        data["native/routes.json"]["nets"] = dict(data=target_tree)
        data["native/model.json"]["cohort_route_model"]["route_costs"]["routes"] = copy.deepcopy(data["native/routes.json"])
        data["native/completion.json"]["preserved_branches_match"] = True
        data["native/route-branches.json"] = dict(version=1, parent=copy.deepcopy(current), target=copy.deepcopy(current),
            nets=dict(data=dict(source=copy.deepcopy(source_tree), physical=copy.deepcopy(target_physical),
                                sinks=["stationary"], wires=copy.deepcopy(prefix))))
        def observed(value, sealed=True):
            self.serial += 1; path, code = bundle(self.root/str(self.serial), value)
            manifest = feedback.load(path/"result.json")
            if sealed:
                manifest["dependencies_sha256"] = {str(source_path):feedback.digest(source_path)}
                (path/"result.json").write_text(json.dumps(manifest))
            return feedback.read_observation(path, code)
        observed(data)
        with self.assertRaisesRegex(feedback.Rejected, "unsealed branch"): observed(data, False)
        wrong = copy.deepcopy(data)
        wrong["native/route-branches.json"]["nets"]["data"]["wires"][1][1] = "different"
        with self.assertRaisesRegex(feedback.Rejected, "protected branch edge"): observed(wrong)
        wrong = copy.deepcopy(data)
        wrong["native/routes.json"]["nets"]["data"]["wires"].pop()
        wrong["native/model.json"]["cohort_route_model"]["route_costs"]["routes"] = copy.deepcopy(wrong["native/routes.json"])
        with self.assertRaisesRegex(feedback.Rejected, "incomplete extended"): observed(wrong)

    def test_preserved_routes_require_matching_native_cost_source(self):
        mutations = [lambda c: c.update(version=0), lambda c: c.pop("observed"), lambda c: c.pop("background"),
                     lambda c: c.pop("reservations"),
                     lambda c: c.update(types=[]), lambda c: c.update(prior=0),
                     lambda c: c["routes"]["parent"].update(rng="stale")]
        for mutate in mutations:
            data = preserved(fixture())
            mutate(data["native/model.json"]["cohort_route_model"]["route_costs"])
            with self.assertRaises(feedback.Rejected):
                self.observation(data)

    def test_rejected_full_route_retains_entire_baseline_bundle(self):
        class Workers:
            root = self.root

            def run(worker, label, plan=None, model=None, routes=None):
                if label.endswith("proposals"):
                    parent = feedback.load(worker.root / "baseline/native/placement.json")
                    proposal = dict(version=1, restored=True, parent=parent["current"], plans=[
                        dict(origin=parent["origin"], parent=parent["current"], moves=[])])
                    return bundle(worker.root / label, {"native/proposals.json": proposal}, code=0)
                data = fixture() if label == "baseline" else fixture(-150, "worse")
                return bundle(worker.root / label, data)

        self.assertFalse(feedback.optimize(Workers(), 2, 4))
        self.assertEqual(feedback.load(self.root / "best.json")["bundle"], "baseline")
        feedback.verify_bundle(self.root / "baseline", 1)
        self.assertFalse(feedback.load(self.root / "decision.json")["timing_pass"])

    def test_proposed_routes_need_matching_request_completion_and_exact_tree(self):
        data = preserved(fixture())
        current = data["native/placement.json"]["current"]
        tree = dict(physical=dict(source="source", sinks=["sink"]), wires=[["source", ""], ["sink", "edge"]])
        data["native/routes.json"]["nets"] = dict(fabric=copy.deepcopy(tree))
        data["native/model.json"]["cohort_route_model"]["route_costs"]["routes"] = copy.deepcopy(data["native/routes.json"])
        data["native/placement.json"]["data_routes"] = dict(version=1, target=copy.deepcopy(current),
            nets=dict(fabric=dict(source="source", sinks=["sink"], wires=copy.deepcopy(tree["wires"]))))
        data["native/route-handoff.json"] = dict(version=1, target=copy.deepcopy(current), nets=dict(fabric=tree))
        data["native/completion.json"]["proposed_routes_match"] = True
        self.observation(data)
        mutations = [lambda d: d["native/completion.json"].pop("proposed_routes_match"),
            lambda d: d["native/placement.json"].update(data_routes="invalid"),
            lambda d: d["native/route-handoff.json"]["target"].update(rng="stale"),
            lambda d: d["native/route-handoff.json"]["nets"]["fabric"]["wires"].pop(),
            lambda d: d["native/placement.json"]["data_routes"]["nets"]["fabric"]["sinks"].append("missing"),
            lambda d: d["native/placement.json"]["data_routes"]["nets"]["fabric"]["wires"].append(["sink", "edge"])]
        for mutate in mutations:
            wrong = copy.deepcopy(data)
            mutate(wrong)
            with self.assertRaises(feedback.Rejected): self.observation(wrong)

    def test_preserved_proposals_cannot_drop_their_selected_trees(self):
        class Workers:
            root = self.root
            def run(worker, label, plan=None, model=None, routes=None):
                if label == "baseline": return bundle(worker.root / label, preserved(fixture()))
                parent = feedback.load(worker.root / "baseline/native/placement.json")
                proposal = dict(version=1, restored=True, parent=parent["current"], plans=[
                    dict(version=1, origin=parent["origin"], parent=parent["current"], moves=[])])
                return bundle(worker.root / label, {"native/proposals.json": proposal}, code=0)
        with self.assertRaisesRegex(feedback.Rejected, "dropped its selected trees"):
            feedback.optimize(Workers(), 1, 4, preserve_routes=True)
        self.assertEqual(feedback.load(self.root / "best.json")["bundle"], "baseline")
        feedback.verify_bundle(self.root / "baseline", 1)

    def test_second_round_replays_measured_winner_and_can_close_timing(self):
        def observed(margin=-100, placement="initial", requested=None):
            data = preserved(fixture(margin, placement))
            if requested is not None:
                data["native/placement.json"]["data_routes"] = copy.deepcopy(requested)
                data["native/completion.json"]["proposed_routes_match"] = True
                data["native/route-handoff.json"] = dict(version=1,
                    target=copy.deepcopy(data["native/placement.json"]["current"]), nets={})
            return data
        class Workers:
            root = self.root

            def run(worker, label, plan=None, model=None, routes=None):
                if label == "baseline":
                    return bundle(worker.root / label, observed())
                if label.endswith("proposals"):
                    selected = feedback.load(worker.root / "best.json")["bundle"]
                    parent = feedback.load(worker.root / selected / "native/placement.json")
                    self.assertEqual(model, worker.root / selected / "native/model.json")
                    self.assertIsNone(routes)
                    if label.startswith("round-2"):
                        self.assertEqual(parent["current"]["placement"]["launch"][0], "round-1")
                        self.assertEqual(plan, worker.root / "round-1-plan-0.json")
                    target_name = label.split("-proposals")[0]
                    target = observed(-70, target_name)["native/placement.json"]["current"]
                    proposal = dict(version=1, restored=True, parent=parent["current"], plans=[
                        dict(version=2, origin=parent["origin"], parent=parent["current"], moves=[],
                             data_routes=dict(version=1, target=target, nets={}))])
                    return bundle(worker.root / label, {"native/proposals.json": proposal}, code=0)
                margin = -70 if label.startswith("round-1") else 10
                selected = feedback.load(worker.root / "best.json")["bundle"]
                self.assertEqual(routes, worker.root / selected / "native/routes.json")
                requested = feedback.load(plan)["data_routes"]
                return bundle(worker.root / label, observed(margin, label.split("-candidate")[0], requested))

        self.assertTrue(feedback.optimize(Workers(), 2, 4, preserve_routes=True))
        best = feedback.load(self.root / "best.json")
        self.assertEqual(best["bundle"], "round-2-candidate-0")
        self.assertTrue(best["qualified"] and best["timing_pass"])
        self.assertEqual(best["plan_sha256"], feedback.digest(self.root / best["plan"]))
        self.assertEqual(best["bundle_sha256"], feedback.digest(self.root / best["bundle"] / "result.json"))
        for name, code in (("baseline", 1), ("round-1-candidate-0", 1), (best["bundle"], 0)):
            feedback.verify_bundle(self.root / name, code)


if __name__ == "__main__":
    unittest.main()
