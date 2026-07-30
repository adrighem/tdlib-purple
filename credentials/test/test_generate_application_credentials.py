#!/usr/bin/env python3

import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest


sys.dont_write_bytecode = True

CREDENTIALS_DIR = Path(__file__).resolve().parents[1]
GENERATOR_PATH = CREDENTIALS_DIR / "generate-application-credentials.py"
STUB_PATH = CREDENTIALS_DIR / "telegram-application-credentials-stub.c"
SOURCE_ROOT = CREDENTIALS_DIR.parent
CMAKE_MODULE_PATH = (
    SOURCE_ROOT / "cmake" / "TelegramApplicationCredentials.cmake"
)
SUBPROCESS_ENV = {
    name: os.environ[name]
    for name in (
        "HOME",
        "LANG",
        "LC_ALL",
        "LOGNAME",
        "PATH",
        "SYSTEMROOT",
        "TEMP",
        "TMP",
        "USER",
        "WINDIR",
    )
    if name in os.environ
}
BUILD_GENERATORS = []
if shutil.which("ninja"):
    BUILD_GENERATORS.append("Ninja")
if shutil.which("make"):
    BUILD_GENERATORS.append("Unix Makefiles")

SPEC = importlib.util.spec_from_file_location(
    "generate_application_credentials", GENERATOR_PATH
)
GENERATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GENERATOR)


class ApplicationCredentialGeneratorTest(unittest.TestCase):
    def setUp(self):
        self.temp_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_directory.name)
        self.private = self.root / "private"
        self.private.mkdir(mode=0o700)
        self.output = self.private / "credentials.c"
        self.state_output = self.private / "credentials-state.h"
        self.api_id = self.root / "api-id"
        self.api_hash = self.root / "api-hash"
        self.synthetic_id = "123456"
        self.synthetic_hash = "a1" * 16

    def tearDown(self):
        self.temp_directory.cleanup()

    def write_private(self, path, value):
        path.write_text(value, encoding="ascii")
        path.chmod(0o600)

    def generate(self, api_id=None, api_hash=None, source_root=None):
        return GENERATOR.generate_provider(
            api_id_path=api_id,
            api_hash_path=api_hash,
            source_root=source_root or self.root / "source",
            stub_path=STUB_PATH,
            output_path=self.output,
            state_output_path=self.state_output,
        )

    def assert_state(self, available):
        state = self.state_output.read_text(encoding="ascii")
        self.assertEqual(
            state,
            GENERATOR._render_provider_state(available).decode("ascii"),
        )
        self.assertNotIn(self.synthetic_id, state)
        self.assertNotIn(self.synthetic_hash, state)
        self.assertNotIn(str(self.api_id), state)
        self.assertNotIn(str(self.api_hash), state)

    def assert_stub(self):
        output = self.output.read_text(encoding="ascii")
        self.assertIn("return NULL;", output)
        self.assertNotIn(self.synthetic_hash, output)
        self.assert_state(False)

    def test_neither_path_generates_unavailable_stub(self):
        self.assertIsNone(self.generate())
        self.assert_stub()

    def test_valid_pair_generates_private_provider_without_plaintext_hash(self):
        self.write_private(self.api_id, self.synthetic_id + "\n")
        self.write_private(self.api_hash, self.synthetic_hash + "\n")

        self.assertIsNone(self.generate(self.api_id, self.api_hash))

        output = self.output.read_text(encoding="ascii")
        self.assertIn("tdlib_purple_application_credentials_embedded", output)
        self.assertNotIn(self.synthetic_hash, output)
        self.assertEqual(self.output.stat().st_mode & 0o777, 0o600)
        self.assertEqual(self.state_output.stat().st_mode & 0o777, 0o600)
        self.assertEqual(self.output.parent.stat().st_mode & 0o777, 0o700)
        self.assert_state(True)

    def test_crlf_is_accepted(self):
        self.write_private(self.api_id, self.synthetic_id + "\r\n")
        self.write_private(self.api_hash, self.synthetic_hash.upper() + "\r\n")

        self.assertIsNone(self.generate(self.api_id, self.api_hash))
        self.assertNotIn(self.synthetic_hash.upper(),
                         self.output.read_text(encoding="ascii"))

    def test_maximum_signed_32_bit_api_id_is_accepted(self):
        self.write_private(self.api_id, "2147483647")
        self.write_private(self.api_hash, self.synthetic_hash)

        self.assertIsNone(self.generate(self.api_id, self.api_hash))

    def test_half_configured_pair_scrubs_previous_provider(self):
        self.write_private(self.api_id, self.synthetic_id)
        self.write_private(self.api_hash, self.synthetic_hash)
        self.assertIsNone(self.generate(self.api_id, self.api_hash))

        self.assertEqual(
            self.generate(self.api_id, None),
            "CREDENTIAL_PATHS_INCOMPLETE",
        )
        self.assert_stub()

    def test_both_missing_files_generate_unavailable_stub(self):
        self.assertIsNone(self.generate(self.api_id, self.api_hash))
        self.assert_stub()

    def test_one_missing_file_is_rejected_and_scrubs_output(self):
        self.write_private(self.api_id, self.synthetic_id)

        self.assertEqual(
            self.generate(self.api_id, self.api_hash),
            "CREDENTIAL_INPUT_MISSING",
        )
        self.assert_stub()

    def test_oversized_input_is_rejected_and_scrubs_output(self):
        self.write_private(self.api_id, self.synthetic_id)
        self.write_private(self.api_hash, self.synthetic_hash)
        self.assertIsNone(self.generate(self.api_id, self.api_hash))

        self.write_private(
            self.api_hash,
            self.synthetic_hash + ("0" * GENERATOR.MAX_INPUT_SIZE),
        )
        self.assertEqual(
            self.generate(self.api_id, self.api_hash),
            "CREDENTIAL_INPUT_UNSAFE",
        )
        self.assert_stub()

    def test_directory_input_is_rejected_and_scrubs_output(self):
        self.write_private(self.api_id, self.synthetic_id)
        self.write_private(self.api_hash, self.synthetic_hash)
        self.assertIsNone(self.generate(self.api_id, self.api_hash))

        directory_input = self.root / "directory-input"
        directory_input.mkdir(mode=0o700)
        self.assertEqual(
            self.generate(self.api_id, directory_input),
            "CREDENTIAL_INPUT_UNSAFE",
        )
        self.assert_stub()

    def test_duplicate_input_path_is_rejected(self):
        self.write_private(self.api_id, self.synthetic_id)

        self.assertEqual(
            self.generate(self.api_id, self.api_id),
            "CREDENTIAL_INPUT_DUPLICATE",
        )
        self.assert_stub()

    def test_invalid_api_ids_are_rejected(self):
        invalid_ids = (
            "",
            "0",
            "01",
            "-1",
            "+1",
            "1 ",
            " 1",
            "1\n2",
            "2147483648",
            "not-a-number",
        )
        self.write_private(self.api_hash, self.synthetic_hash)

        for invalid_id in invalid_ids:
            with self.subTest(api_id=repr(invalid_id)):
                self.write_private(self.api_id, invalid_id)
                self.assertEqual(
                    self.generate(self.api_id, self.api_hash),
                    "CREDENTIAL_API_ID_INVALID",
                )
                self.assert_stub()

    def test_invalid_api_hashes_are_rejected(self):
        invalid_hashes = (
            "",
            "a1" * 15,
            ("a1" * 16) + "0",
            ("a1" * 15) + "zz",
            ("a1" * 15) + "a ",
            ("a1" * 15) + "a\nb",
        )
        self.write_private(self.api_id, self.synthetic_id)

        for invalid_hash in invalid_hashes:
            with self.subTest(api_hash=repr(invalid_hash)):
                self.write_private(self.api_hash, invalid_hash)
                self.assertEqual(
                    self.generate(self.api_id, self.api_hash),
                    "CREDENTIAL_API_HASH_INVALID",
                )
                self.assert_stub()

    @unittest.skipIf(os.name == "nt", "POSIX permissions are unavailable")
    def test_group_or_other_permissions_are_rejected(self):
        self.write_private(self.api_id, self.synthetic_id)
        self.write_private(self.api_hash, self.synthetic_hash)
        self.api_hash.chmod(0o640)

        self.assertEqual(
            self.generate(self.api_id, self.api_hash),
            "CREDENTIAL_INPUT_UNSAFE",
        )
        self.assert_stub()

    def test_symlink_is_rejected(self):
        self.write_private(self.api_id, self.synthetic_id)
        self.write_private(self.api_hash, self.synthetic_hash)
        linked_hash = self.root / "linked-hash"
        linked_hash.symlink_to(self.api_hash)

        self.assertEqual(
            self.generate(self.api_id, linked_hash),
            "CREDENTIAL_INPUT_UNSAFE",
        )
        self.assert_stub()

    def test_symlink_loop_is_rejected_and_scrubs_output(self):
        self.write_private(self.api_id, self.synthetic_id)
        self.write_private(self.api_hash, self.synthetic_hash)
        self.assertIsNone(self.generate(self.api_id, self.api_hash))

        first_link = self.root / "first-link"
        second_link = self.root / "second-link"
        first_link.symlink_to(second_link)
        second_link.symlink_to(first_link)

        self.assertEqual(
            self.generate(self.api_id, first_link),
            "CREDENTIAL_INPUT_UNSAFE",
        )
        self.assert_stub()

    def test_source_tree_input_is_rejected(self):
        source_root = self.root / "source"
        source_root.mkdir()
        source_id = source_root / "api-id"
        self.write_private(source_id, self.synthetic_id)
        self.write_private(self.api_hash, self.synthetic_hash)

        self.assertEqual(
            self.generate(source_id, self.api_hash, source_root),
            "CREDENTIAL_INPUT_IN_SOURCE_TREE",
        )
        self.assert_stub()

    def test_unchanged_pair_preserves_generated_file_timestamp(self):
        self.write_private(self.api_id, self.synthetic_id)
        self.write_private(self.api_hash, self.synthetic_hash)
        self.assertIsNone(self.generate(self.api_id, self.api_hash))
        first_timestamp = self.output.stat().st_mtime_ns
        first_state_timestamp = self.state_output.stat().st_mtime_ns

        time.sleep(0.01)
        self.assertIsNone(self.generate(self.api_id, self.api_hash))

        self.assertEqual(self.output.stat().st_mtime_ns, first_timestamp)
        self.assertEqual(
            self.state_output.stat().st_mtime_ns,
            first_state_timestamp,
        )

    def test_rotation_and_removal_replace_generated_provider(self):
        self.write_private(self.api_id, self.synthetic_id)
        self.write_private(self.api_hash, self.synthetic_hash)
        self.assertIsNone(self.generate(self.api_id, self.api_hash))
        first_output = self.output.read_bytes()
        first_state_timestamp = self.state_output.stat().st_mtime_ns

        self.write_private(self.api_hash, "b2" * 16)
        self.assertIsNone(self.generate(self.api_id, self.api_hash))
        self.assertNotEqual(self.output.read_bytes(), first_output)
        self.assertEqual(
            self.state_output.stat().st_mtime_ns,
            first_state_timestamp,
        )

        self.api_id.unlink()
        self.api_hash.unlink()
        self.assertIsNone(self.generate(self.api_id, self.api_hash))
        self.assert_stub()

        self.write_private(self.api_id, self.synthetic_id)
        self.write_private(self.api_hash, self.synthetic_hash)
        self.assertIsNone(self.generate(self.api_id, self.api_hash))
        self.assert_state(True)

    def test_missing_stub_removes_previous_generated_output(self):
        self.write_private(self.api_id, self.synthetic_id)
        self.write_private(self.api_hash, self.synthetic_hash)
        self.assertIsNone(self.generate(self.api_id, self.api_hash))

        result = GENERATOR.generate_provider(
            api_id_path=self.api_id,
            api_hash_path=self.api_hash,
            source_root=self.root / "source",
            stub_path=self.root / "missing-stub.c",
            output_path=self.output,
            state_output_path=self.state_output,
        )

        self.assertEqual(result, "CREDENTIAL_OUTPUT_ERROR")
        self.assertFalse(self.output.exists())
        self.assertFalse(self.state_output.exists())

    def test_duplicate_output_path_is_rejected_and_scrubbed(self):
        self.write_private(self.api_id, self.synthetic_id)
        self.write_private(self.api_hash, self.synthetic_hash)
        self.output.write_text("stale provider\n", encoding="ascii")
        self.output.chmod(0o600)

        result = GENERATOR.generate_provider(
            api_id_path=self.api_id,
            api_hash_path=self.api_hash,
            source_root=self.root / "source",
            stub_path=STUB_PATH,
            output_path=self.output,
            state_output_path=self.output,
        )

        self.assertEqual(result, "CREDENTIAL_OUTPUT_ERROR")
        self.assertFalse(self.output.exists())

    def test_outputs_cannot_replace_inputs_or_stub(self):
        for output_name in ("provider", "state"):
            for protected_name in ("api-id", "api-hash", "stub"):
                with self.subTest(
                        output=output_name, protected=protected_name):
                    case_root = self.root / (
                        output_name + "-" + protected_name
                    )
                    case_root.mkdir(mode=0o700)
                    private = case_root / "private"
                    private.mkdir(mode=0o700)
                    api_id = case_root / "api-id"
                    api_hash = case_root / "api-hash"
                    stub = case_root / "stub.c"
                    provider = private / "provider.c"
                    state = private / "state.h"

                    self.write_private(api_id, self.synthetic_id)
                    self.write_private(api_hash, self.synthetic_hash)
                    stub.write_bytes(STUB_PATH.read_bytes())
                    stub.chmod(0o600)
                    protected = {
                        "api-id": api_id,
                        "api-hash": api_hash,
                        "stub": stub,
                    }[protected_name]
                    if output_name == "provider":
                        provider = protected
                        state.write_text("stale state\n", encoding="ascii")
                        state.chmod(0o600)
                        safe_output = state
                    else:
                        state = protected
                        provider.write_text(
                            "stale provider\n", encoding="ascii"
                        )
                        provider.chmod(0o600)
                        safe_output = provider
                    original_inputs = {
                        path: path.read_bytes()
                        for path in (api_id, api_hash, stub)
                    }

                    result = GENERATOR.generate_provider(
                        api_id_path=api_id,
                        api_hash_path=api_hash,
                        source_root=case_root / "source",
                        stub_path=stub,
                        output_path=provider,
                        state_output_path=state,
                    )

                    self.assertEqual(result, "CREDENTIAL_OUTPUT_ERROR")
                    for path, content in original_inputs.items():
                        self.assertEqual(path.read_bytes(), content)
                    self.assertFalse(safe_output.exists())

    @unittest.skipIf(os.name == "nt", "hard links vary on Windows")
    def test_output_hard_link_alias_is_rejected(self):
        self.write_private(self.api_id, self.synthetic_id)
        self.write_private(self.api_hash, self.synthetic_hash)
        os.link(self.api_id, self.state_output)
        original_id = self.api_id.read_bytes()
        self.output.write_text("stale provider\n", encoding="ascii")
        self.output.chmod(0o600)

        result = self.generate(self.api_id, self.api_hash)

        self.assertEqual(result, "CREDENTIAL_OUTPUT_ERROR")
        self.assertEqual(self.api_id.read_bytes(), original_id)
        self.assertEqual(self.state_output.read_bytes(), original_id)
        self.assertFalse(self.output.exists())

    def test_symlinked_output_parent_is_rejected_without_following_it(self):
        real_private = self.root / "real-private"
        real_private.mkdir(mode=0o700)
        sentinel = real_private / "credentials.c"
        sentinel.write_text("synthetic sentinel\n", encoding="ascii")
        sentinel.chmod(0o600)

        linked_private = self.root / "linked-private"
        linked_private.symlink_to(real_private, target_is_directory=True)
        linked_output = linked_private / "credentials.c"

        result = GENERATOR.generate_provider(
            api_id_path=None,
            api_hash_path=None,
            source_root=self.root / "source",
            stub_path=STUB_PATH,
            output_path=linked_output,
        )

        self.assertEqual(result, "CREDENTIAL_OUTPUT_ERROR")
        self.assertEqual(
            sentinel.read_text(encoding="ascii"),
            "synthetic sentinel\n",
        )

    def test_cli_diagnostic_contains_only_stable_error_code(self):
        self.write_private(self.api_id, self.synthetic_id)
        self.write_private(self.api_hash, self.synthetic_hash + "x")

        result = subprocess.run(
            (
                sys.executable,
                str(GENERATOR_PATH),
                "--api-id-file",
                str(self.api_id),
                "--api-hash-file",
                str(self.api_hash),
                "--source-root",
                str(self.root / "source"),
                "--stub",
                str(STUB_PATH),
                "--output",
                str(self.output),
                "--state-output",
                str(self.state_output),
            ),
            check=False,
            capture_output=True,
            env=SUBPROCESS_ENV,
            text=True,
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")
        self.assertEqual(result.stderr,
                         "CREDENTIAL_API_HASH_INVALID\n")
        self.assertNotIn(self.synthetic_id, result.stderr)
        self.assertNotIn(self.synthetic_hash, result.stderr)
        self.assertNotIn(str(self.api_id), result.stderr)
        self.assertNotIn(str(self.api_hash), result.stderr)


@unittest.skipUnless(
    shutil.which("cmake") and BUILD_GENERATORS,
    "CMake and a supported build tool are required for build-graph tests",
)
@unittest.skipIf(os.name == "nt", "fixture uses a single-config executable path")
class ApplicationCredentialCMakeGraphTest(unittest.TestCase):
    def setUp(self):
        self.temp_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_directory.name)
        self.source = self.root / "source"
        self.source.mkdir()
        self.synthetic_hash = "b2" * 16

        module_path = CMAKE_MODULE_PATH.as_posix()
        credentials_dir = CREDENTIALS_DIR.as_posix()
        (self.source / "CMakeLists.txt").write_text(
            (
                "cmake_minimum_required(VERSION 3.16 FATAL_ERROR)\n"
                "project(application-credential-graph-test LANGUAGES C)\n"
                "find_package(PkgConfig REQUIRED)\n"
                "pkg_check_modules(GLib REQUIRED IMPORTED_TARGET glib-2.0)\n"
                f'include("{module_path}")\n'
                "tdlib_purple_configure_application_credentials(\n"
                "    fixture PROVIDER_SOURCE REFRESH_TARGET "
                "PROVIDER_STATE_HEADER)\n"
                "add_executable(probe\n"
                "    main.c\n"
                f'    "{credentials_dir}/'
                'telegram-application-credentials.c"\n'
                "    \"${PROVIDER_SOURCE}\")\n"
                "target_include_directories(probe PRIVATE\n"
                f'    "{credentials_dir}"\n'
                '    "${CMAKE_CURRENT_BINARY_DIR}/.private")\n'
                "target_link_libraries(probe PRIVATE PkgConfig::GLib)\n"
                "add_dependencies(probe \"${REFRESH_TARGET}\")\n"
            ),
            encoding="utf-8",
        )
        (self.source / "main.c").write_text(
            (
                '#include "telegram-application-credentials.h"\n'
                '#include "telegram-application-credentials-state.h"\n'
                "#include <stdlib.h>\n"
                "#include <string.h>\n"
                "\n"
                "int main(int argc, char **argv)\n"
                "{\n"
                "    const TdlibPurpleApplicationCredentials *credentials =\n"
                "        tdlib_purple_application_credentials_get();\n"
                "    if ((credentials != NULL) !=\n"
                "            (TDLIB_PURPLE_APPLICATION_CREDENTIALS_AVAILABLE "
                "!= 0)) {\n"
                "        return 4;\n"
                "    }\n"
                "    if (argc == 2 && strcmp(argv[1], \"unavailable\") == 0) {\n"
                "        return credentials == NULL ? 0 : 1;\n"
                "    }\n"
                "    if (argc != 2 || credentials == NULL) {\n"
                "        return 2;\n"
                "    }\n"
                "    return credentials->api_id == strtol(argv[1], NULL, 10)\n"
                "        ? 0 : 3;\n"
                "}\n"
            ),
            encoding="ascii",
        )

    def tearDown(self):
        self.temp_directory.cleanup()

    @staticmethod
    def write_private(path, value):
        path.write_text(value, encoding="ascii")
        path.chmod(0o600)

    def select_generator(self, generator):
        suffix = generator.lower().replace(" ", "-")
        self.build = self.root / ("build-" + suffix)
        self.api_id = self.root / ("api-id-" + suffix)
        self.api_hash = self.root / ("api-hash-" + suffix)

    def configure(self, with_paths, generator):
        command = [
            shutil.which("cmake"),
            "-S", str(self.source),
            "-B", str(self.build),
            "-G", generator,
        ]
        if with_paths:
            command.extend((
                "-DTDLIB_PURPLE_API_ID_FILE:FILEPATH=" + str(self.api_id),
                "-DTDLIB_PURPLE_API_HASH_FILE:FILEPATH=" +
                str(self.api_hash),
            ))
        subprocess.run(
            command,
            check=True,
            capture_output=True,
            env=SUBPROCESS_ENV,
            text=True,
        )

    def build_target(self, target="probe", check=True):
        return subprocess.run(
            (
                shutil.which("cmake"),
                "--build", str(self.build),
                "--target", target,
            ),
            check=check,
            capture_output=True,
            env=SUBPROCESS_ENV,
            text=True,
        )

    def run_probe(self, expected):
        subprocess.run(
            (str(self.build / "probe"), str(expected)),
            check=True,
            capture_output=True,
            env=SUBPROCESS_ENV,
            text=True,
        )

    def test_unconfigured_stub_is_recreated_after_clean(self):
        for generator in BUILD_GENERATORS:
            with self.subTest(generator=generator):
                self.select_generator(generator)
                self.configure(with_paths=False, generator=generator)
                self.build_target()
                self.run_probe("unavailable")

                self.build_target(target="clean")
                self.build_target()
                self.run_probe("unavailable")

    def test_rotation_and_removal_rebuild_the_consumer(self):
        for generator in BUILD_GENERATORS:
            with self.subTest(generator=generator):
                self.select_generator(generator)
                self.write_private(self.api_id, "123456")
                self.write_private(self.api_hash, self.synthetic_hash)
                self.configure(with_paths=True, generator=generator)
                self.build_target()
                self.run_probe("123456")

                self.write_private(self.api_id, "654321")
                self.write_private(self.api_hash, "c3" * 16)
                self.build_target()
                self.run_probe("654321")

                self.api_id.unlink()
                self.api_hash.unlink()
                self.build_target()
                self.run_probe("unavailable")

                self.write_private(self.api_id, "777777")
                self.write_private(self.api_hash, "d4" * 16)
                self.build_target()
                self.run_probe("777777")

    def test_half_removed_pair_fails_then_both_removed_build_stub(self):
        for generator in BUILD_GENERATORS:
            with self.subTest(generator=generator):
                self.select_generator(generator)
                self.write_private(self.api_id, "123456")
                self.write_private(self.api_hash, self.synthetic_hash)
                self.configure(with_paths=True, generator=generator)
                self.build_target()
                self.run_probe("123456")

                self.api_hash.unlink()
                failed_build = self.build_target(check=False)
                self.assertNotEqual(failed_build.returncode, 0)
                self.assertNotIn(self.synthetic_hash, failed_build.stdout)
                self.assertNotIn(self.synthetic_hash, failed_build.stderr)

                self.api_id.unlink()
                self.build_target()
                self.run_probe("unavailable")


if __name__ == "__main__":
    unittest.main()
