#!/usr/bin/env node
'use strict';

/* mtg CLI wrapper
 * 把参数、标准输入输出与退出码原样透传给原生编译器。
 * 退出码必须透传：`?` 错误传播链依赖 Main::main().Result 的非零退出码。 */

const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname, '..');

function findCompiler() {
	const win = process.platform === 'win32';
	const candidates = win
		? [
			path.join(root, 'vendor', 'mtg.exe'), // 随包发布的 Windows x64 二进制
			path.join(root, 'mtg.exe'),          // 在源码仓库内直接开发
			path.join(root, 'vendor', 'mtg'),
		]
		: [
			path.join(root, 'vendor', 'mtg'),    // 本地从源码构建
			path.join(root, 'mtg'),
			path.join(root, 'vendor', 'mtg.exe'),
		];
	return candidates.find((p) => fs.existsSync(p)) || null;
}

const exe = findCompiler();
if (!exe) {
	console.error('[mtg] 未找到编译器二进制。');
	console.error('  Windows x64：包内应自带 vendor/mtg.exe');
	console.error('  其他平台或使用最新源码：运行 npm run build:compiler');
	console.error('  预编译二进制：https://github.com/Wei-Foundation/mtg/releases');
	process.exit(2);
}

const r = spawnSync(exe, process.argv.slice(2), { stdio: 'inherit' });
if (r.error) {
	console.error('[mtg] 无法启动编译器：' + r.error.message);
	process.exit(2);
}
process.exit(r.status === null ? 1 : r.status);
