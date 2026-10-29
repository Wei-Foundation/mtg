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
- [语言示例](#语言示例)
  - [文言文扫雷](#文言文扫雷)
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
hello.exe
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

| 选项 | 说明 |
| --- | --- |
| `-o <文件>` | 指定输出文件路径 |
| `-std:<风格>` | 选择源码方言：`wei27` / `wei` / `default`（默认）、`zh` / `cn` / `chinese` / `中文`、`wenyan` / `classical` / `文言` / `文言文` |
| `-emit-norm:<文件>` | 把方言改写后的 wei27 源码写入指定文件，便于排查问题 |
| `-target <平台>` | 目标平台：`win64` 或 `linux64`，默认为当前平台 |
| `-no-opt` | 关闭编译期优化 |
| `-ir` | 启用实验性 IR 后端 |
| `-wide` | 使用 128 位宽算术（实验性，速度约为一半） |
| `--version` | 显示编译器版本与后端信息 |

> 在 Windows 控制台中，如果直接输入 `-std:中文` 出现乱码，可改用等价的
> `-std:zh`。

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
`9`、`16`、`25`。

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
示范了数组、循环、函数、字符串拼接与栈式漫延。棋盘九道（9×9），藏雷十枚。您可以通过此文件，来基本了解文言文维语言的基本语法。

Windows 下编译运行：

```bat
.\mtg.exe build saolei.wenyan.wei -std:wenyan -o saolei.exe
.\saolei.exe
```

Linux/macOS 下：

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

## 从源码构建

需要支持 C++26 的 GCC 或 Clang（已在 MinGW-w64 GCC 15.2.0 上验证），并建议
开启 `-O3`。

**Linux / macOS：**

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

Linux / macOS 手动编译：

```bash
g++ -std=c++26 -O3 -o mtg src/main.cpp
```

构建完成后检查版本：

```bash
./mtg --version
```

Windows 下对应命令为 `.\mtg.exe --version`。如果能正常打印版本号，就可以开始
编译 Wei 程序了。

`make check` 会构建并打印版本，`make bootstrap-check` 会运行子集语义验收，
`make bootstrap-fixpoint` 会验证 stage2/stage3 字节级一致，`make clean` 会删除
构建产物；Windows 下请将 `make` 换成 `mingw32-make`。

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

当前状态（2026-10-28）：stage0、stage1、stage2 下 10/10 语义验收通过；
CLI 契约 21/21 通过；`bootstrap2.exe` 与 `bootstrap3.exe` 字节级一致
（SHA256 `8C57ADE1C319A9196E5B629235DB32F2AF253EE7CACD0BF9B60F417C268E725E`）。

---

## 项目结构

```text
src/main.cpp          # 编译器实现：前端、优化与 x86-64 代码生成
src/BOOTSTRAP.md      # 子集自举冻结规格（语法、ABI、内存布局、CLI、验收）
src/bootstrap.wei     # stage1 自举源码（子集编译器完整实现）
tests/bootstrap/      # 10 个子集验收程序、run.ps1、cli.ps1、fixpoint.ps1
saolei.wenyan.wei     # 文言文扫雷示例（九道棋盘、十雷、首击安全）
Makefile              # 跨平台构建脚本
build.bat             # Windows 一键构建脚本
.gitignore            # Git 忽略规则
.gitattributes        # Git 行尾与二进制规则
.editorconfig         # 编辑器格式配置
CONTRIBUTING.md       # 贡献指南
CODE_OF_CONDUCT.md    # 行为准则
SECURITY.md           # 安全策略
CHANGELOG.md          # 更新日志
LICENSE               # GNU GPLv3 完整协议文本
Wei.png               # 项目 Logo
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
