// Run with: node tools/test_home_hub_core.mjs
import {existsSync, mkdirSync} from 'node:fs';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {resolve} from 'node:path';

const root = fileURLToPath(new URL('../', import.meta.url));
const outputDirectory = resolve(root, '.pio/home-hub-tests');
mkdirSync(outputDirectory, {recursive: true});
const objectDirectory = resolve(outputDirectory, 'obj');
mkdirSync(objectDirectory, {recursive: true});
const executable = resolve(outputDirectory,
    process.platform === 'win32' ? 'home_hub_core_test.exe' : 'home_hub_core_test');
const configuredCompiler = process.env.CXX;
const mingwCompiler = 'C:/ProgramData/mingw64/mingw64/bin/g++.exe';
const component = resolve(root, 'hub/esp-idf-p4/components/bluepaws_core');
const sources = [
    resolve(component, 'src/map_engine.cpp'),
    resolve(component, 'src/cat_store.cpp'),
    resolve(component, 'src/cat_simulator.cpp'),
    resolve(component, 'src/hub_settings.cpp'),
    resolve(component, 'src/qr_payload.cpp'),
    resolve(root, 'hub/tests/esp-idf-p4/home_hub_core_test.cpp'),
];
let compiled;
if (process.platform === 'win32' && !configuredCompiler && !existsSync(mingwCompiler)) {
    const vswhere = 'C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe';
    const discovered = existsSync(vswhere) ? spawnSync(vswhere, [
        '-latest', '-products', '*',
        '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
        '-property', 'installationPath',
    ], {encoding: 'utf8'}) : null;
    const visualStudio = discovered?.status === 0 ? discovered.stdout.trim() : '';
    const developerShell = visualStudio
        ? resolve(visualStudio, 'Common7/Tools/VsDevCmd.bat') : '';
    if (!developerShell || !existsSync(developerShell)) {
        process.stderr.write('No supported C++ compiler found. Install Visual Studio Build Tools with the Desktop development with C++ workload, install MinGW at C:/ProgramData/mingw64/mingw64, or set CXX.\n');
        process.exit(1);
    }
    const quote = value => `"${value.replaceAll('"', '""')}"`;
    const compilerArguments = [
        '/nologo', '/std:c++17', '/W4', '/WX', '/EHsc',
        '/D_CRT_SECURE_NO_WARNINGS',
        `/I${quote(resolve(component, 'include'))}`,
        ...sources.map(quote),
        `/Fo${quote(`${objectDirectory}/`)}`,
        `/Fe:${quote(executable)}`,
    ].join(' ');
    const command = `call ${quote(developerShell)} -arch=x64 -host_arch=x64 >nul && cl ${compilerArguments}`;
    compiled = spawnSync('cmd.exe', ['/d', '/s', '/c', `"${command}"`], {
        encoding: 'utf8',
        windowsVerbatimArguments: true,
    });
} else {
    const compiler = configuredCompiler ||
        (process.platform === 'win32' ? mingwCompiler : 'g++');
    compiled = spawnSync(compiler, [
        '-std=c++17', '-Wall', '-Wextra', '-Werror',
        `-I${resolve(component, 'include')}`,
        ...sources, '-o', executable,
    ], {encoding: 'utf8'});
}
if (compiled.status !== 0) {
    process.stderr.write(compiled.stderr || compiled.stdout || 'C++ compiler failed to start.\n');
    process.exit(compiled.status ?? 1);
}
const result = spawnSync(executable, [], {encoding: 'utf8'});
if (result.stdout) process.stdout.write(result.stdout);
if (result.stderr) process.stderr.write(result.stderr);
process.exit(result.status ?? 1);
