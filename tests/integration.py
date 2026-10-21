"""Black-box CLI tests, including malformed ELF fixtures generated in a temp directory."""
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

TOOL = str(Path(sys.argv[1]).resolve())
FIX = Path(sys.argv[2]).resolve()
sys.argv = [sys.argv[0]]


def program_headers(data):
    offset = struct.unpack_from("<Q", data, 32)[0]
    count = struct.unpack_from("<H", data, 56)[0]
    return [(offset + i * 56, struct.unpack_from("<IIQQQQQQ", data, offset + i * 56)) for i in range(count)]


def virtual_offset(data, address):
    for _, p in program_headers(data):
        if p[0] == 1 and p[3] <= address < p[3] + p[5]:
            return p[2] + address - p[3]
    raise AssertionError("Fixture virtual address has no file offset")


def dynamic_entries(data):
    _, segment = next((o, p) for o, p in program_headers(data) if p[0] == 2)
    entries = []
    for offset in range(segment[2], segment[2] + segment[5], 16):
        tag, value = struct.unpack_from("<qQ", data, offset)
        entries.append((offset, tag, value))
        if tag == 0:
            break
    return entries


def dynamic(data, tag):
    return next((offset, value) for offset, item, value in dynamic_entries(data) if item == tag)


class LoaderTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def run_tool(self, command, file="app.so", *options, good=True):
        path = Path(file)
        if not path.is_absolute():
            path = FIX / path
        result = subprocess.run([TOOL, command, str(path), "-L", str(FIX), *options],
                                capture_output=True, text=True, timeout=10)
        self.assertGreaterEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("AddressSanitizer", result.stderr)
        self.assertNotIn("runtime error:", result.stderr)
        if good:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        return result

    def mutated(self, name, change, source="app.so"):
        data = bytearray((FIX / source).read_bytes())
        change(data)
        path = self.root / name
        path.write_bytes(data)
        return path

    def test_01_calls_and_arguments(self):
        self.assertIn("= 49", self.run_tool("run").stdout)
        self.assertIn("= 59", self.run_tool("run", "app.so", "--arg1", "10", "--arg2", "2").stdout)
        self.assertIn("= 20", self.run_tool("run", "app.so", "--arg1", "-3", "--arg2", "2").stdout)

    def test_02_symbol_interposition_and_weak_override(self):
        self.assertIn("= 161", self.run_tool("run", "override.so").stdout)

    def test_03_protected_symbol_is_not_preempted_inside_owner(self):
        self.assertIn("= 21", self.run_tool("run", "protected-app.so").stdout)

    def test_04_hash_formats(self):
        for name in ["app-gnu.so", "app-sysv.so"]:
            with self.subTest(name=name):
                self.assertIn("= 49", self.run_tool("run", name).stdout)

    def test_05_no_section_headers_needed(self):
        def strip_headers(data):
            struct.pack_into("<Q", data, 40, 0)
            struct.pack_into("<HHH", data, 58, 0, 0, 0)
        path = self.mutated("sectionless.so", strip_headers)
        self.assertIn("= 49", self.run_tool("run", path).stdout)

    def test_06_graph_and_dot(self):
        output = self.run_tool("deps").stdout
        self.assertIn('[1] "libmath.so"', output)
        self.assertIn('[2] "libbase.so"', output)
        dot = self.run_tool("deps", "app.so", "--dot").stdout
        self.assertIn("n0 -> n1;", dot)
        self.assertIn("n1 -> n2;", dot)

    def test_07_inspection_matches_readelf(self):
        output = self.run_tool("relocs", "libmath.so").stdout
        reference = subprocess.run(["readelf", "-Wr", str(FIX / "libmath.so")],
                                   capture_output=True, text=True, check=True).stdout
        for name in ["R_X86_64_RELATIVE", "R_X86_64_64", "R_X86_64_GLOB_DAT", "R_X86_64_JUMP_SLOT"]:
            self.assertIn(name, output)
            self.assertIn(name, reference)
        self.assertIn("demo_entry", self.run_tool("symbols").stdout)
        self.assertIn("ELF64", self.run_tool("inspect").stdout)

    def test_08_maps_does_not_run_constructors(self):
        result = self.run_tool("maps", "app.so", "--trace")
        self.assertIn("r-x", result.stdout)
        self.assertIn("r--", result.stdout)
        self.assertNotIn("rwx", result.stdout)
        self.assertNotIn("[elflab] init", result.stderr)
        self.assertNotIn("[elflab] fini", result.stderr)

    def test_09_init_and_fini_order(self):
        trace = self.run_tool("run", "app.so", "--trace").stderr
        self.assertLess(trace.index("init libbase.so"), trace.index("init libmath.so"))
        self.assertLess(trace.index("init libmath.so"), trace.index("init app.so"))
        self.assertLess(trace.index("fini app.so"), trace.index("fini libmath.so"))
        self.assertLess(trace.index("fini libmath.so"), trace.index("fini libbase.so"))

    def test_10_missing_dependency(self):
        path = self.root / "app.so"
        shutil.copyfile(FIX / "app.so", path)
        result = subprocess.run([TOOL, "deps", str(path)], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 1)
        self.assertIn("Missing dependency", result.stderr)

    def test_11_undefined_strong_symbol(self):
        self.assertIn("undefined strong symbol", self.run_tool("run", "unresolved.so", good=False).stderr)

    def test_12_tls_is_explicitly_rejected(self):
        self.assertIn("Thread-local storage", self.run_tool("run", "tls.so", good=False).stderr)

    def test_13_non_function_entry_rejected(self):
        self.assertIn("STT_FUNC", self.run_tool("run", "app.so", "--entry", "base_bias", good=False).stderr)

    def test_14_unknown_entry(self):
        self.assertIn("Undefined symbol", self.run_tool("run", "app.so", "--entry", "nonexistent", good=False).stderr)

    def test_15_bad_headers(self):
        changes = {
            "magic": lambda d: d.__setitem__(0, 0),
            "class": lambda d: d.__setitem__(4, 1),
            "endian": lambda d: d.__setitem__(5, 2),
            "arch": lambda d: struct.pack_into("<H", d, 18, 183),
            "phcount": lambda d: struct.pack_into("<H", d, 56, 65535),
            "phoffset": lambda d: struct.pack_into("<Q", d, 32, (1 << 64) - 1),
            "phsize": lambda d: struct.pack_into("<H", d, 54, 1),
        }
        for name, change in changes.items():
            with self.subTest(name=name):
                self.run_tool("inspect", self.mutated(name, change), good=False)

    def test_16_truncated_files(self):
        data = (FIX / "app.so").read_bytes()
        for length in [0, 3, 63, 100, 600]:
            with self.subTest(length=length):
                path = self.root / str(length)
                path.write_bytes(data[:length])
                self.run_tool("inspect", path, good=False)

    def test_17_invalid_segment_sizes_and_alignment(self):
        for field, value in [(32, 4096), (48, 3), (16, (1 << 64) - 32)]:
            with self.subTest(field=field):
                def change(data):
                    offset, _ = next((o, p) for o, p in program_headers(data) if p[0] == 1)
                    struct.pack_into("<Q", data, offset + field, value)
                self.run_tool("inspect", self.mutated("segment", change), good=False)

    def test_18_overlapping_segments(self):
        def change(data):
            loads = [(o, p) for o, p in program_headers(data) if p[0] == 1 and p[6]]
            struct.pack_into("<Q", data, loads[1][0] + 16, 0)
        self.assertIn("Overlapping", self.run_tool("inspect", self.mutated("overlap", change), good=False).stderr)

    def test_19_writable_executable_segment(self):
        def change(data):
            offset, p = next((o, p) for o, p in program_headers(data) if p[0] == 1 and p[1] & 1)
            struct.pack_into("<I", data, offset + 4, p[1] | 2)
        self.assertIn("W^X", self.run_tool("maps", self.mutated("wx", change), good=False).stderr)

    def test_20_bad_symbol_name(self):
        def change(data):
            _, sym = dynamic(data, 6)
            _, size = dynamic(data, 10)
            struct.pack_into("<I", data, virtual_offset(data, sym), size)
        self.assertIn("symbol name", self.run_tool("inspect", self.mutated("name", change), good=False).stderr)

    def test_21_bad_relocation_symbol(self):
        def change(data):
            _, table = dynamic(data, 7)
            struct.pack_into("<Q", data, virtual_offset(data, table) + 8, (999999 << 32) | 8)
        self.assertIn("symbol index", self.run_tool("inspect", self.mutated("bad-index", change, "libmath.so"), good=False).stderr)

    def test_22_relocation_write_to_readonly_segment(self):
        def change(data):
            _, table = dynamic(data, 7)
            struct.pack_into("<Q", data, virtual_offset(data, table), 0)
        self.assertIn("outside writable", self.run_tool("maps", self.mutated("readonly", change, "libmath.so"), good=False).stderr)

    def test_23_unknown_relocation(self):
        def change(data):
            _, table = dynamic(data, 7)
            struct.pack_into("<Q", data, virtual_offset(data, table) + 8, 999)
        self.assertIn("Unsupported relocation", self.run_tool("maps", self.mutated("reloc", change, "libmath.so"), good=False).stderr)

    def test_24_corrupt_initializer_is_rejected_before_execution(self):
        def change(data):
            _, table = dynamic(data, 7)
            struct.pack_into("<q", data, virtual_offset(data, table) + 16, 0)
        result = self.run_tool("run", self.mutated("initializer", change, "libmath.so"), good=False)
        self.assertIn("non-executable", result.stderr)

    def test_25_bad_dynamic_table_entry_size(self):
        def change(data):
            offset, _ = dynamic(data, 9)
            struct.pack_into("<Q", data, offset + 8, 1)
        self.assertIn("DT_RELAENT", self.run_tool("inspect", self.mutated("entry-size", change, "libmath.so"), good=False).stderr)

    def test_26_non_pie_inspects_but_does_not_load(self):
        path = self.mutated("exec", lambda d: struct.pack_into("<H", d, 16, 2))
        self.assertIn("ET_EXEC", self.run_tool("inspect", path).stdout)
        self.assertIn("ET_DYN", self.run_tool("maps", path, good=False).stderr)

    def test_27_text_relocation_marker_rejected(self):
        def change(data):
            offset, _ = dynamic(data, 0x6FFFFFFB)
            struct.pack_into("<qQ", data, offset, 22, 0)
        self.assertIn("text relocations", self.run_tool("maps", self.mutated("textrel", change), good=False).stderr)

    def test_28_bad_gnu_hash_dimensions(self):
        def change(data):
            _, address = dynamic(data, 0x6FFFFEF5)
            struct.pack_into("<I", data, virtual_offset(data, address) + 8, 3)
        self.assertIn("GNU hash", self.run_tool("inspect", self.mutated("gnu", change, "app-gnu.so"), good=False).stderr)

    def test_29_missing_dynamic_terminator(self):
        def change(data):
            _, segment = next((o, p) for o, p in program_headers(data) if p[0] == 2)
            null_offset = dynamic_entries(data)[-1][0]
            for i, offset in enumerate(range(null_offset, segment[2] + segment[5], 16)):
                struct.pack_into("<qQ", data, offset, 0x60000000 + i, 0)
        self.assertIn("DT_NULL", self.run_tool("inspect", self.mutated("null", change), good=False).stderr)

    def test_30_dependency_cycle_is_deduplicated(self):
        # Redirect libmath's dependency to itself without changing the string length.
        data = bytearray((FIX / "libmath.so").read_bytes())
        _, strings = dynamic(data, 5)
        _, name = dynamic(data, 1)
        offset = virtual_offset(data, strings + name)
        data[offset:offset + len(b"libmath.so")] = b"libmath.so"
        path = self.root / "libmath.so"
        path.write_bytes(data)
        result = subprocess.run([TOOL, "deps", str(path)], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('-> [0] "libmath.so"', result.stdout)

    def test_31_deterministic_header_mutation_smoke(self):
        rng = random.Random(7123)
        original = (FIX / "app.so").read_bytes()
        for index in range(80):
            data = bytearray(original)
            for _ in range(1 + index % 4):
                location = rng.randrange(64)
                data[location] ^= rng.randrange(1, 256)
            path = self.root / "mutation"
            path.write_bytes(data)
            result = subprocess.run([TOOL, "inspect", str(path)], capture_output=True, text=True, timeout=5)
            self.assertIn(result.returncode, [0, 1], (index, result.stderr))
            self.assertNotIn("AddressSanitizer", result.stderr)
            self.assertNotIn("runtime error:", result.stderr)

    def test_32_help_and_cli_errors(self):
        self.assertEqual(subprocess.run([TOOL, "--help"], capture_output=True).returncode, 0)
        result = subprocess.run([TOOL, "run", str(FIX / "app.so"), "--arg1", "abc"], capture_output=True)
        self.assertEqual(result.returncode, 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
