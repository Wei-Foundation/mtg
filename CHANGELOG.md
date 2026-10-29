# 更新日志

本项目遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 的格式，版本号沿用编译器 `--version` 输出的版本号。

## [Unreleased]

### Added

- 子集自举冻结规格 `src/BOOTSTRAP.md`：语法、类型、运行时 ABI、PE/栈/堆布局、
  CLI 契约、固定上限与 10 天验收标准
- `src/bootstrap.wei` stage1 自举编译器：词法器、单遍解析/代码生成、x64 指令
  发射、PE32+ 写出器、运行时桩（panic/alloc/free/read/write）与完整 CLI
- `tests/bootstrap/`：`hello`、`arith`、`control`、`func`、`array`、`string`、
  `bytes`、`file`、`cmdline`、`exit` 共 10 个验收用例及 `run.ps1`
- `tests/bootstrap/cli.ps1`：CLI 退出码、参数解析、默认输出名与
  `-RequireBuildSuccess` 完整构建检查
- `tests/bootstrap/fixpoint.ps1` 与 `make bootstrap-fixpoint`：自动完成
  stage0 → stage1 → stage2 → stage3 并比较 stage2/stage3 的 SHA256
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

- stage1 的 `/`、`%` 除零检查原先测试被除数（RCX）而不是除数（RAX），
  导致 `0 / 256`、`0 % 256` 被误判为除零；修正为 `test rax` 后再交换操作数，
  `bootstrap2.exe` 不再在 `Mem::store64` 等路径 panic
- stage0 `IfStmt` 在分支合并点未清除 `last_store_len_/last_store_slot_`，
  分支未命中时会错误复用 RAX 中的旧值（表现为 `if (r != 0) { v = ... }`
  在 r == 0 时返回 0）；两个退出点与合并点均清除缓存
- stage1 解析器：局部名在 `Lex::next` 前保存、函数参数名正确消费、嵌套调用
  前保存被调名、`if/else` 的 `j2` 先发射再回填条件跳转、数组初始化器按
  “元素 0 在最低地址”布局写入
- `__command_line()` 原先误接到 `VirtualAlloc`，恒返回 0；现返回真实 ANSI
  命令行
- 入口桩在泄漏检查前用 ECX 保存 main 返回值，ECX 被格式串指针覆盖；现先写
  `[rsp+0x20]`，泄漏检查后再装入 RCX
- 新增第 13 个 IAT 槽后同步修正 `importSize`、格式串偏移与 ILT 回填

### Changed

- README 补充 Make 构建方式、社区文档链接与子集自举章节
- README 完善文言文扫雷示例的玩法、规则、棋盘符号与示例局面
- 清理已不存在的扫雷示例引用

## [0.02.74282] - 2026-10-26

### Added

- 维语言原生编译器，支持 `wei27`、中文、文言三种源码方言
- Windows x64 与 Linux x64 目标后端
- 模块与部件、数组、元组、模式匹配、编译期求值等语言特性
- 编译期循环优化，以及实验性 IR 后端

### Fixed

- 修复 Windows 控制台输入例程的栈参数顺序问题，避免读取输入时崩溃
