# MTG 子集自举规格（10 天版）

- 规格版本：`bootstrap-subset-v1`（2026-10-27 冻结）
- 基线编译器：`mtg.exe` 0.02.74282（stage0）
- 目标平台：Windows x64（PE32+）
- 状态：第 1 天冻结。语法、类型、运行时 ABI、内存布局、CLI 与验收标准此后不得
  随意变更；确需变更时必须同时更新本文、`tests/bootstrap/` 与 `CHANGELOG.md`。

当前实现状态（2026-10-28）：

- 规格、10 个验收用例与运行器已完成；stage0、`bootstrap.exe`（stage1）与
  `bootstrap2.exe`（stage2）下均为 10/10 通过；
- `src/bootstrap.wei` 已包含完整的词法器、单遍解析/代码生成、x64 指令发射、
  PE32+ 写出器、运行时桩与 CLI；
- `bootstrap2.exe build src/bootstrap.wei -o bootstrap3.exe` 成功，
  `SHA256(bootstrap2.exe) == SHA256(bootstrap3.exe)`，fixpoint 已达成；
- CLI 契约检查 21/21 通过（含 `-RequireBuildSuccess` 重建 `bootstrap.wei`）。

---

## 1. 目标与非目标

### 1.1 自举链路

```text
stage0:  mtg.exe（现有 C++ 编译器）
stage1:  mtg.exe build src/bootstrap.wei -o bootstrap.exe
stage2:  bootstrap.exe build src/bootstrap.wei -o bootstrap2.exe
stage3:  bootstrap2.exe build src/bootstrap.wei -o bootstrap3.exe
fixpoint: SHA256(bootstrap2.exe) == SHA256(bootstrap3.exe)
```

`bootstrap.exe`、`bootstrap2.exe`、`bootstrap3.exe` 都是同一份
`src/bootstrap.wei` 编译出的 Windows x64 控制台程序。自举完成的最低标准不是
“stage1 与 stage0 生成的机器码相同”，而是：

1. `bootstrap.exe` 能编译 `src/bootstrap.wei`，产出可运行的 `bootstrap2.exe`；
2. `bootstrap2.exe` 能编译同一份源码，产出 `bootstrap3.exe`；
3. `bootstrap2.exe` 与 `bootstrap3.exe` 字节级一致；
4. `bootstrap2.exe` 能编译并通过第 9 节的 10 个子集验收用例。

### 1.2 子集范围

本子集只冻结以下能力：

- `create.mod Main::main()` 入口；
- `int`、`char`（8 字节字符串指针）、`ptr.int`、`omni` 四种值类型；
- 局部变量、赋值、整数算术与比较；
- `do.emit::`；
- `if` / `else` / `while`；
- 模块限定的函数调用（`Mod::fn(...)`）与自递归；
- 一维定长 `int` 数组；
- `__command_line`、`__exit`、`__byte_at`、`__store_byte`、
  `__rt_alloc`、`__rt_free`、`__read_file`、`__write_file`；
- Windows x64 PE32+ 输出。

### 1.3 明确不支持

以下内容在 10 天版中不实现，也不作为验收项：

- 模块/部件全局状态（`part`、模块级变量）；
- 元组、模式匹配、`match`、闭包、协程、类；
- `for`、`loop`、`repeat`、`switch`；
- 浮点、布尔独立类型、128 位宽算术；
- 指针算术、指针下标、`*p` 解引用、移位、位运算；
- 多级指针、指针返回类型（`.ptr.int` 返回值）；
- 数组参数按值传递、数组返回、多维数组、动态数组；
- 模块导入（`require` / `import`）、别名、方言改写；
- IR、优化器、Linux ELF、`-wide`、`-emit-norm`；
- `__file_open` / `__file_write` / `__file_close`（stage0 已有，stage1
  不保证；自举只需要整文件读写）；
- 中文 / 文言源码风格。

### 1.4 与 stage0 的关系

stage0 支持的不少扩展**不在**本子集内，例如 `*p`、`else if`、块注释
`/* ... */`、指针算术、`__file_*`。`src/bootstrap.wei` 不得使用这些扩展，
否则 `bootstrap.exe` 无法自编译。stage0 中已经验证可用、但本子集不承诺的
能力，不得写进验收用例。

---

## 2. 词法

### 2.1 源文件

- UTF-8 字节流；允许 BOM，但编译器应在第一个 token 前跳过 BOM。
- 最大 1 MiB（`1 048 576` 字节）；超限报错，退出码 1。
- 换行：`\n`、`\r\n`、`\r` 均视为一个换行。
- 行号、列号从 1 开始。

### 2.2 空白与注释

- 空白：空格、`\t`、`\r`、`\n`。
- 注释：仅 `// ...` 到行尾。
- **不支持** `/* ... */` 块注释；stage0 也不支持，源码中出现会解析失败。
- 注释与空白不产生 token。

### 2.3 标识符与关键字

```text
ident      = ( letter | "_" ) { letter | digit | "_" }
letter     = "A".."Z" | "a".."z"
digit      = "0".."9"
```

- 标识符最长 128 字节；超限报错。
- 关键字（只允许英文 wei27）：

```text
create  mod  int  char  ptr  omni
if  else  while  return  true  false
do  emit
```

- `ptr.int` 由 `ptr`、`.`、`int` 三个 token 组成，不是单个关键字。
- 内置函数名以 `__` 开头，按普通标识符处理，由调用检查识别。

### 2.4 整数字面量

```text
intlit     = digit { digit }
```

- 仅十进制；不支持 `0x`、`0b`、八进制、下划线分隔、正负号、后缀。
- 负数是 `-` 加字面量。
- 取值范围：`0 .. 9223372036854775807`。超出报错，退出码 1。

### 2.5 字符串字面量

```text
stringlit  = '"' { char | escape } '"'
escape     = "\\n" | "\\t" | "\\r" | "\\\\" | "\\\"" | "\\0"
```

- 字符串是 `char` 类型，运行时是 8 字节指针，指向只读数据区中的
  NUL 结尾字节串。
- 单个字面量最长 65 535 字节；字符串池总上限 512 KiB。
- 不允许跨行；未闭合报错。
- `\0` 可以写，但程序不应依赖含内嵌 NUL 的字符串参与 `printf("%s")`。

### 2.6 标点与运算符

```text
::  .  {  }  (  )  [  ]  ,  ;
=  ==  !=  <  >  <=  >=  +  -  *  /  %  !  &&  ||
```

- `=` 是赋值；`==` 是相等比较。
- 不支持 `++`、`--`、`+=`、`<<`、`>>`、`&`、`|`、`^`、`~`、`?:`。
- 单字符 `|` 是词法错误（避免漏写 `||`）。

---

## 3. 语法

### 3.1 EBNF

```ebnf
program      = { funcDecl } EOF ;
funcDecl     = "create" "." "mod" IDENT "::" IDENT
               "(" [ paramList ] ")" [ "." retType ] block ;
paramList    = param { "," param } ;
param        = valueType IDENT ;
valueType    = "int" | "char" | "ptr" "." "int" | "omni" ;
retType      = "int" | "char" | "omni" ;
block        = "{" { stmt } "}" ;

stmt         = varDecl
             | assignStmt
             | callStmt
             | emitStmt
             | ifStmt
             | whileStmt
             | returnStmt
             | block ;

varDecl      = "create" "." valueType IDENT
               [ "[" INT "]" ] [ "=" init ] ";" ;
init         = expr | "{" [ expr { "," expr } ] "}" ;
assignStmt   = lvalue "=" expr ";" ;
lvalue       = IDENT | IDENT "[" expr "]" ;

callStmt     = IDENT "::" IDENT "(" [ argList ] ")" ";" ;
emitStmt     = "do" "." "emit" "::" expr { "::" expr } ";" ;
ifStmt       = "if" "(" expr ")" block [ "else" block ] ;
whileStmt    = "while" "(" expr ")" block ;
returnStmt   = "return" [ expr ] ";" ;

argList      = expr { "," expr } ;

expr         = orExpr ;
orExpr       = andExpr { "||" andExpr } ;
andExpr      = eqExpr { "&&" eqExpr } ;
eqExpr       = relExpr { ( "==" | "!=" ) relExpr } ;
relExpr      = addExpr { ( "<" | ">" | "<=" | ">=" ) addExpr } ;
addExpr      = mulExpr { ( "+" | "-" ) mulExpr } ;
mulExpr      = unary { ( "*" | "/" | "%" ) unary } ;
unary        = ( "-" | "!" ) unary | postfix ;
postfix      = primary { "[" expr "]" } ;

primary      = INT
             | STRING
             | "true"
             | "false"
             | IDENT
             | "(" expr ")"
             | IDENT "::" IDENT "(" [ argList ] ")"
             | BUILTIN "(" [ argList ] ")" ;
```

说明：

- `funcDecl` 只允许带成员的模块形式；`create.mod Foo { ... }` 不在子集内。
- `main` 的返回类型可省略或写 `.int`，但返回值被忽略；进程退出码由
  `__exit` 或正常结束（0）决定。
- `else` 后必须是块；**不支持 `else if`**，必须写成
  `else { if (...) { ... } }`。
- `postfix` 语法上允许多个下标，语义上只允许一次；下标基址必须是局部
  `int` 数组。
- 函数调用必须带模块名和 `::`；不支持裸函数名调用（内置函数除外）。
- 语句和声明都以 `;` 结束（块语句除外）。

### 3.2 运算符优先级与结合性

从低到高：

| 优先级 | 运算符 | 结合性 | 结果 |
| --- | --- | --- | --- |
| 1 | `||` | 左 | int（0/1，短路） |
| 2 | `&&` | 左 | int（0/1，短路） |
| 3 | `==` `!=` | 左 | int（0/1） |
| 4 | `<` `>` `<=` `>=` | 左 | int（0/1） |
| 5 | `+` `-` | 左 | int |
| 6 | `*` `/` `%` | 左 | int |
| 7 | 一元 `-` `!` | 右 | int |
| 8 | `a[i]` | 后缀 | 元素类型 |

- 比较结果统一为 int：真 = 1，假 = 0。
- `!` 对 0 得 1，对非 0 得 0。
- 不允许连续比较作为数学区间使用；`a < b < c` 会按 `(a < b) < c`
  求值，源码不应依赖这种写法。

---

## 4. 类型与语义

### 4.1 值表示

| 类型 | 大小 | 表示 |
| --- | --- | --- |
| `int` | 8 字节 | 有符号 64 位，二进制补码，小端 |
| `char` | 8 字节 | 指向 NUL 结尾字节串的指针 |
| `ptr.int` | 8 字节 | 任意内存地址，不做算术 |
| `omni` | 8 字节 | 无类型 8 字节值，仅用于跨类型桥接 |

- 所有局部变量、参数、数组元素都占 8 字节槽。
- `char`、`ptr.int` 与 `omni` 之间可经 `.omni` 恒等函数转换：

```wei
create.mod Cast::text(omni x).char {
    return x;
}
create.mod Cast::ptr(omni x).omni {
    return x;
}
```

- `int` 与 `char` / `ptr.int` **不能**直接互相赋值；必须经 `.omni` 包装。
  这是 stage0 类型检查器的实际行为。
- `.ptr.int` 不能作为返回类型；返回指针的函数写成 `.omni`，调用侧用
  `create.ptr.int p = Fn();` 接收。
- 字符串字面量类型是 `char`；数组名在表达式中退化为 `ptr.int`。

### 4.2 声明、作用域与初始化

- 变量必须先声明后使用。
- 作用域为块作用域：进入 `{}` 压入作用域，离开弹出。
- 同一作用域内不得重名；子作用域不得遮蔽外层同名变量（简化符号表）。
- 标量声明可带初值；未带初值的标量在 stage0 中值未定义，stage1 会清零。
  源码不得依赖未初始化值。
- 数组声明必须写正整数字面量长度，范围 `1 .. 65536`。
- 数组元素类型只支持 `int`；`create.char a[N]`、`create.ptr.int a[N]`
  不在子集内。
- 数组初始化列表 `{e0, e1, ...}` 的元素个数不得超过长度；不足部分补 0。
  没有初始化列表时全部补 0（stage1 保证；stage0 栈上内容不应依赖）。
- 数组是连续升序布局：`a[i]` 地址 = 基址 + `8*i`，元素 0 在最低地址。
- 数组下标表达式必须是 `int`；运行时检查 `0 <= i < N`，越界时打印
  `runtime panic: array index out of bounds` 并以退出码 1 结束。
- 数组名可作为 `ptr.int` 实参传递，指向元素 0；长度信息不随指针传递。

### 4.3 赋值

- 赋值目标只能是：
  - 局部标量 `x`；
  - 局部数组元素 `a[i]`。
- 赋值表达式本身没有值；`a = b = 1` 不合法。
- 赋值类型必须完全一致，或一侧为 `omni`。
- 数组元素赋值先检查下标，再写 8 字节。

### 4.4 整数运算

- `+`、`-`、`*`：64 位二进制补码。stage1 不做溢出检查，溢出按回绕处理；
  stage0 会在部分溢出场景 panic。依赖溢出的程序不在验收范围。
- `/`：向零截断；`%`：余数符号与被除数相同。
- 除数为 0：打印 `runtime panic: division by zero`，退出码 1。
- `INT64_MIN / -1` 在 stage1 中不保证；源码不得构造该情形。
- 比较按有符号 64 位进行比较。
- `&&`、`||` 必须短路，结果为 0/1。
- 条件表达式（`if`、`while`）接受任意 `int`：0 为假，非 0 为真。

### 4.5 字符串

- `char` 是 8 字节指针；不提供内建长度、拼接、切片、索引。
- 字符串比较必须手写逐字节函数，例如：

```wei
create.mod Str::eq(char a, char b).int {
    create.int i = 0;
    while (1 == 1) {
        if (__byte_at(a, i) != __byte_at(b, i)) { return 0; }
        if (__byte_at(a, i) == 0) { return 1; }
        i = i + 1;
    }
    return 0;
}
```

- 字符串字面量位于只读数据区；不得写入。
- 需要可写字符串时，用 `__rt_alloc` 分配缓冲区，再用 `__store_byte` 写入。

### 4.6 函数

- 函数由 `模块名 + 成员名` 唯一标识；不允许重载。
- 定义顺序即声明顺序：函数必须在首次调用前定义。
- 自递归允许：解析函数头时先登记符号，函数体内部可调用自身。
- 互递归不允许（被调方尚未定义）。
- 参数最多 4 个；参数类型只能是 `int`、`char`、`ptr.int`、`omni`。
- 参数按值传递；数组实参退化为 `ptr.int`。
- 实参从左到右求值；全部求值完成后进入函数。
- 返回类型省略表示 `void`。`void` 函数只能写 `return;`，不能返回值。
- 有返回类型的函数可以 `return expr;`；走到函数末尾时返回 0。
- 返回类型与表达式类型必须一致，或一侧为 `omni`。
- `Main::main` 必须存在且唯一，无参数，返回类型可省略或 `.int`。
- 调用约定：Win64，前 4 个整型/指针参数依次放 `RCX`、`RDX`、`R8`、`R9`；
  返回值放 `RAX`。不使用浮点寄存器。
- 被调函数必须保持 `RBX`、`RBP`、`RDI`、`RSI`、`R12`–`R15`；stage1
  生成的代码只使用易失寄存器，但仍遵守 16 字节栈对齐与 32 字节影子空间。

### 4.7 `do.emit::`

- 语法：`do.emit::expr::expr...;`，至少一个表达式。
- 每个值单独输出一行。
- `int`：有符号十进制，格式 `%lld\n`。
- `char`：按字节串输出到 NUL，再输出 `\n`，格式 `%s\n`。
- `ptr.int` / `omni`：按有符号十进制输出（地址值），仅用于调试。
- 求值顺序从左到右。
- `do.emit::` 只写标准输出，不写标准错误。

### 4.8 控制流

- `if (cond) { ... } else { ... }`；`else` 可省略。
- `while (cond) { ... }`；条件在每轮开始求值。
- `return` 可在任意嵌套块内使用，直接返回当前函数。
- 不支持 `break`、`continue`、`goto`。

---

## 5. 运行时 ABI（stage1 必须实现）

### 5.1 内置函数表

| 名称 | 参数 | 返回 | 说明 |
| --- | --- | --- | --- |
| `__command_line` | 无 | `omni`（`char` 指针） | `GetCommandLineA()` 的完整命令行，含 `argv[0]` |
| `__exit` | `int code` | 无（不返回） | 立即结束进程；建议 `code` 在 0..255 |
| `__byte_at` | `ptr.int base, int off` | `int` | 读 `base[off]`，结果 0..255 |
| `__store_byte` | `ptr.int base, int off, int v` | 无 | 写 `base[off] = v & 0xFF` |
| `__rt_alloc` | `int n` | `omni`（指针） | 分配至少 `n` 字节，16 字节对齐 |
| `__rt_free` | `omni p` | 无 | 释放；stage1 中为空操作，不做泄漏检查 |
| `__read_file` | `char path, ptr.int buf, int max` | `int` | 二进制读，最多 `max` 字节，返回实际字节数；失败或空文件返回 0 |
| `__write_file` | `char path, ptr.int buf, int n` | `int` | 二进制写，截断已有文件，返回实际写入字节数；失败返回 0 |

补充：

- `__read_file` 不追加 NUL；调用方必须在 `buf[n]` 自行写 0（缓冲区需多留 1 字节）。
- `__read_file` 无法区分“空文件”和“打开失败”，都返回 0。CLI 报错文案应说明
  “missing or empty”。
- `__exit` 在 stage0 中直接调用 `ExitProcess`；stage1 生成的程序在
  `ExitProcess` 前应尽量刷新标准输出。若刷新实现不可靠，至少保证每个
  `do.emit::` 以换行结束。
- stage1 不实现 `__file_open` / `__file_write` / `__file_close` /
  `__rt_panic`。`src/bootstrap.wei` 不得调用它们。

### 5.2 调用约定

- 目标：Windows x64（Microsoft x64 ABI）。
- 整型/指针参数：`RCX`、`RDX`、`R8`、`R9`；最多 4 个。
- 返回值：`RAX`。
- 调用前 `RSP % 16 == 0`；被调函数入口 `RSP % 16 == 8`。
- 调用者提供 32 字节影子空间。
- stage1 生成代码只使用 `RAX`、`RCX`、`RDX`、`R8`–`R11`，不改
  `RBX`、`RBP`、`RDI`、`RSI`、`R12`–`R15`。
- 函数序言：

```text
push rbp
mov  rbp, rsp
sub  rsp, <frame_size>      ; frame_size 为 16 的倍数，编译结束时回填
; 保存 RCX/RDX/R8/R9 到参数槽
```

- 函数尾声：

```text
mov  rsp, rbp
pop  rbp
ret
```

### 5.3 生成程序的栈布局

- 标题槽（从 `rbp` 向下）：
  - 临时表达式槽：64 个，每个 8 字节；
  - 参数槽：最多 4 个；
  - 局部变量槽：标量 1 个，数组 `N` 个。
- 标量槽地址：`rbp - 8*(slot+1)`。
- 数组占用 `slot .. slot+N-1`，元素 0 在最低地址：

```text
base(a) = rbp - 8*(slot + N)
addr(a[i]) = base(a) + 8*i
```

- 帧大小在函数体编译完成后回填到 `sub rsp, imm32`。
- 单函数帧上限 1 MiB；超过报错退出 1。

### 5.4 生成程序的堆

- stage1 生成的 PE 在 `.data` 末尾保留 32 MiB 虚拟堆区（`HEAP_SIZE = 32 MiB`）。
  预留较大空间是因为 stage1 编译器自身要同时持有源文件、代码缓冲、数据缓冲、
  回填表和输出 PE；PE 的 BSS 不占文件体积，按需提交。
- 堆指针与堆尾指针放在固定全局槽，由入口桩初始化。
- `__rt_alloc(n)`：
  - `n <= 0` 视为错误；
  - 按 16 字节向上对齐；
  - 若越过堆尾，打印 `runtime panic: heap exhausted`，退出码 1；
  - 返回指针对齐到 16 字节。
- `__rt_free(p)`：空操作。stage1 不做复用、不做泄漏统计、不打印
  `runtime warning: memory leaked at exit`。
- 这与 stage0 的隔离区/空闲链表实现不同，属于已记录的偏差；子集程序不得
  依赖释放后的内容（stage0 会投毒 `0xA5`）。

### 5.5 生成程序的 PE 布局

固定参数：

| 项 | 值 |
| --- | --- |
| Machine | `0x8664` |
| Optional Header Magic | `0x20B`（PE32+） |
| ImageBase | `0x140000000` |
| SectionAlignment | `0x1000` |
| FileAlignment | `0x200` |
| Subsystem | `3`（Console） |
| SizeOfHeaders | `0x200` |
| DllCharacteristics | `0x0100`（NXCOMPAT；不启用 DYNAMICBASE） |
| StackReserve | `0x1000000`（16 MiB） |
| StackCommit | `0x100000`（1 MiB） |
| TimeDateStamp | `0`（保证可重复构建） |

节表：

| 节 | RVA | Raw | 内容 |
| --- | --- | --- | --- |
| `.text` | `0x1000` | `0x200` | 入口桩、函数、运行时桩、panic 桩 |
| `.data` | `align(0x1000 + text_size, 0x1000)` | `0x200 + align(text_size, 0x200)` | IAT/ILT、导入表、格式串、字符串池、堆 |

`.text` 特征：`0x60000020`（code | execute | read）。
`.data` 特征：`0xC0000040`（initialized data | read | write）。

`.data` 固定头部（RVA 相对 `.data` 起点）：

| 偏移 | 内容 |
| --- | --- |
| `0x00` | IAT：`printf`（0）、`fopen`（8）、`fread`（16）、`fwrite`（24）、`fclose`（32）、null（40）、`GetCommandLineA`（48）、`ExitProcess`（56）、null（64） |
| `0x48` | ILT，布局与 IAT 相同 |
| `0x90` | hint/name 表、DLL 名、导入描述符 |
| `0x200` | `"%lld\n"` |
| `0x210` | `"%s\n"` |
| `0x220` | 字符串池起点（向上增长） |
| `align(0x220 + string_pool_size, 16)` | 堆起点；堆虚拟大小 32 MiB |

导入：

| DLL | 函数 |
| --- | --- |
| `msvcrt.dll` | `printf`、`fopen`、`fread`、`fwrite`、`fclose` |
| `kernel32.dll` | `GetCommandLineA`、`ExitProcess` |

- 导入目录 RVA 指向 `.data + 0x90` 区域的描述符数组；描述符为 PE32+ 标准的
  20 字节 `IMAGE_IMPORT_DESCRIPTOR`，`FirstThunk` 指向 IAT，`OriginalFirstThunk`
  指向 ILT，数组以全 0 项结束。
- 未列出的 PE 头字段按 stage0 `Win64Target::finalize` 的取值填写；任何差异
  不得改变加载行为。不生成 `.reloc` 节（无绝对地址重定位，固定 ImageBase）。
- 所有代码对数据区的引用使用 RIP 相对寻址；最终写文件时回填 `disp32`。
- 入口点 = `.text` 起点，入口桩：

```text
push rbp
mov  rbp, rsp
sub  rsp, 0x20
lea  rax, [rip + heap_base]     ; 写文件时回填
mov  [rip + heap_ptr_slot], rax
lea  rax, [rip + heap_end]      ; heap_base + 32 MiB
mov  [rip + heap_end_slot], rax
call Main::main                 ; rel32，写文件时回填
xor  ecx, ecx
call [rip + ExitProcess]
int3
```

- `Main::main` 的返回值被丢弃，正常结束时进程退出码为 0；需要非 0 退出码时
  必须调用 `__exit`。这与 stage0 的 `mod_wrapper` 行为一致。
- `__exit(code)` 直接 `mov ecx, code; call [rip + ExitProcess]; int3`。
- 如果实现选择在 `__exit` 前刷新 stdio，可增加 `fflush` 导入；该扩展不改变
  ABI，但必须在 `CHANGELOG.md` 记录。

---

## 6. Stage1 编译器内部约定

### 6.1 实现风格

- 单遍 / 流式：词法器按需产生 token，解析器边解析边生成代码；不建 AST。
- 不使用任何动态容器；所有表都是固定容量数组。
- 所有可变状态放在一个 `ptr.int ctx` 指向的堆缓冲区中，所有函数首参都是
  `ctx`。
- 不使用模块级可变变量、`part`、全局数组。
- 只使用第 3 节的语法；源码必须能被自身编译。
- 只使用 `//` 注释；不使用 `else if`、`/* */`、移位、指针算术、`*p`。
- 函数参数不超过 4 个；需要更多参数时拆成多次调用或写入 ctx。
- 所有局部变量显式初始化。
- 不用异常；错误通过返回 0 / 设置 `ctx.err` 传播。
- stage0 在大函数中观察到“循环内条件赋值布尔标志、循环外读取”会被错误保留为
  初值（`-std` 判断曾因此失效）。规避方式：把这类判断放进独立函数并直接
  `return`，不要在热循环里维护布尔标志；`src/bootstrap.wei` 的 `Cli::stdOk`
  即为该模式。该问题不影响语义定义，但会影响 stage0 生成代码，必须用
  `mtg.exe` 实测。

### 6.2 64 位内存读写

stage0 不支持指针算术与移位，因此 stage1 用以下模式读写 ctx / 输出缓冲区：

```wei
// 读 8 字节小端无符号值；addr 为基址，off 为字节偏移
create.mod Mem::load64(ptr.int addr, int off).int {
    create.int v = 0;
    create.int i = 7;
    while (i >= 0) {
        v = v * 256 + __byte_at(addr, off + i);
        i = i - 1;
    }
    return v;
}

// 写 8 字节小端值
create.mod Mem::store64(ptr.int addr, int off, int value) {
    create.int v = value;
    create.int i = 0;
    while (i < 8) {
        __store_byte(addr, off + i, v % 256);
        v = v / 256;
        i = i + 1;
    }
}
```

约束：

- `load64` 只用于非负值（偏移、长度、token 类型、代码字节）。负数必须先
  加偏置或另行编码。
- 有符号除法向零截断；`v % 256` 对非负 `v` 结果在 0..255。
- 写有符号 64 位值时，不要直接用 `% 256` 循环；先转换为无符号位模式或
  只存非负值。

### 6.3 固定上限

| 常量 | 值 | 说明 |
| --- | --- | --- |
| `MAX_SOURCE` | 1 MiB | 源文件字节数 |
| `MAX_CODE` | 4 MiB | 生成代码缓冲区 |
| `MAX_DATA_RAW` | 1 MiB | `.data` 初始化部分（含字符串池） |
| `MAX_STRING_POOL` | 512 KiB | 字符串字面量总字节 |
| `MAX_STRING_LITERAL` | 65535 | 单字面量 |
| `MAX_IDENT` | 128 | 标识符 |
| `MAX_FUNCS` | 512 | 函数/module 成员 |
| `MAX_SYMBOLS` | 1024 | 函数 + 当前作用域局部变量 |
| `MAX_NAMED_LOCALS` | 256 | 单函数命名局部变量 |
| `MAX_FRAME_SLOTS` | 131072 | 单函数帧槽（含数组元素），约 1 MiB |
| `MAX_ARRAY_ELEMS` | 65536 | 单数组 |
| `MAX_PARAMS` | 4 | 单函数参数 |
| `MAX_ARGS` | 4 | 单次调用实参 |
| `MAX_NEST` | 64 | 表达式/块嵌套 |
| `MAX_FIXUPS` | 65536 | 代码/数据回填项 |
| `MAX_PATH` | 4096 | 输入/输出路径 |
| `HEAP_SIZE` | 32 MiB | 生成程序的堆 |
| `MAX_OUTPUT` | 8 MiB | 输出 PE 文件 |

超限一律：设置错误位置，打印 `<file>:<line>:<col>: <message>`，退出码 1。
CLI 用法错误退出码 2。

### 6.4 符号表

每个函数符号至少保存：

- 模块名（源缓冲区偏移 + 长度）；
- 成员名（源缓冲区偏移 + 长度）；
- 代码偏移（函数体起点，写文件时用于 rel32 回填）；
- 参数个数；
- 参数类型（4 个槽）；
- 返回类型；
- 是否已生成代码（用于自递归回填）。

每个局部变量符号至少保存：

- 名字（偏移 + 长度）；
- 槽号；
- 数组长度（0 表示标量）；
- 类型。

名字不复制，直接引用源缓冲区切片；源缓冲区在编译期间不得移动或释放。

### 6.5 代码生成约定

- 表达式结果统一放 `RAX`。
- 二元运算：左操作数存 `temp[depth]`，右操作数求值到 `RAX`，再取左操作数到
  `RCX` 并运算；`depth` 为编译期递归深度。
- 函数调用：实参从左到右求值到 `temp` 槽，再装入 `RCX/RDX/R8/R9`，`call`
  的 rel32 先填 0 并登记回填项。
- 分支：`jcc rel32` 和 `jmp rel32` 先填 0，编译完目标块后回填。
- 数组下标：结果在 `RAX`；`cmp rax, N; jb ok`，否则跳到越界 panic 桩；
  计算地址时使用生成的 `shl rax, 3`（机器码可以移位，源码不行）。
- 字符串：写入字符串池，登记 `lea rax, [rip+disp32]` 回填项。
- 数据引用：登记 `(patch_pos, data_offset)`；写文件时按最终 `.data` RVA
  计算 `disp32 = data_rva + data_offset - (patch_pos + 4)`。
- 所有跳转/调用的回填都在 `.text` 内完成后再计算节布局。

### 6.6 stage0 无参 mod 成员的自动入口

stage0 会把每个**无参** `create.mod` 成员收集进 `__mod_entry`，按 `priority`
升序、同优先级按声明序自动调用；只要存在这种成员，入口就从 `Main::main`
切换到该 wrapper。因此 `src/bootstrap.wei` 中禁止写无参辅助函数：除
`Main::main()` 外，所有函数至少带一个参数（通常首参为 `ptr.int ctx`）。
需要无参逻辑时用哑参数，例如 `Pe::runSelfTest(int mode)`。

---

## 7. CLI 契约

### 7.1 命令形式

```text
bootstrap.exe
bootstrap.exe version | -v | --version
bootstrap.exe build <file.wei> [选项]
```

### 7.2 行为与退出码

| 场景 | 输出 | 退出码 |
| --- | --- | --- |
| 无参数 | stage1 启动横幅 | 0 |
| `version` / `-v` / `--version` | 版本行 | 0 |
| `build` 缺少文件 | `error: subcommand 'build' requires a file` | 2 |
| 未知子命令且缺少文件 | `error: subcommand '<x>' requires a file` | 2 |
| 未知子命令且有文件 | `error: unknown subcommand '<x>'` + 用法 | 2 |
| 源文件不存在/为空 | `<file>:?:?: cannot read source file (missing or empty)` | 1 |
| 源文件超过 1 MiB | `<file>:?:?: source exceeds 1048576 bytes` | 1 |
| 词法/语法/类型/代码生成错误 | `<file>:<line>:<col>: <message>` | 1 |
| 输出写入失败 | `<file>:?:?: cannot write output file: <path>` | 1 |
| 成功 | `[done] <out> (win64)` | 0 |
| `-std:` 值不支持 | `error: unsupported -std value '<v>'` | 2 |
| `-target` 非 `win64` | `error: target '<t>' is not supported` | 2 |
| `-wide` / `-emit-norm:` | `error: option '<opt>' is not supported` | 2 |

### 7.3 选项解析

- 命令行来自 `GetCommandLineA()`；参数按空白（空格/制表符）分隔，双引号可包裹
  含空格的参数。stage1 不实现反斜杠转义与 Windows 的复杂引号规则；路径中若
  含双引号或反斜杠转义，需在后续版本补齐。
- `-o <path>`：指定输出路径；重复出现时最后一个生效。
- `-std:wei27`、`-std:wei`、`-std:default`：等价，接受。
- `-std:zh`、`-std:cn`、`-std:chinese`、`-std:中文`、`-std:wenyan`、
  `-std:classical`、`-std:文言`、`-std:文言文`：报错退出 2。
- `-target win64`：接受；`-target linux64` 或其他值：报错退出 2。
- `-no-opt`、`-ir`、`-no-ir`：接受并忽略（stage1 没有优化器/IR）。
- `-wide`、`-emit-norm:<file>`：报错退出 2（子集不支持）。
- 其他以 `-` 开头的未知选项：与 stage0 一致，忽略。
- 选项从第 3 个参数（索引 3）开始解析；`-o`/`-target` 缺少值视为用法错误，
  退出 2。

### 7.4 默认输出名

与 stage0 `basenameNoExt` 一致：

1. 去掉路径部分（最后一个 `/` 或 `\` 之后）；
2. 去掉最后一个 `.` 及其后缀；
3. 追加 `.exe`。

例：`tests/bootstrap/hello.wei` → 当前目录 `hello.exe`；`a.b.wei` →
`a.b.exe`。

### 7.5 版本与横幅

stage1 使用 ASCII，避免控制台代码页问题：

```text
Module Target Generator 0.02.74282 - bootstrap stage1 (win64)
```

启动横幅：

```text
Module Target Generator - bootstrap stage1
```

stage3 稳定后可以把文案调整回与 stage0 一致；文案不属于自举 fixpoint 的
比较对象。

---

## 8. 验收用例

### 8.1 用例表

`tests/bootstrap/` 下每个 `.wei` 对应一个 `.expected`，可选 `.args` 与
`.exit`。输出按行精确比较（CRLF 归一化为 LF），退出码必须一致。

| 用例 | 参数 | 退出码 | 期望输出 |
| --- | --- | --- | --- |
| `hello` | 无 | 0 | `Hello, bootstrap!` |
| `arith` | 无 | 0 | `10 4 21 2 1 -7 20`（逐行） |
| `control` | 无 | 0 | `107 5 ok`（逐行） |
| `func` | 无 | 0 | `720 10`（逐行） |
| `array` | 无 | 0 | `10 40 100 99 99`（逐行） |
| `string` | 无 | 0 | `abc def`（逐行） |
| `bytes` | 无 | 0 | `65 66`（逐行） |
| `file` | 无 | 0 | `2 2 72 105`（逐行） |
| `cmdline` | `alpha "beta gamma"` | 0 | `3 alpha`（逐行） |
| `exit` | 无 | 7 | `before` |

说明：

- `cmdline` 的 `argc` 包含 `argv[0]`，所以两个实参得到 3。
- `exit` 在 `__exit(7)` 前的输出必须出现，`after` 不得出现。
- `file` 在当前工作目录创建 `bootstrap-file.tmp`，运行器应在临时目录中执行。

### 8.2 运行方式

第 1 天（stage0 语义基线）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/bootstrap/run.ps1
```

第 10 天（stage1 自举验收）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/bootstrap/run.ps1 -Compiler .\bootstrap2.exe
```

运行器要求：

- 编译每个 `.wei`，编译失败即该用例失败；
- 用 `.args` 启动程序，捕获 stdout + stderr；
- 与 `.expected` 归一化比较；
- 检查 `.exit`（缺省 0）；
- 汇总 `passed/failed`，有失败时退出码 1；
- 使用独立临时目录，避免 `file` 用例互相污染。

### 8.3 Fixpoint 验收

```powershell
.\bootstrap.exe build src\bootstrap.wei -o bootstrap2.exe
.\bootstrap2.exe build src\bootstrap.wei -o bootstrap3.exe
Get-FileHash .\bootstrap2.exe -Algorithm SHA256
Get-FileHash .\bootstrap3.exe -Algorithm SHA256
```

要求两个 SHA256 完全相同。若不同，先检查：

- PE 头 `TimeDateStamp` 是否为 0；
- 字符串池/符号表遍历顺序是否稳定；
- 回填顺序是否影响字节；
- 是否把临时路径、时间、随机数写进了输出。

### 8.4 CLI 验收

| 命令 | 期望退出码 |
| --- | --- |
| `bootstrap2.exe` | 0 |
| `bootstrap2.exe --version` | 0 |
| `bootstrap2.exe version` | 0 |
| `bootstrap2.exe -v` | 0 |
| `bootstrap2.exe build` | 2 |
| `bootstrap2.exe nope a.wei` | 2 |
| `bootstrap2.exe build missing.wei` | 1 |
| `bootstrap2.exe build hello.wei` | 0，生成 `hello.exe` |
| `bootstrap2.exe build hello.wei -o out.exe` | 0，生成 `out.exe` |
| `bootstrap2.exe build hello.wei -std:zh` | 2 |
| `bootstrap2.exe build hello.wei -target linux64` | 2 |

第 1 天 CLI 骨架检查：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/bootstrap/cli.ps1 -Compiler .\bootstrap.exe
```

第 10 天完整 CLI 检查：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/bootstrap/cli.ps1 -Compiler .\bootstrap2.exe -RequireBuildSuccess
```

不带 `-RequireBuildSuccess` 时，脚本允许 `build` 以退出码 1 报
`frontend/codegen not implemented yet`，用于 Day 1–7 的增量开发；带该开关后，
`build` 必须成功并实际生成 PE。

---

## 9. 与 stage0 的已知偏差

这些偏差是 10 天版的明确取舍，验收时不要求与 stage0 完全一致：

1. stage1 不做整数溢出检查；溢出回绕。
2. stage1 的除零/越界 panic 文案与 stage0 略有不同，但退出码均为 1。
3. stage1 生成的程序不打印 `runtime warning: memory leaked at exit`。
4. stage1 的 `__rt_free` 是空操作，不实现隔离区与投毒。
5. stage1 CLI 文案为 ASCII，stage0 为中文。
6. stage1 未初始化局部变量清零；stage0 为栈上未定义值。
7. stage1 不支持 `__file_*`、`__rt_panic`、`*p`、指针算术。
8. stage1 生成 PE 固定 ImageBase，不启用 ASLR；stage0 也不启用。
9. stage1 `-std` 只接受 wei27 等价值；中文/文言报错退出 2。
10. stage1 生成的程序默认不主动刷新 stdio；若实测丢输出，再引入
    `fflush` 并记录变更。

---

## 10. 10 天里程碑

| 天 | 目标 | 退出条件 |
| --- | --- | --- |
| 1 | 冻结规格、验收用例、CLI 骨架 | `run.ps1` 10/10；`bootstrap.wei` 可被 stage0 编译 |
| 2 | 64 位读写辅助、词法器 | 能 token 化全部验收源码并报准确行列 |
| 3 | 符号表、声明/语句解析、代码缓冲 | 能解析 `bootstrap.wei` 自身 |
| 4 | 整数/比较/控制流代码生成 | `hello`、`arith`、`control` 通过 |
| 5 | 函数、参数、数组、字符串 | `func`、`array`、`string` 通过 |
| 6 | PE 写出器、IAT、入口桩、重定位回填 | stage1 能生成可运行 PE |
| 7 | 内置函数运行时桩 | `bytes`、`file`、`cmdline`、`exit` 通过 |
| 8 | 用 stage1 编译自身并修复差异 | 产出可运行 `bootstrap2.exe` |
| 9 | fixpoint、全部验收、CLI 验收 | `bootstrap2 == bootstrap3`；10/10 与 CLI 表全过 |
| 10 | 文档、CHANGELOG、清理与发布 | `make all` / `make check` / 验收脚本全绿 |

---

## 11. 变更控制

- 本文是 `bootstrap-subset-v1` 的唯一权威定义。
- 任何语法、ABI、内存布局、CLI 退出码或验收用例的变更，必须同时：
  1. 修改本文并提升规格版本；
  2. 修改 `tests/bootstrap/` 中受影响的用例；
  3. 在 `CHANGELOG.md` 记录；
  4. 确认 stage0 与 stage1 都能接受新语法（若涉及语法）。
- 不允许为了让 stage1 “先跑起来”而静默放宽验收；做不到的能力应明确报错，
  而不是生成错误结果。
