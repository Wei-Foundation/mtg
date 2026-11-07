#!/usr/bin/env node
'use strict';

/* 冒烟测试：确认 npm 包里的 CLI wrapper 能真正调用到原生编译器。 */

const { spawnSync } = require('node:child_process');
const path = require('node:path');

const cli = path.resolve(__dirname, '..', 'bin', 'mtg.js');
const r = spawnSync(process.execPath, [cli, '--version'], { encoding: 'utf8' });
const out = (r.stdout || '') + (r.stderr || '');

if (r.status !== 0 || !out.includes('Module Target Generator')) {
	console.error('[mtg] 冒烟测试失败：' + (out.trim() || '（无输出）'));
	process.exit(1);
}
console.log('[mtg] 冒烟测试通过：' + out.trim().split('\n')[0]);
