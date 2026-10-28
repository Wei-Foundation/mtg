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

根目录的 `saolei.wenyan.wei` 是一个可玩的扫雷游戏：九道棋盘、十枚雷，首击必安。
以 `1` 发土、`2` 树旗、`4` 新局、`0` 退出；棋盘与计数用汉字显示。

```bash
mtg build saolei.wenyan.wei -std:wenyan -o saolei.exe
./saolei.exe
```

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

Windows 下对应命令为 `mtg.exe --version`。如果能正常打印版本号，就可以开始
编译 Wei 程序了。

`make check` 会构建并打印版本，`make bootstrap-check` 会运行子集验收脚本，
`make clean` 会删除构建产物；Windows 下请将 `make` 换成 `mingw32-make`。

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

运行 stage0 语义验收：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests\bootstrap\run.ps1
```

CLI 契约检查（默认允许 Day 1 的“尚未实现代码生成”状态）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests\bootstrap\cli.ps1 -Compiler .\bootstrap.exe
```

第 1 天（2026-10-28）状态：10/10 验收用例通过；`bootstrap.wei` 的 CLI 骨架
可编译并完成参数解析、源文件读取与退出码处理，词法分析器和代码生成器尚未接入。

---

## 项目结构

```text
src/main.cpp          # 编译器实现：前端、优化与 x86-64 代码生成
src/BOOTSTRAP.md      # 子集自举冻结规格（语法、ABI、内存布局、CLI、验收）
src/bootstrap.wei     # stage1 自举源码（子集 CLI 骨架）
tests/bootstrap/      # 10 个子集验收程序、期望输出与 run.ps1
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
