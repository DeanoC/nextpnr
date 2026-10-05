#!/usr/bin/env python3
"""Bounded seed-race collection and prefix-only offline evaluation.

The module intentionally uses only the Python standard library.  Collection is
driven by a JSON manifest; evaluation consumes fully observed synthetic or real
run records, but hides observations after each policy decision point.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import contextlib
import ctypes
import hashlib
import json
import math
import os
import select
import shutil
import signal
import stat
import struct
import subprocess
import sys
import threading
import time
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any, BinaryIO, Dict, Iterable, List, Mapping, Optional, Sequence, Tuple

try:
    import fcntl
except ImportError:  # pragma: no cover - collection fails closed without Linux seals.
    fcntl = None


SCHEMA_VERSION = 1
TERMINAL_STATUSES = {
    "completed",
    "incomplete_evidence",
    "routing_failure",
    "timing_constraint_failure",
    "analogue_timing_failure",
    "process_failure",
    "timeout",
    "cancelled",
    "not_started_total_budget",
    "launch_error",
    "runner_error",
}
PREFIX_FEATURES = {
    "elapsed_seconds", "phase", "attempt", "round", "work", "searches", "node_expansions", "traversals",
    "backend_retries", "unrouted_connections", "overused_wires", "total_excess_occupancy", "wire_count",
    "affected_nets", "binding_failures", "architecture_failures", "plateau_length", "recent_progress",
    "improvement", "congestion_weight", "bounding_box_expansion", "retry_count", "fallback_count",
    "table_wns_ns", "table_tns_ns", "table_failing_endpoints", "repaired_connections",
    "displaced_connections", "frozen_connections", "repair_improvement", "timing_model", "analogue_wns_ns",
    "analogue_tns_ns", "analogue_clocks",
}
BASE_ENVIRONMENT_VARIABLES = ("PATH",)
TERMINATION_GRACE_SECONDS = 2.0
INOTIFY_MUTATION_EVENTS = (0x00000002 | 0x00000004 | 0x00000008 | 0x00000040 | 0x00000080 |
                           0x00000100 | 0x00000200 | 0x00000400 | 0x00000800)
if os.name == "nt":  # Minimum variables required to create ordinary Windows child processes.
    BASE_ENVIRONMENT_VARIABLES += ("COMSPEC", "PATHEXT", "SYSTEMROOT", "WINDIR")


def _positive_number(value: Any, field: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{field} must be a positive finite number")
    try:
        number = float(value)
    except (OverflowError, ValueError):
        raise ValueError(f"{field} must be a positive finite number") from None
    if not math.isfinite(number) or number <= 0:
        raise ValueError(f"{field} must be a positive finite number")
    return number


def _positive_int(value: Any, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise ValueError(f"{field} must be a positive integer")
    return value


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _sha256_stream(stream: BinaryIO) -> str:
    digest = hashlib.sha256()
    stream.seek(0)
    for block in iter(lambda: stream.read(1024 * 1024), b""):
        digest.update(block)
    stream.seek(0)
    return digest.hexdigest()


def _descriptor_path(stream: BinaryIO) -> str:
    """Return a child-visible path for an inherited, already-open descriptor."""
    return _descriptor_number_path(stream.fileno())


def _descriptor_number_path(descriptor: int) -> str:
    """Return a child-visible path for an inherited descriptor number."""
    if not sys.platform.startswith("linux"):
        raise RuntimeError("immutable seed-race snapshots require Linux sealed descriptors")
    candidate = Path("/proc/self/fd") / str(descriptor)
    if candidate.exists():
        return str(candidate)
    raise RuntimeError("immutable seed-race snapshots require /proc/self/fd")


def _copy_stream_to_readonly_descriptor(source: BinaryIO, source_name: str,
                                        snapshot_path: Optional[Path],
                                        mode_mask: int) -> BinaryIO:
    """Copy an already-open stable source into a sealed memfd."""
    if fcntl is None or not hasattr(os, "memfd_create"):
        raise RuntimeError("immutable seed-race snapshots require Linux memfd seals")
    descriptor = os.memfd_create(
        "nextpnr-seed-race-" + _safe_component(Path(source_name).name),
        flags=os.MFD_ALLOW_SEALING,
    )
    destination = os.fdopen(descriptor, "w+b")
    try:
        source.flush()
        before = os.fstat(source.fileno())
        source.seek(0)
        shutil.copyfileobj(source, destination)
        after = os.fstat(source.fileno())
        source.seek(0)
        stable_fields = ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns")
        if any(getattr(before, field) != getattr(after, field) for field in stable_fields):
            raise RuntimeError(f"source changed while snapshotting: {source_name}")
        if destination.tell() != before.st_size:
            raise RuntimeError(f"source size changed while snapshotting: {source_name}")
        destination.flush()
        os.fchmod(destination.fileno(), before.st_mode & mode_mask)
        destination.seek(0)
        if snapshot_path is not None:
            with snapshot_path.open("xb") as display:
                shutil.copyfileobj(destination, display)
                display.flush()
                os.fsync(display.fileno())
                os.fchmod(display.fileno(), before.st_mode & mode_mask)
        seals = fcntl.F_SEAL_SEAL | fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_GROW | fcntl.F_SEAL_WRITE
        fcntl.fcntl(destination.fileno(), fcntl.F_ADD_SEALS, seals)
        if fcntl.fcntl(destination.fileno(), fcntl.F_GET_SEALS) & seals != seals:
            raise RuntimeError(f"unable to seal snapshot: {source_name}")
        destination.seek(0)
    except BaseException:
        destination.close()
        raise
    return destination


def _copy_to_readonly_descriptor(source_path: Path, snapshot_path: Optional[Path],
                                 mode_mask: int) -> BinaryIO:
    """Copy stable source bytes into a sealed memfd and a display-only file."""
    with source_path.open("rb") as source:
        return _copy_stream_to_readonly_descriptor(
            source, str(source_path), snapshot_path, mode_mask)


def _file_contains_any(path: Path, needles: Sequence[bytes]) -> bool:
    """Search a large binary without loading it all into memory."""
    overlap = max(map(len, needles)) - 1
    tail = b""
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            window = tail + block
            if any(needle in window for needle in needles):
                return True
            tail = window[-overlap:] if overlap else b""
    return False


def _watch_tree_mutations(root: Path) -> int:
    """Watch a display tree so owner chmod/swap/restore attacks fail closed."""
    libc = ctypes.CDLL(None, use_errno=True)
    descriptor = libc.inotify_init1(os.O_NONBLOCK | os.O_CLOEXEC)
    if descriptor < 0:
        raise OSError(ctypes.get_errno(), "inotify_init1 failed")
    try:
        directories = [root] + sorted(
            (item for item in root.rglob("*") if item.is_dir()), key=str)
        for path in directories:
            if libc.inotify_add_watch(
                    descriptor, os.fsencode(path), INOTIFY_MUTATION_EVENTS) < 0:
                raise OSError(
                    ctypes.get_errno(), f"cannot watch frozen runtime directory: {path}")
    except BaseException:
        os.close(descriptor)
        raise
    return descriptor


def _tree_was_not_mutated(descriptor: Optional[int]) -> bool:
    if descriptor is None:
        return True
    try:
        return not os.read(descriptor, 65536)
    except BlockingIOError:
        return True


def _json_dump(path: Path, value: Any) -> None:
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
    temporary.replace(path)


def _safe_component(value: Any) -> str:
    text = str(value)
    safe = "".join(ch if ch.isalnum() or ch in "._-" else "_" for ch in text)
    if not safe or safe in {".", ".."}:
        raise ValueError(f"unsafe path component: {text!r}")
    return safe[:80]


def _require_keys(mapping: Mapping[str, Any], keys: Iterable[str], context: str) -> None:
    missing = sorted(set(keys) - set(mapping))
    if missing:
        raise ValueError(f"{context} missing required fields: {', '.join(missing)}")


def validate_collection_manifest(document: Mapping[str, Any]) -> Dict[str, Any]:
    """Validate and normalize a collection manifest without touching the filesystem."""
    _require_keys(document, ("cohort", "architecture", "command", "seeds", "repeats", "limits"),
                  "manifest")
    if document.get("schema_version", SCHEMA_VERSION) != SCHEMA_VERSION:
        raise ValueError("unsupported collection manifest schema_version")
    cohort = document["cohort"]
    if not isinstance(cohort, dict):
        raise ValueError("cohort must be an object")
    _require_keys(cohort, ("id", "design_id", "mapped_design_id", "constraint_family"), "cohort")
    if not all(isinstance(cohort[name], str) and cohort[name] for name in
               ("id", "design_id", "mapped_design_id", "constraint_family")):
        raise ValueError("cohort identity fields must be non-empty strings")
    if not isinstance(document["architecture"], str) or not document["architecture"]:
        raise ValueError("architecture must be a non-empty string")
    command = document["command"]
    if not isinstance(command, list) or not command or not all(isinstance(arg, str) for arg in command):
        raise ValueError("command must be a non-empty argv string array")
    _validate_seed_binding(command, Path(command[0]).name)
    seeds = document["seeds"]
    if not isinstance(seeds, list) or not seeds or any(isinstance(seed, (dict, list, bool)) for seed in seeds):
        raise ValueError("seeds must be a non-empty scalar array")
    if len({str(seed) for seed in seeds}) != len(seeds):
        raise ValueError("seeds must be unique")
    repeats = _positive_int(document["repeats"], "repeats")
    limits = document["limits"]
    if not isinstance(limits, dict):
        raise ValueError("limits must be an object")
    _require_keys(limits, ("per_run_seconds", "total_seconds", "concurrency"), "limits")
    per_run = _positive_number(limits["per_run_seconds"], "limits.per_run_seconds")
    total = _positive_number(limits["total_seconds"], "limits.total_seconds")
    concurrency = _positive_int(limits["concurrency"], "limits.concurrency")
    inputs = document.get("inputs", [])
    if not isinstance(inputs, list):
        raise ValueError("inputs must be an array")
    for item in inputs:
        if (not isinstance(item, dict) or not isinstance(item.get("path"), str) or
                not isinstance(item.get("role"), str) or not item.get("role")):
            raise ValueError("each input needs non-empty string path and role")
    artifacts = document.get("artifacts", {})
    if (not isinstance(artifacts, dict) or
            not all(isinstance(k, str) and k and isinstance(v, str) and v
                    for k, v in artifacts.items())):
        raise ValueError("artifacts must map names to relative paths")
    if {"stdout", "stderr"}.intersection(artifacts):
        raise ValueError("stdout and stderr are reserved collector artifact names")
    for relative in artifacts.values():
        if Path(relative).is_absolute() or ".." in Path(relative).parts:
            raise ValueError("artifact paths must stay within a run directory")
    normalized_artifacts = [Path(relative) for relative in artifacts.values()]
    reserved_artifacts = [Path(name) for name in
                          ("manifest.json", "result.json", "stdout.log", "stderr.log")]
    all_artifacts = normalized_artifacts + reserved_artifacts
    if any(path == Path(".") for path in normalized_artifacts):
        raise ValueError("artifact paths must not overlap run directories or evidence files")
    for index, path in enumerate(all_artifacts):
        for other in all_artifacts[index + 1:]:
            if path == other or path in other.parents or other in path.parents:
                raise ValueError("artifact paths must not overlap each other or collector evidence files")
    environment = document.get("environment", {})
    if not isinstance(environment, dict) or not all(isinstance(k, str) and isinstance(v, str) for k, v in environment.items()):
        raise ValueError("environment must be an explicit string map")
    loader_controls = sorted(
        name for name in environment if name.startswith("LD_") or name == "GLIBC_TUNABLES")
    if loader_controls:
        raise ValueError(
            "dynamic-loader environment controls are unsupported for frozen collection: " +
            ", ".join(loader_controls))
    required_clocks = document.get("required_clocks", [])
    if (not isinstance(required_clocks, list) or
            not all(isinstance(name, str) and name for name in required_clocks) or
            len(set(required_clocks)) != len(required_clocks)):
        raise ValueError("required_clocks must be a unique string array")
    provenance = document.get("provenance", {})
    if (not isinstance(provenance, dict) or
            not isinstance(provenance.get("source_revision"), str) or
            not provenance.get("source_revision") or
            not isinstance(provenance.get("dirty"), bool) or
            provenance.get("runtime_environment_id") != "auto"):
        raise ValueError(
            "provenance requires source_revision, boolean dirty, and runtime_environment_id=auto")
    return {
        "schema_version": SCHEMA_VERSION,
        "cohort": dict(cohort),
        "architecture": document["architecture"],
        "command": list(command),
        "seeds": list(seeds),
        "repeats": repeats,
        "limits": {"per_run_seconds": per_run, "total_seconds": total, "concurrency": concurrency},
        "inputs": [dict(item) for item in inputs],
        "artifacts": dict(artifacts),
        "environment": dict(environment),
        "required_clocks": list(required_clocks),
        "cwd": document.get("cwd"),
        "provenance": dict(provenance),
    }


def _expand_argv(template: Sequence[str], fields: Mapping[str, str]) -> List[str]:
    result = []
    for argument in template:
        try:
            result.append(argument.format_map(fields))
        except KeyError as error:
            raise ValueError(f"unknown command placeholder: {error.args[0]}") from error
    return result


def _rewrite_input_argument(argument: str, replacements: Mapping[str, str], base: Path) -> str:
    """Redirect direct input arguments, including --option=PATH forms, to snapshots."""
    def replacement_for(value: str) -> Optional[str]:
        replacement = replacements.get(value)
        if replacement is not None:
            return replacement
        try:
            path = Path(value)
            resolved = (base / path).resolve() if not path.is_absolute() else path.resolve()
            return replacements.get(str(resolved))
        except (OSError, ValueError):
            return None

    replacement = replacement_for(argument)
    if replacement is not None:
        return replacement
    option, separator, value = argument.partition("=")
    if separator:
        replacement = replacement_for(value)
        if replacement is not None:
            return option + separator + replacement
    return argument


def _option_values(argv: Sequence[str], option: str) -> List[str]:
    values = []
    option_argv = argv[:argv.index("--")] if "--" in argv else argv
    for index, argument in enumerate(option_argv):
        if argument == option:
            if index + 1 >= len(option_argv):
                raise ValueError(f"{option} requires a value")
            values.append(option_argv[index + 1])
        elif argument.startswith(option + "="):
            values.append(argument.partition("=")[2])
    return values


def _validate_seed_binding(argv: Sequence[str], binary_name: str) -> None:
    option_argv = argv[:argv.index("--")] if "--" in argv else argv
    if any(argument == "--randomize-seed" or argument.startswith("--randomize-seed=")
           for argument in option_argv):
        raise ValueError("seed-racing collection forbids --randomize-seed")
    if binary_name.startswith("nextpnr"):
        if _option_values(option_argv, "--seed") != ["{seed}"]:
            raise ValueError("nextpnr collection requires exactly one --seed {seed} binding before --")
    elif "{seed}" not in argv[1:]:
        raise ValueError("collection command requires an exact {seed} argv binding")


def _resolved_binary(argv0: str, cwd: Optional[str], environment: Mapping[str, str]) -> Optional[Path]:
    candidate = Path(argv0)
    if candidate.is_absolute() or candidate.parent != Path("."):
        if not candidate.is_absolute() and cwd:
            candidate = Path(cwd) / candidate
        return candidate.resolve() if candidate.is_file() else None
    found = shutil.which(argv0, path=environment.get("PATH", os.environ.get("PATH")))
    return Path(found).resolve() if found else None


def _validate_collection_executable(executable: Path,
                                    dependency_paths: Sequence[Path] = ()) -> None:
    """Keep the collector scoped to native nextpnr executables."""
    if not executable.name.startswith("nextpnr"):
        raise ValueError(
            "collection requires a native nextpnr ELF executable; standalone language "
            "interpreters and generic launchers have unbounded implicit module/resource inputs")
    python_markers = (b"Py_InitializeFromConfig\x00", b"Py_Initialize\x00")
    python_runtime = next(
        (path for path in (executable, *dependency_paths)
         if _file_contains_any(path, python_markers) or
         (_elf_dynamic_names(path)[0] or "").startswith("libpython")),
        None)
    if python_runtime is not None:
        raise ValueError(
            "collection requires nextpnr built with BUILD_PYTHON=OFF; embedded Python "
            f"runtime {python_runtime} loads implicit standard-library, site, and "
            "extension-module inputs")
    if not executable.name.startswith("nextpnr-mistral"):
        raise ValueError(
            "seed-racing collection currently supports only nextpnr-mistral because "
            "other architectures lack authoritative final analogue timing evidence")
    if not _file_contains_any(executable, (b"nextpnr.seed-racing.native.v1\x00",)):
        raise ValueError(
            "collection requires a native nextpnr build with the seed-racing contract; "
            "a nextpnr-like filename is not executable authentication")


def _child_environment(explicit: Mapping[str, str]) -> Dict[str, str]:
    """Build the complete recorded child environment without ambient routing controls."""
    environment = {name: os.environ[name] for name in BASE_ENVIRONMENT_VARIABLES if name in os.environ}
    if os.name != "nt":
        environment["LC_ALL"] = "C"
    environment.update(explicit)
    return environment


def _cpu_identity() -> Dict[str, str]:
    identity: Dict[str, str] = {}
    try:
        for line in Path("/proc/cpuinfo").read_text(encoding="utf-8").splitlines():
            if not line.strip() and identity:
                break
            name, separator, value = line.partition(":")
            key = name.strip()
            if separator and key in {"vendor_id", "model name", "cpu family", "model",
                                     "stepping", "microcode"}:
                identity[key] = value.strip()
    except (OSError, UnicodeError):
        pass
    return identity


def _elf_interpreter(path: Path) -> Optional[Path]:
    """Read PT_INTERP without trusting another executable during collection."""
    with path.open("rb") as stream:
        ident = stream.read(16)
        if len(ident) != 16 or ident[:4] != b"\x7fELF" or ident[4] not in (1, 2) or ident[5] not in (1, 2):
            return None
        endian = "<" if ident[5] == 1 else ">"
        header_format = endian + ("HHIIIIIHHHHHH" if ident[4] == 1 else "HHIQQQIHHHHHH")
        header_size = struct.calcsize(header_format)
        header = struct.unpack(header_format, stream.read(header_size))
        program_offset, program_entry_size, program_count = (
            (header[4], header[8], header[9]) if ident[4] == 1 else (header[4], header[8], header[9]))
        program_format = endian + ("IIIIIIII" if ident[4] == 1 else "IIQQQQQQ")
        for index in range(program_count):
            stream.seek(program_offset + index * program_entry_size)
            program = struct.unpack(program_format, stream.read(struct.calcsize(program_format)))
            if program[0] != 3:  # PT_INTERP
                continue
            offset = program[1] if ident[4] == 1 else program[2]
            size = program[4] if ident[4] == 1 else program[5]
            stream.seek(offset)
            value = stream.read(size).split(b"\0", 1)[0].decode("utf-8")
            return Path(value).resolve()
    return None


def _elf_dynamic_names(path: Path) -> Tuple[Optional[str], List[str]]:
    """Read DT_SONAME and DT_NEEDED without executing another program."""
    with path.open("rb") as stream:
        ident = stream.read(16)
        if len(ident) != 16 or ident[:4] != b"\x7fELF" or ident[4] not in (1, 2) or ident[5] not in (1, 2):
            raise ValueError(f"runtime object is not a supported ELF file: {path}")
        elf_class = ident[4]
        endian = "<" if ident[5] == 1 else ">"
        header_format = endian + ("HHIIIIIHHHHHH" if elf_class == 1 else "HHIQQQIHHHHHH")
        header_size = struct.calcsize(header_format)
        header_data = stream.read(header_size)
        if len(header_data) != header_size:
            raise ValueError(f"truncated ELF header: {path}")
        header = struct.unpack(header_format, header_data)
        program_offset, program_entry_size, program_count = header[4], header[8], header[9]
        program_format = endian + ("IIIIIIII" if elf_class == 1 else "IIQQQQQQ")
        program_size = struct.calcsize(program_format)
        if program_entry_size < program_size:
            raise ValueError(f"invalid ELF program header size: {path}")
        loads: List[Tuple[int, int, int]] = []
        dynamic: Optional[Tuple[int, int]] = None
        for index in range(program_count):
            stream.seek(program_offset + index * program_entry_size)
            program_data = stream.read(program_size)
            if len(program_data) != program_size:
                raise ValueError(f"truncated ELF program headers: {path}")
            program = struct.unpack(program_format, program_data)
            if elf_class == 1:
                kind, offset, address, file_size = program[0], program[1], program[2], program[4]
            else:
                kind, offset, address, file_size = program[0], program[2], program[3], program[5]
            if kind == 1:  # PT_LOAD
                loads.append((address, offset, file_size))
            elif kind == 2:  # PT_DYNAMIC
                dynamic = (offset, file_size)
        if dynamic is None:
            return None, []
        entry_format = endian + ("iI" if elf_class == 1 else "qQ")
        entry_size = struct.calcsize(entry_format)
        string_address = None
        string_size = None
        needed_offsets: List[int] = []
        soname_offset = None
        for offset in range(dynamic[0], dynamic[0] + dynamic[1], entry_size):
            stream.seek(offset)
            entry_data = stream.read(entry_size)
            if len(entry_data) != entry_size:
                raise ValueError(f"truncated ELF dynamic table: {path}")
            tag, value = struct.unpack(entry_format, entry_data)
            if tag == 0:  # DT_NULL
                break
            if tag == 1:  # DT_NEEDED
                needed_offsets.append(value)
            elif tag == 5:  # DT_STRTAB
                string_address = value
            elif tag == 10:  # DT_STRSZ
                string_size = value
            elif tag == 14:  # DT_SONAME
                soname_offset = value
        if string_address is None or string_size is None:
            if needed_offsets or soname_offset is not None:
                raise ValueError(f"ELF dynamic names lack a bounded string table: {path}")
            return None, []
        string_offset = None
        for address, offset, file_size in loads:
            if address <= string_address and string_address + string_size <= address + file_size:
                string_offset = offset + string_address - address
                break
        if string_offset is None:
            raise ValueError(f"ELF dynamic string table is outside file-backed segments: {path}")
        stream.seek(string_offset)
        strings = stream.read(string_size)
        if len(strings) != string_size:
            raise ValueError(f"truncated ELF dynamic string table: {path}")

    def name_at(offset: int) -> str:
        if offset < 0 or offset >= len(strings):
            raise ValueError(f"invalid ELF dynamic string offset: {path}")
        end = strings.find(b"\0", offset)
        if end < 0:
            raise ValueError(f"unterminated ELF dynamic string: {path}")
        try:
            value = strings[offset:end].decode("utf-8")
        except UnicodeDecodeError as error:
            raise ValueError(f"non-UTF-8 ELF dynamic name: {path}") from error
        if not value or "/" in value:
            raise ValueError(f"invalid ELF dynamic name: {path}")
        return value

    soname = name_at(soname_offset) if soname_offset is not None else None
    return soname, [name_at(offset) for offset in needed_offsets]


def _runtime_environment_evidence(executable: Path, environment: Mapping[str, str],
                                  share_directory: Optional[Path] = None,
                                  recorded_executable: Optional[Path] = None,
                                  pass_fds: Sequence[int] = ()) -> Dict[str, Any]:
    """Derive content-addressed loader/library/platform evidence for execution."""
    recorded_executable = recorded_executable or executable
    with executable.open("rb") as stream:
        prefix = stream.read(4096)
    if not prefix.startswith(b"\x7fELF"):
        raise ValueError(
            "collection requires a native nextpnr ELF executable; shebang commands are unsupported")
    paths = set()
    dependency_bindings: Dict[str, Path] = {}
    completed = subprocess.run(
        ["ldd", str(executable)], env=dict(environment), stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True, check=False, pass_fds=tuple(pass_fds))
    if completed.returncode != 0:
        diagnostic = (completed.stdout + "\n" + completed.stderr).lower()
        if "not a dynamic executable" not in diagnostic and "statically linked" not in diagnostic:
            raise ValueError(
                f"cannot derive runtime dependencies for {recorded_executable}: "
                f"{completed.stderr.strip()}")
    else:
        for line in completed.stdout.splitlines():
            words = line.strip().split()
            candidate = None
            needed_name = None
            if "=>" in words and words.index("=>") + 1 < len(words):
                needed_name = words[0]
                candidate = words[words.index("=>") + 1]
            elif words:
                candidate = words[0]
            if candidate and candidate.startswith("/") and Path(candidate).is_file():
                resolved = Path(candidate).resolve()
                paths.add(resolved)
                if needed_name is not None:
                    previous = dependency_bindings.setdefault(needed_name, resolved)
                    if previous != resolved:
                        raise ValueError(
                            f"runtime dependency name resolves to multiple objects: {needed_name}")
    files = [{"path": str(recorded_executable), "sha256": sha256_file(executable)}]
    files.extend({"path": str(path), "sha256": sha256_file(path)}
                 for path in sorted(paths, key=str))
    if share_directory is not None:
        for path in sorted(share_directory.rglob("*"), key=str):
            if path.is_file():
                files.append({"path": str(path.resolve()), "sha256": sha256_file(path)})
    optional_system_paths = (
        Path("/etc/os-release"), Path("/sys/devices/system/cpu/online"),
        Path("/sys/devices/virtual/dmi/id/product_uuid"),
        Path("/proc/driver/nvidia/version"))
    for path in optional_system_paths:
        if not path.is_file():
            continue
        try:
            digest = sha256_file(path)
        except OSError:
            continue
        files.append({"path": str(path), "sha256": digest})
    uname = os.uname()
    dynamic_loader = _elf_interpreter(executable)
    if dynamic_loader is not None:
        paths.add(dynamic_loader)
        if not any(item["path"] == str(dynamic_loader) for item in files):
            files.append({"path": str(dynamic_loader), "sha256": sha256_file(dynamic_loader)})
    bound_dependencies = []
    for needed_name, path in sorted(dependency_bindings.items()):
        soname, unused_needed = _elf_dynamic_names(path)
        if soname != needed_name:
            raise ValueError(
                f"runtime dependency {path} cannot be sealed by preload: "
                f"DT_NEEDED {needed_name!r} does not match DT_SONAME {soname!r}")
        bound_dependencies.append({"needed": needed_name, "path": str(path), "soname": soname})
    provided = set(dependency_bindings)
    if dynamic_loader is not None:
        loader_soname, unused_loader_needed = _elf_dynamic_names(dynamic_loader)
        if loader_soname is not None:
            provided.add(loader_soname)
    closure_objects = [executable] + [path for path in paths if path != dynamic_loader]
    for path in closure_objects:
        unused_soname, needed_names = _elf_dynamic_names(path)
        missing = sorted(set(needed_names) - provided)
        if missing:
            raise ValueError(
                f"runtime dependency closure for {path} is unresolved: {', '.join(missing)}")
    manifest = {"runtime_binary": str(recorded_executable),
                "launchers": [str(recorded_executable)], "files": files,
                "execution": {
                    "kind": "elf",
                    "dynamic_loader": str(dynamic_loader) if dynamic_loader is not None else None,
                    "dependency_paths": sorted(str(path) for path in paths),
                    "dependency_bindings": bound_dependencies},
                "platform": {"sysname": uname.sysname, "release": uname.release,
                             "machine": uname.machine},
                "cpu": _cpu_identity()}
    encoded = json.dumps(manifest, sort_keys=True, separators=(",", ":"),
                         allow_nan=False).encode("utf-8")
    return {"runtime_environment_id": "sha256:" + hashlib.sha256(encoded).hexdigest(),
            "manifest": manifest}


def _verify_runtime_environment_evidence(
        evidence: Mapping[str, Any],
        sealed_files: Sequence[Mapping[str, str]] = (),
        sealed_root: Optional[Path] = None) -> bool:
    manifest = evidence.get("manifest")
    if not isinstance(manifest, dict) or not isinstance(manifest.get("files"), list):
        return False
    encoded = json.dumps(manifest, sort_keys=True, separators=(",", ":"),
                         allow_nan=False).encode("utf-8")
    if evidence.get("runtime_environment_id") != \
            "sha256:" + hashlib.sha256(encoded).hexdigest():
        return False
    for item in manifest["files"]:
        if (not isinstance(item, dict) or not isinstance(item.get("path"), str) or
                not isinstance(item.get("sha256"), str)):
            return False
        path = Path(item["path"])
        if not path.is_file() or sha256_file(path) != item["sha256"]:
            return False
    for item in sealed_files:
        if (not isinstance(item, Mapping) or not isinstance(item.get("path"), str) or
                not isinstance(item.get("sha256"), str)):
            return False
        path = Path(item["path"])
        if not path.is_file() or sha256_file(path) != item["sha256"]:
            return False
    if sealed_root is not None:
        expected = {item["path"] for item in sealed_files
                    if sealed_root in Path(item["path"]).parents}
        actual = {str(path) for path in sealed_root.rglob("*") if path.is_file()}
        if actual != expected:
            return False
    uname = os.uname()
    return (manifest.get("platform") == {
        "sysname": uname.sysname, "release": uname.release, "machine": uname.machine} and
            manifest.get("cpu") == _cpu_identity())


def _terminate_owned_child(process: subprocess.Popen[Any], graceful: bool = True) -> None:
    """Terminate a still-PID-anchored child group and reap its leader."""
    if os.name == "posix":
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        deadline = time.monotonic() + (TERMINATION_GRACE_SECONDS if graceful else 0.0)
        while _live_process_group_members(process.pid) and time.monotonic() < deadline:
            time.sleep(min(0.01, max(0.0, deadline - time.monotonic())))
        if _live_process_group_members(process.pid):
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        quiescence_deadline = time.monotonic() + TERMINATION_GRACE_SECONDS
        while (_live_process_group_members(process.pid) and
               time.monotonic() < quiescence_deadline):
            time.sleep(min(0.01, max(0.0, quiescence_deadline - time.monotonic())))
        survivors = _live_process_group_members(process.pid)
        if survivors:
            raise RuntimeError(
                f"owned process group {process.pid} did not quiesce: {survivors}")
        try:
            process.wait(timeout=TERMINATION_GRACE_SECONDS)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
        return
    if process.poll() is not None:  # pragma: no cover - exercised on Windows only
        return
    process.terminate()  # pragma: no cover
    try:  # pragma: no cover
        process.wait(timeout=TERMINATION_GRACE_SECONDS)
    except subprocess.TimeoutExpired:  # pragma: no cover
        try:
            process.kill()
        finally:
            process.wait()


def _live_process_group_members(group_id: int) -> List[int]:
    """Return non-zombie Linux members while the unreaped leader anchors PGID."""
    members = []
    proc = Path("/proc")
    if not proc.is_dir():
        raise RuntimeError("owned process-group verification requires Linux /proc")
    for entry in proc.iterdir():
        if not entry.name.isdigit():
            continue
        try:
            stat = (entry / "stat").read_text(encoding="utf-8")
            fields = stat[stat.rfind(")") + 2:].split()
            state, process_group = fields[0], int(fields[2])
        except (FileNotFoundError, ProcessLookupError, PermissionError, ValueError, IndexError):
            continue
        if process_group == group_id and state != "Z":
            members.append(int(entry.name))
    return members


@dataclass(frozen=True)
class RunSpec:
    run_id: str
    seed: Any
    repeat: int
    directory: Path
    argv: Tuple[str, ...]


def _probe_native_contract(argv: Sequence[str], cwd: Optional[str],
                           environment: Mapping[str, str], pass_fds: Sequence[int]) -> None:
    try:
        contract = subprocess.run(
            list(argv), cwd=cwd, env=dict(environment), stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, check=False, timeout=10,
            pass_fds=tuple(pass_fds))
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ValueError("native nextpnr seed-racing contract probe failed") from error
    if contract.returncode != 0 or contract.stdout != "nextpnr.seed-racing.native.v1\n":
        raise ValueError(
            "collection executable failed the native nextpnr seed-racing contract probe")


class Collector:
    def __init__(self, manifest: Mapping[str, Any], output_root: Path):
        self.manifest = validate_collection_manifest(manifest)
        self.output_root = output_root.resolve()
        self._active: Dict[str, subprocess.Popen[Any]] = {}
        self._cancelled_runs = set()
        self._frozen_inputs: Optional[List[Dict[str, Any]]] = None
        self._frozen_binary: Optional[Dict[str, Any]] = None
        self._input_replacements: Dict[str, str] = {}
        self._input_roles: Dict[str, str] = {}
        self._snapshot_streams: List[BinaryIO] = []
        self._snapshot_fds: List[int] = []
        self._binary_launch_path: Optional[str] = None
        self._runtime_launch_prefix: List[str] = []
        self._runtime_environment: Dict[str, str] = {}
        self._child_environment: Optional[Dict[str, str]] = None
        self._cohort_identity: Optional[Dict[str, Any]] = None
        self._runtime_evidence: Optional[Dict[str, Any]] = None
        self._runtime_sealed_files: List[Dict[str, str]] = []
        self._runtime_sealed_root: Optional[Path] = None
        self._runtime_share_watch: Optional[int] = None
        self._cohort_lock: Optional[BinaryIO] = None
        self._lock = threading.Lock()
        self._cancelled = threading.Event()

    def plan(self) -> List[RunSpec]:
        cohort_id = _safe_component(self.manifest["cohort"]["id"])
        command_base = Path(self.manifest["cwd"] or os.getcwd())
        specs = []
        for seed in self.manifest["seeds"]:
            for repeat in range(1, self.manifest["repeats"] + 1):
                base = f"seed-{_safe_component(seed)}-repeat-{repeat}"
                # UUIDs make independent collectors non-overwriting without relying on a race-prone scan.
                run_id = f"{base}-{uuid.uuid4().hex[:12]}"
                directory = self.output_root / cohort_id / run_id
                fields = {
                    "seed": str(seed),
                    "repeat": str(repeat),
                    "run_id": run_id,
                    "run_dir": str(directory),
                    "telemetry": str(directory / self.manifest["artifacts"].get(
                        "telemetry", "telemetry.jsonl")),
                    "report": str(directory / self.manifest["artifacts"].get(
                        "final_report", "report.json")),
                }
                argv = _expand_argv(self.manifest["command"], fields)
                argv = [_rewrite_input_argument(argument, self._input_replacements, command_base)
                        for argument in argv]
                if self._frozen_binary is not None:
                    argv[0] = self._frozen_binary["snapshot_path"]
                specs.append(RunSpec(run_id, seed, repeat, directory, tuple(argv)))
        return specs

    def _snapshot_binary(self) -> Dict[str, Any]:
        if self._child_environment is None:
            raise RuntimeError("cohort environment was not frozen")
        environment = self._child_environment
        resolved = _resolved_binary(self.manifest["command"][0], self.manifest["cwd"], environment)
        if resolved is None:
            raise ValueError(f"cannot resolve cohort executable: {self.manifest['command'][0]}")
        _validate_collection_executable(resolved)
        snapshot_root = self.output_root / f"cohort-binary-{uuid.uuid4().hex}"
        snapshot_root.mkdir(parents=True, exist_ok=False)
        snapshot_bin = snapshot_root / "bin"
        snapshot_bin.mkdir()
        snapshot = snapshot_bin / _safe_component(resolved.name)
        frozen = _copy_to_readonly_descriptor(resolved, snapshot, 0o555)
        self._snapshot_streams.append(frozen)
        self._binary_launch_path = _descriptor_path(frozen)
        executable_dir = resolved.parent
        share_candidates = [executable_dir / "share"]
        if resolved.name.startswith("nextpnr"):
            share_candidates.extend((executable_dir.parent / "share" / "nextpnr",
                                     executable_dir.parent / "share"))
        source_share = next((path.resolve() for path in share_candidates if path.is_dir()), None)
        snapshot_share = None
        if source_share is not None:
            snapshot_share = snapshot_bin / "share"
            shutil.copytree(source_share, snapshot_share, symlinks=False)
        self._runtime_evidence = _runtime_environment_evidence(
            Path(self._binary_launch_path), environment, source_share,
            recorded_executable=resolved, pass_fds=(frozen.fileno(),))
        execution = self._runtime_evidence["manifest"]["execution"]
        _validate_collection_executable(
            Path(self._binary_launch_path),
            tuple(Path(path) for path in execution["dependency_paths"]))
        expected_runtime_hashes = {
            item["path"]: item["sha256"] for item in self._runtime_evidence["manifest"]["files"]}
        if _sha256_stream(frozen) != expected_runtime_hashes.get(str(resolved)):
            raise RuntimeError("cohort executable changed while deriving its runtime closure")
        dependency_bindings = {
            item["path"]: item["needed"] for item in execution["dependency_bindings"]}
        sealed_runtime: Dict[str, str] = {}
        for source_text in execution["dependency_paths"]:
            source = Path(source_text)
            frozen_runtime = _copy_to_readonly_descriptor(source, None, 0o555)
            self._snapshot_streams.append(frozen_runtime)
            if _sha256_stream(frozen_runtime) != expected_runtime_hashes.get(str(source)):
                raise RuntimeError(f"runtime dependency changed while snapshotting: {source}")
            expected_name = dependency_bindings.get(str(source))
            if expected_name is not None:
                soname, unused_needed = _elf_dynamic_names(Path(_descriptor_path(frozen_runtime)))
                if soname != expected_name:
                    raise RuntimeError(
                        f"runtime dependency identity changed while snapshotting: {source}")
            sealed_runtime[str(source)] = _descriptor_path(frozen_runtime)
        if source_share is not None:
            source_files = {str(path.relative_to(source_share)): sha256_file(path)
                            for path in source_share.rglob("*") if path.is_file()}
            snapshot_files = {str(path.relative_to(snapshot_share)): sha256_file(path)
                              for path in snapshot_share.rglob("*") if path.is_file()}
            if source_files != snapshot_files:
                raise RuntimeError("runtime share tree changed while it was snapshotted")
            for path in snapshot_share.rglob("*"):
                path.chmod(0o555 if path.is_dir() else 0o444)
            snapshot_share.chmod(0o555)
            self._runtime_share_watch = _watch_tree_mutations(snapshot_share)
        snapshot_bin.chmod(0o555)
        runtime_directory_fd = os.open(snapshot_bin, os.O_RDONLY | os.O_DIRECTORY)
        self._snapshot_fds.append(runtime_directory_fd)
        runtime_directory = Path(_descriptor_number_path(runtime_directory_fd))
        self._runtime_environment["NEXTPNR_EXECUTABLE_DIR"] = str(runtime_directory)
        dynamic_loader = execution["dynamic_loader"]
        if dynamic_loader is not None:
            if not Path(dynamic_loader).name.startswith("ld-linux"):
                raise RuntimeError(
                    f"unsupported dynamic loader for frozen collection: {dynamic_loader}")
            loader = sealed_runtime[dynamic_loader]
            original_runtime_binary = self._runtime_evidence["manifest"]["runtime_binary"]
            preload = [sealed_runtime[path] for path in execution["dependency_paths"]
                       if path not in {dynamic_loader, original_runtime_binary, str(resolved)}]
            loader_options = ["--argv0", original_runtime_binary, "--inhibit-cache"]
            if preload:
                loader_options += ["--preload", ":".join(preload)]
            loader_options[1] = str(resolved)
            self._runtime_launch_prefix = [loader] + loader_options + [self._binary_launch_path]
        contract_argv = ((self._runtime_launch_prefix or [self._binary_launch_path]) +
                         ["--seed-racing-contract"])
        contract_environment = dict(environment)
        contract_environment.update(self._runtime_environment)
        _probe_native_contract(
            contract_argv, self.manifest["cwd"], contract_environment,
            tuple(stream.fileno() for stream in self._snapshot_streams) +
            tuple(self._snapshot_fds))
        if snapshot_share is not None:
            consumed_share = runtime_directory / "share"
            self._runtime_sealed_files.extend(
                {"path": str(consumed_share / relative), "sha256": digest}
                for relative, digest in sorted(snapshot_files.items()))
            self._runtime_sealed_root = consumed_share
        self.manifest["provenance"]["runtime_environment_id"] = \
            self._runtime_evidence["runtime_environment_id"]
        return {
            "requested_path": self.manifest["command"][0],
            "resolved_path": str(resolved),
            "snapshot_path": str(snapshot),
            "launch_path": self._binary_launch_path,
            "sha256": _sha256_stream(frozen),
            "runtime_executable_dir": str(executable_dir),
            "runtime_share_directory": str(source_share) if source_share is not None else None,
            "runtime_environment": self._runtime_evidence,
        }

    def _snapshot_inputs(self) -> List[Dict[str, Any]]:
        snapshot_root = self.output_root / f"cohort-inputs-{uuid.uuid4().hex}"
        snapshot_root.mkdir(parents=True, exist_ok=False)
        records = []
        base = Path(self.manifest["cwd"] or os.getcwd())
        for index, item in enumerate(self.manifest["inputs"]):
            path = Path(item["path"])
            resolved = (base / path).resolve() if not path.is_absolute() else path.resolve()
            record = dict(item)
            if not resolved.is_file():
                raise ValueError(f"cannot snapshot declared cohort input: {item['path']}")
            snapshot = snapshot_root / f"{index:04d}-{_safe_component(resolved.name)}"
            frozen = _copy_to_readonly_descriptor(resolved, snapshot, 0o444)
            self._snapshot_streams.append(frozen)
            stable_path = _descriptor_path(frozen)
            digest = _sha256_stream(frozen)
            record.update({"resolved_path": str(resolved), "snapshot_path": str(snapshot),
                           "launch_path": stable_path, "sha256": digest})
            self._input_replacements[item["path"]] = stable_path
            self._input_replacements[str(resolved)] = stable_path
            self._input_roles[item["path"]] = item["role"]
            self._input_roles[str(resolved)] = item["role"]
            records.append(record)
        return records

    def _requires_execution_identity(self) -> bool:
        return _option_values(self.manifest["command"], "--router") == ["gpu"]

    def _validate_router_binding(self) -> None:
        if self._frozen_binary is None:
            raise RuntimeError("cohort executable was not frozen")
        binary_name = Path(self._frozen_binary["resolved_path"]).name
        if binary_name.startswith("nextpnr-generic"):
            raise ValueError(
                "seed-racing collection rejects nextpnr-generic because it does not emit "
                "authoritative final analogue timing evidence")
        if binary_name.startswith("nextpnr-mistral") and self.manifest["architecture"] != "mistral":
            raise ValueError(
                "nextpnr-mistral collection requires the manifest architecture 'mistral'")
        gpu_capable = (binary_name.startswith("nextpnr-mistral") or
                       self.manifest["architecture"] == "mistral")
        if gpu_capable and len(_option_values(self.manifest["command"], "--router")) != 1:
            raise ValueError(
                "GPU-capable nextpnr collection requires exactly one explicit --router binding")
        if not gpu_capable:
            return
        command = self.manifest["command"]
        if "--" in command and command.index("--") + 1 < len(command):
            raise ValueError("GPU-capable collection forbids positional Python run scripts")
        for option in ("--run", "--pre-pack", "--pre-place", "--pre-route"):
            if _option_values(command, option):
                raise ValueError(
                    f"GPU-capable collection forbids route-mutating Python hook {option}")
        declared_router = _option_values(command, "--router")[0]
        if declared_router != "gpu":
            raise ValueError(
                "seed-racing collection currently requires --router gpu because router1/router2 "
                "do not emit equivalent terminal legality telemetry")
        for option in ("--json", "--read"):
            for value in _option_values(command, option):
                base = Path(self.manifest["cwd"] or os.getcwd())
                path = Path(value)
                resolved = (base / path).resolve() if not path.is_absolute() else path.resolve()
                record = next((item for item in (self._frozen_inputs or [])
                               if item["resolved_path"] == str(resolved)), None)
                if record is None:
                    continue
                try:
                    document = json.loads(Path(record["launch_path"]).read_text(encoding="utf-8"))
                except (OSError, UnicodeError, json.JSONDecodeError) as error:
                    raise ValueError(f"cannot audit router settings in {option} input: {value}") from error
                pending = [document]
                while pending:
                    node = pending.pop()
                    if isinstance(node, dict):
                        settings = node.get("settings")
                        if isinstance(settings, dict) and "router" in settings and \
                                str(settings["router"]) != declared_router:
                            raise ValueError(
                                f"{option} input overrides declared router {declared_router!r}")
                        pending.extend(node.values())
                    elif isinstance(node, list):
                        pending.extend(node)

    def _validate_telemetry_binding(self) -> None:
        if not self._requires_execution_identity():
            return
        if _option_values(self.manifest["command"], "--gpu-telemetry") != ["{telemetry}"]:
            raise ValueError(
                "direct nextpnr collection requires exactly one --gpu-telemetry {telemetry} binding")

    def _validate_seed_binding(self) -> None:
        if self._frozen_binary is None:
            raise RuntimeError("cohort executable was not frozen")
        _validate_seed_binding(self.manifest["command"],
                               Path(self._frozen_binary["resolved_path"]).name)

    def _validate_implicit_runtime_inputs(self) -> None:
        if self._frozen_binary is None:
            raise RuntimeError("cohort executable was not frozen")
        binary_name = Path(self._frozen_binary["resolved_path"]).name
        if (self.manifest["architecture"] != "himbaechel" and
                not binary_name.startswith("nextpnr-himbaechel")):
            return
        chipdb_values = _option_values(self.manifest["command"], "--chipdb")
        if len(chipdb_values) != 1:
            raise ValueError(
                "nextpnr-himbaechel collection requires exactly one explicit --chipdb "
                "declared in inputs")
        chipdb = Path(chipdb_values[0])
        base = Path(self.manifest["cwd"] or os.getcwd())
        resolved = (base / chipdb).resolve() if not chipdb.is_absolute() else chipdb.resolve()
        if chipdb_values[0] not in self._input_replacements and str(resolved) not in self._input_replacements:
            raise ValueError("nextpnr-himbaechel --chipdb must be declared in inputs")

    def _validate_known_input_options(self) -> None:
        if self._frozen_binary is None:
            raise RuntimeError("cohort executable was not frozen")
        if "--" in self.manifest["command"] and \
                self.manifest["command"].index("--") + 1 < len(self.manifest["command"]):
            raise ValueError(
                "nextpnr positional Python scripts are unsupported because imported modules "
                "and interpreter resources cannot be bounded")
        input_options = {
            "--json": "mapped_netlist", "--sdc": "constraints", "--qsf": "constraints",
            "--chipdb": "chipdb", "--pcf": "constraints", "--pdc": "constraints",
            "--xdc": "constraints", "--cst": "constraints", "--lpf": "constraints",
            "--read": "design_input", "--pre-pack": "python_hook",
            "--pre-place": "python_hook", "--pre-route": "python_hook",
            "--post-route": "python_hook", "--on-failure": "python_hook",
            "--run": "python_hook", "--fes-cart": "mapped_netlist",
            "--remap-critical": "timing_report", "--remap-plan": "remap_plan",
            "--remap-post-plan": "remap_plan",
            "--remap-comb-critical": "timing_report",
            "--remap-comb-plan": "remap_plan",
            "--remap-decompose-critical": "timing_report",
            "--remap-lut-pair-critical": "timing_report",
            "--remap-lut-driver-critical": "timing_report",
        }
        base = Path(self.manifest["cwd"] or os.getcwd())
        for option, expected_role in input_options.items():
            for value in _option_values(self.manifest["command"], option):
                if expected_role == "python_hook":
                    raise ValueError(
                        f"nextpnr Python hook {option} is unsupported because imported modules "
                        "and interpreter resources cannot be bounded")
                if expected_role == "remap_plan":
                    raise ValueError(
                        f"nextpnr remap plan {option} is unsupported because its nested report "
                        "paths cannot be descriptor-bound")
                path = Path(value)
                resolved = (base / path).resolve() if not path.is_absolute() else path.resolve()
                if value not in self._input_replacements and str(resolved) not in self._input_replacements:
                    raise ValueError(f"nextpnr input {option} must be declared in inputs: {value}")
                role = self._input_roles.get(value, self._input_roles.get(str(resolved)))
                if role != expected_role:
                    raise ValueError(
                        f"nextpnr input {option} requires role {expected_role!r}, got {role!r}")
        binary_name = Path(self._frozen_binary["resolved_path"]).name
        if binary_name.startswith("nextpnr-mistral") or self.manifest["architecture"] == "mistral":
            unsupported = sorted(name for name in self.manifest["environment"]
                                 if name.startswith("NEXTPNR_MISTRAL_"))
            if unsupported:
                raise ValueError(
                    "Mistral path/control environment is unsupported for frozen collection: " +
                    ", ".join(unsupported))

    def _validate_no_undeclared_file_arguments(self, specs: Sequence[RunSpec]) -> None:
        base = Path(self.manifest["cwd"] or os.getcwd())
        frozen_paths = set(self._input_replacements.values())
        for spec in specs:
            for index, argument in enumerate(spec.argv[1:], start=1):
                value = argument.partition("=")[2] if "=" in argument else argument
                if (not value or value.startswith("-") or value in frozen_paths or
                        spec.argv[index - 1] == "-c"):
                    continue
                path = Path(value)
                resolved = (base / path).resolve() if not path.is_absolute() else path.resolve()
                if (self._frozen_binary is not None and
                        str(resolved) == self._frozen_binary["resolved_path"]):
                    continue
                if resolved.is_file():
                    raise ValueError(
                        f"expanded command file argument must be declared in inputs: {value}")

    def _close_snapshots(self) -> None:
        for stream in self._snapshot_streams:
            stream.close()
        self._snapshot_streams.clear()
        for descriptor in self._snapshot_fds:
            os.close(descriptor)
        self._snapshot_fds.clear()
        if self._runtime_share_watch is not None:
            os.close(self._runtime_share_watch)
            self._runtime_share_watch = None

    def _validate_declared_inputs_bound(self, specs: Sequence[RunSpec]) -> None:
        for record in self._frozen_inputs or []:
            launch_path = record["launch_path"]
            for spec in specs:
                values = [argument.partition("=")[2] if "=" in argument else argument
                          for argument in spec.argv]
                if launch_path not in values:
                    raise ValueError(
                        f"declared input is not bound to every command argv: {record['path']}")

    def _build_cohort_identity(self, execution_identity: Optional[Mapping[str, str]] = None) -> Dict[str, Any]:
        if self._frozen_binary is None or self._frozen_inputs is None or self._child_environment is None:
            raise RuntimeError("cohort evidence was not frozen")
        binary = {name: self._frozen_binary[name] for name in
                  ("requested_path", "resolved_path", "sha256", "runtime_executable_dir",
                   "runtime_share_directory", "runtime_environment")}
        inputs = [{name: item.get(name) for name in
                   ("path", "resolved_path", "role", "sha256")}
                  for item in self._frozen_inputs]
        environment = dict(self._child_environment)
        environment.update(self._runtime_environment)
        if "NEXTPNR_EXECUTABLE_DIR" in environment:
            environment["NEXTPNR_EXECUTABLE_DIR"] = "$SEALED_COHORT_RUNTIME/bin"
        record = {
            "schema_version": SCHEMA_VERSION,
            "cohort": self.manifest["cohort"],
            "architecture": self.manifest["architecture"],
            "command": self.manifest["command"],
            "cwd": str(Path(self.manifest["cwd"] or os.getcwd()).resolve()),
            "environment": environment,
            "provenance": self.manifest["provenance"],
            "binary": binary,
            "inputs": inputs,
            "artifacts": self.manifest["artifacts"],
            "required_clocks": self.manifest["required_clocks"],
            "limits": self.manifest["limits"],
        }
        if execution_identity is not None:
            record["execution_identity"] = dict(execution_identity)
        encoded = json.dumps(record, sort_keys=True, separators=(",", ":"),
                             allow_nan=False).encode("utf-8")
        return {"fingerprint_sha256": hashlib.sha256(encoded).hexdigest(),
                "manifest": record}

    def _cohort_identity_path(self) -> Path:
        return self.output_root / f"cohort-{_safe_component(self.manifest['cohort']['id'])}.json"

    def _lock_and_check_cohort_identity(self) -> Optional[Dict[str, Any]]:
        if fcntl is None:
            raise RuntimeError("cohort collection requires POSIX file locking")
        if self._cohort_identity is None:
            raise RuntimeError("cohort identity basis was not built")
        lock_path = self._cohort_identity_path().with_suffix(".lock")
        self._cohort_lock = lock_path.open("a+b")
        fcntl.flock(self._cohort_lock.fileno(), fcntl.LOCK_EX)
        path = self._cohort_identity_path()
        if not path.exists():
            return None
        try:
            existing = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            raise ValueError(f"existing cohort identity is unreadable: {path}") from error
        existing_manifest = existing.get("manifest") if isinstance(existing, dict) else None
        if not isinstance(existing_manifest, dict):
            raise ValueError(f"existing cohort identity is invalid: {path}")
        encoded = json.dumps(existing_manifest, sort_keys=True, separators=(",", ":"),
                             allow_nan=False).encode("utf-8")
        fingerprint = existing.get("fingerprint_sha256")
        if (not isinstance(fingerprint, str) or
                hashlib.sha256(encoded).hexdigest() != fingerprint):
            raise ValueError(f"existing cohort identity fingerprint is invalid: {path}")
        existing_basis = dict(existing_manifest)
        existing_basis.pop("execution_identity", None)
        if existing_basis != self._cohort_identity["manifest"]:
            raise ValueError(
                f"cohort id {self.manifest['cohort']['id']!r} already has a different fingerprint")
        return existing

    def _claim_cohort_identity(self, existing: Optional[Mapping[str, Any]]) -> None:
        if self._cohort_identity is None:
            raise RuntimeError("cohort identity was not built")
        if existing is not None:
            if existing != self._cohort_identity:
                raise ValueError(
                    f"cohort id {self.manifest['cohort']['id']!r} already has a different execution identity")
            return
        _json_dump(self._cohort_identity_path(), self._cohort_identity)

    def _run_one(self, spec: RunSpec, deadline: float) -> Dict[str, Any]:
        if self._cancelled.is_set():
            return {"run_id": spec.run_id, "seed": spec.seed, "repeat": spec.repeat, "status": "cancelled",
                    "process_started": False, "termination_reason": "collector_cancelled_before_launch"}
        now = time.monotonic()
        if now >= deadline:
            return {"run_id": spec.run_id, "seed": spec.seed, "repeat": spec.repeat,
                    "status": "not_started_total_budget", "process_started": False,
                    "termination_reason": "total_budget_expired_before_run_setup"}
        spec.directory.mkdir(parents=True, exist_ok=False)
        for relative in self.manifest["artifacts"].values():
            (spec.directory / relative).parent.mkdir(parents=True, exist_ok=True)
        artifact_paths = {"stdout": spec.directory / "stdout.log",
                          "stderr": spec.directory / "stderr.log"}
        artifact_paths.update({name: spec.directory / relative
                               for name, relative in self.manifest["artifacts"].items()})
        artifact_sources: Dict[str, BinaryIO] = {}
        artifact_initial_stats: Dict[str, os.stat_result] = {}
        try:
            for name, path in artifact_paths.items():
                stream = path.open("x+b")
                artifact_sources[name] = stream
                artifact_initial_stats[name] = os.fstat(stream.fileno())
        except BaseException:
            for stream in artifact_sources.values():
                stream.close()
            raise
        artifact_replacements = {
            str(path): _descriptor_path(artifact_sources[name])
            for name, path in artifact_paths.items() if name not in {"stdout", "stderr"}
        }

        def rewrite_artifact(argument: str) -> str:
            if argument in artifact_replacements:
                return artifact_replacements[argument]
            prefix, separator, value = argument.partition("=")
            if separator and value in artifact_replacements:
                return prefix + separator + artifact_replacements[value]
            return argument

        execution_spec_argv = tuple(rewrite_artifact(argument) for argument in spec.argv)
        if self._child_environment is None:
            raise RuntimeError("cohort environment was not frozen")
        environment = dict(self._child_environment)
        environment.update(self._runtime_environment)
        started_wall = time.time()
        if self._frozen_inputs is None or self._frozen_binary is None:
            raise RuntimeError("collector inputs and executable were not frozen before launch")
        immutable = {
            "schema_version": SCHEMA_VERSION,
            "run_id": spec.run_id,
            "cohort": self.manifest["cohort"],
            "seed": spec.seed,
            "replicate_index": spec.repeat,
            "argv": list(spec.argv),
            "execution_argv": (self._runtime_launch_prefix + list(execution_spec_argv[1:])
                               if self._runtime_launch_prefix else list(execution_spec_argv)),
            "cwd": str(Path(self.manifest["cwd"] or os.getcwd()).resolve()),
            "environment": environment,
            "limits": self.manifest["limits"],
            "provenance": self.manifest["provenance"],
            "inputs": self._frozen_inputs,
            "binary": self._frozen_binary,
            "artifacts": self.manifest["artifacts"],
            "cohort_identity_basis": self._cohort_identity,
            "started_unix_seconds": started_wall,
        }
        manifest_path = spec.directory / "manifest.json"
        _json_dump(manifest_path, immutable)
        manifest_sha256 = sha256_file(manifest_path)
        result: Dict[str, Any] = {"run_id": spec.run_id, "seed": spec.seed, "repeat": spec.repeat,
                                  "process_started": False}
        stdout_path, stderr_path = artifact_paths["stdout"], artifact_paths["stderr"]
        started = time.monotonic()
        process: Optional[subprocess.Popen[Any]] = None
        try:
            stdout, stderr = artifact_sources["stdout"], artifact_sources["stderr"]
            with contextlib.nullcontext():
                with self._lock:
                    if self._cancelled.is_set():
                        result.update({"status": "cancelled",
                                       "termination_reason": "collector_cancelled_before_launch"})
                    elif time.monotonic() >= deadline:
                        result.update({"status": "not_started_total_budget",
                                       "termination_reason": "total_budget_expired_before_launch"})
                    else:
                        execution_argv = self._runtime_launch_prefix + list(execution_spec_argv[1:])
                        execution_path = (execution_argv[0] if self._runtime_launch_prefix
                                          else self._binary_launch_path)
                        process = subprocess.Popen(
                            execution_argv if self._runtime_launch_prefix else list(execution_spec_argv),
                            cwd=self.manifest["cwd"], env=environment, stdout=stdout, stderr=stderr,
                            executable=execution_path,
                            pass_fds=(tuple(stream.fileno() for stream in self._snapshot_streams) +
                                      tuple(self._snapshot_fds) +
                                      tuple(stream.fileno() for stream in artifact_sources.values())),
                            start_new_session=True,
                        )
                        result["process_started"] = True
                        self._active[spec.run_id] = process
                if process is not None:
                    timeout = min(self.manifest["limits"]["per_run_seconds"],
                                  max(0.001, deadline - time.monotonic()))
                    wait_deadline = time.monotonic() + timeout
                    pidfd = None
                    group_quiesced = False
                    try:
                        pidfd = os.pidfd_open(process.pid)
                        while True:
                            with self._lock:
                                cancelled = spec.run_id in self._cancelled_runs
                            if cancelled:
                                _terminate_owned_child(process)
                                group_quiesced = True
                                result.update({"status": "cancelled",
                                               "termination_reason": "collector_cancelled",
                                               "return_code": process.returncode,
                                               "signal": -process.returncode if process.returncode and
                                               process.returncode < 0 else None})
                                break
                            remaining = wait_deadline - time.monotonic()
                            if remaining <= 0:
                                _terminate_owned_child(process)
                                group_quiesced = True
                                reason = ("total_budget" if time.monotonic() >= deadline
                                          else "per_run_timeout")
                                result.update({"status": "timeout", "termination_reason": reason,
                                               "return_code": process.returncode,
                                               "signal": -process.returncode if process.returncode and
                                               process.returncode < 0 else None})
                                break
                            if select.select([pidfd], [], [], min(0.05, remaining))[0]:
                                # The unreaped leader still anchors the process-group ID while
                                # residual descendants are terminated and artifacts quiesce.
                                _terminate_owned_child(process, graceful=False)
                                group_quiesced = True
                                with self._lock:
                                    cancelled = spec.run_id in self._cancelled_runs
                                return_code = process.returncode
                                status = ("cancelled" if cancelled else
                                          "completed" if return_code == 0 else "process_failure")
                                result.update({"status": status, "return_code": return_code,
                                               "signal": -return_code if return_code < 0 else None})
                                if cancelled:
                                    result["termination_reason"] = "collector_cancelled"
                                break
                    finally:
                        if not group_quiesced:
                            _terminate_owned_child(process, graceful=False)
                        if pidfd is not None:
                            os.close(pidfd)
        except OSError as error:
            result.update({"status": "launch_error", "error": f"{type(error).__name__}: {error}"})
        finally:
            with self._lock:
                self._active.pop(spec.run_id, None)
        result["elapsed_seconds"] = time.monotonic() - started
        if sha256_file(manifest_path) != manifest_sha256:
            raise RuntimeError("run manifest changed while the worker was active")
        artifact_streams = []
        artifacts = {}
        evidence_artifacts = {}
        try:
            for name, path in artifact_paths.items():
                source = artifact_sources[name]
                anchored = os.fstat(source.fileno())
                try:
                    visible = path.stat(follow_symlinks=False)
                except FileNotFoundError as error:
                    raise RuntimeError(f"artifact path was removed while collecting: {path}") from error
                if (visible.st_dev, visible.st_ino) != (anchored.st_dev, anchored.st_ino):
                    raise RuntimeError(f"artifact path was replaced while collecting: {path}")
                initial = artifact_initial_stats[name]
                produced = (name in {"stdout", "stderr"} or anchored.st_size != initial.st_size or
                            anchored.st_mtime_ns != initial.st_mtime_ns or
                            anchored.st_ctime_ns != initial.st_ctime_ns)
                if not produced:
                    artifacts[name] = {"path": str(path), "sha256": None,
                                       "available": False}
                    evidence_artifacts[name] = dict(artifacts[name])
                    continue
                if not stat.S_ISREG(anchored.st_mode):
                    raise RuntimeError(f"artifact is not a regular non-symlink file: {path}")
                frozen = _copy_stream_to_readonly_descriptor(source, str(path), None, 0o444)
                artifact_streams.append(frozen)
                digest = _sha256_stream(frozen)
                artifacts[name] = {"path": str(path), "sha256": digest, "available": True}
                evidence_artifacts[name] = {
                    "path": _descriptor_path(frozen), "sha256": digest, "available": True}
            result["artifacts"] = artifacts
            result["manifest_sha256"] = manifest_sha256
            result = classify_collected_result(
                result, evidence_artifacts, self.manifest["required_clocks"],
                self._requires_execution_identity())
            return result
        finally:
            for stream in artifact_streams:
                stream.close()
            for stream in artifact_sources.values():
                stream.close()

    def _bind_execution_identity(self, results: Sequence[Mapping[str, Any]]) -> Optional[Dict[str, str]]:
        observed = [result.get("outcome", {}).get("execution_backend")
                    if isinstance(result.get("outcome"), dict) else None for result in results]
        backends = {backend for backend in observed if isinstance(backend, str) and backend}
        required = self._requires_execution_identity()
        launched = [result for result in results if result.get("process_started") is not False]
        if not backends:
            if required and launched:
                raise RuntimeError("GPU cohort has no attested execution backend")
            return None
        missing_required = [result.get("run_id") for result, backend in zip(results, observed)
                            if (not isinstance(backend, str) or not backend) and
                            result.get("process_started") is not False]
        if missing_required:
            raise RuntimeError(
                f"launched GPU runs lack execution backend attestation: {missing_required}")
        if len(backends) != 1:
            raise RuntimeError(
                f"GPU cohort observed multiple execution backends: {sorted(backends)}")
        backend = next(iter(backends))
        if not _exact_execution_backend(backend):
            raise RuntimeError(
                f"GPU cohort backend lacks exact device attestation: {backend}")
        if self._runtime_evidence is None:
            raise RuntimeError("runtime environment evidence is unavailable")
        return {"backend": backend,
                "runtime_environment_id": self._runtime_evidence["runtime_environment_id"]}

    def _write_final_results(self, results: Sequence[Dict[str, Any]],
                             specs: Sequence[RunSpec]) -> None:
        if self._cohort_identity is None:
            raise RuntimeError("cohort identity was not finalized")
        directories = {spec.run_id: spec.directory for spec in specs}
        expected_backend = self._cohort_identity["manifest"].get(
            "execution_identity", {}).get("backend")
        for result in results:
            observed_backend = (result.get("outcome", {}).get("execution_backend")
                                if isinstance(result.get("outcome"), dict) else None)
            if self._requires_execution_identity() or expected_backend is not None:
                result["execution_identity_verified"] = (
                    isinstance(observed_backend, str) and observed_backend == expected_backend)
            result["cohort_identity"] = self._cohort_identity
            directory = directories.get(result["run_id"])
            if directory is None or not directory.is_dir():
                continue
            path = directory / "result.json"
            payload = dict(result)
            payload.pop("result_sha256", None)
            _json_dump(path, payload)
            result["result_sha256"] = sha256_file(path)

    def _write_failure_summary(self, results: Sequence[Mapping[str, Any]], error: BaseException) -> None:
        summary = {"schema_version": SCHEMA_VERSION,
                   "cohort": self.manifest["cohort"],
                   "cohort_identity_basis": self._cohort_identity,
                   "results": list(results),
                   "collection_error": f"{type(error).__name__}: {error}"}
        _json_dump(self.output_root / f"collection-failure-{uuid.uuid4().hex}.json", summary)

    def cancel(self) -> None:
        with self._lock:
            self._cancelled.set()
            for run_id, process in self._active.items():
                self._cancelled_runs.add(run_id)

    def run(self, dry_run: bool = False) -> List[Dict[str, Any]]:
        if dry_run:
            specs = self.plan()
            return [{"run_id": s.run_id, "seed": s.seed, "repeat": s.repeat, "directory": str(s.directory), "argv": list(s.argv), "status": "dry_run"} for s in specs]
        if not hasattr(os, "pidfd_open") or not Path("/proc").is_dir():
            raise RuntimeError("collection requires Linux pidfds and /proc")
        self.output_root.mkdir(parents=True, exist_ok=True)
        self._child_environment = _child_environment(self.manifest["environment"])
        try:
            self._frozen_binary = self._snapshot_binary()
            self._frozen_inputs = self._snapshot_inputs()
            self._validate_seed_binding()
            self._validate_implicit_runtime_inputs()
            self._validate_known_input_options()
            self._validate_router_binding()
            self._validate_telemetry_binding()
            specs = self.plan()
            self._validate_declared_inputs_bound(specs)
            self._validate_no_undeclared_file_arguments(specs)
            self._cohort_identity = self._build_cohort_identity()
            existing_identity = self._lock_and_check_cohort_identity()
            deadline = time.monotonic() + self.manifest["limits"]["total_seconds"]
            executor = concurrent.futures.ThreadPoolExecutor(
                max_workers=self.manifest["limits"]["concurrency"])
            future_specs = []
            futures_by_run = {}
            interrupted = None
            try:
                for spec in specs:
                    future = executor.submit(self._run_one, spec, deadline)
                    future_specs.append((future, spec))
                    futures_by_run[spec.run_id] = future
                for future, _spec in future_specs:
                    try:
                        future.result()
                    except Exception:
                        pass  # gather every terminal state below
            except KeyboardInterrupt as error:
                interrupted = error
                self.cancel()
                for future, _spec in future_specs:
                    future.cancel()
            finally:
                executor.shutdown(wait=True, cancel_futures=self._cancelled.is_set())
            results = []
            for spec in specs:
                future = futures_by_run.get(spec.run_id)
                if future is None:
                    results.append({"run_id": spec.run_id, "seed": spec.seed,
                                    "repeat": spec.repeat, "status": "cancelled",
                                    "process_started": False, "termination_reason":
                                    "collector_cancelled_before_submission"})
                    continue
                if future.cancelled():
                    results.append({"run_id": spec.run_id, "seed": spec.seed,
                                    "repeat": spec.repeat, "status": "cancelled",
                                    "process_started": False,
                                    "termination_reason": "collector_cancelled_before_launch"})
                    continue
                try:
                    results.append(future.result())
                except Exception as error:  # preserve other completed runs
                    results.append({"run_id": spec.run_id, "seed": spec.seed,
                                    "repeat": spec.repeat, "status": "runner_error",
                                    "error": f"{type(error).__name__}: {error}"})
            try:
                if not _tree_was_not_mutated(self._runtime_share_watch):
                    raise RuntimeError("runtime share tree was mutated during cohort collection")
                if not _verify_runtime_environment_evidence(
                        self._runtime_evidence, self._runtime_sealed_files,
                        self._runtime_sealed_root):
                    raise RuntimeError("runtime environment changed during cohort collection")
                execution_identity = self._bind_execution_identity(results)
            except BaseException as error:
                self._write_failure_summary(results, error)
                if interrupted is not None:
                    raise interrupted
                raise
            try:
                if execution_identity is not None:
                    self._cohort_identity = self._build_cohort_identity(execution_identity)
                elif existing_identity is not None:
                    self._cohort_identity = dict(existing_identity)
                claimable = (existing_identity is not None or execution_identity is not None or
                             not self._requires_execution_identity())
                if claimable:
                    self._claim_cohort_identity(existing_identity)
                self._write_final_results(results, specs)
            except BaseException as error:
                self._write_failure_summary(results, error)
                if interrupted is not None:
                    raise interrupted
                raise
            summary = {"schema_version": SCHEMA_VERSION,
                       "cohort": self.manifest["cohort"],
                       "cohort_identity": self._cohort_identity, "results": results}
            _json_dump(self.output_root / f"collection-{uuid.uuid4().hex}.json", summary)
            if interrupted is not None:
                raise interrupted
            return results
        finally:
            if self._cohort_lock is not None:
                self._cohort_lock.close()
                self._cohort_lock = None
            self._close_snapshots()


def _finite_json_tree(value: Any) -> bool:
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return _finite_number(value)
    if isinstance(value, list):
        return all(_finite_json_tree(item) for item in value)
    if isinstance(value, dict):
        return all(_finite_json_tree(item) for item in value.values())
    return True


def load_jsonl(path: Path) -> Tuple[List[Dict[str, Any]], bool]:
    """Load a valid telemetry prefix and flag truncation or schema/order errors."""
    records, truncated = [], False
    expected_sequence = 0
    prior_elapsed = -math.inf
    run_id = None
    terminal_seen = False
    phase_stack: List[Tuple[str, int]] = []
    with path.open("r", encoding="utf-8") as stream:
        for line in stream:
            if not line.strip():
                continue
            try:
                value = json.loads(
                    line, parse_constant=lambda token: (_ for _ in ()).throw(
                        ValueError(f"non-finite JSON number: {token}")))
                schema_version = value.get("schema_version") if isinstance(value, dict) else None
                sequence = value.get("sequence") if isinstance(value, dict) else None
                if (not isinstance(value, dict) or not _finite_json_tree(value) or
                        isinstance(schema_version, bool) or not isinstance(schema_version, int) or
                        schema_version != SCHEMA_VERSION or isinstance(sequence, bool) or
                        not isinstance(sequence, int) or sequence != expected_sequence or
                        not isinstance(value.get("run_id"), str) or not value["run_id"] or
                        value.get("event") not in
                        {"run_start", "phase_start", "iteration", "repair_round",
                         "phase_end", "run_end"} or
                        terminal_seen):
                    raise ValueError
                elapsed = value.get("elapsed_s")
                if (not _finite_number(elapsed) or elapsed < 0 or elapsed < prior_elapsed):
                    raise ValueError
                if run_id is None:
                    run_id = value["run_id"]
                elif value["run_id"] != run_id:
                    raise ValueError
                event, phase, attempt = value["event"], value.get("phase"), value.get("attempt")
                if expected_sequence == 0 and event != "run_start":
                    raise ValueError
                if expected_sequence != 0 and event == "run_start":
                    raise ValueError
                if event == "run_start":
                    if phase is not None or attempt is not None:
                        raise ValueError
                elif event == "phase_start":
                    if (not isinstance(phase, str) or not phase or isinstance(attempt, bool) or
                            not isinstance(attempt, int) or attempt < 0):
                        raise ValueError
                    phase_stack.append((phase, attempt))
                elif event == "phase_end":
                    if (not isinstance(phase, str) or isinstance(attempt, bool) or
                            not isinstance(attempt, int) or attempt < 0 or not phase_stack or
                            phase_stack[-1] != (phase, attempt)):
                        raise ValueError
                    phase_stack.pop()
                elif event in {"iteration", "repair_round"}:
                    if (not isinstance(phase, str) or not phase or isinstance(attempt, bool) or
                            not isinstance(attempt, int) or attempt < 0 or not phase_stack or
                            phase_stack[-1] != (phase, attempt) or
                            (event == "repair_round" and phase != "timing_repair")):
                        raise ValueError
                elif event == "run_end":
                    if phase_stack or phase is not None or attempt is not None:
                        raise ValueError
                records.append(value)
                expected_sequence += 1
                prior_elapsed = float(elapsed)
                terminal_seen = value["event"] == "run_end"
            except (json.JSONDecodeError, ValueError):
                truncated = True
                break
    return records, truncated or not terminal_seen or bool(phase_stack)


def _finite_number(value: Any) -> bool:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return False
    try:
        return math.isfinite(float(value))
    except (OverflowError, ValueError):
        return False


def _exact_execution_backend(value: Any) -> bool:
    if value == "cpu-reference":
        return True
    if not isinstance(value, str):
        return False
    fields = value.split(":", 3)
    if len(fields) != 4 or fields[0] not in {"cuda", "hip"}:
        return False
    device_uuid = fields[1]
    return (len(device_uuid) == 32 and
            all(character in "0123456789abcdefABCDEF" for character in device_uuid) and
            any(character != "0" for character in device_uuid) and
            bool(fields[2]) and bool(fields[3]))


def _final_timing_evidence(path: Optional[Path], required_clocks: Sequence[str]) -> Dict[str, Any]:
    evidence: Dict[str, Any] = {
        "required_clocks": list(required_clocks),
        "analogue_clocks": [],
        "analogue_timing_pass": None,
        "reason": None,
    }
    if not required_clocks:
        evidence["reason"] = "required_clocks_not_declared"
        return evidence
    if path is None or not path.is_file():
        evidence["reason"] = "final_report_missing"
        return evidence
    try:
        with path.open(encoding="utf-8") as stream:
            report = json.load(stream)
    except (OSError, UnicodeError, json.JSONDecodeError):
        evidence["reason"] = "final_report_invalid"
        return evidence
    normalized = report.get("outcome") if isinstance(report, dict) else None
    if isinstance(normalized, dict) and isinstance(normalized.get("analogue_clocks"), list):
        by_name: Dict[str, Dict[str, Any]] = {}
        duplicate_names = set()
        for clock in normalized["analogue_clocks"]:
            if not isinstance(clock, dict) or not isinstance(clock.get("name"), str):
                continue
            if clock["name"] in by_name:
                duplicate_names.add(clock["name"])
            else:
                by_name[clock["name"]] = clock
        clocks = []
        for name in required_clocks:
            source = by_name.get(name)
            available = (name not in duplicate_names and source is not None and
                         source.get("available") is True and
                         _finite_number(source.get("setup_wns_ns")) and
                         _finite_number(source.get("hold_wns_ns")))
            clock = {"name": name, "available": available}
            if available:
                clock.update({"setup_wns_ns": float(source["setup_wns_ns"]),
                              "hold_wns_ns": float(source["hold_wns_ns"])})
            clocks.append(clock)
        evidence["analogue_clocks"] = clocks
        if all(clock["available"] for clock in clocks):
            evidence["analogue_timing_pass"] = all(
                min(clock["setup_wns_ns"], clock["hold_wns_ns"]) > 0 for clock in clocks)
        else:
            evidence["reason"] = "required_clock_timing_missing"
        return evidence
    summary = report.get("timing_summary") if isinstance(report, dict) else None
    if isinstance(summary, dict):
        if summary.get("final_analogue_model") is not True:
            evidence["reason"] = "report_is_not_final_analogue_model"
            return evidence
        sources = summary.get("clocks")
        if not isinstance(sources, dict):
            evidence["reason"] = "final_report_timing_missing"
            return evidence
        clocks = []
        for name in required_clocks:
            source = sources.get(name)
            available = (isinstance(source, dict) and
                         _finite_number(source.get("setup_wns_ns")) and
                         _finite_number(source.get("hold_wns_ns")))
            clock = {"name": name, "available": available}
            if available:
                clock.update({"setup_wns_ns": float(source["setup_wns_ns"]),
                              "hold_wns_ns": float(source["hold_wns_ns"])})
            clocks.append(clock)
        evidence["analogue_clocks"] = clocks
        if all(clock["available"] for clock in clocks):
            evidence["analogue_timing_pass"] = all(
                min(clock["setup_wns_ns"], clock["hold_wns_ns"]) > 0 for clock in clocks)
        else:
            evidence["reason"] = "required_clock_timing_missing"
        return evidence
    fmax = report.get("fmax") if isinstance(report, dict) else None
    if not isinstance(fmax, dict):
        evidence["reason"] = "final_report_timing_missing"
        return evidence
    clocks = []
    for name in required_clocks:
        source = fmax.get(name)
        available = (isinstance(source, dict) and _finite_number(source.get("achieved")) and
                     _finite_number(source.get("constraint")) and
                     float(source["achieved"]) > 0 and float(source["constraint"]) > 0)
        clock = {"name": name, "available": available, "hold_available": False}
        if available:
            achieved, constraint = float(source["achieved"]), float(source["constraint"])
            clock.update({"achieved_mhz": achieved, "constraint_mhz": constraint,
                          "setup_wns_ns": 1000.0 / constraint - 1000.0 / achieved})
        clocks.append(clock)
    evidence["analogue_clocks"] = clocks
    if all(clock["available"] for clock in clocks):
        if not all(clock["setup_wns_ns"] > 0 for clock in clocks):
            evidence["analogue_timing_pass"] = False
        evidence["reason"] = "hold_not_available_in_standard_report"
    else:
        evidence["reason"] = "required_clock_timing_missing"
    return evidence


def classify_collected_result(result: Mapping[str, Any], artifacts: Mapping[str, Any],
                              required_clocks: Sequence[str],
                              require_execution_backend: bool = False) -> Dict[str, Any]:
    classified = dict(result)
    process_status = classified["status"]
    classified["process_status"] = process_status
    telemetry_info = artifacts.get("telemetry")
    telemetry_path = (Path(telemetry_info["path"]) if isinstance(telemetry_info, dict) and
                      telemetry_info.get("available") is True else None)
    records: List[Dict[str, Any]] = []
    telemetry_incomplete = True
    if telemetry_path is not None:
        try:
            records, telemetry_incomplete = load_jsonl(telemetry_path)
        except (OSError, UnicodeError):
            pass
    terminal = records[-1] if records and records[-1].get("event") == "run_end" else None
    setup_records = [record for record in records
                     if record.get("event") == "phase_end" and record.get("phase") == "setup"]
    setup_backends = [record.get("backend") for record in setup_records]
    unique_backends = {backend for backend in setup_backends
                       if isinstance(backend, str) and backend}
    execution_backend = None
    if (setup_records and len(unique_backends) == 1 and
            len(setup_backends) == len([backend for backend in setup_backends
                                        if isinstance(backend, str) and backend])):
        execution_backend = next(iter(unique_backends))
    elif require_execution_backend or setup_records:
        telemetry_incomplete = True
    routing_legal = terminal.get("routing_legal") if isinstance(terminal, dict) else None
    if not isinstance(routing_legal, bool):
        routing_legal = None
        telemetry_incomplete = True
    timing_gate_present = isinstance(terminal, dict) and "timing_gate_pass" in terminal
    timing_gate_pass = terminal.get("timing_gate_pass") if timing_gate_present else None
    if not isinstance(timing_gate_pass, bool):
        timing_gate_pass = None
        if routing_legal is True:
            telemetry_incomplete = True
    report_info = artifacts.get("final_report")
    report_path = (Path(report_info["path"]) if isinstance(report_info, dict) and
                   report_info.get("available") is True else None)
    timing = _final_timing_evidence(report_path, required_clocks)
    outcome = {
        "telemetry_complete": not telemetry_incomplete,
        "legal_route": routing_legal,
        "timing_gate_pass": timing_gate_pass,
        "execution_backend": execution_backend,
        "required_clocks": timing["required_clocks"],
        "analogue_clocks": timing["analogue_clocks"],
        "analogue_timing_pass": timing["analogue_timing_pass"],
        "timing_evidence_reason": timing["reason"],
    }
    outcome["evidence_complete"] = (
        outcome["telemetry_complete"] and routing_legal is not None and
        timing["analogue_timing_pass"] is not None)
    classified["outcome"] = outcome
    if process_status in {"timeout", "cancelled", "launch_error", "not_started_total_budget"}:
        classification = process_status
    elif routing_legal is False:
        classification = "routing_failure"
    elif routing_legal is True and timing_gate_pass is False:
        classification = "timing_constraint_failure"
    elif routing_legal is True and timing["analogue_timing_pass"] is False:
        classification = "analogue_timing_failure"
    elif process_status == "process_failure":
        classification = "process_failure"
    elif not outcome["evidence_complete"]:
        classification = "incomplete_evidence"
    else:
        classification = "completed"
    classified["status"] = classification
    classified["outcome_classification"] = classification
    return classified


def validate_dataset(document: Mapping[str, Any]) -> List[Dict[str, Any]]:
    if document.get("schema_version", SCHEMA_VERSION) != SCHEMA_VERSION or not isinstance(document.get("runs"), list):
        raise ValueError("unsupported evaluator dataset")
    required_declarations = document.get("required_clocks")
    if not isinstance(required_declarations, list):
        raise ValueError("dataset.required_clocks must be an array of design/constraint declarations")
    required_by_design = {}
    for declaration in required_declarations:
        if not isinstance(declaration, dict):
            raise ValueError("each required-clock declaration must be an object")
        _require_keys(declaration, ("mapped_design_id", "constraint_family", "clocks"),
                      "required-clock declaration")
        key = (declaration["mapped_design_id"], declaration["constraint_family"])
        names = declaration["clocks"]
        if (not all(isinstance(item, str) and item for item in key) or key in required_by_design or
                not isinstance(names, list) or not names or
                not all(isinstance(name, str) and name for name in names) or
                len(set(names)) != len(names)):
            raise ValueError("required-clock declarations must be unique with non-empty clock arrays")
        required_by_design[key] = list(names)
    cohort_identities = document.get("cohort_identities")
    if not isinstance(cohort_identities, dict):
        raise ValueError("dataset.cohort_identities must bind every cohort fingerprint")
    normalized = []
    run_ids = set()
    for run in document["runs"]:
        _require_keys(run, ("run_id", "cohort_id", "cohort_fingerprint_sha256",
                            "mapped_design_id", "constraint_family", "seed", "status",
                            "duration_seconds", "outcome_observed_seconds", "observations",
                            "outcome"), "run")
        if not isinstance(run["run_id"], str) or not run["run_id"] or run["run_id"] in run_ids:
            raise ValueError("run_id values must be non-empty and unique")
        run_ids.add(run["run_id"])
        if not all(isinstance(run[name], str) and run[name] for name in
                   ("cohort_id", "mapped_design_id", "constraint_family")):
            raise ValueError(
                "run cohort_id, mapped_design_id, and constraint_family must be non-empty strings")
        if not isinstance(run["status"], str) or run["status"] not in TERMINAL_STATUSES:
            raise ValueError("run status must be a supported terminal status")
        duration = _positive_number(run["duration_seconds"], "duration_seconds")
        observations = run["observations"]
        if not isinstance(observations, list):
            raise ValueError("observations must be an array")
        prior = -math.inf
        clean = []
        for observation in observations:
            if not isinstance(observation, dict) or "elapsed_seconds" not in observation:
                raise ValueError("each observation needs elapsed_seconds")
            if not _finite_number(observation["elapsed_seconds"]):
                raise ValueError("observation times must be finite numeric values")
            elapsed = float(observation["elapsed_seconds"])
            if elapsed < prior or elapsed < 0 or elapsed > duration:
                raise ValueError("observation times must be finite, monotonic, and within duration")
            prior = elapsed
            normalized_observation = dict(observation)
            normalized_observation["elapsed_seconds"] = elapsed
            clean.append(normalized_observation)
        outcome_observed = _positive_number(run["outcome_observed_seconds"],
                                            "outcome_observed_seconds")
        if outcome_observed > duration or (clean and outcome_observed < clean[-1]["elapsed_seconds"]):
            raise ValueError("outcome availability must follow observations and fit within duration")
        outcome = run["outcome"]
        if not isinstance(outcome, dict):
            raise ValueError("outcome must be an object")
        legal = outcome.get("legal_route") is True
        timing_gate_pass = outcome.get("timing_gate_pass")
        if timing_gate_pass is not None and not isinstance(timing_gate_pass, bool):
            raise ValueError("outcome.timing_gate_pass must be boolean or null")
        if run["status"] == "completed" and timing_gate_pass is False:
            raise ValueError("completed run contradicts failed outcome timing gate")
        required_clocks = outcome.get("required_clocks")
        if (not isinstance(required_clocks, list) or not required_clocks or
                not all(isinstance(name, str) and name for name in required_clocks) or
                len(set(required_clocks)) != len(required_clocks)):
            raise ValueError("outcome.required_clocks must be a non-empty unique string array")
        design_key = (run["mapped_design_id"], run["constraint_family"])
        canonical_clocks = required_by_design.get(design_key)
        if canonical_clocks is None:
            raise ValueError(f"required clocks are not declared for {design_key!r}")
        if required_clocks != canonical_clocks:
            raise ValueError(
                f"outcome.required_clocks does not match dataset declaration for {design_key!r}")
        identity = cohort_identities.get(run["cohort_id"])
        fingerprint = run["cohort_fingerprint_sha256"]
        if (not isinstance(identity, dict) or not isinstance(fingerprint, str) or
                identity.get("fingerprint_sha256") != fingerprint or
                not isinstance(identity.get("manifest"), dict)):
            raise ValueError("run cohort fingerprint is not bound by cohort_identities")
        encoded = json.dumps(identity["manifest"], sort_keys=True, separators=(",", ":"),
                             allow_nan=False).encode("utf-8")
        if hashlib.sha256(encoded).hexdigest() != fingerprint:
            raise ValueError("cohort identity fingerprint does not match its canonical manifest")
        identity_manifest = identity["manifest"]
        identity_command = identity_manifest.get("command")
        nextpnr_binary = (Path(identity_command[0]).name
                          if isinstance(identity_command, list) and identity_command and
                          isinstance(identity_command[0], str) else "")
        router_values = (_option_values(identity_command, "--router")
                         if isinstance(identity_command, list) else [])
        gpu_capable = (nextpnr_binary.startswith(("nextpnr-mistral", "nextpnr-generic")) or
                       identity_manifest.get("architecture") in {"mistral", "generic"})
        if gpu_capable and len(router_values) != 1:
            raise ValueError("GPU-capable nextpnr cohort identity lacks an explicit router")
        declared_gpu = router_values == ["gpu"]
        requires_execution_identity = declared_gpu
        observed_backend = outcome.get("execution_backend")
        never_launched = run.get("process_started") is False
        if requires_execution_identity or "execution_identity" in identity_manifest or \
                isinstance(observed_backend, str):
            execution_identity = identity_manifest.get("execution_identity")
            if (not isinstance(execution_identity, dict) or
                    not isinstance(execution_identity.get("backend"), str) or
                    not execution_identity["backend"]):
                raise ValueError("GPU run execution backend is not bound by its cohort identity")
            if never_launched:
                if observed_backend is not None:
                    raise ValueError("never-launched GPU run must not claim an execution backend")
            elif observed_backend != execution_identity["backend"]:
                raise ValueError("GPU run execution backend is not bound by its cohort identity")
            elif not _exact_execution_backend(observed_backend):
                raise ValueError("GPU run execution backend lacks exact device attestation")
            runtime_id = execution_identity.get("runtime_environment_id")
            runtime_binary = identity_manifest.get("binary")
            runtime_evidence = (runtime_binary.get("runtime_environment")
                                if isinstance(runtime_binary, dict) else None)
            runtime_manifest = (runtime_evidence.get("manifest")
                                if isinstance(runtime_evidence, dict) else None)
            runtime_manifest_id = None
            if isinstance(runtime_manifest, dict):
                runtime_encoded = json.dumps(
                    runtime_manifest, sort_keys=True, separators=(",", ":"),
                    allow_nan=False).encode("utf-8")
                runtime_manifest_id = "sha256:" + hashlib.sha256(runtime_encoded).hexdigest()
            provenance = identity_manifest.get("provenance")
            if (not isinstance(runtime_id, str) or not runtime_id.startswith("sha256:") or
                    len(runtime_id) != 71 or
                    any(character not in "0123456789abcdef" for character in runtime_id[7:]) or
                    not isinstance(runtime_evidence, dict) or
                    runtime_evidence.get("runtime_environment_id") != runtime_id or
                    runtime_manifest_id != runtime_id or
                    not isinstance(provenance, dict) or
                    provenance.get("runtime_environment_id") != runtime_id):
                raise ValueError(
                    "GPU run runtime environment is not cross-bound by its cohort identity")
        identity_cohort = identity_manifest.get("cohort")
        if (not isinstance(identity_cohort, dict) or
                identity_cohort.get("id") != run["cohort_id"] or
                identity_cohort.get("mapped_design_id") != run["mapped_design_id"] or
                identity_cohort.get("constraint_family") != run["constraint_family"] or
                identity["manifest"].get("required_clocks") != canonical_clocks):
            raise ValueError("run design and required clocks do not match its cohort identity")
        clocks = outcome.get("analogue_clocks")
        clock_by_name = {}
        if isinstance(clocks, list):
            for clock in clocks:
                if not isinstance(clock, dict) or not isinstance(clock.get("name"), str):
                    raise ValueError("each analogue clock needs a string name")
                if clock["name"] in clock_by_name:
                    raise ValueError("analogue clock names must be unique")
                clock_by_name[clock["name"]] = clock
        missing_required = [name for name in required_clocks if name not in clock_by_name]
        required_records = [clock_by_name[name] for name in required_clocks if name in clock_by_name]
        def finite_metric(value: Any) -> bool:
            return _finite_number(value)
        timing_available = (not missing_required and len(required_records) == len(required_clocks) and
                            all(clock.get("available") is True and finite_metric(clock.get("setup_wns_ns")) and
                                finite_metric(clock.get("hold_wns_ns")) for clock in required_records))
        if not timing_available:
            analogue_pass = False
            final_margin = None
        else:
            margins = [min(float(clock["setup_wns_ns"]), float(clock["hold_wns_ns"]))
                       for clock in required_records]
            analogue_pass = all(margin > 0 for margin in margins)
            final_margin = min(margins)
        copy = dict(run)
        copy.update({"duration_seconds": duration, "outcome_observed_seconds": outcome_observed,
                     "observations": clean,
                     "success": (legal and analogue_pass and timing_gate_pass is not False and
                                 run["status"] == "completed"),
                     "legal_route": legal, "analogue_timing_pass": analogue_pass,
                     "timing_available": timing_available, "missing_required_clocks": missing_required,
                     "final_multi_clock_margin_ns": final_margin})
        normalized.append(copy)
    return normalized


def observation_at(run: Mapping[str, Any], checkpoint: float) -> Optional[Dict[str, Any]]:
    """Return a defensive copy of the latest prefix observation, never final data."""
    visible = [obs for obs in run["observations"] if obs["elapsed_seconds"] <= checkpoint]
    if not visible:
        return None
    return {key: value for key, value in visible[-1].items() if key in PREFIX_FEATURES}


def heuristic_score(observation: Optional[Mapping[str, Any]]) -> Tuple[float, ...]:
    """A transparent prefix-only score; larger tuples rank better."""
    if observation is None:
        return (-math.inf,)
    overuse = observation.get("total_excess_occupancy")
    if overuse is None:
        overuse = observation.get("overused_wires")
    unrouted = observation.get("unrouted_connections")
    progress = observation.get("recent_progress")
    table_wns = observation.get("table_wns_ns")
    return (
        -float(overuse) if _finite_number(overuse) else -math.inf,
        -float(unrouted) if _finite_number(unrouted) else -math.inf,
        float(progress) if _finite_number(progress) else -math.inf,
        float(table_wns) if _finite_number(table_wns) else -math.inf,
    )


def _metrics(retained: Sequence[Mapping[str, Any]], population: Sequence[Mapping[str, Any]], aggregate: float, makespan: float, label: str) -> Dict[str, Any]:
    successes = [run for run in population if run["success"]]
    retained_successes = [run for run in retained if run["success"]]
    recall = len(retained_successes) / len(successes) if successes else None
    margins = [run["final_multi_clock_margin_ns"] for run in retained_successes if run["final_multi_clock_margin_ns"] is not None]
    return {
        "cost_model": label,
        "population_runs": len(population),
        "population_eventual_successes": len(successes),
        "retained_runs": len(retained),
        "retained_eventual_successes": len(retained_successes),
        "eventual_success_recall": recall,
        "false_rejections": len(successes) - len(retained_successes),
        "at_least_one_success": bool(retained_successes),
        "aggregate_compute_seconds": aggregate,
        "serial_wall_clock_seconds": makespan,
        "final_multi_clock_margin_ns": max(margins) if margins else None,
    }


def _scheduler_key(scheduler_seed: int, namespace: str, position: int) -> str:
    """Portable pseudo-random ordering independent of Python's random module."""
    value = f"seed-racing-scheduler-v1\0{scheduler_seed}\0{namespace}\0{position}"
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def successive_halving(
    runs: Sequence[Mapping[str, Any]], checkpoints: Sequence[float], quotas: Sequence[int],
    exploratory_survivors: int, scheduler_seed: int, restart: bool,
    score_overhead_seconds: float = 0.0,
    budget_seconds: Optional[float] = None,
) -> Dict[str, Any]:
    if not checkpoints or len(checkpoints) != len(quotas):
        raise ValueError("checkpoints and quotas must be non-empty and equal length")
    if any(b <= a for a, b in zip(checkpoints, checkpoints[1:])) or any(value <= 0 for value in checkpoints):
        raise ValueError("checkpoints must be fixed, positive, increasing decision points")
    if any(isinstance(q, bool) or not isinstance(q, int) or q <= 0 for q in quotas):
        raise ValueError("quotas must be positive integers")
    if exploratory_survivors < 0:
        raise ValueError("exploratory_survivors cannot be negative")
    budget_limit = (math.inf if budget_seconds is None else
                    _positive_number(budget_seconds, "budget_seconds"))
    survivors = list(runs)
    terminal_successes = []
    stage_records, aggregate = [], 0.0
    prior_checkpoint = 0.0
    budget_exhausted = False
    for stage_index, (checkpoint, quota) in enumerate(zip(checkpoints, quotas)):
        evaluated = list(survivors)
        if restart:
            stage_cost = sum(min(run["duration_seconds"], checkpoint) for run in evaluated)
        else:
            stage_cost = sum(max(0.0, min(run["duration_seconds"], checkpoint) -
                                 min(run["duration_seconds"], prior_checkpoint))
                             for run in evaluated)
        terminal = [run for run in evaluated
                    if run.get("outcome_observed_seconds", run["duration_seconds"]) <= checkpoint]
        active = [run for run in evaluated if run not in terminal]
        stage_cost += score_overhead_seconds * len(active)
        if aggregate + stage_cost > budget_limit:
            censored = budget_limit - aggregate
            aggregate = budget_limit
            budget_exhausted = True
            stage_records.append({"checkpoint": checkpoint,
                                  "evaluated": [run["run_id"] for run in evaluated],
                                  "budget_exhausted_before_stage": True,
                                  "censored_compute_seconds": censored,
                                  "terminal_successes": [], "terminal_failures": [],
                                  "promoted": [], "exploratory": []})
            survivors = []
            break
        aggregate += stage_cost
        terminal_successes.extend(run for run in terminal if run["success"])
        # An independent scheduler RNG breaks score ties. Neither numeric seed nor
        # seed-bearing run IDs are visible to ranking.
        scored = [(heuristic_score(observation_at(run, checkpoint)),
                   _scheduler_key(scheduler_seed, f"tie:{stage_index}", position), run)
                  for position, run in enumerate(active)]
        scored.sort(key=lambda item: (item[0], item[1]), reverse=True)
        keep = min(quota, len(scored))
        explore = min(exploratory_survivors, keep)
        ranked_count = keep - explore
        ranked = [run for _, _, run in scored[:ranked_count]]
        remaining = [run for _, _, run in scored[ranked_count:] if run not in ranked]
        remaining = sorted(
            enumerate(remaining),
            key=lambda item: _scheduler_key(
                scheduler_seed, f"explore:{stage_index}", item[0]))
        explored = [run for _, run in remaining[:min(explore, len(remaining))]]
        survivors = ranked + explored
        stage_records.append({
            "checkpoint": checkpoint,
            "evaluated": [run["run_id"] for run in evaluated],
            "terminal_successes": [run["run_id"] for run in terminal if run["success"]],
            "terminal_failures": [run["run_id"] for run in terminal if not run["success"]],
            "promoted": [run["run_id"] for run in survivors],
            "exploratory": [run["run_id"] for run in explored],
        })
        prior_checkpoint = checkpoint
    completed_survivors = []
    for run in survivors:
        final_cost = (run["duration_seconds"] if restart else
                      max(0.0, run["duration_seconds"] -
                          min(run["duration_seconds"], prior_checkpoint)))
        if aggregate + final_cost > budget_limit:
            aggregate = budget_limit
            budget_exhausted = True
            break
        aggregate += final_cost
        completed_survivors.append(run)
    retained = terminal_successes + completed_survivors
    metrics = _metrics(retained, runs, aggregate, aggregate,
                       "restart_execution" if restart else "ideal_resumable_simulation")
    metrics.update({"policy": "successive_halving", "scheduler_seed": scheduler_seed,
                    "scheduler_algorithm": "sha256-order-v1", "stages": stage_records,
                    "retained": [run["run_id"] for run in retained],
                    "budget_seconds": budget_seconds,
                    "budget_remaining_seconds": (None if budget_seconds is None else
                                                 max(0.0, budget_limit - aggregate)),
                    "budget_exhausted": budget_exhausted})
    return metrics


def random_full_run_baseline(runs: Sequence[Mapping[str, Any]], budget_seconds: float, scheduler_seed: int) -> Dict[str, Any]:
    _positive_number(budget_seconds, "budget_seconds")
    order = [run for _, run in sorted(
        enumerate(runs),
        key=lambda item: _scheduler_key(scheduler_seed, "full-run", item[0]))]
    completed, consumed, first_success = [], 0.0, None
    for run in order:
        if consumed + run["duration_seconds"] > budget_seconds:
            consumed = budget_seconds  # final observation is censored and cannot produce a success label
            break
        consumed += run["duration_seconds"]
        completed.append(run)
        if first_success is None and run["success"]:
            first_success = consumed
    metrics = _metrics(completed, runs, consumed, consumed, "observed_full_run_serial")
    metrics.update({"policy": "random_full_run", "scheduler_seed": scheduler_seed,
                    "scheduler_algorithm": "sha256-order-v1",
                    "order": [run["run_id"] for run in order],
                    "completed": [run["run_id"] for run in completed],
                    "time_to_first_success_seconds": first_success,
                    "budget_seconds": budget_seconds,
                    "budget_remaining_seconds": max(0.0, budget_seconds - consumed),
                    "budget_exhausted": len(completed) != len(order)})
    return metrics


def evaluate(document: Mapping[str, Any], checkpoints: Sequence[float], quotas: Sequence[int], exploratory: int, scheduler_seeds: Sequence[int], budget: float) -> Dict[str, Any]:
    runs = validate_dataset(document)
    # Replicates/prefixes stay together because the unit of scheduling is the complete run.
    return {
        "schema_version": SCHEMA_VERSION,
        "evaluation_population": {"runs": len(runs), "cohorts": sorted({run["cohort_id"] for run in runs}), "mapped_design_constraint_families": sorted({f'{run["mapped_design_id"]}:{run["constraint_family"]}' for run in runs}), "scope": "single-design" if len({(run["mapped_design_id"], run["constraint_family"]) for run in runs}) == 1 else "cross-design-descriptive"},
        "random_full_run": [random_full_run_baseline(runs, budget, seed) for seed in scheduler_seeds],
        "successive_halving_ideal": [successive_halving(runs, checkpoints, quotas, exploratory, seed, False, budget_seconds=budget) for seed in scheduler_seeds],
        "successive_halving_restart": [successive_halving(runs, checkpoints, quotas, exploratory, seed, True, budget_seconds=budget) for seed in scheduler_seeds],
    }


def _comma_numbers(value: str, integer: bool = False) -> List[Any]:
    converter = int if integer else float
    return [converter(item) for item in value.split(",") if item]


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="operation", required=True)
    collect = subparsers.add_parser("collect", help="execute a bounded collection manifest")
    collect.add_argument("manifest", type=Path)
    collect.add_argument("--output", required=True, type=Path)
    collect.add_argument("--dry-run", action="store_true")
    replay = subparsers.add_parser("evaluate", help="replay prefix-only policies offline")
    replay.add_argument("dataset", type=Path)
    replay.add_argument("--checkpoints", required=True)
    replay.add_argument("--quotas", required=True)
    replay.add_argument("--exploratory-survivors", type=int, default=1)
    replay.add_argument("--scheduler-seeds", default="0,1,2,3,4")
    replay.add_argument("--budget-seconds", required=True, type=float)
    replay.add_argument("--output", type=Path)
    arguments = parser.parse_args(argv)
    try:
        with arguments.manifest.open(encoding="utf-8") if arguments.operation == "collect" else arguments.dataset.open(encoding="utf-8") as stream:
            document = json.load(stream)
        if arguments.operation == "collect":
            result = Collector(document, arguments.output).run(arguments.dry_run)
        else:
            result = evaluate(document, _comma_numbers(arguments.checkpoints), _comma_numbers(arguments.quotas, True), arguments.exploratory_survivors, _comma_numbers(arguments.scheduler_seeds, True), arguments.budget_seconds)
        if getattr(arguments, "output", None) and arguments.operation == "evaluate":
            _json_dump(arguments.output, result)
        else:
            json.dump(result, sys.stdout, indent=2, sort_keys=True, allow_nan=False)
            sys.stdout.write("\n")
        return 0
    except (OSError, ValueError, json.JSONDecodeError) as error:
        parser.error(str(error))
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
