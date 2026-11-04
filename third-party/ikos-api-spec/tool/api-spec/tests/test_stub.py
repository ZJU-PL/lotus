import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
INSTALLED_IKOS = REPO_ROOT / 'analyzer/python'
sys.path.insert(0, str(INSTALLED_IKOS))

from ikos import analyzer, api_census, settings  # noqa: E402


def load_source_stub():
    path = REPO_ROOT / 'analyzer/python/ikos/stub.py'
    spec = importlib.util.spec_from_file_location('ikos.stub_under_test',
                                                  path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


stub = load_source_stub()


class StubTypeParsingTests(unittest.TestCase):
    def test_quoted_cpp_type_and_return_attributes(self):
        line = ('declare dereferenceable(32) align 8 nonnull i8* @f('
                '%"class.std::__cxx11::basic_string"* noundef)')
        sig = stub.parse_signature(line)

        self.assertEqual(
            sig.params[0][0],
            ('ptr', ('named', 'class.std::__cxx11::basic_string')))
        self.assertEqual(sig.ret_attrs, {'nonnull'})

    def test_multilevel_function_pointer(self):
        node, end = stub.parse_type('i32 (...)**', 0)

        self.assertEqual(end, len('i32 (...)**'))
        self.assertEqual(
            node,
            ('ptr', ('ptr', ('func', ('int', 32), [], True))))

    def test_packed_struct(self):
        node, end = stub.parse_type('<{ i8, i32 }>', 0)

        self.assertEqual(end, len('<{ i8, i32 }>'))
        self.assertEqual(node, ('agg', True, [('int', 8), ('int', 32)]))

    def test_quoted_typedef_name(self):
        parsed = stub.parse_typedef_line(
            '%"class.std::pair" = type { i32, i32 }')

        self.assertEqual(parsed,
                         ('class.std::pair', '{ i32, i32 }'))


class StubRenderingTests(unittest.TestCase):
    def render(self, line, typedefs=None):
        info = stub.ExternInfo('f', 'input.bc', line,
                               stub.parse_signature(line))
        return stub.render_stub(info, 'f', '', typedefs or {})

    def test_void_callback_is_rendered_as_function_pointer(self):
        source = self.render('declare i8* @f(void (i8*)*)')

        self.assertIn('typedef void (*ikos_stub_fn_0)(void*);', source)
        self.assertIn('void* f(ikos_stub_fn_0 p0) {', source)

    def test_cpp_pointer_uses_forward_declaration_and_hashed_key(self):
        line = 'declare void @f(%"class.std::Foo"*)'
        original = stub.parse_signature(line)
        source = self.render(line)
        tag = stub._c_type_tag('class.std::Foo')

        self.assertIn('struct %s;' % tag, source)
        self.assertNotIn('struct %s {' % tag, source)

        rendered = stub.parse_signature(
            'declare void @f(%%struct.%s*)' % tag)
        self.assertEqual(stub.signature_key(original),
                         stub.signature_key(rendered))

    def test_complete_cpp_object_can_be_used_by_value(self):
        typedefs = {'class.std::Foo': '{ i32 }'}
        source = self.render('declare void @f(%"class.std::Foo")',
                             typedefs)

        self.assertIn('struct ikos_cxx_', source)
        self.assertIn('    int f0;', source)

    def test_small_nontrivial_cpp_sret_is_rejected(self):
        line = ('declare void @f(%"class.std::Foo"*'
                ' sret(%"class.std::Foo") align 1)')
        info = stub.ExternInfo('f', 'input.bc', line,
                               stub.parse_signature(line))

        with self.assertRaisesRegex(
                stub.ScaffoldError,
                r'small non-trivial C\+\+ sret return'):
            stub.render_stub(info, 'f', '', {'class.std::Foo': '{ i32 }'})


class StubCompileTests(unittest.TestCase):
    def test_cpp_and_callback_skeletons_compile_and_match(self):
        llvm_dis = str(Path(settings.LLVM_BIN_DIR) / 'llvm-dis')
        api_census._init_worker(llvm_dis)
        cases = (
            ('declare void @cpp(%"class.std::Foo"*)',
             {'class.std::Foo': '{ i32 }'}),
            ('declare i8* @cb(void (i8*)*)', {}),
        )

        with tempfile.TemporaryDirectory() as directory:
            for index, (line, typedefs) in enumerate(cases):
                info = stub.ExternInfo('case%d' % index, 'input.bc', line,
                                       stub.parse_signature(line))
                source = stub.render_stub(info, info.name, '', typedefs)
                source_path = Path(directory) / ('case%d.c' % index)
                bitcode_path = Path(directory) / ('case%d.bc' % index)
                source_path.write_text(source)
                analyzer.clang(str(bitcode_path), str(source_path),
                               disable_warnings=True, colors=False)

                compiled = stub.scan_bitcode(str(bitcode_path))[1][info.name]
                self.assertEqual(stub.signature_key(compiled.signature),
                                 stub.signature_key(info.signature))

    def test_libc_named_stubs_compile_with_intrinsics(self):
        llvm_dis = str(Path(settings.LLVM_BIN_DIR) / 'llvm-dis')
        api_census._init_worker(llvm_dis)
        declarations = (
            'declare i32 @atoi(i8* noundef)',
            'declare double @atof(i8* noundef)',
            'declare i64 @strtol(i8* noundef, i8** noundef, i32 noundef)',
            'declare i64 @strtoul(i8* noundef, i8** noundef, i32 noundef)',
        )

        with tempfile.TemporaryDirectory() as directory:
            for index, line in enumerate(declarations):
                info = stub.ExternInfo('case%d' % index, 'input.bc', line,
                                       stub.parse_signature(line))
                source = stub.render_stub(info, info.name, '', {})
                source_path = Path(directory) / ('case%d.c' % index)
                bitcode_path = Path(directory) / ('case%d.bc' % index)
                source_path.write_text(source)
                analyzer.clang(str(bitcode_path), str(source_path),
                               disable_warnings=True, colors=False)

                compiled = stub.scan_bitcode(str(bitcode_path))[1][info.name]
                self.assertEqual(stub.signature_key(compiled.signature),
                                 stub.signature_key(info.signature))


if __name__ == '__main__':
    unittest.main()
