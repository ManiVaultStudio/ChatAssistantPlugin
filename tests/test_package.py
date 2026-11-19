import hashlib
import json
import pathlib
import subprocess
import tempfile
import unittest
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'cmake' / 'AcquirePackage.cmake'

class PackageTests(unittest.TestCase):
    def test_checksummed_release_preserves_reports_and_license(self):
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            release = root / 'release'
            release.mkdir()
            contents = {'assistant.gguf': b'GGUFsample', 'knowledge.db': b'index',
                        'answer_review.json': b'{"questions":34}', 'BASE_MODEL_LICENSE.txt': b'license'}
            for name, data in contents.items():
                (release / name).write_bytes(data)
            (release / 'SHA256SUMS.txt').write_text(''.join(
                f'{hashlib.sha256(data).hexdigest()}  {name}\n' for name, data in contents.items()))
            output = root / 'stage'
            result = self.run_cmake(output, ASSISTANT_PACKAGE_DIR=release)
            self.assertEqual(result.returncode, 0, result.stderr)
            for name, data in contents.items():
                self.assertEqual((output / name).read_bytes(), data)
            stamp = (output / 'assistant.gguf').stat().st_mtime_ns
            result = self.run_cmake(output, ASSISTANT_PACKAGE_DIR=release)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((output / 'assistant.gguf').stat().st_mtime_ns, stamp)

    def test_corrupt_release_fails_before_replacing_staged_files(self):
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            release = root / 'release'
            release.mkdir()
            (release / 'assistant.gguf').write_bytes(b'GGUFsample')
            (release / 'knowledge.db').write_bytes(b'index')
            (release / 'SHA256SUMS.txt').write_text('0' * 64 + '  assistant.gguf\n')
            output = root / 'stage'
            output.mkdir()
            (output / 'knowledge.db').write_bytes(b'previous index')
            result = self.run_cmake(output, ASSISTANT_PACKAGE_DIR=release)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('checksum mismatch', result.stderr)
            self.assertEqual((output / 'knowledge.db').read_bytes(), b'previous index')

    def run_cmake(self, output, **variables):
        args = ['cmake', f'-DASSISTANT_PACKAGE_OUTPUT={output}']
        for key, value in variables.items():
            args.append(f'-D{key}={value}')
        args.extend(['-P', str(SCRIPT)])
        return subprocess.run(args, text=True, capture_output=True)

    def test_local_package_stages_files(self):
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            release = root / 'release'
            release.mkdir()
            (release / 'assistant.gguf').write_bytes(b'GGUFsample')
            (release / 'knowledge.db').write_bytes(b'SQLite format 3\\0')
            (release / 'manifest.json').write_text('{}')
            output = root / 'stage'
            result = self.run_cmake(output, ASSISTANT_PACKAGE_DIR=release)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((output / 'assistant.gguf').read_bytes(), b'GGUFsample')
            self.assertTrue((output / 'knowledge.db').is_file())

    def test_local_shards(self):
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            release = root / 'release'
            (release / 'model-shards').mkdir(parents=True)
            (release / 'model-shards' / 'assistant-00001-of-00002.gguf').write_bytes(b'first')
            (release / 'model-shards' / 'assistant-00002-of-00002.gguf').write_bytes(b'second')
            (release / 'knowledge.db').write_bytes(b'index')
            output = root / 'stage'
            result = self.run_cmake(output, ASSISTANT_PACKAGE_DIR=release)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(len(list((output / 'model-shards').glob('*.gguf'))), 2)

    def test_switching_package_removes_stale_model(self):
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            first = root / 'first'
            second = root / 'second'
            first.mkdir()
            (second / 'model-shards').mkdir(parents=True)
            (first / 'assistant.gguf').write_bytes(b'old model')
            (first / 'knowledge.db').write_bytes(b'old index')
            (second / 'model-shards' / 'assistant-00001-of-00001.gguf').write_bytes(b'new model')
            (second / 'knowledge.db').write_bytes(b'new index')
            output = root / 'stage'
            self.assertEqual(self.run_cmake(output, ASSISTANT_PACKAGE_DIR=first).returncode, 0)
            self.assertEqual(self.run_cmake(output, ASSISTANT_PACKAGE_DIR=second).returncode, 0)
            self.assertFalse((output / 'assistant.gguf').exists())
            self.assertEqual((output / 'knowledge.db').read_bytes(), b'new index')
            self.assertTrue((output / 'model-shards' / 'assistant-00001-of-00001.gguf').exists())

    def test_missing_database_fails(self):
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            release = root / 'release'
            release.mkdir()
            (release / 'assistant.gguf').write_bytes(b'GGUF')
            result = self.run_cmake(root / 'output', ASSISTANT_PACKAGE_DIR=release)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('knowledge.db', result.stderr)

    def test_no_package_allowed_if_not_required(self):
        with tempfile.TemporaryDirectory() as td:
            result = self.run_cmake(pathlib.Path(td) / 'output')
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_no_package_fails_if_required(self):
        with tempfile.TemporaryDirectory() as td:
            result = self.run_cmake(pathlib.Path(td) / 'output', ASSISTANT_PACKAGE_REQUIRED='ON')
            self.assertNotEqual(result.returncode, 0)

    def test_unverified_download_rejected(self):
        with tempfile.TemporaryDirectory() as td:
            result = self.run_cmake(pathlib.Path(td) / 'output',
                ASSISTANT_PACKAGE_URL='https://example.com/archive.zip',
                ASSISTANT_PACKAGE_SHA256='bad')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('ASSISTANT_PACKAGE_SHA256', result.stderr)

    def test_http_download_rejected(self):
        with tempfile.TemporaryDirectory() as td:
            result = self.run_cmake(pathlib.Path(td) / 'output',
                ASSISTANT_PACKAGE_URL='http://example.com/archive.zip',
                ASSISTANT_PACKAGE_SHA256='a'*64)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('HTTPS', result.stderr)

    def test_plugin_has_no_example_dependencies(self):
        cmake = (ROOT / 'CMakeLists.txt').read_text()
        self.assertIn('ManiVault::Core', cmake)
        self.assertIn('Qt6::Sql', cmake)
        self.assertNotIn('PointData', cmake)
        self.assertNotIn('WebEngine', cmake)
        metadata = json.loads((ROOT / 'PluginInfo.json').read_text())
        self.assertEqual(metadata['dependencies'], [])
        self.assertEqual(metadata['type'], 'View')

    def test_no_network_in_cpp_runtime(self):
        sources = '\n'.join(f.read_text(encoding='utf-8') for f in (ROOT / 'src').glob('*.cpp'))
        for forbidden in ('QNetworkAccessManager', 'QNetworkRequest', 'curl_easy_', 'QTcpSocket'):
            self.assertNotIn(forbidden, sources)
        self.assertIn('QProcess', sources)
        self.assertIn('bm25(chunks)', sources)

if __name__ == '__main__':
    unittest.main()
