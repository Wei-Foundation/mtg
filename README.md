<p align="center">
  <img src="Wei.png" width="80" alt="Wei Logo">
</p>

<h1 align="center">Module Target Generator</h1>


<p align="center">
  <a href="https://opensource.org/licenses/GPL-3.0">
    <img src="https://img.shields.io/badge/License-GPL_3.0-blue.svg" alt="License">
  </a>
</p>

---

## 目录

- [简介](#简介)
- [快速开始](#快速开始)
  - [命令格式](#命令格式)
- [语言示例](#语言示例)
  - [循环与数组](#循环与数组)
  - [模块与部件](#模块与部件)
  - [中文与文言文](#中文与文言文)
  - [文言文扫雷](#文言文扫雷)
- [错误处理与诊断](#错误处理与诊断)
  - [Result 与 `?`](#result-与-)
  - [自动错误传播链](#自动错误传播链)
  - [诊断信息结构](#诊断信息结构)
  - [错误码手册](#错误码手册)
  - [验收](#验收)
- [通过 npm 安装](#通过-npm-安装)
- [从源码构建](#从源码构建)
- [子集自举（进行中）](#子集自举进行中)
- [项目结构](#项目结构)
- [参与贡献](#参与贡献)
- [许可证](#许可证)

---

## 简介

**Module Target Generator** 是维语言（Wei）的原生编译器。它把 `.wei`
源文件直接编译成 x86-64 机器码，不依赖虚拟机或解释器，当前支持生成
**Windows x64** 与 **Linux x64** 程序。

主要特性：

- 单文件实现，内置词法分析、语法分析、类型检查与 x86-64 代码生成
- 三种源码风格：`wei27`（默认）、`中文`、`文言`
- 编译期优化：等差/平方和等循环闭式解替换、常量循环求值
- 模块与部件、数组、元组、模式匹配、编译期求值等语言特性
- 可选实验性 IR 路径（三地址码 + 线性扫描寄存器分配）

> `-ir` 与 `-wide` 目前属于实验特性，不建议在生产代码中依赖。

仓库还附有一个用文言方言编写的[扫雷示例](#文言文扫雷)，可直接编译游玩。

---

## 快速开始

### 1. 编写源文件

新建 `hello.wei`：

```wei
create.mod Main::main() {
    do.emit::"Hello, World!";
}
```

### 2. 编译

```bash
mtg build hello.wei -o hello.exe   # Windows
mtg build hello.wei -o hello       # Linux
```

省略 `-o` 时，会在当前目录生成与源文件同名的可执行文件；Windows 下自动补
`.exe` 后缀。

### 3. 运行

Windows：

```bat
.\hello.exe
```

Linux：

```bash
./hello
```

### 命令格式

> 下文以 `mtg` 表示编译器可执行文件；Windows 下若未将其加入 `PATH`，请写作
> `.\mtg.exe`。

```text
mtg build <文件.wei> [选项]
```

| `build` 选项 | 说明 |
| --- | --- |
| `-o <文件>` | 指定输出文件路径；省略时与源文件同名，Windows 下补 `.exe` |
| `-std:<风格>` | 方言：`wei27` / `wei` / `default`（默认）、`zh` / `cn` / `chinese` / `中文`、`wenyan` / `classical` / `文言` / `文言文` |
| `-emit-norm:<文件>` | 把方言改写后的 wei27 源码写入指定文件，便于排查问题 |
| `-target <平台>` | `win64` 或 `linux64`，默认为当前平台 |
| `-no-opt` | 关闭编译期优化 |
| `-ir` / `-no-ir` | 启用 / 关闭实验性 IR 后端（默认关闭） |
| `-wide` | 128 位宽算术（实验性，速度约为一半） |
| `-g` | 生成调试信息 |
| `-check=<等级>` | 检查强度：`none` / `min` / `standard`（默认）/ `full` |
| `-no-error-trace` / `-error-trace` | 关闭 / 开启 `?` 传播链站点打印（默认开启） |
| `--verbose` | 实时打印 8 个编译阶段的进度 |
| `-no-color` / `-color` | 强制关闭 / 开启彩色诊断（默认按终端自动判断） |

子命令（不带源文件参数）：

| 子命令 | 说明 |
| --- | --- |
| `mtg --version` | 版本、方言、目标与后端信息 |
| `mtg --pipeline` | 打印完整编译流程、每步职责与相关选项 |
| `mtg --list-codes` | 列出全部内置错误码 |
| `mtg explain <错误码>` | 查看某个错误码的中英说明与可编译修复示例 |
| `mtg precompile <文件.wei>` | 把库预编译成 `.mod` 模块接口 |

> 在 Windows 控制台中，如果直接输入 `-std:中文` 出现乱码，可改用等价的
> `-std:zh`。诊断的颜色、粗体与下划线只在真实终端生效；输出被重定向到
> 文件或管道时自动降级为纯文本，也可用 `NO_COLOR=1` 强制关闭。

---

## 语言示例

### 循环与数组

`for <i>;` 会自动创建循环变量 `i`，默认初值为 `1`。数组下标从 `0` 开始：

```wei
create.mod Main::main() {
    create.int n = 5;
    create.int squares[5];

    for <i>; i <= n; i++ {
        squares[i - 1] = i * i;
    }

    for <i>; i <= n; i++ {
        do.emit::squares[i - 1];
    }
}
```

`do.emit::` 的每个输出值单独占一行，因此上面的程序会依次输出 `1`、`4`、
`9`、`16`、`25`。这个换行是后端统一补的，想再空一行就写换行标记
`outlb` / `newline`（中文方言写 `换行`），它本身是一个空输出值，只产生
一次换行：

```wei
do.emit::"A"::outlb::"B";   // A / 空行 / B
```

### 模块与部件

`part` 定义模块内的常量成员，通过 `模块>部件.成员` 访问：

```wei
create.mod Demo {
    part calc {
        int base = 40;
        int extra = 2;
    }
}

create.mod Main::main() {
    do.emit::Demo>calc.base;
    do.emit::Demo>calc.extra;
}
```

### 中文与文言文

同一个程序也可以用中文关键字书写：

```wei
创建.模块 Main::main() {
    创建.整型 n = 3;
    输出 "你好，维语言！";
    输出 n;
}
```

```bash
mtg build hello-zh.wei -std:zh -o hello-zh.exe
```

文言文编写示例（外层仍使用 `create.mod` 声明入口）：

```wei
create.mod Main::main() {
    吾有一數 曰甲。其值為十。書之甲。
}
```

```bash
mtg build hello-wenyan.wei -std:wenyan -o hello-wenyan.exe
```

该程序输出 `10`。

### 文言文扫雷

[`saolei.wenyan.wei`](saolei.wenyan.wei) 是一个用文言方言写成的可玩扫雷，
示范了数组、循环、函数、字符串拼接与栈式漫延。棋盘九道（9×9），藏雷十枚，
也适合当作文言文维语言的入门读物。

Windows 下编译运行：

```bat
.\mtg.exe build saolei.wenyan.wei -std:wenyan -o saolei.exe
.\saolei.exe
```

Linux 下：

```bash
mtg build saolei.wenyan.wei -std:wenyan -o saolei
./saolei
```

**操作**

| 输入 | 动作 |
| --- | --- |
| `1` | 发土（翻开一格） |
| `2` | 树旗 / 拔旗 |
| `4` | 新局 |
| `0` | 退出 |

选择 `1` 或 `2` 后，再依次输入行、列（`1`～`9`）。终局后可输入 `1` 再局、
`0` 退出。

**规则**

- 首击必安：第一次发土后才布雷，并避开该格周围九宫。
- 空白格向八邻自动漫延；数字格显示周围八格的雷数。
- 最多树十面旗；翻开全部 71 个安全格即胜，触雷即败。
- 胜局将所有雷显示为 `旗`；败局将错旗显示为 `誤`。

**棋盘符号**

| 符号 | 含义 |
| --- | --- |
| `■` | 未翻开 |
| `□` | 已翻开的空白格 |
| `一`～`八` | 周围八格的雷数 |
| `旗` | 已树旗 |
| `雷` | 败局中显示的雷 |
| `爆` | 触发的雷 |
| `誤` | 败局中显示的错旗 |

棋盘、行列与计数用汉字数字显示；命令与坐标输入使用阿拉伯数字。

首击 `5,5` 后的示例局面：

```text
《掃雷》　九道縱橫，十雷潛伏

餘雷：十　已開：五十三／七十一

　　一 二 三 四 五 六 七 八 九
一　■ ■ ■ ■ ■ ■ 一 □ □
二　■ ■ ■ ■ ■ ■ 二 一 □
三　■ ■ ■ 一 一 三 ■ 二 □
四　■ ■ ■ 一 □ 二 ■ 二 □
五　■ 二 一 一 □ 一 一 一 □
六　一 一 □ □ □ □ □ □ □
七　□ □ □ □ □ □ □ □ □
八　一 一 二 一 一 一 一 一 □
九　■ ■ ■ ■ ■ ■ ■ 一 □

雷潛於下，慎爾所發。
1 發土　2 樹旗　4 新局　0 退出
```

源码约 370 行，入口为 `create.mod Main::main()`；雷区、状态、邻雷数与漫延栈
各用一个 81 元素的数组表示。汉字数字拆成 `漢字`、`漢尾`、`漢數` 三个函数生成，
避免在函数调用后直接接条件分支。

---

## 错误处理与诊断

### Result 与 `?`

用 `.Result` 声明函数返回显式错误，约定返回值 `>= 0` 为 `Ok(值)`、`< 0` 为
`Err(-错误码)`；`?` 把 `Err` 沿调用栈向上传播：

```wei
create.mod Math::div(int a, int b).Result {
    if (b == 0) { return -7; }
    return a / b;
}

create.mod Math::safe(int a, int b).Result {
    create.int v = Math::div(a, b)?;
    return v + 100;
}

create.mod Main::main().Result {
    create.int v = Math::safe(20, 0)?;
    do.emit::v;
    return 0;
}
```

`?` 只能出现在返回 `.Result` 的函数里，否则编译器报 `E0277` 并把 `^` 精确
标在 `?` 上 —— 错误码必须有人接住，不能像 C 那样静默忽略返回值。

### 自动错误传播链

Rust 的 `?` 只把 `Err` 原样上抛，想知道它经过哪些调用点通常得借助
`anyhow` 与 `RUST_BACKTRACE`。MTG 在**编译期**把每个 `?` 的源文件、行列、
所在函数与被调函数写进生成物，出错时由内向外逐层打印，不需要任何第三方库，
也不依赖运行时符号表：

```text
[错误传播] trace.wei:7:35   Math::safe   Math::div()?
[错误传播] trace.wei:12:37  Main::main   Math::safe()?
```

`Main::main` 的 `?` 触发时，还会以错误码的绝对值结束进程（上例退出码为 `7`），
避免错误被静默吞掉。加 `-no-error-trace` 可关闭站点打印，此时生成代码里
不注入任何额外指令：

```bash
mtg build trace.wei -o trace.exe -no-error-trace
```

### 诊断信息结构

报错不是一句孤立的 message。每条诊断都回答四个问题：**错在哪、来自哪个
文件、编译停在哪一步、接下来怎么改**。以 `E0277` 为例（下列为管道输出，
真实终端里标题、文件名与出错片段会带粗体/下划线）：

```text
error[E0277]: '?' 只能在返回 Result 的函数中使用
  --> tests/rustplus/e0277.wei:7:36
  = 来源: H:\mtg\tests\rustplus\e0277.wei  (主源文件, 共 9 行)
  |
 7 |     create.int v = Math::div(10, 2)?;
  |                                    ^ 这个 '?' 无处传播错误
  = help: 把当前函数改为返回 .Result，或改用显式判断返回值是否 < 0
  = help: 运行 mtg explain E0277 查看完整说明与可编译的修复示例
  = 预判: 把当前函数的返回类型改成 .Result，或去掉 '?' 并显式处理 Err
  = 阶段: 类型与可见性 [6/8] - 类型推导、export / fileprivate 判定、Result 约束
  = 流程: 读取源文件 -> ... -> [类型与可见性] -> 代码生成 -> 封装写盘
  = 继续: 修好后重新执行 mtg build tests/rustplus/e0277.wei；完整流程见 mtg --pipeline
```

- **粗体 / 下划线**：`error[码]` 与消息加粗，`-->` 的文件名、`= 来源:` 的
  绝对路径、以及源码行里真正出错的那段文本加下划线；下面的 `^` 仍按 CJK
  显示宽度精确对齐。Windows 会自动开启控制台 VT 模式，管道/CI 下自动关色。
- **预判**：除了 `help`，还会给出「你大概率是想……」。拼错标识符时会扫描
  源码里已声明的变量名、关键字表与内置函数，按编辑距离给出最相近的候选，
  例如 `coumt` 会提示 `count`；其余情况按错误类别给出最可能的成因。
- **文件来源**：显示绝对路径、总行数，并区分是主源文件还是导入模块，多文件
  编译时不必再猜错误出自哪一份 `.wei`。
- **编译如何进行**：`= 阶段` 给出失败所在步骤（共 8 步）及该步职责，
  `= 流程` 用方括号标出当前停下的位置，`= 继续` 给出重跑命令。
  完整说明用 `mtg --pipeline`，构建时加 `--verbose` 可逐步看到：

```text
  [1/8] 读取源文件 - 按二进制读入 .wei，记录源目录供 import 就近查找
  [2/8] 方言改写   - 中文 / 文言 → wei27；-emit-norm 导出改写结果
  [3/8] 词法分析   - 字符流 → token 流
  [4/8] 语法分析   - token 流 → AST
  [5/8] 单态化求值 - 泛型实例化、编译期确定性循环折叠
  [6/8] 类型与可见性 - 类型推导、export 判定、Result 约束
  [7/8] 代码生成   - AST → x86-64 机器码；注入 '?' 传播站点
  [8/8] 封装写盘   - PE32+ / ELF64 头、重定位、导入表
```

### 错误码手册

诊断里的每个错误码都能在编译器内置手册中查到，中英双语、含成因、最小复现
与可直接编译的修复片段，完全离线：

```bash
mtg --list-codes       # 列出全部错误码
mtg explain E0277      # 等价于 mtg --explain E0277
```

```
error[E0277]: '?' 只能在返回 Result 的函数中使用
           The '?' operator requires a Result-returning function

  成因 / 错误示例 / 修复 ...
```

若诊断以富文本形式输出，颜色会自动按 TTY 开关，也可通过 `NO_COLOR` 或
`WEI_FORCE_COLOR` 覆盖；`help` 行会直接提示可查询的手册命令。

### 验收

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests\rustplus\run.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tests\emit\run.ps1
# 或：mingw32-make emit-check
```

`rustplus` 共 25 项断言：错误码手册与 `--pipeline`、Ok/Err 两条路径、两层
传播链、关闭开关、`E0277` 精确诊断，以及诊断里的「来源 / 预判 / 阶段 / 流程 /
继续」五行。`emit` 是 `outlb` 换行次数的字节级回归用例。

---

## 通过 npm 安装

包名 `@wei-foundation/mtg`，安装后提供 `mtg` 命令，参数与直接使用
`mtg.exe` 完全一致。当前版本 **0.9.5**（增强诊断：粗体/下划线、预判、
文件来源、编译阶段与流程；并修复 `outlb` 换行两次），发布在
**npmjs.com 公共仓库**，无需账号、令牌或任何 registry 配置：

```bash
npm install -g @wei-foundation/mtg
```

```bash
mtg --version
mtg build hello.wei -o hello.exe
mtg explain E0277
```

用 `npm view @wei-foundation/mtg version` 可校验发布状态，应返回 `0.9.5`。

**备用源**：`.github/workflows/publish-ghpkg.yml` 用 GitHub Actions 的
`GITHUB_TOKEN`（`packages: write`）把同一版本发布到 **GitHub Packages**
（`npm.pkg.github.com`）。从该源按包名安装需要带 `read:packages` 权限的
GitHub 令牌：

```bash
npm config set @wei-foundation:registry https://npm.pkg.github.com
npm config set //npm.pkg.github.com/:_authToken 这里换成你的PAT
npm install -g @wei-foundation/mtg
```

注意这是**二选一**的备用方案：一旦把 `@wei-foundation:registry` 指向
`npm.pkg.github.com`，按包名安装就只会走 GitHub Packages。想回到公共仓库，
执行 `npm config delete @wei-foundation:registry`。

**免令牌安装**：[Mtg Stable Channel](https://github.com/Wei-Foundation/mtg/releases/tag/Mtg-Stable-Channel)
页面同时提供 `wei-foundation-mtg-0.9.5.tgz` 附件，任何人可直接安装，
无需账号或令牌：

```bash
npm install -g https://github.com/Wei-Foundation/mtg/releases/download/Mtg-Stable-Channel/wei-foundation-mtg-0.9.5.tgz
```

在本机仓库内也可以直接用 `npm pack` 产出的 tarball：

```bash
npm install -g H:\mtg\wei-foundation-mtg-0.9.5.tgz
```

包内自带 Windows x64 预编译的 `vendor/mtg.exe`，与 Release 附件字节一致；
CLI 会原样透传退出码，因此 `?` 传播链与非零错误码在 npm 安装方式下同样
有效。Linux x64 或希望使用最新源码时，可用随包附带的 `src/main.cpp`
本地构建：

```bash
npm run build:compiler   # 需要支持 C++26 的 GCC / Clang
```

在本仓库内开发或打包（包没有运行时依赖，不需要先 `npm install`）：

```bash
npm test                 # 冒烟测试：mtg --version
npm pack                 # 生成 wei-foundation-mtg-0.9.5.tgz
```

---

## 从源码构建

需要支持 C++26 的 GCC 或 Clang（已在 MinGW-w64 GCC 15.2.0 上验证），并建议
开启 `-O3`。

**Linux：**

```bash
make
```

**Windows（MinGW-w64）：**

```bat
mingw32-make
```

也可以直接运行 `build.bat`，或手动编译：

```bat
g++ -std=c++26 -O3 -o mtg.exe src\main.cpp
```

Linux 手动编译：

```bash
g++ -std=c++26 -O3 -o mtg src/main.cpp
```

构建完成后检查版本：

```bash
./mtg --version
```

Windows 下对应命令为 `.\mtg.exe --version`。如果能正常打印版本号，就可以开始
编译 Wei 程序了。

`make check` 会构建并打印版本，`make emit-check` 会跑 `outlb` 换行回归，
`make bootstrap-check` 会运行子集语义验收，`make bootstrap-fixpoint` 会验证
stage2/stage3 字节级一致（当前受保留字冲突阻塞，见 CHANGELOG），
`make clean` 会删除构建产物；Windows 下请将 `make` 换成 `mingw32-make`。

想了解一次 `mtg build` 内部依次做了什么，运行 `mtg --pipeline`；加
`--verbose` 可看到 8 个阶段的实时进度。

---

## 子集自举（进行中）

仓库正在用 Wei 语言实现一个可自编译的编译器子集，路线为：

```text
mtg.exe（stage0）→ bootstrap.exe（stage1）→ bootstrap2.exe（stage2）
→ bootstrap3.exe；要求 bootstrap2 与 bootstrap3 字节级一致
```

- 冻结规格：[`src/BOOTSTRAP.md`](src/BOOTSTRAP.md)
- stage1 源码：[`src/bootstrap.wei`](src/bootstrap.wei)
- 验收用例：[`tests/bootstrap/`](tests/bootstrap/)

当前只覆盖 10 天版子集：`Main::main`、整数/字符串变量、赋值、算术、比较、
`do.emit::`、`if` / `while`、函数调用、一维定长数组、Windows x64 PE，以及
`__command_line`、`__exit`、`__byte_at`、`__store_byte`、`__rt_alloc`、
`__rt_free`、`__read_file`、`__write_file`。模块/部件全局状态、元组、
模式匹配、IR、优化器、Linux ELF 与中文/文言方言不在本阶段范围内。

构建 stage1：

```powershell
.\mtg.exe build src\bootstrap.wei -o bootstrap.exe
```

运行子集语义验收（`-Compiler` 可指定 stage0、stage1 或 stage2）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests\bootstrap\run.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tests\bootstrap\run.ps1 -Compiler .\bootstrap2.exe
```

CLI 契约检查（`-RequireBuildSuccess` 额外要求编译器能重建 `src/bootstrap.wei`）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests\bootstrap\cli.ps1 -Compiler .\bootstrap2.exe -RequireBuildSuccess
```

验证 fixpoint（stage0 → stage1 → stage2 → stage3）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests\bootstrap\fixpoint.ps1
# 或
mingw32-make bootstrap-fixpoint
```

当前状态（2026-11-08）：`tests/bootstrap/run.ps1` 语义验收 10/10 通过；
`tests/rustplus/run.ps1` 25/25、`tests/emit/run.ps1` 2/2 通过。
`tests/bootstrap/cli.ps1` 为 14/20（6 项是子集冻结期留下的过时期望），
`make bootstrap-fixpoint` 仍被 `src/bootstrap.wei` 与关键字表冲突阻塞，
尚未达到 stage2 / stage3 字节级一致。两项均记录在
[CHANGELOG 的 Known Issues](CHANGELOG.md)。

---

## 项目结构

```text
src/main.cpp                 # 编译器实现：前端、诊断、优化与 x86-64 代码生成
src/BOOTSTRAP.md             # 子集自举冻结规格（语法、ABI、内存布局、CLI、验收）
src/bootstrap.wei            # stage1 自举源码（子集编译器完整实现）
bin/mtg.js                   # npm 包的 CLI 入口（透传参数与退出码）
scripts/build.js             # npm run build:compiler：随包源码本地构建
scripts/smoke.js             # npm test 冒烟测试
vendor/mtg.exe               # npm 包内的 Windows x64 预编译编译器
package.json                 # npm 包定义（@wei-foundation/mtg）
.github/workflows/           # GitHub Actions：发布到 GitHub Packages
tests/rustplus/              # Result / 错误传播链 / 诊断 25 项验收
tests/emit/                  # do.emit:: 换行次数字节级回归
tests/bootstrap/             # 10 个子集验收程序、run.ps1、cli.ps1、fixpoint.ps1
saolei.wenyan.wei            # 文言文扫雷示例（九道棋盘、十雷、首击安全）
Makefile                     # 跨平台构建脚本（含 emit-check 等目标）
build.bat                    # Windows 一键构建脚本
.gitignore / .gitattributes  # 忽略规则 / 行尾与二进制规则
.editorconfig                # 编辑器格式配置
CONTRIBUTING.md              # 贡献指南
CODE_OF_CONDUCT.md           # 行为准则
SECURITY.md                  # 安全策略
CHANGELOG.md                 # 更新日志
LICENSE                      # GNU GPLv3 完整协议文本
Wei.png                      # 项目 Logo
README.md
```

---

## 参与贡献

欢迎通过 Issue 反馈问题，或提交 Pull Request 参与开发。

提交前请阅读[贡献指南](CONTRIBUTING.md)与[行为准则](CODE_OF_CONDUCT.md)。
安全问题请按[安全策略](SECURITY.md)私下报告。

---

## 许可证

Module Target Generator 采用 [GNU 通用公共许可证第三版（GPLv3）](LICENSE) 授权。
完整协议文本见仓库根目录的 [`LICENSE`](LICENSE) 文件，取自
[GNU 官方发布页](https://www.gnu.org/licenses/gpl-3.0.txt)。
