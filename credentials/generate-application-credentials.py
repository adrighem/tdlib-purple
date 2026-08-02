#!/usr/bin/env python3

import argparse
import os
from pathlib import Path
import re
import stat
import sys
import tempfile


API_ID_PATTERN = re.compile(r"[1-9][0-9]{0,9}\Z", re.ASCII)
API_HASH_PATTERN = re.compile(r"[0-9a-fA-F]{32}\Z", re.ASCII)
MAX_INPUT_SIZE = 64


class CredentialInputError(Exception):
    def __init__(self, code):
        super().__init__(code)
        self.code = code


def _path_exists(path):
    try:
        path.lstat()
        return True
    except FileNotFoundError:
        return False
    except OSError as error:
        raise CredentialInputError("CREDENTIAL_INPUT_UNSAFE") from error


def _is_within(path, root):
    resolved_path = path.resolve(strict=False)
    resolved_root = root.resolve(strict=False)
    try:
        return os.path.commonpath((resolved_path, resolved_root)) == str(
            resolved_root
        )
    except ValueError:
        return False


def _paths_alias(first, second, tolerate_second_errors=False):
    try:
        first_absolute = os.path.normcase(os.path.abspath(first))
        second_absolute = os.path.normcase(os.path.abspath(second))
        if first_absolute == second_absolute:
            return True

        first_resolved = os.path.normcase(
            str(first.resolve(strict=False))
        )
    except (OSError, RuntimeError, ValueError) as error:
        raise OSError("could not compare output paths") from error

    try:
        second_resolved = os.path.normcase(
            str(second.resolve(strict=False))
        )
        if first_resolved == second_resolved:
            return True

        return os.path.samefile(first, second)
    except FileNotFoundError:
        return False
    except (OSError, RuntimeError, ValueError) as error:
        if tolerate_second_errors:
            return False
        raise OSError("could not compare output paths") from error


def _read_private_ascii_file(path, invalid_value_code):
    try:
        if path.is_symlink():
            raise CredentialInputError("CREDENTIAL_INPUT_UNSAFE")
    except OSError as error:
        raise CredentialInputError("CREDENTIAL_INPUT_UNSAFE") from error

    flags = os.O_RDONLY
    flags |= getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)

    try:
        descriptor = os.open(path, flags)
    except FileNotFoundError:
        raise CredentialInputError("CREDENTIAL_INPUT_MISSING")
    except OSError:
        raise CredentialInputError("CREDENTIAL_INPUT_UNSAFE")

    try:
        file_stat = os.fstat(descriptor)
        if not stat.S_ISREG(file_stat.st_mode):
            raise CredentialInputError("CREDENTIAL_INPUT_UNSAFE")

        if os.name != "nt":
            if hasattr(os, "geteuid") and file_stat.st_uid != os.geteuid():
                raise CredentialInputError("CREDENTIAL_INPUT_UNSAFE")
            if stat.S_IMODE(file_stat.st_mode) & 0o077:
                raise CredentialInputError("CREDENTIAL_INPUT_UNSAFE")

        chunks = []
        remaining = MAX_INPUT_SIZE + 1
        while remaining > 0:
            try:
                chunk = os.read(descriptor, remaining)
            except OSError as error:
                raise CredentialInputError(
                    "CREDENTIAL_INPUT_UNSAFE"
                ) from error
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)

        value = b"".join(chunks)
        if len(value) > MAX_INPUT_SIZE:
            raise CredentialInputError("CREDENTIAL_INPUT_UNSAFE")
    finally:
        os.close(descriptor)

    if value.endswith(b"\r\n"):
        value = value[:-2]
    elif value.endswith(b"\n"):
        value = value[:-1]

    if b"\r" in value or b"\n" in value or b"\0" in value:
        raise CredentialInputError(invalid_value_code)

    try:
        return value.decode("ascii")
    except UnicodeDecodeError:
        raise CredentialInputError(invalid_value_code)


def _render_provider(api_id, api_hash):
    encoded_hash = ", ".join(str(ord(character)) for character in api_hash)
    return (
        "/* Generated in a private build tree. Do not copy into source. */\n"
        '#include "telegram-application-credentials-private.h"\n'
        "\n"
        "static const TdlibPurpleApplicationCredentials credentials = {\n"
        f"    {api_id},\n"
        f"    {{{encoded_hash}, 0}},\n"
        "};\n"
        "\n"
        "const TdlibPurpleApplicationCredentials *\n"
        "tdlib_purple_application_credentials_embedded(void)\n"
        "{\n"
        "    return &credentials;\n"
        "}\n"
    ).encode("ascii")


def _render_provider_state(available):
    value = 1 if available else 0
    return (
        "/* Generated provider state. Contains no credentials. */\n"
        "#ifndef TDLIB_PURPLE_APPLICATION_CREDENTIALS_STATE_H\n"
        "#define TDLIB_PURPLE_APPLICATION_CREDENTIALS_STATE_H\n"
        "\n"
        "#define TDLIB_PURPLE_APPLICATION_CREDENTIALS_AVAILABLE "
        f"{value}\n"
        "\n"
        "#endif\n"
    ).encode("ascii")


def _prepare_private_output_parent(path):
    parent = path.parent
    previous_umask = os.umask(0o077)
    try:
        parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    finally:
        os.umask(previous_umask)

    parent_stat = parent.lstat()
    if stat.S_ISLNK(parent_stat.st_mode) or not stat.S_ISDIR(
            parent_stat.st_mode):
        raise OSError("unsafe output directory")

    if os.name != "nt":
        if (hasattr(os, "geteuid") and
                parent_stat.st_uid != os.geteuid()):
            raise OSError("unsafe output directory")
        parent.chmod(0o700)

    verified_stat = parent.lstat()
    if (stat.S_ISLNK(verified_stat.st_mode) or
            not stat.S_ISDIR(verified_stat.st_mode) or
            (verified_stat.st_dev, verified_stat.st_ino) !=
            (parent_stat.st_dev, parent_stat.st_ino)):
        raise OSError("unsafe output directory")

    return verified_stat


def _remove_private_output(path):
    try:
        parent_stat = path.parent.lstat()
        if (stat.S_ISLNK(parent_stat.st_mode) or
                not stat.S_ISDIR(parent_stat.st_mode)):
            return

        output_stat = path.lstat()
        if (stat.S_ISREG(output_stat.st_mode) or
                stat.S_ISLNK(output_stat.st_mode)):
            path.unlink()
    except (FileNotFoundError, OSError):
        return


def _write_private_output(path, content):
    parent_stat = _prepare_private_output_parent(path)

    if path.exists() and not path.is_symlink():
        try:
            if path.read_bytes() == content:
                if os.name != "nt":
                    path.chmod(0o600)
                return
        except OSError:
            pass

    descriptor, temporary_name = tempfile.mkstemp(
        prefix=".application-credentials-",
        suffix=".tmp",
        dir=path.parent,
    )
    temporary_path = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(content)
            output.flush()
            os.fsync(output.fileno())
        if os.name != "nt":
            temporary_path.chmod(0o600)

        verified_parent_stat = path.parent.lstat()
        if (stat.S_ISLNK(verified_parent_stat.st_mode) or
                not stat.S_ISDIR(verified_parent_stat.st_mode) or
                (verified_parent_stat.st_dev, verified_parent_stat.st_ino) !=
                (parent_stat.st_dev, parent_stat.st_ino)):
            raise OSError("output directory changed")

        os.replace(temporary_path, path)
        if os.name != "nt":
            path.chmod(0o600)
    finally:
        try:
            temporary_path.unlink()
        except FileNotFoundError:
            pass


def generate_provider(
    api_id_path,
    api_hash_path,
    source_root,
    output_path,
    state_output_path=None,
):
    output_path = Path(output_path)
    if api_id_path is not None:
        api_id_path = Path(api_id_path)
    if api_hash_path is not None:
        api_hash_path = Path(api_hash_path)
    if state_output_path is not None:
        state_output_path = Path(state_output_path)

    output_paths = [output_path]
    if state_output_path is not None:
        output_paths.append(state_output_path)
    read_paths = []
    if api_id_path is not None:
        read_paths.append(api_id_path)
    if api_hash_path is not None:
        read_paths.append(api_hash_path)

    protected_output_indexes = set()
    outputs_alias = False
    try:
        for index, candidate in enumerate(output_paths):
            for other_output in output_paths[index + 1:]:
                if _paths_alias(candidate, other_output):
                    outputs_alias = True
            for read_path in read_paths:
                if _paths_alias(
                        candidate, read_path, tolerate_second_errors=True):
                    protected_output_indexes.add(index)
    except OSError:
        return "CREDENTIAL_OUTPUT_ERROR"

    if outputs_alias or protected_output_indexes:
        for index, candidate in enumerate(output_paths):
            if index not in protected_output_indexes:
                _remove_private_output(candidate)
        return "CREDENTIAL_OUTPUT_ERROR"

    def remove_outputs():
        _remove_private_output(output_path)
        if state_output_path is not None:
            _remove_private_output(state_output_path)

    def write_outputs(provider):
        try:
            _write_private_output(output_path, provider)
            if state_output_path is not None:
                _write_private_output(
                    state_output_path,
                    _render_provider_state(True),
                )
        except OSError:
            remove_outputs()
            raise

    def fail(code):
        remove_outputs()
        return code

    try:
        if api_id_path is None and api_hash_path is None:
            return fail("CREDENTIAL_PATHS_REQUIRED")

        if api_id_path is None or api_hash_path is None:
            return fail("CREDENTIAL_PATHS_INCOMPLETE")

        source_root = Path(source_root)

        id_exists = _path_exists(api_id_path)
        hash_exists = _path_exists(api_hash_path)
        if not id_exists and not hash_exists:
            return fail("CREDENTIAL_INPUT_MISSING")
        if not id_exists or not hash_exists:
            return fail("CREDENTIAL_INPUT_MISSING")

        if (api_id_path.resolve(strict=False) ==
                api_hash_path.resolve(strict=False)):
            return fail("CREDENTIAL_INPUT_DUPLICATE")

        if (_is_within(api_id_path, source_root) or
                _is_within(api_hash_path, source_root)):
            return fail("CREDENTIAL_INPUT_IN_SOURCE_TREE")

        api_id = _read_private_ascii_file(
            api_id_path, "CREDENTIAL_API_ID_INVALID"
        )
        api_hash = _read_private_ascii_file(
            api_hash_path, "CREDENTIAL_API_HASH_INVALID"
        )
    except CredentialInputError as error:
        return fail(error.code)
    except (OSError, RuntimeError, ValueError, TypeError):
        return fail("CREDENTIAL_INPUT_UNSAFE")

    if API_ID_PATTERN.fullmatch(api_id) is None:
        return fail("CREDENTIAL_API_ID_INVALID")

    try:
        numeric_api_id = int(api_id, 10)
    except ValueError:
        return fail("CREDENTIAL_API_ID_INVALID")
    if numeric_api_id > 2147483647:
        return fail("CREDENTIAL_API_ID_INVALID")

    if API_HASH_PATTERN.fullmatch(api_hash) is None:
        return fail("CREDENTIAL_API_HASH_INVALID")

    try:
        write_outputs(_render_provider(api_id, api_hash))
    except OSError:
        return "CREDENTIAL_OUTPUT_ERROR"

    return None


def _parse_arguments():
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--api-id-file")
    parser.add_argument("--api-hash-file")
    parser.add_argument("--source-root", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--state-output")
    return parser.parse_args()


def main():
    try:
        arguments = _parse_arguments()
        error = generate_provider(
            api_id_path=arguments.api_id_file,
            api_hash_path=arguments.api_hash_file,
            source_root=arguments.source_root,
            output_path=arguments.output,
            state_output_path=arguments.state_output,
        )
    except Exception:
        error = "CREDENTIAL_OUTPUT_ERROR"

    if error is not None:
        print(error, file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
