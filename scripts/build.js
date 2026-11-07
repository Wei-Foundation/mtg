#!/usr/bin/env node
'use strict';

/* 从随包附带的 src/main.cpp 构建原生编译器。
 * 用于 Linux x64，或 Windows 上想用最新源码替换 vendor/mtg.exe 的场景。
 * 需要支持 C++26 的 GCC / Clang（已在 MinGW-w64 GCC 15.2.0 验证）。 */

const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname, '..');
const src = path.join(root, 'src', 'main.cpp');

if (!fs.existsSync(src)) {
	console.error('[mtg] 缺少 src/main.cpp，无法从源码构建。');
	console.error('  请从 Release 下载预编译二进制并放入 vendor/ 目录。');
	process.exit(1);
}

const vendor = path.join(root, 'vendor');
fs.mkdirSync(vendor, { recursive: true });
const out = path.join(vendor, process.platform === 'win32' ? 'mtg.exe' : 'mtg');

const cxx = process.env.CXX || 'g++';
const args = ['-std=c++26', '-O3', '-o', out, src];
console.log('[mtg] ' + cxx + ' ' + args.join(' '));

const r = spawnSync(cxx, args, { stdio: 'inherit' });
if (r.error || r.status !== 0) {
	console.error('[mtg] 构建失败：需要支持 C++26 的 GCC 或 Clang。');
	process.exit(1);
}
console.log('[mtg] 已生成 ' + out);
