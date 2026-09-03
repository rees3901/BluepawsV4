// Run with: node tools/test_home_hub_core.mjs
import {mkdirSync} from 'node:fs';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {resolve} from 'node:path';

const root = fileURLToPath(new URL('../', import.meta.url));
const outputDirectory = resolve(root, '.pio/home-hub-tests');
mkdirSync(outputDirectory, {recursive: true});
const executable = resolve(outputDirectory,
    process.platform === 'win32' ? 'home_hub_core_test.exe' : 'home_hub_core_test');
const compiler = process.env.CXX ||
    (process.platform === 'win32' ? 'C:/ProgramData/mingw64/mingw64/bin/g++.exe' : 'g++');
const component = resolve(root, 'home-hub/firmware/components/bluepaws_core');
const sources = [
    resolve(component, 'src/map_engine.cpp'),
    resolve(component, 'src/cat_store.cpp'),
    resolve(component, 'src/cat_simulator.cpp'),
    resolve(component, 'src/hub_settings.cpp'),
    resolve(component, 'src/qr_payload.cpp'),
    resolve(root, 'home-hub/tests/home_hub_core_test.cpp'),
];
const compiled = spawnSync(compiler, [
    '-std=c++17', '-Wall', '-Wextra', '-Werror',
    `-I${resolve(component, 'include')}`,
    ...sources, '-o', executable,
], {encoding: 'utf8'});
if (compiled.status !== 0) {
    process.stderr.write(compiled.stderr || compiled.stdout);
    process.exit(compiled.status ?? 1);
}
const result = spawnSync(executable, [], {encoding: 'utf8'});
process.stdout.write(result.stdout);
process.stderr.write(result.stderr);
process.exit(result.status ?? 1);
