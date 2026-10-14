"""Exercise the Dockerfile's actual ownership-patch command without Docker."""

import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class OwnershipPatchRecipe(unittest.TestCase):
    dependency = "picoquic"
    patch_name = "0006-own-application-close-reason.patch"
    diagnostic = "picoquic close-reason ownership patch"

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name).resolve()
        self.provider = self.root / self.dependency
        self.provider.mkdir()
        subprocess.run(["git", "init", "-q", str(self.provider)], check=True)
        self.source = self.provider / "reason.c"
        self.source.write_text("borrowed\n", encoding="ascii")
        patch = self.root / "libmoq/patches" / self.dependency / self.patch_name
        patch.parent.mkdir(parents=True)
        patch.write_text(
            "diff --git a/reason.c b/reason.c\n"
            "--- a/reason.c\n+++ b/reason.c\n@@ -1 +1 @@\n-borrowed\n+owned\n",
            encoding="ascii",
        )
        lines = (ROOT / "tools/moq-interop-client/Dockerfile").read_text().splitlines()
        start = lines.index(f"RUN cd /src/{self.dependency} \\")
        command = []
        for line in lines[start:]:
            command.append(line.removesuffix("\\"))
            if not line.endswith("\\"):
                break
        self.command = " ".join(command)[4:].replace("/src", str(self.root))

    def run_recipe(self):
        return subprocess.run(
            ["/bin/sh", "-c", self.command], capture_output=True, text=True
        )

    def test_clean_and_already_applied(self):
        for expected_output in ("", f"{self.diagnostic} already applied\n"):
            result = self.run_recipe()
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr, "")
            self.assertEqual(result.stdout, expected_output)
            self.assertEqual(self.source.read_text(), "owned\n")

    def test_conflict_is_unchanged(self):
        self.source.write_text("conflict\n", encoding="ascii")
        result = self.run_recipe()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(f"{self.diagnostic} conflicts", result.stderr)
        self.assertEqual(self.source.read_text(), "conflict\n")

    def test_missing_repository(self):
        (self.provider / ".git").rename(self.root / "saved-git")
        result = self.run_recipe()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must be its own git root", result.stderr)
        self.assertEqual(self.source.read_text(), "borrowed\n")

    def test_enclosing_repository_is_not_provider(self):
        (self.provider / ".git").rename(self.root / "saved-git")
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        result = self.run_recipe()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must be its own git root", result.stderr)
        self.assertEqual(self.source.read_text(), "borrowed\n")

    def test_unavailable_worktree_gitfile(self):
        (self.provider / ".git").rename(self.root / "saved-git")
        (self.provider / ".git").write_text("gitdir: /missing/host/worktree\n", encoding="ascii")
        result = self.run_recipe()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must be its own git root", result.stderr)
        self.assertEqual(self.source.read_text(), "borrowed\n")


class PicotlsPatchRecipe(OwnershipPatchRecipe):
    dependency = "picotls"
    patch_name = "0001-initialize-compiler-visible-scratch.patch"
    diagnostic = "picotls scratch initialization patch"


if __name__ == "__main__":
    unittest.main()
