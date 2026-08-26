<p align="center">
  <img src="\Wei.png" width="80" alt="Wei Logo">
</p>


<h1 align="center">Wei-Toolchain</h1>

<p align="center">
  <a href="https://opensource.org/licenses/GPL-3.0">
    <img src="https://img.shields.io/badge/License-GPL_3.0-blue.svg" alt="License">
  </a>
</p>


---

## Introduction

Wei-Toolchain is an open-source, cross-platform, high-performance Toolchain for Wei 27, delivering faster execution than C++ and Rust in common algorithmic benchmarks.

---

## Quick Start

**Compile Wei programs with Wei-Toolchain**

1. Download the Wei-Toolchain executable

2. Write a simple Wei program:

```wei
create.mod main() {
    emit>"Hello, World!";
}
```

3. Compile using the Build subcommand:

```bash
./wt build Test.wei
```

---

## Language Examples

### Loop with auto-created variable

```wei
create.mod main() {
    create.int n = 10;
    for <i>; i < n; i++ {
        emit>i;
    }
}
```

### Sub-module with lazy access

```wei
create.mod main() {
    create.part calc {
        return 1 + 2;
    }
    emit>calc;
}
```

### Arrays starting at 1

```wei
create.mod main() {
    create.int arr[5];
    arr[1] = 10;
    arr[5] = 50;
    emit>arr[1];
}
```

---

## Build and Run

This project requires: **G++/Clang**

Note: Ensure your compiler supports the C++26 standard and enable `-O3` optimization during build.

**Build Wei-Toolchain:**

```bash
g++ -std=c++26 -O3 -o wt src/wt.cpp
```

**Run:**

```bash
./wt --version
```

Expected output: the compiler version number. If it prints correctly, you are ready to use it.

---

## Contributing

If you want to participate in development, please submit your changes via Pull Request.

---

## License

Wei-Toolchain is licensed under the [GPL-3.0 License](https://opensource.org/licenses/GPL-3.0).
