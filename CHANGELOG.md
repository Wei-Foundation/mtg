# 更新日志

本项目遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 的格式，版本号沿用编译器 `--version` 输出的版本号。

## [Unreleased]

### Added

- `Makefile`，支持 `make` / `mingw32-make` 构建、`make check` 与 `make clean`
- `.gitignore`、`.gitattributes`、`.editorconfig`
- 贡献指南、行为准则、安全策略与更新日志
- 完整的 GPLv3 许可证文本

### Changed

- README 补充 Make 构建方式与社区文档链接
- 清理已不存在的扫雷示例引用

## [0.02.74282] - 2026-10-26

### Added

- 维语言原生编译器，支持 `wei27`、中文、文言三种源码方言
- Windows x64 与 Linux x64 目标后端
- 模块与部件、数组、元组、模式匹配、编译期求值等语言特性
- 编译期循环优化，以及实验性 IR 后端

### Fixed

- 修复 Windows 控制台输入例程的栈参数顺序问题，避免读取输入时崩溃
