# 更新日志

本项目遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 的格式，版本号沿用编译器 `--version` 输出的版本号。

## [Unreleased]

### Added

- [`BEYOND-RUST.md`](BEYOND-RUST.md)：超越 Rust 的长期技术计划。明确「不比生态、
  只比六个可度量维度」的边界，给出错误流图（EFG）、区域 + 线性能力令牌、
  零工具链自举、诊断即修复四条不抄 Rust 的技术主线，以及 P0–P5 每期的
  交付物、验收命令、退出条件与「明确不做」清单；所有性能数字一律标 `待测`，
  未达标项必须公开留档，禁止下调指标
- README 目录、简介与项目结构挂上该计划的入口

---

## [0.9.5] - 2026-11-08

### Added

- 诊断信息增强：标题与错误码**粗体**、`-->` 文件名与「来源」绝对路径
  加**下划线**、源码行里真正出错的片段也加下划线（终端 VT 模式；
  管道 / CI / `NO_COLOR` 下自动降级为纯文本）
- 每条诊断新增四行：`= 来源:`（绝对路径、总行数、主源文件 / 导入模块）、
  `= 预判:`（最可能的真实意图）、`= 阶段:` 与 `= 流程:`（8 步编译流程里
  停在哪一步）、`= 继续:`（修复后重跑的命令）
- 预判会扫描源码里已声明的变量、关键字表与内置函数，按编辑距离给出
  最接近的候选（例如 `coumt` → `count`）
- `mtg --pipeline`：完整编译流程、每步职责与相关选项；
  `mtg build ... --verbose`：实时打印 8 个阶段进度；
  `-no-color` / `-color` 显式控制配色
- `tests/rustplus/run.ps1` 扩到 25 项断言，覆盖 `--pipeline` 与新增的
  来源 / 预判 / 阶段 / 流程 / 继续 五行；脚本显式把输出编码设为 UTF-8

### Fixed

- `do.emit::` 的 `outlb` / `newline` / `换行` 一次换两行（见 0.9.4 Known Issues
  中记录的成因），改为空串输出值后一次标记恰好一次换行；
  新增 `tests/emit/` 字节级回归与 `make emit-check`
- Windows 下 ANSI 序列在非 VT 控制台会变成乱码：现在自动检测 TTY 并开启
  `ENABLE_VIRTUAL_TERMINAL_PROCESSING`，非终端时完全不输出控制序列
- 恢复被误删的 `saolei.wenyan.wei`（311 行，已用当前编译器验证可构建），
  README 里的扫雷章节不再是死链

### Changed

- README 全面校正：补全目录、修正 macOS 相关错误描述、补齐 `build` 全选项与
  子命令表、新增「诊断信息结构」章节、把自举状态与 Known Issues 对齐、
  项目结构补上 npm / 测试 / workflow 目录
- 清理仓库根目录的 `hello.*`、`src/main.exe`、旧 tarball、
  `package-lock.json` 与 `node_modules/`，并在 `.gitignore` 加上对应规则
- npm 包版本 0.9.4 → 0.9.5；`vendor/mtg.exe` 与 `mtg.exe` 重新构建，
  两处字节一致。2026-11-08 晚又按用户要求从同一份源码整体重编译了一次
  （源码未改，PE COFF 时间戳导致字节不同），当前 SHA256
  `0361e539e923da9e868deb42bf27dca9c5645b9b1a87a60ee2b03a055471687e`；
  tarball 与 Release 附件同步重建
- `.github/workflows/publish-ghpkg.yml` 的发布步骤改为幂等：版本号已存在于
  GitHub Packages 时跳过而不是让 CI 变红（重建二进制不改版本号的推送场景）

---

## [0.9.4] - 2026-11-07

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
- `tests/emit/` 与 `make emit-check`：`do.emit::` 换行标记的字节级回归用例
- `@wei-foundation/mtg@0.9.4` 已发布到 npmjs.com 公共仓库（`latest`），
  `npm install -g @wei-foundation/mtg` 可直接安装，无需账号或令牌；
  首次发布由具有「绕过两步验证」权限的 granular access token 完成
- `.github/workflows/publish-ghpkg.yml`：用 GitHub Actions 的 `GITHUB_TOKEN`
  （`packages: write`）把同一版本发布到 GitHub Packages，作为备用源
  （该源需要 `read:packages` 令牌）；Mtg Stable Channel Release 同时附带
  `wei-foundation-mtg-0.9.4.tgz`，供无需令牌的安装方式使用

### Fixed

- `do.emit::` 的换行标记（`outlb` / `newline` / 中文 `换行`）原先被解析成
  字符串常量 `"\n"`，而每个输出值后端都用 `"%s\n"` 打印，于是
  `printf("%s\n", "\n")` 一次打印两个换行：写一次 `outlb` 却空两行。
  现改为空串常量，一次标记恰好产生一次换行，win64 / linux64 一致。
  新增 `tests/emit/` 字节级回归用例与 `make emit-check`
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
  `tests/rustplus/run.ps1`（25/25）不受影响。
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
