# 更新日志

本项目遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 的格式，版本号沿用编译器 `--version` 输出的版本号。

## [Unreleased]

### Added

- 子集自举冻结规格 `src/BOOTSTRAP.md`：语法、类型、运行时 ABI、PE/栈/堆布局、
  CLI 契约、固定上限与 10 天验收标准
- `src/bootstrap.wei` stage1 CLI 骨架：`--version`、`build` 参数解析、源文件
  读取、默认输出名、`-o`、`-std`、`-target` 与退出码
- `tests/bootstrap/`：`hello`、`arith`、`control`、`func`、`array`、`string`、
  `bytes`、`file`、`cmdline`、`exit` 共 10 个验收用例及 `run.ps1`
- `tests/bootstrap/cli.ps1`：CLI 退出码、参数解析、默认输出名与
  `-RequireBuildSuccess` 完整构建检查
- stage0 新增 `__exit(code)` 内置函数（Windows x64，直接调用 `ExitProcess`）
- stage0 新增 `GetCommandLineA` 导入（第 13 个 IAT 槽），供 `__command_line()`
  使用
- `Makefile`，支持 `make` / `mingw32-make` 构建、`make check` 与 `make clean`
- `.gitignore`、`.gitattributes`、`.editorconfig`
- 贡献指南、行为准则、安全策略与更新日志
- 完整的 GPLv3 许可证文本
- 文言文扫雷示例 `saolei.wenyan.wei`：九道棋盘、十雷、首击安全，支持树旗、
  漫延、胜负判定与再局

### Fixed

- `__command_line()` 原先误接到 `VirtualAlloc`，恒返回 0；现返回真实 ANSI
  命令行
- 入口桩在泄漏检查前用 ECX 保存 main 返回值，ECX 被格式串指针覆盖；现先写
  `[rsp+0x20]`，泄漏检查后再装入 RCX
- 新增第 13 个 IAT 槽后同步修正 `importSize`、格式串偏移与 ILT 回填

### Changed

- README 补充 Make 构建方式、社区文档链接与子集自举章节
- 清理已不存在的扫雷示例引用

## [0.02.74282] - 2026-10-26

### Added

- 维语言原生编译器，支持 `wei27`、中文、文言三种源码方言
- Windows x64 与 Linux x64 目标后端
- 模块与部件、数组、元组、模式匹配、编译期求值等语言特性
- 编译期循环优化，以及实验性 IR 后端

### Fixed

- 修复 Windows 控制台输入例程的栈参数顺序问题，避免读取输入时崩溃
