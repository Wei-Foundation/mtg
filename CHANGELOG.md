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
- 内置错误码手册：14 条错误码的中英双语条目，含成因、最小复现与可直接编译的
  修复片段；新增 `mtg explain <码>` / `mtg --explain <码>` / `mtg --list-codes`
  子命令，完全离线，不依赖网络或外部文档
- `?` 自动错误传播链：编译期把每个 `?` 的源文件、行列、所在函数与被调函数
  注入生成物，Err 时由内向外逐层打印完整链路，无需 anyhow / backtrace 之类
  的第三方依赖；`-no-error-trace` 可关闭，关闭后不注入任何额外指令
- `Main::main().Result` 中 `?` 触发时以错误码绝对值调用 `ExitProcess`，
  Rust 风格的错误返回不再静默失败
- `tests/rustplus/` 验收套件：`ok.wei`、`trace.wei`、`e0277.wei` 与 `run.ps1`，
  覆盖手册、Ok/Err 路径、两层传播链、关闭开关与精确诊断共 19 项断言
- npm 分发包 `@wei-foundation/mtg`（0.9.4）：提供 `mtg` CLI，原样透传参数、
  标准输入输出与退出码；自带 Windows x64 预编译二进制 `vendor/mtg.exe`
  （与 Mtg Stable Channel Release 附件字节一致），其他平台可用随包附带的
  `src/main.cpp` 执行 `npm run build:compiler` 构建；另含 `npm test` 冒烟测试
  与 `npm pack` 打包白名单
- `.github/workflows/publish-ghpkg.yml`：用 GitHub Actions 的 `GITHUB_TOKEN`
  （`packages: write`）把 `@wei-foundation/mtg` 发布到 GitHub Packages，
  绕开 npmjs.com 的两步验证发布限制；Mtg Stable Channel Release 同时附带
  `wei-foundation-mtg-0.9.4.tgz`，供无需令牌的安装方式使用

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
- `Main::main().Result` 的错误码原先被入口桩丢弃，进程退出码恒为 0
  （`return -7` 与 `?` 传播均静默失败）；现由 `?` 的 Err 分支直接以错误码
  绝对值退出，退出码可观测
- `E0277` 诊断原先标在整个调用表达式上，现精确标在 `?` 上（`Call` 新增
  `try_line` / `try_col`），并补上指向 `mtg explain E0277` 的 help 行
- `--list-codes` 的中文列改为按显示宽度补齐，修正 CJK 占 2 列导致的错位
- `?` 传播链中的源路径统一为 `/`，消除 `H:/dir\file.wei` 混用分隔符
- 修复模块成员名与宽度关键字的冲突：`u8` / `u16` / `u32` / `u64` 登记为
  关键字后，`src/bootstrap.wei` 的 `Code::u8` 等定义与调用被拒，stage0 重建
  stage1 时断在 `bootstrap.wei:1117`。现仅在 `::` 之后把宽度关键字按普通
  标识符处理，类型位置与其它语法不受影响

### Changed

- README 补充 Make 构建方式、社区文档链接与子集自举章节
- README 完善文言文扫雷示例的玩法、规则、棋盘符号与示例局面
- README 新增「错误处理与诊断」章节（Result / `?`、传播链、错误码手册与验收），
  并在选项表登记 `-no-error-trace`
- 清理已不存在的扫雷示例引用

### Known Issues

- `make bootstrap-fixpoint` 目前仍失败：自举源码 `src/bootstrap.wei` 中还有
  其它标识符与关键字表冲突（如参数名 `target`，见 `bootstrap.wei:1175`），
  stage0 无法重建 stage1。关键字表扩展后与 4004 行自举源码共有 57 个重合词，
  需要把保留字改为上下文敏感关键字，或系统性重命名自举源码里的标识符，
  属于独立的一期工作。`tests/bootstrap/run.ps1`（10/10）与
  `tests/rustplus/run.ps1`（19/19）不受影响。
- `tests/bootstrap/cli.ps1`（不带 `-RequireBuildSuccess`）目前 14/20：
  `build-done-text` 期望 `[done]` 而 CLI 输出 `[完成]`；`std-zh` /
  `std-wenyan` / `-target linux` / `-wide` 四条期望"被拒绝"，但三种方言、
  linux64 目标与 128 位宽算术现已是正式支持的选项；`o-missing-value`
  反映 `-o` 缺参数时缺少校验。均为子集冻结时期留下的过时期望，
  与本批次改动无关。

## [0.02.74282] - 2026-10-26

### Added

- 维语言原生编译器，支持 `wei27`、中文、文言三种源码方言
- Windows x64 与 Linux x64 目标后端
- 模块与部件、数组、元组、模式匹配、编译期求值等语言特性
- 编译期循环优化，以及实验性 IR 后端

### Fixed

- 修复 Windows 控制台输入例程的栈参数顺序问题，避免读取输入时崩溃
