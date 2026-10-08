import unittest
import analogue_fanout_screen as experiment


class GateTests(unittest.TestCase):
    def setUp(self):
        self.row = dict(status="analogue_timing_failure", legal_route=True)
        self.report = dict(timing_summary=dict(clocks={
            "pixel_clk": dict(setup_wns_ns=-.1, hold_wns_ns=.7),
            "system": dict(setup_wns_ns=1, hold_wns_ns=.7)}),
            critical_paths=[dict(to="posedge pixel_clk", **{"from": "posedge pixel_clk"}, path=[dict(net=experiment.NET)])],
            detailed_net_timings=[dict(net=experiment.NET, endpoints=[dict(cell=str(i), port="D") for i in range(65)])])

    def test_reproduced_target_accepts(self):
        self.assertTrue(experiment.reproduce_gate(self.row, self.report))

    def test_success_or_timeout_is_not_reproduction(self):
        for status in ("completed", "timeout"):
            row = dict(self.row, status=status)
            self.assertFalse(experiment.reproduce_gate(row, self.report))

    def test_other_clock_failure_blocks(self):
        self.report["timing_summary"]["clocks"]["system"]["hold_wns_ns"] = -.1
        self.assertFalse(experiment.reproduce_gate(self.row, self.report))

    def test_cross_domain_path_does_not_qualify(self):
        self.report["critical_paths"][0]["from"] = "<async>"
        self.assertFalse(experiment.reproduce_gate(self.row, self.report))

    def test_duplicate_sinks_do_not_cross_cutoff(self):
        self.report["detailed_net_timings"][0]["endpoints"][-1] = dict(cell="0", port="D")
        self.assertFalse(experiment.reproduce_gate(self.row, self.report))

    def test_zero_setup_respects_strict_positive_gate(self):
        self.report["timing_summary"]["clocks"]["pixel_clk"]["setup_wns_ns"] = 0
        self.assertTrue(experiment.reproduce_gate(self.row, self.report))


if __name__ == "__main__":
    unittest.main()
