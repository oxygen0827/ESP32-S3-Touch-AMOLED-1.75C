from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
SCRIPTS = REPO / "scripts"
sys.path.insert(0, str(SCRIPTS))

import classify_changes  # noqa: E402


class ClassifyChangesTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.config = classify_changes._load_config(REPO / "ci-routing-config.json")

    def classify(self, *paths: str) -> dict[str, object]:
        return classify_changes.classify(list(paths), self.config)

    def test_markdown_never_selects_examples(self) -> None:
        result = self.classify(
            "README.md",
            "examples/esp-idf/01_AXP2101/README.md",
            "examples/arduino/examples/01_HelloWorld/README.md",
            "examples/arduino/libraries/lvgl/README.md",
        )
        self.assertEqual(result["esp_idf"]["mode"], "none")
        self.assertEqual(result["arduino"]["mode"], "none")
        self.assertTrue(result["docs_only"])

    def test_direct_sources_select_only_the_affected_entries(self) -> None:
        result = self.classify(
            "examples/esp-idf/02_lvgl_demo_v9/main/main.c",
            "examples/esp-idf/02_lvgl_demo_v9/CMakeLists.txt",
            "examples/arduino/examples/06_ES7210/06_ES7210.ino",
        )
        self.assertEqual(result["esp_idf"], {
            "mode": "selected",
            "selectors": ["examples/esp-idf/02_lvgl_demo_v9"],
        })
        self.assertEqual(result["arduino"], {
            "mode": "selected",
            "selectors": ["examples/arduino/examples/06_ES7210"],
        })

    def test_governance_only_changes_select_no_examples(self) -> None:
        result = self.classify("LICENSE", ".github/ISSUE_TEMPLATE/config.yml")
        self.assertEqual(result["esp_idf"]["mode"], "none")
        self.assertEqual(result["arduino"]["mode"], "none")

    def test_shared_and_global_inputs(self) -> None:
        idf = self.classify("config/sdkconfig.defaults")
        self.assertEqual(idf["esp_idf"]["mode"], "all")
        self.assertEqual(idf["arduino"]["mode"], "none")

        arduino = self.classify("examples/arduino/libraries/Mylibrary/pin_config.h")
        self.assertEqual(arduino["esp_idf"]["mode"], "none")
        self.assertEqual(arduino["arduino"]["mode"], "all")

        global_result = self.classify(".github/workflows/examples.yml")
        self.assertEqual(global_result["esp_idf"]["mode"], "all")
        self.assertEqual(global_result["arduino"]["mode"], "all")

    def test_firmware_is_reported_but_never_built(self) -> None:
        result = self.classify(
            "Firmware/README.md",
            "Firmware/project/main/app.c",
            "Firmware/factory.bin",
            "Firmware/resources.zip",
        )
        self.assertEqual(result["esp_idf"]["mode"], "none")
        self.assertEqual(result["arduino"]["mode"], "none")
        self.assertEqual(len(result["firmware_paths"]), 4)
        self.assertEqual(result["release_artifact_paths"], [
            "Firmware/factory.bin",
            "Firmware/resources.zip",
        ])
        self.assertFalse(result["docs_only"])

    def test_unknown_non_document_path_routes_all(self) -> None:
        result = self.classify("tooling/custom.build")
        self.assertEqual(result["esp_idf"]["mode"], "all")
        self.assertEqual(result["arduino"]["mode"], "all")
        self.assertEqual(result["unknown_paths"], ["tooling/custom.build"])

    def test_manual_dispatch_routes_all(self) -> None:
        result = classify_changes.classify([], self.config, force_all=True)
        self.assertEqual(result["esp_idf"]["mode"], "all")
        self.assertEqual(result["arduino"]["mode"], "all")
        self.assertFalse(result["docs_only"])

    def test_exact_workflow_cli_handles_rename_and_deletion(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            changes = tmp_path / "changed-files.txt"
            output = tmp_path / "github-output.txt"
            changes.write_text(
                "R100\texamples/esp-idf/01_AXP2101/main/old.c\tdocs/old.md\n"
                "D\texamples/arduino/examples/02_GFX_AsciiTable/02_GFX_AsciiTable.ino\n",
                encoding="utf-8",
            )
            command = [
                sys.executable,
                str(SCRIPTS / "classify_changes.py"),
                "--repo", str(REPO),
                "--changed-files-from", str(changes),
                "--config", str(REPO / "ci-routing-config.json"),
                "--github-output", str(output),
            ]
            completed = subprocess.run(command, check=False, capture_output=True, text=True)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            values = dict(
                line.split("=", 1)
                for line in output.read_text(encoding="utf-8").splitlines()
            )
            result = json.loads(values["routing"])
            self.assertEqual(result["esp_idf"]["selectors"], [
                "examples/esp-idf/01_AXP2101"
            ])
            self.assertEqual(result["arduino"]["selectors"], [
                "examples/arduino/examples/02_GFX_AsciiTable"
            ])

    def test_empty_changed_file_scope_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            changes = Path(tmp) / "empty.txt"
            changes.write_text("", encoding="utf-8")
            completed = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPTS / "classify_changes.py"),
                    "--repo", str(REPO),
                    "--changed-files-from", str(changes),
                    "--config", str(REPO / "ci-routing-config.json"),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(completed.returncode, 2)
            self.assertIn("changed-file set is empty", completed.stderr)

    def test_workflow_uses_the_tested_classifier_invocation(self) -> None:
        workflow = (REPO / ".github/workflows/examples.yml").read_text(encoding="utf-8")
        self.assertIn(
            "scripts/classify_changes.py --repo . --changed-files-from changed-files.txt "
            "--config ci-routing-config.json --github-output \"$GITHUB_OUTPUT\"",
            workflow,
        )


if __name__ == "__main__":
    unittest.main()
