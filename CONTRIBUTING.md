# 贡献指南

感谢你愿意为 Module Target Generator（MTG）做出贡献。

## 开始之前

- 提交 Issue 前，请先搜索现有 Issue，避免重复。
- 较大的功能改动建议先开 Issue 讨论，确认方向后再实现。
- 请遵守[行为准则](CODE_OF_CONDUCT.md)。
- 安全问题请按[安全策略](SECURITY.md)私下报告，不要公开披露细节。

## 开发环境

- 支持 C++26 的 GCC 或 Clang（已在 MinGW-w64 GCC 15.2.0 上验证）
- GNU Make（Windows 可使用 MinGW 提供的 `mingw32-make`）
- Git

## 构建

Linux / macOS：

```bash
make
```

Windows（MinGW-w64）：

```bat
mingw32-make
```

也可以直接运行仓库中的 `build.bat`，或手动编译。

Linux / macOS：

```bash
g++ -std=c++26 -O3 -o mtg src/main.cpp
```

Windows：

```bat
g++ -std=c++26 -O3 -o mtg.exe src\main.cpp
```

构建完成后检查版本：

```bash
./mtg --version
```

## 测试

当前仓库还没有独立的自动化测试套件。提交前请至少执行：

```bash
make check
```

并手动验证受影响的编译路径：

- 修改前端或代码生成时，用最小 `.wei` 程序编译并实际运行。
- 修改 Windows 相关代码时，请在 Windows 上重新编译并测试。
- 修改输入、输出或目标平台相关代码时，请同时检查 Windows 与 Linux 行为。

## 代码风格

- C++ 源码使用 UTF-8、LF 换行和制表符缩进，请遵循 `src/main.cpp` 的现有风格。
- 文档使用中文，代码标识符和命令保持原有写法。
- 保留单文件编译器的整体结构，避免无必要的拆分。
- 不要提交构建产物（`mtg`、`mtg.exe`、`*.o`、`*.obj` 等）。

## 提交与 Pull Request

- 每个 Pull Request 尽量只做一件逻辑相关的事。
- 在描述中说明改动原因、行为变化和验证方式。
- 面向用户的改动请同步更新 `README.md` 和 `CHANGELOG.md`。
- 新增文件请确认许可证兼容 GPLv3。

提交贡献即表示你同意将贡献内容按 [GNU GPLv3](LICENSE) 授权。
