import json
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class BackendTests(unittest.TestCase):
    def select(self, backend, platform, available):
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            script = root / 'select.cmake'
            output = root / 'selected.txt'
            module = ROOT / 'cmake' / 'SelectGpuBackend.cmake'
            script.write_text('cmake_minimum_required(VERSION 3.22)\n'
                + f'include("{module.as_posix()}")\n'
                + 'assistant_select_gpu_backend("${REQUESTED}" "${PLATFORM}" "${AVAILABLE}" selected)\n'
                + 'file(WRITE "${OUTPUT}" "${selected}")\n', encoding='utf-8')
            result = subprocess.run(['cmake', f'-DREQUESTED={backend}', f'-DPLATFORM={platform}',
                f'-DAVAILABLE={available}', f'-DOUTPUT={output.as_posix()}', '-P', str(script)],
                text=True, capture_output=True)
            return result, output.read_text() if output.exists() else ''

    def test_auto_platform_backends(self):
        for platform, available, expected in (
            ('Darwin', 'OFF', 'METAL'), ('Darwin', 'ON', 'METAL'),
            ('Windows', 'ON', 'VULKAN'), ('Linux', 'ON', 'VULKAN'),
            ('Windows', 'OFF', 'CPU'), ('Linux', 'OFF', 'CPU')):
            with self.subTest(platform=platform, available=available):
                result, selected = self.select('AUTO', platform, available)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(selected, expected)

    def test_explicit_cpu_fallback_on_every_platform(self):
        for platform in ('Darwin', 'Windows', 'Linux'):
            result, selected = self.select('CPU', platform, 'ON')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(selected, 'CPU')

    def test_metal_rejected_on_other_platforms(self):
        for platform in ('Windows', 'Linux'):
            result, _ = self.select('METAL', platform, 'ON')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('requires macOS', result.stderr)

    def test_unknown_backend_rejected(self):
        result, _ = self.select('invalid', 'Windows', 'ON')
        self.assertNotEqual(result.returncode, 0)

    def test_gpu_presets_cover_native_platforms(self):
        presets = json.loads((ROOT / 'CMakePresets.json').read_text(encoding='utf-8-sig'))
        configs = {p['name']: p for p in presets['configurePresets']}
        for name, backend in (('macos-arm64', 'METAL'), ('macos-intel', 'METAL'),
                              ('windows-gpu', 'VULKAN'), ('linux-gpu', 'VULKAN')):
            self.assertEqual(configs[name]['cacheVariables']['ASSISTANT_GPU_BACKEND'], backend)
        self.assertEqual(configs['macos-arm64']['cacheVariables']['CMAKE_OSX_ARCHITECTURES'], 'arm64')
        self.assertEqual(configs['macos-intel']['cacheVariables']['CMAKE_OSX_ARCHITECTURES'], 'x86_64')


if __name__ == '__main__':
    unittest.main()
