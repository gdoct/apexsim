"""The verifier's items file: edits keep the rest of the file, the review rules, the state changes.

    python -m unittest discover -s scripts/verify/tests
"""

import json
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from verifier import schema  # noqa: E402

ITEM = {
    "id": "v-r1-rain-light", "title": "The rain light in the wet", "area": "cars", "kind": "screen",
    "check_by": "r1-spa-wet", "created": "2026-10-20", "commit": "ee3c368f",
    "what": "The rain light in the wet; a body with no car_rainlight slot has none.", "status": "judged",
    "evidence": [{"run": "r1-spa-wet", "date": "2026-10-25", "shots": [3, 4], "kind": "shot"}],
    "verdict": {"date": "2026-10-25", "by": "agent", "says": "not-readable", "text": "Not readable at these poses."},
}
OTHER = {"id": "v-r2-marbles", "title": "Marbles", "area": "track", "kind": "screen", "check_by": "r2-stint",
         "what": "Marbles and the rubbered line.", "status": "open"}


class ItemsFileTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.path = Path(self.dir.name) / "items.yml"
        f = schema.ItemsFile(self.path)
        f.add(ITEM)
        f.add(OTHER)
        f.save()
        # A hand-written comment in each item, the way the agent may leave one.
        text = self.path.read_text(encoding="utf-8")
        text = text.replace("  status: judged\n", "  # judged from the cockpit shots\n  status: judged\n")
        text = text.replace("- id: v-r2-marbles\n", "# R2\n- id: v-r2-marbles\n")
        self.path.write_text(text, encoding="utf-8")

    def tearDown(self):
        self.dir.cleanup()

    def test_round_trip(self):
        items = schema.ItemsFile(self.path).items()
        self.assertEqual(items[0], ITEM)
        self.assertEqual(items[1], OTHER)

    def test_a_review_changes_only_status_and_review(self):
        before = self.path.read_text(encoding="utf-8")
        f = schema.ItemsFile(self.path)
        schema.apply_review(f, "v-r1-rain-light", "confirmed", {"at": "2026-10-26T21:14:00", "score": 4, "comment": ""})
        f.save()
        after = self.path.read_text(encoding="utf-8")
        removed = [l for l in before.splitlines() if l not in after.splitlines()]
        added = [l for l in after.splitlines() if l not in before.splitlines()]
        self.assertEqual(removed, ["  status: judged"])
        self.assertEqual(added, ["  status: confirmed", "  review:", "    at: 2026-10-26T21:14:00", "    score: 4",
                                 "    processed: false"])
        self.assertIn("# judged from the cockpit shots", after)
        self.assertIn("# R2\n- id: v-r2-marbles", after)
        self.assertEqual(schema.ItemsFile(self.path).get("v-r1-rain-light")["review"]["at"], "2026-10-26T21:14:00")

    def test_review_rules(self):
        self.assertIsNone(schema.review_problem("confirmed", {"score": 4}))
        self.assertIsNone(schema.review_problem("later", {}))
        self.assertIn("comment", schema.review_problem("not-right", {"comment": " "}))
        self.assertIn("comment", schema.review_problem("cant-tell", {}))
        self.assertIn("not-right", schema.review_problem("confirmed", {"score": 2}))
        self.assertIsNotNone(schema.review_problem("judged", {}))
        f = schema.ItemsFile(self.path)
        with self.assertRaises(ValueError):
            schema.apply_review(f, "v-r1-rain-light", "not-right", {"score": 2})

    def test_save_refuses_a_file_changed_on_disk(self):
        f = schema.ItemsFile(self.path)
        loaded = f.loaded_digest
        self.path.write_text(self.path.read_text(encoding="utf-8") + "\n", encoding="utf-8")
        schema.apply_review(f, "v-r1-rain-light", "later", {})
        with self.assertRaises(schema.Changed):
            f.save(expect_digest=loaded)

    def test_new_evidence_reopens_a_review_and_keeps_it_in_notes(self):
        f = schema.ItemsFile(self.path)
        schema.apply_review(f, "v-r1-rain-light", "not-right", {"at": "2026-10-26T21:00:00", "score": 2, "comment": "Too dim."})
        schema.add_evidence(f, "v-r1-rain-light", {"run": "r1-spa-wet-2", "date": "2026-10-27", "shots": [1], "kind": "shot"})
        item = f.get("v-r1-rain-light")
        self.assertEqual(item["status"], "judged")
        self.assertNotIn("review", item)
        self.assertEqual(item["notes"][-1], {"date": "2026-10-26", "text": "Review (not-right, score 2): Too dim."})
        self.assertEqual([e["run"] for e in item["evidence"]], ["r1-spa-wet", "r1-spa-wet-2"])

    def test_a_verdict_judges_an_open_item(self):
        f = schema.ItemsFile(self.path)
        schema.set_verdict(f, "v-r2-marbles", {"date": "2026-10-27", "by": "agent", "says": "looks-right", "text": "Seen."})
        self.assertEqual(f.get("v-r2-marbles")["status"], "judged")

    def test_validate(self):
        bad = dict(OTHER, id="r2 marbles", area="weather", status="judged")
        errors, _ = schema.validate([ITEM, bad, dict(ITEM)], check_paths=False)
        text = "\n".join(errors)
        self.assertIn("id is not", text)
        self.assertIn("area 'weather'", text)
        self.assertIn("judged without a verdict", text)
        self.assertIn("id used twice", text)

    def test_register_run(self):
        original = schema.ITEMS_YML
        schema.ITEMS_YML = self.path
        try:
            done = schema.register_run("r2-stint", [{"item": "v-r2-marbles"}, {}, {"item": ["v-r2-marbles", "v-nope"]}],
                                       "abcd1234", "2026-10-27")
        finally:
            schema.ITEMS_YML = original
        self.assertEqual(done, ["v-r2-marbles: shots 1, 3", "v-nope: no such item"])
        ev = schema.ItemsFile(self.path).get("v-r2-marbles")["evidence"]
        self.assertEqual(ev, [{"run": "r2-stint", "date": "2026-10-27", "commit": "abcd1234", "shots": [1, 3], "kind": "shot"}])


class SittingAndStaleTest(unittest.TestCase):
    def test_answers(self):
        self.assertEqual(schema.answer_review(["OK", "weak"], "OK", "2026-10-26T20:00:00"),
                         ("confirmed", {"at": "2026-10-26T20:00:00", "answer": "OK"}))
        self.assertEqual(schema.answer_review(["OK", "weak"], "weak", "t", " at Lesmo "),
                         ("not-right", {"at": "t", "answer": "weak", "comment": "weak: at Lesmo"}))
        self.assertEqual(schema.answer_review([], "not felt", "t")[0], "not-right")
        self.assertEqual(schema.answer_review(["done"], "done", "t", "910")[1]["comment"], "910")
        with self.assertRaises(ValueError):
            schema.answer_review(["OK", "weak"], "strong", "t")
        status, review = schema.answer_review(["OK", "weak"], "weak", "t")
        self.assertIsNone(schema.review_problem(status, review))

    def test_every_sitting_check_is_an_item(self):
        ids = {i["id"] for i in schema.ItemsFile().items()}
        closed = {i["id"] for i in schema.ItemsFile(schema.CLOSED_YML).items()} if schema.CLOSED_YML.exists() else set()
        for block in schema.load_sittings():
            for check in block["checks"]:
                self.assertIn(check["item"], ids | closed)

    def test_stale(self):
        ancestry = {("aaaa", "bbbb"): True, ("bbbb", "aaaa"): False}
        known = lambda older, newer: ancestry.get((older, newer))  # noqa: E731
        item = dict(ITEM, commit="aaaa", evidence=[{"run": "r1", "date": "2026-10-25", "commit": "bbbb"}])
        self.assertIsNone(schema.stale(item, known))
        item = dict(ITEM, commit="bbbb", evidence=[{"run": "r1", "date": "2026-10-25", "commit": "aaaa"}])
        self.assertEqual(schema.stale(item, known), "r1")
        # Unknown commits, or none: the dates decide.
        item = dict(ITEM, commit="cccc", created="2026-10-26", evidence=[{"run": "r1", "date": "2026-10-25", "commit": "dddd"}])
        self.assertEqual(schema.stale(item, known), "r1")
        item = dict(ITEM, created="2026-10-20", evidence=[{"run": "r1", "date": "2026-10-25"}])
        self.assertIsNone(schema.stale(item, known))
        self.assertIsNone(schema.stale(dict(ITEM, evidence=[]), known))

    def test_history(self):
        with tempfile.TemporaryDirectory() as d:
            original = schema.VERIFY_OUT
            schema.VERIFY_OUT = Path(d)
            try:
                work = Path(d) / "r1"
                for n, commit in enumerate(["c1", "c2", "c3", "c4", "c5"]):
                    self.assertEqual(schema.keep_previous(work) is not None, n > 0)
                    (work / "shots").mkdir(parents=True, exist_ok=True)
                    (work / "shots" / "01.jpg").write_text(commit)
                    schema.stamp_run(work, commit, f"2026-10-2{n}")
                self.assertEqual([h["commit"] for h in schema.history("r1")], ["c5", "c4", "c3", "c2"])
                self.assertEqual(schema.shot_path("r1", 1, "c3").read_text(), "c3")
                self.assertEqual(schema.shot_path("r1", 1, "c5").read_text(), "c5")
                self.assertEqual(schema.shot_path("r1", 1).read_text(), "c5")
                # A generation no longer kept falls back to the latest shots.
                self.assertEqual(schema.shot_path("r1", 1, "c1").read_text(), "c5")
            finally:
                schema.VERIFY_OUT = original


class ServerTest(unittest.TestCase):
    """POST /api/reviews: the digest check, the rules, and the write."""

    def setUp(self):
        import verifier.schema  # noqa: F401
        sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
        import importlib.util
        spec = importlib.util.spec_from_file_location("verifier_cli", Path(__file__).resolve().parent.parent / "verifier.py")
        self.cli = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.cli)
        self.dir = tempfile.TemporaryDirectory()
        self.path = Path(self.dir.name) / "items.yml"
        f = schema.ItemsFile(self.path)
        f.add(ITEM)
        f.save()
        self.original = schema.ITEMS_YML
        schema.ITEMS_YML = self.path
        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), self.cli.Handler)
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        self.base = f"http://127.0.0.1:{self.httpd.server_address[1]}"

    def tearDown(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        schema.ITEMS_YML = self.original
        self.dir.cleanup()

    def call(self, path, body=None):
        req = urllib.request.Request(self.base + path, data=json.dumps(body).encode() if body is not None else None,
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req) as res:
                return res.status, json.loads(res.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read())

    def test_reviews(self):
        status, items = self.call("/api/items")
        self.assertEqual(status, 200)
        review = {"id": "v-r1-rain-light", "status": "not-right", "review": {"at": "2026-10-26T21:14:00", "score": 2}}
        status, body = self.call("/api/reviews", {"digest": items["digest"], "reviews": [review]})
        self.assertEqual(status, 400)
        self.assertIn("comment", body["error"])
        status, _ = self.call("/api/reviews", {"digest": "stale", "reviews": []})
        self.assertEqual(status, 409)
        review["review"]["comment"] = "Too dim from the chase camera."
        status, body = self.call("/api/reviews", {"digest": items["digest"], "reviews": [review]})
        self.assertEqual(status, 200)
        item = schema.ItemsFile(self.path).get("v-r1-rain-light")
        self.assertEqual(item["status"], "not-right")
        self.assertEqual(item["review"]["comment"], "Too dim from the chase camera.")
        self.assertFalse(item["review"]["processed"])

    def test_a_sitting_answer(self):
        f = schema.ItemsFile(self.path)
        f.add(dict(OTHER, id="v-sit-a-monza-letgo", kind="sitting", check_by="a-monza"))
        f.save()
        _, items = self.call("/api/items")
        answer = {"id": "v-sit-a-monza-letgo", "answer": "rings", "note": "twice", "at": "2026-10-26T20:00:00"}
        status, body = self.call("/api/reviews", {"digest": items["digest"], "reviews": [answer]})
        self.assertEqual(status, 200, body)
        item = schema.ItemsFile(self.path).get("v-sit-a-monza-letgo")
        self.assertEqual(item["status"], "not-right")
        self.assertEqual(item["review"]["answer"], "rings")
        self.assertEqual(item["review"]["comment"], "rings: twice")
        _, items = self.call("/api/items")
        status, body = self.call("/api/reviews", {"digest": items["digest"], "reviews": [dict(answer, answer="loud")]})
        self.assertEqual(status, 400)
        status, sittings = self.call("/api/sittings")
        self.assertEqual(status, 200)
        self.assertTrue(any(c["item"] == "v-sit-a-monza-letgo" for b in sittings["blocks"] for c in b["checks"]))


class BacklogTest(unittest.TestCase):
    def test_the_backlog_is_valid(self):
        f = schema.ItemsFile()
        closed = schema.ItemsFile(schema.CLOSED_YML).items() if schema.CLOSED_YML.exists() else []
        errors, _ = schema.validate(f.items(), closed, check_paths=False)
        self.assertEqual(errors, [])


if __name__ == "__main__":
    unittest.main()
