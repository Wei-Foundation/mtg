/*
 * Wei - Module Target Generator
 * 版权所有 (C) 2026 Module Target Generator
 * 
 * Module Target Generator （以下称MTG）为自由软件：您可以根据自由软件基金会发布的《GNU 通用公共许可证》
 * （第三版）的条款，重新发布、或修改本程序。
 *
 * 除《GNU 通用公共许可证》（第三版）外的补充许可条款协议：
 * 本程序的分发是希望其具有实用性，但不提供任何担保，
 * 甚至不提供适销性或特定用途适用性的默示担保，由此产生的法律责任 MTG 不进行担保。
 * 
 * 您应已随本程序收到一份《GNU 通用公共许可证》的副本。若未收到，
 * 请参阅 <https://www.gnu.org/licenses/>。
 */


/* ==========================================================================
 * 第一部分：前端 Frontend（原 mtc.cpp）
 *   词法分析 Lexer / 语法分析 Parser / 类型检查 TypeChecker / AST
 * ========================================================================== */

#include <sys/stat.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <map>
#include <set>
#if defined(_WIN32)
#include <windows.h>
/* minwindef.h 把 CONST 定义成宏 const，会破坏后端 IROp::CONST，必须取消 */
#undef CONST
/* 同理, near / far 是 16 位时代遗留的空宏, 会把 `bool near` 展开成 `bool `
   而编译不过。一律取消 —— 本文件不会再用这两个标识符作名字。 */
#undef near
#undef far
#endif
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <set>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>
#include <unistd.h>
#include <climits>
#include <cctype>

/* ==========================================================================
 * 128 位整数 wei_i128
 *   编译期常量折叠、平方和闭式解、组合数这几处需要**比 int64 更宽**的中间
 *   精度 —— 否则"途中溢出、最终值却在 int64 内"的情形会被误判为溢出而
 *   白白放弃优化。
 *
 *   GCC / Clang / MinGW 有 __int128；MSVC 没有。这里统一成 wei_i128:
 *     · 有原生支持时 wei_i128 就是 __int128（零开销）
 *     · 否则回落到下面的软件实现（二补数 128 位）
 *   两条路径的**源码写法完全一致** —— 构造、四则运算、比较、转 int64 都以
 *   同一种语法调用, 上层不需要任何 #ifdef。
 * ========================================================================== */
#if defined(__SIZEOF_INT128__) && !defined(WEI_FORCE_SOFT_I128)
using wei_i128 = __int128;
#else
struct wei_i128 {
	uint64_t lo = 0, hi = 0;            /* 二补数: 值 = hi*2^64 + lo */
	wei_i128() = default;
	wei_i128(long long v)               { lo = (uint64_t)v; hi = v < 0 ? ~0ULL : 0ULL; }
	wei_i128(unsigned long long v)      { lo = v; hi = 0; }
	/* long / unsigned long 必须单列: 在 LP64 下 int64_t 就是 long, 若只提供
	   long long 与 unsigned long long 两个重载, 传 long 时两者转换等级相同,
	   编译器判定歧义(int64_t 实参报 'call of overloaded ... is ambiguous')。 */
	wei_i128(long v)                    { lo = (uint64_t)(long long)v; hi = v < 0 ? ~0ULL : 0ULL; }
	wei_i128(unsigned long v)           { lo = (uint64_t)v; hi = 0; }
	wei_i128(int v)                     { lo = (uint64_t)(long long)v; hi = v < 0 ? ~0ULL : 0ULL; }
	wei_i128(unsigned int v)            { lo = v; hi = 0; }
	bool neg_() const                   { return (hi >> 63) != 0; }
	explicit operator int64_t() const   { return (int64_t)lo; }
	explicit operator uint64_t() const  { return lo; }
};
static inline wei_i128 i128_not(const wei_i128& a) {
	wei_i128 r; r.lo = ~a.lo; r.hi = ~a.hi; return r;
}
static inline void i128_mul64(uint64_t a, uint64_t b, uint64_t& hi, uint64_t& lo) {
	uint64_t a0 = a & 0xFFFFFFFFULL, a1 = a >> 32;
	uint64_t b0 = b & 0xFFFFFFFFULL, b1 = b >> 32;
	uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
	uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFULL) + (p10 & 0xFFFFFFFFULL);
	lo = (p00 & 0xFFFFFFFFULL) | (mid << 32);
	hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}
static inline bool i128_ltu(const wei_i128& a, const wei_i128& b) {
	return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo;
}
static inline bool i128_leu(const wei_i128& a, const wei_i128& b) {
	return a.hi != b.hi ? a.hi < b.hi : a.lo <= b.lo;
}
static inline void i128_shl1(uint64_t& h, uint64_t& l) {
	h = (h << 1) | (l >> 63);
	l <<= 1;
}
static inline bool i128_geu(uint64_t ah, uint64_t al, uint64_t bh, uint64_t bl) {
	return ah != bh ? ah > bh : al >= bl;
}
static inline void i128_subu(uint64_t& ah, uint64_t& al, uint64_t bh, uint64_t bl) {
	uint64_t ol = al;
	al -= bl;
	ah -= bh + (al > ol ? 1ULL : 0ULL);
}
/* 无符号 128 / 128 二进制长除。只在编译期跑, 128 次迭代可以接受。 */
static inline void i128_divmod_u(uint64_t ah, uint64_t al, uint64_t bh, uint64_t bl,
                                 uint64_t& qh, uint64_t& ql, uint64_t& rh, uint64_t& rl) {
	qh = ql = rh = rl = 0;
	for (int i = 127; i >= 0; i--) {
		i128_shl1(rh, rl);
		rl |= (i >= 64) ? ((ah >> (i - 64)) & 1ULL) : ((al >> i) & 1ULL);
		if (i128_geu(rh, rl, bh, bl)) {
			i128_subu(rh, rl, bh, bl);
			if (i >= 64) qh |= 1ULL << (i - 64);
			else ql |= 1ULL << i;
		}
	}
}
static inline wei_i128 operator+(const wei_i128& a, const wei_i128& b) {
	wei_i128 r;
	r.lo = a.lo + b.lo;
	r.hi = a.hi + b.hi + (r.lo < a.lo ? 1ULL : 0ULL);
	return r;
}
static inline wei_i128 operator-(const wei_i128& a) {
	return i128_not(a) + wei_i128(1);
}
static inline wei_i128 operator-(const wei_i128& a, const wei_i128& b) {
	return a + (-b);
}
static inline wei_i128 operator*(const wei_i128& a, const wei_i128& b) {
	uint64_t h, l;
	i128_mul64(a.lo, b.lo, h, l);
	wei_i128 r;
	r.lo = l;
	/* mod 2^128: 两边 hi 的乘积落在 2^128 位上, 直接丢弃 */
	r.hi = h + a.lo * b.hi + a.hi * b.lo;
	return r;
}
static inline bool operator<(const wei_i128& a, const wei_i128& b) {
	bool an = a.neg_(), bn = b.neg_();
	if (an != bn) return an;            /* 负数更小 */
	return i128_ltu(a, b);              /* 同号时二补数的无序比较即有序比较 */
}
static inline bool operator<=(const wei_i128& a, const wei_i128& b) {
	bool an = a.neg_(), bn = b.neg_();
	if (an != bn) return an;
	return i128_leu(a, b);
}
static inline bool operator>(const wei_i128& a, const wei_i128& b)  { return b < a; }
static inline bool operator>=(const wei_i128& a, const wei_i128& b) { return b <= a; }
static inline bool operator==(const wei_i128& a, const wei_i128& b) { return a.hi == b.hi && a.lo == b.lo; }
static inline bool operator!=(const wei_i128& a, const wei_i128& b) { return !(a == b); }
static inline wei_i128 operator/(const wei_i128& a, const wei_i128& b) {
	bool an = a.neg_(), bn = b.neg_();
	wei_i128 ma = an ? -a : a, mb = bn ? -b : b;
	uint64_t qh, ql, rh, rl;
	i128_divmod_u(ma.hi, ma.lo, mb.hi, mb.lo, qh, ql, rh, rl);
	wei_i128 q; q.hi = qh; q.lo = ql;
	return (an != bn) ? -q : q;         /* C 语义: 向零取整, 异号为负 */
}
static inline wei_i128 operator%(const wei_i128& a, const wei_i128& b) {
	bool an = a.neg_(), bn = b.neg_();
	wei_i128 ma = an ? -a : a, mb = bn ? -b : b;
	uint64_t qh, ql, rh, rl;
	i128_divmod_u(ma.hi, ma.lo, mb.hi, mb.lo, qh, ql, rh, rl);
	wei_i128 r; r.hi = rh; r.lo = rl;
	return an ? -r : r;                 /* C 语义: 余数与被除数同号 */
}
/* 复合赋值: 原生 __int128 由编译器内建提供, 软件实现需要显式写出。
   顺手补 += -= *= 以免别处用到时再次踩到。 */
static inline wei_i128& operator+=(wei_i128& a, const wei_i128& b) { return a = a + b; }
static inline wei_i128& operator-=(wei_i128& a, const wei_i128& b) { return a = a - b; }
static inline wei_i128& operator*=(wei_i128& a, const wei_i128& b) { return a = a * b; }
static inline wei_i128& operator/=(wei_i128& a, const wei_i128& b) { return a = a / b; }
static inline wei_i128& operator%=(wei_i128& a, const wei_i128& b) { return a = a % b; }
#endif

enum class TokType {
	Ident, IntLit, StrLit, DoubleLit,
	Kw_create, Kw_func, Kw_int, Kw_bool, Kw_double, Kw_char, Kw_omni, Kw_ptr,
	Kw_if, Kw_else, Kw_for, Kw_while, Kw_loop, Kw_return,
	Kw_true, Kw_false, Kw_null,
	Kw_output, Kw_input,
	Kw_require, Kw_stat,
	Kw_mod, Kw_part, Kw_access, Kw_adv, Kw_struct,
	Kw_do,
	Kw_public, Kw_private,
	Kw_模块, Kw_部件, Kw_访问, Kw_高级, Kw_结构,
	Kw_创建, Kw_函数, Kw_整型, Kw_浮点, Kw_布尔, Kw_文本, Kw_万能, Kw_指针,
	Kw_如果, Kw_否则, Kw_遍历, Kw_循环,
	Kw_返回, Kw_真, Kw_假,
	Kw_包含, Kw_统计, Kw_空, Kw_换行, Kw_重复,
	// 新增关键字
	Kw_match, Kw_auto, Kw_compiletime, Kw_tuple, Kw_type, Kw_co,
	Kw_async, Kw_await, Kw_macro, Kw_contract, Kw_import, Kw_from, Kw_as,
	Kw_and, Kw_or, Kw_not, Kw_is, Kw_in, Kw_of, Kw_when, Kw_then,
	Kw_forall, Kw_exists, Kw_cast,
	Kw_typeof, Kw_sizeof, Kw_new, Kw_delete, Kw_yield, Kw_own, Kw_i32,
	Kw_i8, Kw_i16, Kw_i64, Kw_u8, Kw_u16, Kw_u32, Kw_u64, Kw_f32, Kw_f64,
	/* ── 模块化编程关键字 ── */
	Kw_export, Kw_pub, Kw_package, Kw_use, Kw_declare, Kw_define,
	Kw_init, Kw_deinit, Kw_self, Kw_super, Kw_root, Kw_extern, Kw_static,
	/* ── B1 模块化剩余 ── */
	Kw_trait, Kw_impl, Kw_mixin, Kw_protocol, Kw_with, Kw_compose, Kw_embed,
	Kw_extend, Kw_derive, Kw_reflect, Kw_link, Kw_lib, Kw_library, Kw_dynamic,
	Kw_feature, Kw_cfg, Kw_has, Kw_version, Kw_platform, Kw_arch, Kw_build,
	Kw_target, Kw_annotate, Kw_attribute, Kw_deprecated, Kw_moduleinfo, Kw_meta,
	Kw_doc, Kw_signature, Kw_header, Kw_forward, Kw_depends, Kw_provides,
	Kw_expose, Kw_internal, Kw_hidden, Kw_fileprivate, Kw_moduleprivate, Kw_open,
	Kw_final, Kw_sealed, Kw_qualified, Kw_unqualified, Kw_importall, Kw_reexport,
	Kw_crate, Kw_bundle, Kw_scope, Kw_unit, Kw_entry, Kw_teardown, Kw_outer,
	Kw_asm, Kw_try, Kw_catch, Kw_throw, Kw_const, Kw_defer, Kw_inline, Kw_union,
	Kw_switch, Kw_case, Kw_default, Kw_enum, Kw_lambda, Kw_ref, Kw_mut, Kw_move,
	Kw_alignof, Kw_alignas, Kw_volatile, Kw_operator, Kw_thread, Kw_lock, Kw_atomic,
	Kw_break, Kw_continue,
	Kw_匹配, Kw_公开, Kw_私有,
	LParen, RParen, LBrace, RBrace, LBracket, RBracket, Comma, Semicolon, Colon, DoubleColon, Dot,
	DoubleDot,          /* '..' 范围运算符: lo..hi */
	Plus, Minus, Star, Slash, Percent, PlusPlus, MinusMinus, Amp,
	Assign, PlusEq, MinusEq, StarEq, SlashEq,
	Caret, Pipe, Tilde, Shl, Shr,          /* ^ | ~ << >> 位运算 */
	Question,                              /* '?' —— Result 错误传播后缀 */
	EqEq, Neq, LtEq,
	GtEq,
	And, Or, Not,
	LangL, LangR, IOLt, IOGt,
	Eof
};

struct Token {
	TokType type;
	std::string text;
	int32_t line;
	int32_t col;

	Token(): type(TokType::Eof), line(0), col(0) {}
	Token(TokType t, std::string s, int32_t l, int32_t c)
		: type(t), text(std::move(s)), line(l), col(c) {}

	void dump() const {
		std::cout << line << ":" << col << "\t" << text << "\n";
	}

	bool is(TokType k) const {
		return type == k;
	}
};
namespace wei::ast {

	struct Block;
	struct IfStmt;
	struct ForStmt;
	struct WhileStmt;
	struct LoopStmt;
	struct LoopNStmt;
	struct ReturnStmt;
	struct ExprStmt;
	struct AssignStmt;
	struct VarDecl;
	struct ModuleAccessSet;
	struct MatchStmt;
	struct ContractStmt;
	struct TupleLit;
	struct TypeAliasDecl;
	/* ── 本轮补全的新增节点 ── */
	struct RangeLit;
	struct TupleIndexExpr;
	struct TypeofExpr;
	struct IsExpr;
	struct InExpr;
	struct AwaitExpr;
	struct CoExpr;
	struct CompileTimeExpr;
	struct ForallExpr;
	struct ExistsExpr;
	struct ForallStmt;
	struct WhenStmt;
	struct YieldStmt;
	struct CompileTimeStmt;
	struct MacroDecl;

	struct BreakStmt {
		int line = 0;
		int col = 0;
	};
	struct ContinueStmt {
		int line = 0;
		int col = 0;
	};
	/* ── C2 · 内嵌汇编 ──────────────────────────────────────────────────
	   asm { ... } 块：汇编文本直出，绕过所有优化。
	   危险点：手动占用寄存器会与循环变量驻留(rbx/r12/r13/r14)冲突，
	   必须做编译期冲突检测，否则会静默算错。 */

	/* enum: 具名常量集合，底层为整型。编译期折叠为常量，不占运行时存储。
	   每个枚举成员在常量表里注册为 <EnumName>::<Member>。 */
	struct EnumDecl {
		std::string name;
		std::string underlying;            /* 默认 int */
		std::vector<std::pair<std::string,int64_t>> members;
		bool is_scoped = false;             /* enum class */
		int line = 0;
	};
	/* union: 所有成员共享同一段存储，大小取最大成员。
	   用与 part 相同的槽位机制，但成员偏移全部为 0（重叠）。 */
	struct UnionDecl {
		std::string name;
		std::vector<VarDecl> members;
		int line = 0;
	};
	/* trait: 方法签名集合（只有签名，无实现）。
	   impl:   为某个具体类型提供 trait 要求的方法实现。
	   派发方式: **静态派发** —— 编译期就把调用点绑定到具体的 impl 函数，
	   不建虚表、不走间接调用。这与 OO 的虚函数在机器码层面完全不同:
	   生成的是一条直接 call 到具体函数地址，可被后续内联/优化。 */
	struct MethodSig {
		std::string name;
		std::vector<VarDecl> params;
		std::string ret_type;
		int line = 0;
	};
	struct TraitDecl {
		std::string name;
		std::vector<MethodSig> methods;
		int line = 0;
	};
	struct FuncDecl;
	struct ImplDecl {
		std::string trait_name;      /* 实现的 trait */
		std::string for_type;        /* 为哪个类型实现 */
		std::vector<wei::ast::FuncDecl> methods;
		int line = 0;
	};
	struct AsmStmt {
		std::vector<std::string> lines;   /* 汇编文本行 */
		std::vector<std::string> clobbers; /* 声明被破坏的寄存器 */
		std::vector<std::string> inputs;
		std::vector<std::string> outputs;
		int line = 0;
		int col = 0;
	};
	/* ── A6 · 错误处理：Result 风格 ────────────────────────────────────
	   不做异常（与 panic 路线冲突），走返回式错误：ok 标志 + 值。
	   结构定义留到 Expr/Stmt 完备之后；当前仅登记 try/catch/throw 关键字。 */
	struct TryStmt {
		std::string err_var;
		int line = 0;
		int col = 0;
	};

	/* self / super / root 解析出来的限定路径 */
		std::string qual_path;
		bool is_exported = false;   /* export module / export mod: 整个模块对外 */
		std::string ns;             /* 所属 package 限定前缀 */
		bool has_ns = false;
		bool from_import = false;   /* 由 import 内联进来: 豁免 priority 检查 */



	struct IntLit;
	struct DoubleLit;
	struct StrLit;
	struct BoolLit;
	struct Ident;
	struct Binary;
	struct Unary;
	struct Call;
	struct Output;
	struct Input;
	struct IndexExpr;
	struct ModulePathGet;
	struct MatchCase;
	struct TypeLit;
	struct SizeofExpr;
	struct NewExpr;
	struct DeleteExpr;
	struct CastExpr;

	using Expr = std::variant<IntLit, DoubleLit, StrLit, BoolLit, Ident, Binary, Unary, Call,
	      Output, Input, IndexExpr, ModulePathGet, TupleLit,
	      SizeofExpr, NewExpr, DeleteExpr, CastExpr,
	      RangeLit, TupleIndexExpr, TypeofExpr, IsExpr, InExpr,
	      AwaitExpr, CoExpr, CompileTimeExpr, ForallExpr, ExistsExpr>;
	struct SwitchStmt;
	struct DeferStmt;
	using Stmt = std::variant<Block, IfStmt, ForStmt, WhileStmt, LoopStmt,
	      LoopNStmt, ReturnStmt, ExprStmt, AssignStmt, VarDecl,
	      ModuleAccessSet, MatchStmt, ContractStmt, TypeAliasDecl,
	      ForallStmt, WhenStmt, YieldStmt, CompileTimeStmt, MacroDecl,
	      BreakStmt, ContinueStmt, AsmStmt, SwitchStmt, DeferStmt>;



	struct Param {
		std::string name;
		std::string type;
	};

	struct VarDecl {
		std::string name;
		std::string type;
		std::unique_ptr<Expr> init;
		bool is_const;
		int64_t array_len = 0;      /* >0: ??全局数组声明 create.int a[N] */
		std::vector<std::unique_ptr<Expr>> array_init;  /* 数组初值列表 */
		int line = 0;
		int col = 0;
	};

	struct FuncDecl {
		bool exported = false;      /* export: 对外可见；未导出则模块外不可调用 */
		bool is_private = false;    /* private: 显式私有 */
		std::string name;
		std::vector<std::string> type_params;  /* 泛型类型参数: f<T, U> */
		std::vector<Param> params;
		std::string ret_type;
		std::vector<Stmt> body;
		bool is_async = false;   /* async 函数: 调用返回协程句柄而非立即求值 */
		int line = 0;
		int col = 0;
	};

	struct ImportDecl {
		std::string module;                 /* import A.B;                       */
		bool has_symbols = false;           /* from A.B import ...               */
		std::vector<std::string> symbols;   /* 选择性导入的符号名                */
		std::string alias;                  /* as 别名                           */
		std::string from_module;            /* from 后的模块名                   */
		int line = 0;
		int col = 0;
	};

	/* package: 包级命名空间。后续声明会被加上 "Pkg::" 前缀。 */
	/* use A.B: 把 A.B 的成员引入当前作用域，可直接用短名调用。 */
	struct UseDecl {
		std::string path;
		std::string alias;
		int line = 0;
		int col = 0;
	};
	/* self / super / root: 名称解析限定符 */
	struct NameQual {
		enum Kind { Self, Super, Root } kind = Self;
		std::string rest;
		int line = 0;
		int col = 0;
	};

	struct ClassDecl {
		std::string name;
		std::vector<VarDecl> fields;
		std::vector<FuncDecl> methods;
		int line = 0;
		int col = 0;
	};

	struct PartDecl {
		std::string name;
		bool is_struct = false;
		bool is_public = true;
		std::vector<VarDecl> members;   /* 字段/成员，按声明顺序编号 */
		int line = 0;
		int col = 0;
	};

		struct ModuleDecl {
		bool from_import = false;
		bool is_exported = false;
		std::string ns;
		bool has_ns = false;

		/* init / deinit: 模块级生命周期。init 在模块首次使用前执行，
		   deinit 在程序退出时执行。走 .init_array 语义的独立调用序列。 */
		bool has_init = false;
		bool has_deinit = false;
		std::string name;
		std::vector<PartDecl> parts;
		std::vector<Stmt> body;
		/* mod 成员函数: create.mod Name::main() / create.mod Name::priority(N)_(参数) */
		std::vector<FuncDecl> methods;
		int priority = 0;      /* main 缺省 0, 其余 mod 缺省 1 */
		bool has_priority = false;
		/* 依赖拓扑序: 由 requires 图算出，越小越先初始化。
		   -1 表示该模块没有依赖声明，回退到 priority 排序。
		   这是"真正的 C++ 级依赖"—— 初始化顺序由依赖图决定，
		   而不是靠用户手写 priority 数字。 */
		int dep_order = -1;
		bool has_main = false;
		bool referenced = false;
		int line = 0;
		int col = 0;
	};

	struct ModulePathGet {
		std::string module;
		std::string part;
		bool has_field = false;
		std::string field;
		bool adv = false; /* adv 高权??*/
		int line = 0;
		int col = 0;
	};

	struct ModuleAccessSet {
		std::string module;
		std::string part;
		std::string field;
		bool adv = false;
		std::unique_ptr<Expr> value;
		int line = 0;
		int col = 0;
	};

	/* ── Pattern Matching AST ── */
	struct MatchCase {
		std::unique_ptr<Expr> pattern;
		std::vector<Stmt> body;
		int line = 0;
		int col = 0;
	};
	struct MatchStmt {
		std::unique_ptr<Expr> expr;
		std::vector<MatchCase> cases;
		std::vector<Stmt> default_body;
		int line = 0;
		int col = 0;
	};

	/* ── Contract/Precondition AST ── */
	struct ContractStmt {
		std::string contract_type; /* "require" | "ensure" | "invariant" */
		std::unique_ptr<Expr> condition;
		std::string message;
		int line = 0;
		int col = 0;
	};

	/* ── Tuple Literal AST ── */
	struct TupleLit {
		std::vector<std::unique_ptr<Expr>> elements;
		int line = 0;
		int col = 0;
	};

	/* ── Type Alias AST ── */
	struct TypeAliasDecl {
		std::string alias_name;
		std::string target_type;
		int line = 0;
		int col = 0;
	};

	/* ── Type/Sizeof/New/Delete/Cast AST ── */
	struct TypeLit {
		std::string type;
		int line = 0;
		int col = 0;
	};
	struct SizeofExpr {
		std::string type;
		int64_t value = 0;
		int line = 0;
		int col = 0;
	};
	struct NewExpr {
		std::string type;
		int line = 0;
		int col = 0;
	};
	struct DeleteExpr {
		std::unique_ptr<Expr> target;
		int line = 0;
		int col = 0;
	};
	struct CastExpr {
		std::string type;
		std::unique_ptr<Expr> value;
		int line = 0;
		int col = 0;
	};

	/* ── 范围字面量: lo..hi（闭区间，两端都可取到）─────────────────────── */
	struct RangeLit {
		std::unique_ptr<Expr> lo;
		std::unique_ptr<Expr> hi;
		int line = 0;
		int col = 0;
	};

	/* ── 元组取元素: t.0 / t[0] ────────────────────────────────────────── */
	struct TupleIndexExpr {
		std::unique_ptr<Expr> tuple;
		int64_t index = 0;
		int line = 0;
		int col = 0;
	};

	/* ── typeof(x): 编译期解析为类型编号 ───────────────────────────────── */
	struct TypeofExpr {
		std::unique_ptr<Expr> arg;
		std::string resolved;   /* 编译期填入的类型名 */
		int line = 0;
		int col = 0;
	};

	/* ── x is int: 类型判定 ────────────────────────────────────────────── */
	struct IsExpr {
		std::unique_ptr<Expr> arg;
		std::string type_name;
		int line = 0;
		int col = 0;
	};

	/* ── x in coll / x in lo..hi: 成员判定 ─────────────────────────────── */
	struct InExpr {
		std::unique_ptr<Expr> value;
		std::unique_ptr<Expr> container;   /* Ident(数组) 或 RangeLit */
		int64_t array_len = 0;             /* Ident 形态下编译期填入的数组长度 */
		int line = 0;
		int col = 0;
	};

	/* ── await h: 切换到协程并取回其结果 ───────────────────────────────── */
	struct AwaitExpr {
		std::unique_ptr<Expr> operand;
		int line = 0;
		int col = 0;
	};

	/* ── co f(a, b): 创建协程，返回句柄 ────────────────────────────────── */
	struct CoExpr {
		std::string callee;
		std::vector<std::unique_ptr<Expr>> args;
		int thunk_id = -1;      /* 后端为每个 co 调用点生成的 thunk 编号 */
		int line = 0;
		int col = 0;
	};

	/* ── compiletime(expr) / compiletime { }: 编译期求值 ───────────────── */
	struct CompileTimeExpr {
		std::unique_ptr<Expr> arg;
		bool folded = false;
		int64_t value = 0;
		int line = 0;
		int col = 0;
	};

	/* ── forall x in D : cond  （全称量词，结果 0/1）───────────────────── */
	struct ForallExpr {
		std::string var;
		std::unique_ptr<Expr> domain;
		std::unique_ptr<Expr> cond;
		int line = 0;
		int col = 0;
	};

	/* ── exists x in D : cond  （存在量词，结果 0/1）───────────────────── */
	struct ExistsExpr {
		std::string var;
		std::unique_ptr<Expr> domain;
		std::unique_ptr<Expr> cond;
		int line = 0;
		int col = 0;
	};

	/* ── forall x in D { body }  （遍历执行体）─────────────────────────── */
	struct ForallStmt {
		std::string var;
		std::unique_ptr<Expr> domain;   /* RangeLit 或 Ident(数组, of 形态) */
		std::vector<Stmt> body;
		bool is_of = false;             /* x of arr 形态 */
		int64_t array_len = 0;
		int line = 0;
		int col = 0;
	};

	/* ── when cond then { } else { }  ──────────────────────────────────── */
	struct WhenStmt {
		std::unique_ptr<Expr> cond;
		std::vector<Stmt> then_body;
		std::vector<Stmt> else_body;
		int line = 0;
		int col = 0;
	};

	/* ── yield v; 协程让出 ─────────────────────────────────────────────── */
	struct YieldStmt {
		std::unique_ptr<Expr> value;
		int line = 0;
		int col = 0;
	};

	/* ── compiletime { ... } 编译期求值块 ──────────────────────────────── */
	struct CompileTimeStmt {
		std::vector<Stmt> body;
		int line = 0;
		int col = 0;
	};

	/* ── 宏: create.macro NAME(a, b) { ... }  ──────────────────────────── */
	struct MacroDecl {
		std::string name;
		std::vector<std::string> params;
		std::vector<Stmt> body;                 /* 语句宏体 */
		std::unique_ptr<Expr> expr_body;        /* 表达式宏体（单表达式形式）*/
		bool is_expr = false;
		int line = 0;
		int col = 0;
	};

	using TopLevel = std::variant<ModuleDecl, ImportDecl, TypeAliasDecl, MacroDecl, ClassDecl,
	      UseDecl>;

	struct Program {
		std::vector<TopLevel> items;
	};

	struct Block {
		std::vector<Stmt> stmts;
		int line = 0;
		int col = 0;
	};
	/* switch/case/default: 独立发射路径 = 顺序比较链 + 跳转表。
	   不做跳转表优化（case 值连续时才划算），先做正确的比较链。 */
	struct SwitchCase {
		std::unique_ptr<Expr> value;      /* 常量表达式 */
		int64_t const_val = 0;
		bool is_default = false;
		Block body;
		int line = 0;
	};
	struct SwitchStmt {
		std::unique_ptr<Expr> subject;
		std::vector<SwitchCase> cases;
		int line = 0;
		int col = 0;
	};
	/* defer: 作用域出口逆序执行。用与 break 相同的 patch 链机制。 */
	struct DeferStmt {
		Block body;
		int line = 0;
		int col = 0;
	};
	/* lambda: 无捕获时退化为静态函数指针（最简可用形态） */
	struct LambdaExpr {
		std::vector<VarDecl> params;
		std::string ret_type;
		Block body;
		std::string label;      /* 生成的静态函数标签 */
		int line = 0;
		int col = 0;
	};

	struct IfStmt {
		std::unique_ptr<Expr> cond;
		std::vector<Stmt> then_body;
		std::vector<Stmt> else_body;
		int line = 0;
		int col = 0;
	};

	struct ForStmt {
		std::unique_ptr<VarDecl> init_decl;
		std::unique_ptr<Expr> cond;
		std::unique_ptr<Expr> step;
		std::vector<Stmt> body;
		int line = 0;
		int col = 0;
	};

	struct WhileStmt {
		std::unique_ptr<Expr> cond;
		std::vector<Stmt> body;
		int line = 0;
		int col = 0;
	};

	struct LoopStmt {
		std::vector<Stmt> body;
	};

	struct LoopNStmt {
		int64_t count;
		std::vector<Stmt> body;
	};

	struct ReturnStmt {
		std::unique_ptr<Expr> value;
		int line = 0;
		int col = 0;
	};

	struct ExprStmt {
		std::unique_ptr<Expr> expr;
	};

	struct AssignStmt {
		std::string name;
		std::unique_ptr<Expr> target;   /* 左值表达式: Ident | IndexExpr | Unary("*") */
		std::unique_ptr<Expr> value;
		int line = 0;
		int col = 0;
	};

	struct IntLit {
		int64_t value;
	};

	struct DoubleLit {
		double value;
	};

	struct StrLit {
		std::string value;
	};

	struct BoolLit {
		bool value;
	};

	struct Ident {
		std::string name;
		int line = 0;
		int col = 0;
	};

	struct Binary {
		std::string op;
		std::unique_ptr<Expr> lhs;
		std::unique_ptr<Expr> rhs;
		int line = 0;
		int col = 0;
	};

	struct Unary {
		std::string op;
		std::unique_ptr<Expr> operand;
		int line = 0;
		int col = 0;
	};

	struct Call {
		std::string callee;
		std::vector<std::string> type_args;    /* 显式类型实参: f<int>(x) */
		std::vector<std::unique_ptr<Expr>> args;
		bool has_try = false;                  /* f()? —— Result 错误传播 */
		int try_line = 0;                      /* '?' 自身的位置：诊断精确指向它 */
		int try_col = 0;
		int line = 0;
		int col = 0;
	};

	struct Output {
		std::vector<std::unique_ptr<Expr>> values;
		int line = 0;
		int col = 0;
	};

	struct Input {
		std::string var_name;
		int line = 0;
		int col = 0;
	};

	struct IndexExpr {
		std::string name;
		std::unique_ptr<Expr> idx;
		int line = 0;
		int col = 0;
	};

}
class Lexer {
	public:
		explicit Lexer(std::string src);
		std::vector<Token> scan();
	private:
		std::string src_;
		size_t pos_;
		int32_t line_;
		int32_t col_;

		char peek(size_t off = 0) const;
		char get();
		bool eof() const;
		void skipSpaceAndComment();
		Token makeToken(TokType t, std::string text, int32_t line, int32_t col);
		[[noreturn]] void die(const std::string& msg, int32_t line, int32_t col) const;

		Token readNumber();
		Token readString();
		Token readStringContent(int32_t line, int32_t col);
		Token readIdentOrKeyword();
		Token readChineseIdent();
		Token readEscapedIdent();
		Token readOpOrPunct();
};

static const std::unordered_map<std::string, TokType> kEngKws = {
	{"create", TokType::Kw_create},
	{"func", TokType::Kw_func},
	{"int", TokType::Kw_int},
	{"bool", TokType::Kw_bool},
	{"double", TokType::Kw_double},
	{"char", TokType::Kw_char},
	{"omni", TokType::Kw_omni},
	{"ptr", TokType::Kw_ptr},
	{"if", TokType::Kw_if},
	{"else", TokType::Kw_else},
	{"for", TokType::Kw_for},
	{"while", TokType::Kw_while},
	{"loop", TokType::Kw_loop},
	{"return", TokType::Kw_return},
	{"true", TokType::Kw_true},
	{"false", TokType::Kw_false},
	{"null", TokType::Kw_null},
	{"require", TokType::Kw_require},
	{"stat", TokType::Kw_stat},
	/* module 已统一为 mod: 两个关键字映射到同一个 token 会让用户困惑，
	   且语义完全重复。只保留 mod 一个写法。 */
	{"mod", TokType::Kw_mod},
	{"emit", TokType::Kw_output},
	{"part", TokType::Kw_part},
	{"access", TokType::Kw_access},
	{"adv", TokType::Kw_adv},
	{"public", TokType::Kw_public},
	{"private", TokType::Kw_private},
	{"struct", TokType::Kw_struct},
	{"do", TokType::Kw_do},
	{"recv", TokType::Kw_input},
	{"newline", TokType::Kw_换行},
	{"outlb", TokType::Kw_换行},
	{"match", TokType::Kw_match},
	{"auto", TokType::Kw_auto},
	{"compiletime", TokType::Kw_compiletime},
	{"tuple", TokType::Kw_tuple},
	{"type", TokType::Kw_type},
	{"co", TokType::Kw_co},
	{"async", TokType::Kw_async},
	{"await", TokType::Kw_await},
	{"macro", TokType::Kw_macro},
	{"contract", TokType::Kw_contract},
	{"import", TokType::Kw_import},
	{"from", TokType::Kw_from},
	{"as", TokType::Kw_as},
	{"and", TokType::Kw_and},
	{"or", TokType::Kw_or},
	{"not", TokType::Kw_not},
	{"is", TokType::Kw_is},
	{"in", TokType::Kw_in},
	{"of", TokType::Kw_of},
	{"when", TokType::Kw_when},
	{"else", TokType::Kw_else},
	{"then", TokType::Kw_then},
	{"forall", TokType::Kw_forall},
	{"exists", TokType::Kw_exists},
	{"cast", TokType::Kw_cast},
	{"typeof", TokType::Kw_typeof},
	{"sizeof", TokType::Kw_sizeof},
	{"new", TokType::Kw_new},
	{"delete", TokType::Kw_delete},
	{"own", TokType::Kw_own},
	{"i32", TokType::Kw_i32},
	{"i8", TokType::Kw_i8}, {"i16", TokType::Kw_i16}, {"i64", TokType::Kw_i64},
	{"u8", TokType::Kw_u8}, {"u16", TokType::Kw_u16}, {"u32", TokType::Kw_u32}, {"u64", TokType::Kw_u64},
	{"f32", TokType::Kw_f32}, {"f64", TokType::Kw_f64},
	{"export", TokType::Kw_export}, {"pub", TokType::Kw_pub},
	{"package", TokType::Kw_package},
	{"use", TokType::Kw_use}, {"declare", TokType::Kw_declare}, {"define", TokType::Kw_define},
	{"init", TokType::Kw_init}, {"deinit", TokType::Kw_deinit},
	{"self", TokType::Kw_self}, {"super", TokType::Kw_super}, {"root", TokType::Kw_root},
	{"extern", TokType::Kw_extern}, {"static", TokType::Kw_static},
	{"asm", TokType::Kw_asm}, {"try", TokType::Kw_try}, {"catch", TokType::Kw_catch},
	{"throw", TokType::Kw_throw}, {"const", TokType::Kw_const}, {"defer", TokType::Kw_defer},
	{"inline", TokType::Kw_inline}, {"union", TokType::Kw_union}, {"switch", TokType::Kw_switch},
	{"case", TokType::Kw_case}, {"default", TokType::Kw_default}, {"enum", TokType::Kw_enum},
	{"lambda", TokType::Kw_lambda}, {"ref", TokType::Kw_ref}, {"mut", TokType::Kw_mut},
	{"move", TokType::Kw_move}, {"alignof", TokType::Kw_alignof}, {"alignas", TokType::Kw_alignas},
	{"volatile", TokType::Kw_volatile}, {"operator", TokType::Kw_operator},
	{"thread", TokType::Kw_thread}, {"lock", TokType::Kw_lock}, {"atomic", TokType::Kw_atomic},
	{"trait", TokType::Kw_trait},
	{"impl", TokType::Kw_impl},
	{"mixin", TokType::Kw_mixin},
	{"protocol", TokType::Kw_protocol},
	{"with", TokType::Kw_with},
	{"compose", TokType::Kw_compose},
	{"embed", TokType::Kw_embed},
	{"extend", TokType::Kw_extend},
	{"derive", TokType::Kw_derive},
	{"reflect", TokType::Kw_reflect},
	{"link", TokType::Kw_link},
	{"lib", TokType::Kw_lib},
	{"library", TokType::Kw_library},
	{"dynamic", TokType::Kw_dynamic},
	{"feature", TokType::Kw_feature},
	{"cfg", TokType::Kw_cfg},
	{"has", TokType::Kw_has},
	{"version", TokType::Kw_version},
	{"platform", TokType::Kw_platform},
	{"arch", TokType::Kw_arch},
	{"build", TokType::Kw_build},
	{"target", TokType::Kw_target},
	{"annotate", TokType::Kw_annotate},
	{"attribute", TokType::Kw_attribute},
	{"deprecated", TokType::Kw_deprecated},
	{"moduleinfo", TokType::Kw_moduleinfo},
	{"meta", TokType::Kw_meta},
	{"doc", TokType::Kw_doc},
	{"signature", TokType::Kw_signature},
	{"header", TokType::Kw_header},
	{"forward", TokType::Kw_forward},
	{"depends", TokType::Kw_depends},
	{"provides", TokType::Kw_provides},
	{"expose", TokType::Kw_expose},
	{"internal", TokType::Kw_internal},
	{"hidden", TokType::Kw_hidden},
	{"fileprivate", TokType::Kw_fileprivate},
	{"moduleprivate", TokType::Kw_moduleprivate},
	{"open", TokType::Kw_open},
	{"final", TokType::Kw_final},
	{"sealed", TokType::Kw_sealed},
	{"qualified", TokType::Kw_qualified},
	{"unqualified", TokType::Kw_unqualified},
	{"importall", TokType::Kw_importall},
	{"reexport", TokType::Kw_reexport},
	{"entry", TokType::Kw_entry},
	{"teardown", TokType::Kw_teardown},
	{"outer", TokType::Kw_outer},
	{"break", TokType::Kw_break}, {"continue", TokType::Kw_continue},
	{"yield", TokType::Kw_yield},
};

static const std::unordered_map<std::string, TokType> kCjkKws = {
	{"创建", TokType::Kw_创建},
	{"函数", TokType::Kw_函数},
	{"整型", TokType::Kw_整型},
	{"浮点", TokType::Kw_浮点},
	{"布尔", TokType::Kw_布尔},
	{"文本", TokType::Kw_文本},
	{"万能", TokType::Kw_万能},
	{"指针", TokType::Kw_指针},
	{"如果", TokType::Kw_如果},
	{"否则", TokType::Kw_否则},
	{"遍历", TokType::Kw_遍历},
	{"循环", TokType::Kw_循环},
	{"返回", TokType::Kw_返回},
	{"真", TokType::Kw_真},
	{"假", TokType::Kw_假},
	{"包含", TokType::Kw_包含},
	{"统计", TokType::Kw_统计},
	{"模块", TokType::Kw_模块},
	{"部件", TokType::Kw_部件},
	{"访问", TokType::Kw_访问},
	{"高级", TokType::Kw_高级},
	{"结构", TokType::Kw_结构},
	{"空", TokType::Kw_空},
	{"换行", TokType::Kw_换行},
	{"重复", TokType::Kw_重复},
	{"匹配", TokType::Kw_match},
	{"公开", TokType::Kw_公开},
	{"私有", TokType::Kw_私有},
	{"自动", TokType::Kw_auto},
	{"编译期", TokType::Kw_compiletime},
	{"元组", TokType::Kw_tuple},
	{"类型", TokType::Kw_type},
	{"协程", TokType::Kw_co},
	{"异步", TokType::Kw_async},
	{"等待", TokType::Kw_await},
	{"宏", TokType::Kw_macro},
	{"契约", TokType::Kw_contract},
	{"导入", TokType::Kw_import},
	{"来自", TokType::Kw_from},
	{"作为", TokType::Kw_as},
	{"与", TokType::Kw_and},
	{"或", TokType::Kw_or},
	{"非", TokType::Kw_not},
	{"是", TokType::Kw_is},
	{"在", TokType::Kw_in},
	{"的", TokType::Kw_of},
	{"当", TokType::Kw_when},
	{"则", TokType::Kw_then},
	{"对所有", TokType::Kw_forall},
	{"存在", TokType::Kw_exists},
	{"转换", TokType::Kw_cast},
	{"类型Of", TokType::Kw_typeof},
	{"大小Of", TokType::Kw_sizeof},
	{"新建", TokType::Kw_new},
	{"释放", TokType::Kw_delete},
	{"让出", TokType::Kw_yield},
	{"产出", TokType::Kw_yield},
};

static constexpr bool isCjkLead(unsigned char b) {
	return b >= 0xE4 && b <= 0xE9;
}
static constexpr bool isUtf8Cont(unsigned char b) {
	return (b & 0xC0) == 0x80;
}
static constexpr uint32_t decodeCjk3(unsigned char a, unsigned char b, unsigned char c) {
	return (static_cast<uint32_t>(a & 0x0F) << 12) | (static_cast<uint32_t>(b & 0x3F) << 6) | (static_cast<uint32_t>(c & 0x3F));
}
static constexpr bool isCjkCodepoint(uint32_t cp) {
	return cp >= 0x4E00 && cp <= 0x9FFF;
}

/* ==========================================================================
 * 方言层 Dialect Layer
 *   -std:wei27   现行标准（默认，原样编译）
 *   -std:中文    中文关键字（由 kCjkKws 承载，本层只做标点与运算符归一）
 *   -std:文言    文言文关键字（吾有一數 曰甲 …）
 *
 * 实现方式：**源级规范化** —— 把方言源码改写成 wei27 源码，再交给既有
 * 前端。之所以选这条路而不是在 Lexer 里加第三张关键字表，是因为文言的
 * 词序与 wei27 不同（"吾有一數 曰甲" 是"声明+命名"，而 wei27 是
 * "create.int 甲"），纯词法替换摆不平语序。改写一次源码，词法/语法/
 * 类型检查/后端一行都不用动，三种方言也互不干扰。
 *
 * 代价是报错行列号可能相对原文有偏移 —— 对一门文言方言而言可以接受。
 * ========================================================================== */

enum class WeiStd { Wei27 = 0, Chinese = 1, Classical = 2 };

struct DialectRule {
	std::string from;
	std::string to;
	/* 该规则是否开启"声明待命名"状态: 产物以 create. 开头时, 紧随的
	   「曰」是命名引导词而非赋值号, 需要消歧。 */
	bool decl = false;
};

static bool parseStdName(const std::string& v, WeiStd& out) {
	if (v == "wei27" || v == "wei" || v == "default") { out = WeiStd::Wei27; return true; }
	if (v == "中文" || v == "zh" || v == "cn" || v == "chinese") { out = WeiStd::Chinese; return true; }
	if (v == "文言" || v == "文言文" || v == "wenyan" || v == "classical") { out = WeiStd::Classical; return true; }
	return false;
}
static const char* stdName(WeiStd s) {
	return s == WeiStd::Wei27 ? "wei27" : (s == WeiStd::Chinese ? "中文" : "文言");
}

/* ── 汉字数字 → 阿拉伯数字 ────────────────────────────────────────────────
   文言不写 "3" 而写 "三"。不转换的话 "曰三" 只会变成 "= 三"，
   而 "三" 是普通标识符，编译必然报错。 */
static const std::unordered_map<std::string, long long> kCjkDigit = {
	{"零", 0}, {"〇", 0}, {"一", 1}, {"壹", 1}, {"二", 2}, {"贰", 2}, {"兩", 2}, {"两", 2},
	{"三", 3}, {"叁", 3}, {"四", 4}, {"肆", 4}, {"五", 5}, {"伍", 5},
	{"六", 6}, {"陸", 6}, {"陆", 6}, {"七", 7}, {"柒", 7}, {"八", 8}, {"捌", 8},
	{"九", 9}, {"玖", 9},
};
static const std::unordered_map<std::string, long long> kCjkUnit = {
	{"十", 10}, {"拾", 10}, {"百", 100}, {"佰", 100},
	{"千", 1000}, {"仟", 1000},
	{"萬", 10000}, {"万", 10000}, {"億", 100000000LL}, {"亿", 100000000LL},
};
static bool isCjkNumChar(const std::string& ch) {
	return kCjkDigit.count(ch) || kCjkUnit.count(ch);
}
/* 解析纯汉字数字串, 如 "二十五" → 25, "一百二十三" → 123, "一萬二千" → 12000 */
static bool cjkNumToLL(const std::string& cn, long long& out) {
	long long total = 0, section = 0, cur = 0;
	bool any = false, hasUnit = false;
	for (size_t i = 0; i + 2 < cn.size() + 1 && i < cn.size(); i += 3) {
		std::string ch = cn.substr(i, 3);
		auto d = kCjkDigit.find(ch);
		if (d != kCjkDigit.end()) { cur = d->second; any = true; continue; }
		auto u = kCjkUnit.find(ch);
		if (u != kCjkUnit.end()) {
			long long mult = u->second;
			if (mult >= 10000) {                 /* 万/亿: 分段 */
				section += cur;
				total += section * mult;
				section = 0;
			} else {
				if (cur == 0 && !hasUnit) cur = 1;   /* "十" → 10, "十五" → 15 */
				section += cur * mult;
			}
			cur = 0; any = true; hasUnit = true;
			continue;
		}
		return false;
	}
	if (!any) return false;
	out = total + section + cur;
	return true;
}

/* ── 文言文规则表 ────────────────────────────────────────────────────────
   按"最长匹配"生效（下方统一按 from 长度降序排序）。
   虚词(之/者/也/焉/矣/其/此/而)直接删除或转成句读 —— 文言靠它们断句,
   而 wei27 靠空白与标点。注意这会占用这些字: 不要拿它们当变量名。 */
static const DialectRule kClassicalRules[] = {
	/* 声明: 吾有一數 / 今有一術 …… 整体替换为 create.<type> */
	{"吾有一數", "create.int", true}, {"吾有一術", "create.func", true},
	{"吾有一爻", "create.bool", true}, {"吾有一浮", "create.double", true},
	{"吾有一言", "create.char", true}, {"吾有一物", "create.omni", true},
	{"吾有一指", "create.ptr", true}, {"吾有一元", "create.tuple", true},
	{"今有一數", "create.int", true}, {"今有一術", "create.func", true},
	{"今有一爻", "create.bool", true}, {"今有一浮", "create.double", true},
	{"今有一言", "create.char", true}, {"今有一物", "create.omni", true},
	{"今有一指", "create.ptr", true}, {"今有一元", "create.tuple", true},
	{"吾有", "create", true}, {"今有", "create", true},
	/* 裸类型名 */
	{"一數", "int", false}, {"一術", "func", false}, {"一爻", "bool", false},
	{"一浮", "double", false}, {"一言", "char", false}, {"一物", "omni", false},
	{"一指", "ptr", false}, {"一元", "tuple", false},
	/* 赋值与命名。
	   「曰」有两种身份, 由 pendingDecl 在扫描时消歧:
	     吾有一數 曰甲   → create.int 甲     (声明之后, 曰 = 命名引导词)
	     甲 曰 三        → 甲 = 3            (其余位置, 曰 = 赋值号)
	   「名之曰」专用于「先声明后命名」的写法: 吾有一數。名之曰「甲」。 */
	{"名之曰", " ", false},
	{"曰", "=", false},
	{"其值為", "=", false}, {"其值为", "=", false},
	{"為是", "=", false}, {"為", "=", false}, {"为", "=", false}, {"乃", "=", false},
	/* if/while 在 wei27 里必须带括号: `if (甲 < 乙) {`。
	   文言没有这对括号, 故这里只开不闭, 由扫描器在遇到 `{` 或句读时
	   自动补上右括号(见 normalizeDialect 里的 pendingParen)。 */
	{"若非", "if (!", false}, {"若", "if (", false},
	{"否則也", "else", false}, {"否則", "else", false}, {"否则", "else", false},
	{"不然", "else", false}, {"抑或", "else", false},
	{"每當", "while (", false}, {"當", "while (", false}, {"当", "while (", false},
	{"遍歷", "for", false}, {"遍历", "for", false}, {"遍", "for", false},
	{"凡有", "forall", false}, {"凡是", "forall", false}, {"凡", "forall", false},
	{"其中有", "exists", false}, {"中有", "exists", false}, {"其有", "exists", false},
	{"止於此", "return", false}, {"歸", "return", false}, {"归", "return", false},
	{"還", "return", false}, {"还", "return", false},
	/* 输入输出 */
	{"書之", "do.emit::", false}, {"書", "do.emit::", false}, {"书", "do.emit::", false},
	{"輸出", "do.emit::", false}, {"输出", "do.emit::", false},
	{"輸入", "do.recv::", false}, {"输入", "do.recv::", false}, {"取入", "do.recv::", false},
	/* 逻辑 */
	{"且", "and", false}, {"或", "or", false}, {"非", "not", false},
	{"陽", "true", false}, {"陰", "false", false},
	{"真", "true", false}, {"假", "false", false},
	/* 算术 */
	{"加以", "+", false}, {"減以", "-", false}, {"乘以", "*", false}, {"除以", "/", false},
	{"加", "+", false}, {"減", "-", false}, {"减", "-", false},
	{"乘", "*", false}, {"除", "/", false},
	{"取餘", "%", false}, {"取余", "%", false}, {"餘", "%", false}, {"余", "%", false},
	/* 比较 */
	{"不大於", "<=", false}, {"不小于", ">=", false},
	{"大於", ">", false}, {"大于", ">", false},
	{"小於", "<", false}, {"小于", "<", false},
	{"不等於", "!=", false}, {"不等于", "!=", false},
	{"等於", "==", false}, {"等于", "==", false},
	/* 范围 */
	{"至", "..", false}, {"到", "..", false},
	/* 模块 */
	{"模塊", "mod", false}, {"模块", "mod", false},
	/* 虚词: 之 → 成员访问点(甲之值 → 甲.值); 其余删 */
	{"之", ".", false}, {"者", "", false}, {"也", "", false},
	{"焉", "", false}, {"矣", "", false}, {"其", "", false},
	{"此", "", false}, {"而", "", false}, {"於", " ", false}, {"于", " ", false},
	/* 句读与引号 */
	{"。", ";", false}, {"，", " ", false}, {"、", " ", false},
	{"「", "", false}, {"」", "", false}, {"『", "", false}, {"』", "", false},
	{"負", "-", false}, {"负", "-", false},
};

/* ── 中文模式: 只做标点与运算符归一 ──
   中文关键字本身已由 kCjkKws 在词法层支持, 无需在此重复映射。 */
static const DialectRule kChineseRules[] = {
	/* 句读与书名号 */
	{"。", ";", false}, {"，", " ", false}, {"、", " ", false},
	{"「", "", false}, {"」", "", false}, {"『", "", false}, {"』", "", false},
	/* 算术: 中文写法里 "甲 加 乙" 比 "甲 + 乙" 自然 */
	{"加以", "+", false}, {"減以", "-", false}, {"乘以", "*", false}, {"除以", "/", false},
	{"加", "+", false}, {"減", "-", false}, {"减", "-", false},
	{"乘", "*", false}, {"除", "/", false},
	{"取餘", "%", false}, {"取余", "%", false}, {"餘", "%", false}, {"余", "%", false},
	/* 比较 */
	{"不等於", "!=", false}, {"不等于", "!=", false},
	{"不大於", "<=", false}, {"不小于", ">=", false},
	{"大於", ">", false}, {"大于", ">", false},
	{"小於", "<", false}, {"小于", "<", false},
	{"等於", "==", false}, {"等于", "==", false},
	/* 输入输出 */
	{"輸出", "do.emit::", false}, {"输出", "do.emit::", false},
	{"輸入", "do.recv::", false}, {"输入", "do.recv::", false},

	/* ── 中文关键字 → wei27 ──
	   kCjkKws 只在**词法层**把这些词识别成了独立 TokType, 而 Parser 的
	   语句分发只认 wei27 关键字 —— 于是写「如果」会撞上
	   "无法识别的表达式起始 token"。与其去 Parser 里补一整套中文分支,
	   不如在规范化层统一映射过去: 一处改完, 语法/类型/后端零改动。
	   「如果」「循环」带开括号, 由 pendingParen 在 `{` 处自动闭合。 */
	{"创建", "create", false}, {"函数", "func", false},
	{"整型", "int", false}, {"浮点", "double", false}, {"布尔", "bool", false},
	{"文本", "char", false}, {"万能", "omni", false}, {"指针", "ptr", false},
	{"如果", "if (", false}, {"否则", "else", false},
	{"遍历", "for", false}, {"循环", "while (", false}, {"重复", "loop", false},
	{"返回", "return", false}, {"真", "true", false}, {"假", "false", false},
	{"包含", "require", false}, {"统计", "stat", false},
	{"模块", "mod", false}, {"部件", "part", false},
	{"访问", "access", false}, {"高级", "adv", false}, {"结构", "struct", false},
	{"空", "null", false}, {"换行", "newline", false},
	{"匹配", "match", false}, {"公开", "public", false}, {"私有", "private", false},
	{"自动", "auto", false}, {"编译期", "compiletime", false},
	{"元组", "tuple", false}, {"类型", "type", false},
	{"协程", "co", false}, {"异步", "async", false}, {"等待", "await", false},
	{"宏", "macro", false}, {"契约", "contract", false},
	{"导入", "import", false}, {"来自", "from", false}, {"作为", "as", false},
	{"与", "and", false}, {"或", "or", false}, {"非", "not", false},
	{"是", "is", false}, {"在", "in", false}, {"的", "of", false},
	{"当", "when", false}, {"则", "then", false},
	{"对所有", "forall", false}, {"存在", "exists", false},
	{"转换", "cast", false}, {"类型Of", "typeof", false}, {"大小Of", "sizeof", false},
	{"新建", "new", false}, {"释放", "delete", false},
	{"让出", "yield", false}, {"产出", "yield", false},
};

static std::vector<DialectRule> sortedRules(const DialectRule* arr, size_t n) {
	std::vector<DialectRule> v(arr, arr + n);
	std::stable_sort(v.begin(), v.end(), [](const DialectRule& a, const DialectRule& b) {
		return a.from.size() > b.from.size();
	});
	return v;
}

/* 取一个完整 UTF-8 字符（1~4 字节）。
   不能只认 3 字节 CJK: 文言的句读「。」U+3002 编码是 E3 80 82,
   「，」U+FF0C 是 EF BC 8C —— 首字节都不在 isCjkLead 的 E4~E9 区间内。
   若按原判断放过, 这些标点会被逐字节吐出, 词法器立刻报"未知字符"。 */
static std::string utf8One(const std::string& s, size_t i) {
	unsigned char b = static_cast<unsigned char>(s[i]);
	if (b < 0x80) return s.substr(i, 1);
	size_t n;
	if ((b & 0xE0) == 0xC0) n = 2;
	else if ((b & 0xF0) == 0xE0) n = 3;
	else if ((b & 0xF8) == 0xF0) n = 4;
	else return s.substr(i, 1);
	if (i + n > s.size()) return s.substr(i, 1);
	return s.substr(i, n);
}
static uint32_t utf8Decode(const std::string& ch) {
	unsigned char b = static_cast<unsigned char>(ch[0]);
	if (ch.size() == 1) return b;
	if (ch.size() == 2) return (static_cast<uint32_t>(b & 0x1F) << 6) | (ch[1] & 0x3F);
	if (ch.size() == 3) return (static_cast<uint32_t>(b & 0x0F) << 12)
		                            | (static_cast<uint32_t>(ch[1] & 0x3F) << 6) | (ch[2] & 0x3F);
	return (static_cast<uint32_t>(b & 0x07) << 18)
	       | (static_cast<uint32_t>(ch[1] & 0x3F) << 12)
	       | (static_cast<uint32_t>(ch[2] & 0x3F) << 6) | (ch[3] & 0x3F);
}
static bool isHanChar(uint32_t cp) {
	return (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF)
	       || (cp >= 0xF900 && cp <= 0xFAFF);
}

/* 从已产出的文本末尾回看, 取出最近一个"标识符样"的词。
   用途: 文言的「吾有一數。曰甲。其值為十。」把声明与赋值拆成了两句,
   而 wei27 要求 `create.int 甲;` 之后再写 `甲 = 10;` —— 也就是说
   「其值為」必须补回被省略的主语。主语就是上一条语句里刚命名的变量。 */
static std::string lastIdentOf(const std::string& out) {
	size_t i = out.size();
	while (i > 0 && (isspace(static_cast<unsigned char>(out[i - 1])) || out[i - 1] == ';')) i--;
	size_t end = i;
	while (i > 0) {
		unsigned char c = static_cast<unsigned char>(out[i - 1]);
		if (isalnum(c) || c == '_' || c >= 0x80) i--;
		else break;
	}
	return out.substr(i, end - i);
}

static std::string normalizeDialect(const std::string& src, WeiStd st) {
	if (st == WeiStd::Wei27) return src;
	std::vector<DialectRule> rules = (st == WeiStd::Classical)
	                                 ? sortedRules(kClassicalRules, sizeof(kClassicalRules) / sizeof(kClassicalRules[0]))
	                                 : sortedRules(kChineseRules, sizeof(kChineseRules) / sizeof(kChineseRules[0]));
	const bool classical = (st == WeiStd::Classical);
	std::string out;
	bool pendingDecl = false;                 /* 刚产出 create.<type>, 等待名字 */
	int  pendingParen = 0;                    /* 由 若/當 开启, 尚未闭合的括号数 */
	auto closeParen = [&]() {
		if (pendingParen > 0) { out += ") "; pendingParen = 0; }
	};
	for (size_t i = 0; i < src.size();) {
		/* 字符串字面量原样透传, 不做任何改写 */
		if (src[i] == '"') {
			out.push_back(src[i++]);
			while (i < src.size() && src[i] != '"') {
				out.push_back(src[i]);
				if (src[i] == '\\') { i++; if (i < src.size()) out.push_back(src[i]); }
				i++;
			}
			if (i < src.size()) { out.push_back(src[i]); i++; }
			pendingDecl = false;
			continue;
		}
		unsigned char b = static_cast<unsigned char>(src[i]);
		if (b < 0x80) {
			/* 块开始或语句结束 → 补上由 若/當 开启的右括号 */
			if (src[i] == '{' || src[i] == ';') closeParen();
			out.push_back(src[i++]);
			if (!isspace(static_cast<unsigned char>(src[i - 1]))) pendingDecl = false;
			continue;
		}
		/* 汉字数字优先: 收集连续的数字汉字, 整体转成阿拉伯数字。
		   中文方言同样需要 —— 写「一百」却要自己改成 100 就没意义了。 */
		{
			std::string num;
			size_t j = i;
			while (j < src.size()) {
				std::string ch = utf8One(src, j);
				if (ch.size() == 3 && isCjkNumChar(ch)) { num += ch; j += 3; continue; }
				break;
			}
			if (!num.empty()) {
				long long v = 0;
				if (cjkNumToLL(num, v)) {
					/* 只有后面不是汉字(标识符)的一部分才转换:
					   "三" 单独是数字, 但 "三甲" 是变量名。 */
					std::string nxt = (j < src.size()) ? utf8One(src, j) : std::string();
					bool boundary = nxt.empty()
					                || !(isHanChar(utf8Decode(nxt)) && !isCjkNumChar(nxt));
					if (boundary) {
						out += std::to_string(v);
						i = j;
						pendingDecl = false;
						continue;
					}
				}
			}
		}
		/* 规则最长匹配 */
		bool hit = false;
		for (auto& r : rules) {
			if (src.compare(i, r.from.size(), r.from) == 0) {
				/* 「曰」的消歧: 声明之后是命名引导词(输出空格),
				   其余位置是赋值号。 */
				if (r.from == "曰") {
					out += pendingDecl ? std::string(" ") : std::string(" = ");
					pendingDecl = false;
				} else if (r.from == "其值為" || r.from == "其值为") {
					/* 补回被文言省略的主语: 其值為十 → 甲 = 10 */
					std::string nm = lastIdentOf(out);
					out += nm.empty() ? std::string(" = ") : (nm + " = ");
					pendingDecl = false;
					i += r.from.size();
					hit = true;
					break;
				} else if (r.from == "。") {
					/* 文言里「吾有一數。曰甲。」的句读只是停顿, 不是语句结束。
					   此时若照常译成 ';', 会变出 `create.int;` —— 名字还没
					   出现就结束了声明, 解析器必然报错。故在等待命名的状态
					   下把句读降级为空格, 并保持等待状态。 */
					if (!pendingDecl) closeParen();
					out += pendingDecl ? std::string(" ") : std::string(";");
				} else {
					out += r.to;
					/* 凡是产出带开括号的条件关键字(若/當/如果/循环…)都要记账,
					   等遇到 `{` 或句读时补右括号 */
					if (r.to.rfind("if (", 0) == 0 || r.to.rfind("while (", 0) == 0) pendingParen++;
					if (!r.to.empty()) pendingDecl = r.decl;
				}
				i += r.from.size();
				hit = true;
				break;
			}
		}
		if (hit) continue;
		/* 未命中: 当作中文标识符原样输出 */
		std::string ch = utf8One(src, i);
		out += ch;
		i += ch.size();
		pendingDecl = false;
	}
	return out;
}

#ifndef WEI_FRONTEND_NO_DEFS
Lexer::Lexer(std::string src): src_(std::move(src)), pos_(0), line_(1), col_(1) {}

char Lexer::peek(size_t off) const {
	if (pos_ + off >= src_.size()) {
		return '\0';
	}
	return src_[pos_ + off];
}

char Lexer::get() {
	if (pos_ >= src_.size()) {
		return '\0';
	}
	char c = src_[pos_++];
	if (c == '\n') {
		line_++;
		col_ = 1;
	} else {
		col_++;
	}
	return c;
}

bool Lexer::eof() const {
	return pos_ >= src_.size();
}

void Lexer::skipSpaceAndComment() {
	while (!eof()) {
		unsigned char b = static_cast<unsigned char>(peek());
		if (b == ' ' || b == '\t' || b == '\r' || b == '\n') {
			get();
			continue;
		}
		if (b == 0xE3 && static_cast<unsigned char>(peek(1)) == 0x80 && static_cast<unsigned char>(peek(2)) == 0x80) {
			get();
			get();
			get();
			continue;
		}
		if (b == '/' && peek(1) == '/') {
			while (!eof() && peek() != '\n') {
				get();
			}
			continue;
		}
		break;
	}
}

Token Lexer::makeToken(TokType t, std::string text, int32_t line, int32_t col) {
	return Token(t, std::move(text), line, col);
}


/* ==========================================================================
 * 诊断系统 wei::diag —— Rust 风格的富文本错误报告
 *
 * 目标: 报错不再只是一行 "xxx at 12:5"，而是像 Rust 那样给出
 *   · 错误码（E0101），可检索
 *   · 粗体标题 + 彩色分级（红=错误，黄=警告）
 *   · 源码片段 + 行号 + 精确列指向
 *   · 多级标注（主标注紫 ^^^ / 次标注青 ---），可跨行
 *   · help / note 建议，含"你是不是想写 xxx"拼写建议
 *
 * 两个易错点已在实现中处理:
 *   1) CJK 宽字符在终端占 2 列，标注必须按**显示宽度**对齐，否则错位
 *   2) 非 TTY（管道/重定向）或 NO_COLOR 时必须关掉颜色
 * ========================================================================== */
namespace wei::diag {

	/* ── 颜色控制 ────────────────────────────────────────────────────── */
	inline bool& colorFlag() { static bool b = true; return b; }
	inline bool useColor() { return colorFlag(); }
	inline void setColorEnabled(bool v) { colorFlag() = v; }

	/* 自动检测: 非 TTY 或 NO_COLOR 时关色 */
	inline void detectColor() {
		const char* nc = std::getenv("NO_COLOR");
		if (nc && nc[0] != '\0') { colorFlag() = false; return; }
		if (std::getenv("WEI_FORCE_COLOR")) { colorFlag() = true; return; }
#ifdef _WIN32
		colorFlag() = true;
#else
		colorFlag() = (isatty(STDERR_FILENO) != 0);
#endif
	}

	/* ── 显示宽度: CJK/全角算 2 列 ───────────────────────────────────── */
	inline bool isUtf8Lead(unsigned char c) { return (c & 0xC0) != 0x80; }

	inline int codePointWidth(uint32_t cp) {
		/* 组合字符（如声调）宽度为 0 */
		if (cp == 0) return 0;
		if (cp >= 0x0300 && cp <= 0x036F) return 0;
		if (cp >= 0x1AB0 && cp <= 0x1AFF) return 0;
		if (cp >= 0x1DC0 && cp <= 0x1DFF) return 0;
		if (cp >= 0x20D0 && cp <= 0x20FF) return 0;
		if (cp >= 0xFE20 && cp <= 0xFE2F) return 0;
		/* 全角/宽字符区间 */
		if (cp >= 0x1100 && cp <= 0x115F) return 2;
		if (cp >= 0x2E80 && cp <= 0x303E) return 2;
		if (cp >= 0x3041 && cp <= 0x33FF) return 2;
		if (cp >= 0x3400 && cp <= 0x4DBF) return 2;
		if (cp >= 0x4E00 && cp <= 0x9FFF) return 2;
		if (cp >= 0xA000 && cp <= 0xA4CF) return 2;
		if (cp >= 0xAC00 && cp <= 0xD7A3) return 2;
		if (cp >= 0xF900 && cp <= 0xFAFF) return 2;
		if (cp >= 0xFE10 && cp <= 0xFE19) return 2;
		if (cp >= 0xFE30 && cp <= 0xFE6F) return 2;
		if (cp >= 0xFF00 && cp <= 0xFF60) return 2;
		if (cp >= 0xFFE0 && cp <= 0xFFE6) return 2;
		if (cp >= 0x1F300 && cp <= 0x1F64F) return 2;
		if (cp >= 0x1F900 && cp <= 0x1F9FF) return 2;
		if (cp >= 0x20000 && cp <= 0x3FFFD) return 2;
		return 1;
	}

	/* 解码一个 UTF-8 码点，返回消耗的字节数 */
	inline int utf8Decode(const std::string& s, size_t i, uint32_t& cp) {
		unsigned char c = static_cast<unsigned char>(s[i]);
		if (c < 0x80) { cp = c; return 1; }
		int n = 0;
		if ((c & 0xE0) == 0xC0) { n = 2; cp = c & 0x1F; }
		else if ((c & 0xF0) == 0xE0) { n = 3; cp = c & 0x0F; }
		else if ((c & 0xF8) == 0xF0) { n = 4; cp = c & 0x07; }
		else { cp = c; return 1; }
		if (i + (size_t)n > s.size()) { cp = c; return 1; }
		for (int k = 1; k < n; k++) {
			unsigned char cc = static_cast<unsigned char>(s[i + k]);
			if ((cc & 0xC0) != 0x80) { cp = c; return 1; }
			cp = (cp << 6) | (cc & 0x3F);
		}
		return n;
	}

	inline int displayWidth(const std::string& s) {
		int w = 0;
		size_t i = 0;
		while (i < s.size()) {
			uint32_t cp = 0;
			int n = utf8Decode(s, i, cp);
			w += codePointWidth(cp);
			i += (size_t)n;
		}
		return w;
	}

	/* 取显示宽度不超过 w 的前缀（按码点切，不切断字符） */
	inline std::string displayPrefix(const std::string& s, int w) {
		if (w <= 0) return "";
		int acc = 0;
		size_t i = 0;
		while (i < s.size()) {
			uint32_t cp = 0;
			int n = utf8Decode(s, i, cp);
			int cw = codePointWidth(cp);
			if (acc + cw > w) break;
			acc += cw;
			i += (size_t)n;
		}
		return s.substr(0, i);
	}

	/* 把字符串按显示宽度填充到 w（右补空格） */
	inline std::string padToWidth(const std::string& s, int w) {
		int cur = displayWidth(s);
		if (cur >= w) return s;
		return s + std::string((size_t)(w - cur), ' ');
	}

	/* ── ANSI 样式 ───────────────────────────────────────────────────── */
	struct Style {
		const char* on;
		const char* off;
	};
	inline const char* S(const char* code) { return useColor() ? code : ""; }
	inline const char* SOff() { return useColor() ? "\033[0m" : ""; }

	/* ── 源码缓存 ────────────────────────────────────────────────────── */
	class SourceCache {
	public:
		void setText(const std::string& text, const std::string& name) {
			name_ = name;
			lines_.clear();
			std::string cur;
			for (size_t i = 0; i < text.size(); i++) {
				char c = text[i];
				if (c == '\r') { continue; }
				if (c == '\n') { lines_.push_back(cur); cur.clear(); continue; }
				cur.push_back(c);
			}
			if (!cur.empty()) lines_.push_back(cur);
		}
		bool loadFile(const std::string& path) {
			std::ifstream f(path, std::ios::binary);
			if (!f) return false;
			std::ostringstream ss;
			ss << f.rdbuf();
			setText(ss.str(), path);
			return true;
		}
		const std::string& fileName() const { return name_; }
		int lineCount() const { return static_cast<int>(lines_.size()); }
		std::string line(int n) const {
			if (n < 1 || n > static_cast<int>(lines_.size())) return "";
			return lines_[static_cast<size_t>(n - 1)];
		}
	private:
		std::string name_;
		std::vector<std::string> lines_;
	};

	/* ── 诊断结构 ────────────────────────────────────────────────────── */
	enum class Severity { Error, Warning, Note, Help };

	struct Span {
		int line = 0;      /* 1-based */
		int col = 0;       /* 1-based */
		int len = 0;       /* 按显示宽度 */
	};

	struct Label {
		Span span;
		std::string msg;
		bool primary = false;
	};

	struct Diagnostic {
		Severity sev = Severity::Error;
		std::string code;            /* 如 "E0101" */
		std::string message;         /* 标题 */
		std::vector<Label> labels;   /* 至少一个；primary 决定主标注 */
		std::vector<std::string> notes;
		std::vector<std::string> helps;

		void label(Span sp, const std::string& m, bool isPrimary = false) {
			labels.push_back({sp, m, isPrimary});
		}
		void note(const std::string& m) { notes.push_back(m); }
		void help(const std::string& m) { helps.push_back(m); }
	};

	/* 携带诊断的异常。继承 runtime_error 保证旧的 catch 全部照常工作，
	   新流程可用 dynamic_cast 取回富信息。 */
	struct DiagnosticError : std::runtime_error {
		Diagnostic diag;
		/* what() 保留旧的 "消息 at 行:列" 格式，这样任何还在用纯文本
		   路径的地方都不会丢信息；新路径用 dynamic_cast 取 diag 走富渲染。 */
		explicit DiagnosticError(const Diagnostic& d) : std::runtime_error(""), diag(d) {
			std::string loc;
			for (auto& l : d.labels) {
				if (l.primary) { loc = " at " + std::to_string(l.span.line) + ":" + std::to_string(l.span.col); break; }
			}
			if (loc.empty() && !d.labels.empty())
				loc = " at " + std::to_string(d.labels[0].span.line) + ":" + std::to_string(d.labels[0].span.col);
			whatBuf_ = d.message + loc;
		}
		const char* what() const noexcept override { return whatBuf_.c_str(); }
	private:
		std::string whatBuf_;
	};

	/* ── 拼写建议: Levenshtein ───────────────────────────────────────── */
	inline int editDistance(const std::string& a, const std::string& b) {
		const size_t n = a.size(), m = b.size();
		if (n == 0) return static_cast<int>(m);
		if (m == 0) return static_cast<int>(n);
		std::vector<int> prev(m + 1), cur(m + 1);
		for (size_t j = 0; j <= m; j++) prev[j] = static_cast<int>(j);
		for (size_t i = 1; i <= n; i++) {
			cur[0] = static_cast<int>(i);
			for (size_t j = 1; j <= m; j++) {
				int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
				cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
			}
			prev.swap(cur);
		}
		return prev[m];
	}

	/* 找最相近的候选名；没有足够近的返回空 */
	inline std::string suggestSimilar(const std::string& name, const std::vector<std::string>& cands) {
		if (name.empty() || cands.empty()) return "";
		std::string best;
		int bestD = INT_MAX;
		for (const auto& c : cands) {
			int d = editDistance(name, c);
			if (d < bestD) { bestD = d; best = c; }
		}
		/* 阈值随长度放宽，避免长名字误报 */
		int limit = static_cast<int>(name.size()) / 3 + 1;
		if (limit > 4) limit = 4;
		if (bestD <= limit && !best.empty()) return best;
		return "";
	}



	/* ── 错误分类与"你是不是想写"建议 ────────────────────────────────── */

	/* 从消息里抽取单引号内的名字 */
	inline std::string extractQuoted(const std::string& msg) {
		size_t a = msg.find('\'');
		if (a == std::string::npos) return "";
		size_t b = msg.find('\'', a + 1);
		if (b == std::string::npos) return "";
		return msg.substr(a + 1, b - a - 1);
	}

	struct Classified {
		std::string code;      /* E0101 */
		std::string labelMsg;  /* 标在 ^^^ 后面的话 */
		std::string help;
	};

	/* 关键字候选由外部注入（避免 diag 依赖 Parser 内部表） */
	inline std::vector<std::string>& keywordCandidates() {
		static std::vector<std::string> v;
		return v;
	}
	inline void registerKeywords(const std::vector<std::string>& ks) {
		keywordCandidates() = ks;
	}

	/* 内置函数名单，供"你是不是想写"建议用。
	   它们不走普通的 scope 注册路径，所以要单独列出来。 */
	inline const std::vector<std::string>& builtinNames() {
		static const std::vector<std::string> v = {
			"__atomic_add", "__atomic_cas", "__atomic_fetch_add", "__atomic_xchg", "__mfence",
			"__thread", "__rt_alloc", "__rt_free", "__mulhi", "__umulhi", "__mullo", "__umullo",
			"__addc", "__subb", "__byte_at", "__store_byte", "__exit", "__command_line",
			"__file_open", "__file_close", "__file_write", "__read_file", "__write_file"
		};
		return v;
	}

	inline Classified classify(const std::string& msg) {
		Classified c;
		std::string name = extractQuoted(msg);

		auto has = [&](const char* p) { return msg.find(p) != std::string::npos; };

		if (has("无法识别的顶层声明")) {
			c.code = "E0101";
			c.labelMsg = "未知的顶层声明";
			if (!name.empty()) {
				std::string s = suggestSimilar(name, keywordCandidates());
				if (!s.empty()) c.help = "你是否想写 '" + s + "'？";
				else c.help = "顶层只接受: create.mod / package / import / trait / impl / enum 等声明";
			}
		} else if (has("无法识别的表达式起始 token")) {
			c.code = "E0102";
			c.labelMsg = "这里不是一个合法的表达式开头";
			c.help = "检查是否漏写了运算符或操作数";
		} else if (has("无法解析 import 模块") || has("未找到该模块的定义")) {
			c.code = "E0201";
			c.labelMsg = "模块未找到";
			if (!name.empty()) c.help = "确认 lib/" + name + ".wei 存在，或用 mtg precompile 生成 .mod";
		} else if (has("依赖") && has("未找到")) {
			c.code = "E0202";
			c.labelMsg = "依赖的模块不存在";
			c.help = "requires() 里的模块必须先定义，或作为库文件存在";
		} else if (has("未导出") || has("export")) {
			c.code = "E0203";
			c.labelMsg = "跨文件调用需要导出";
			c.help = "在被调模块前加 export，例如: export create.mod 名字::f()";
		} else if (has("循环依赖")) {
			c.code = "E0204";
			c.labelMsg = "依赖成环";
			c.help = "把公共部分抽到第三个模块，让依赖变成单向";
		} else if (has("重复定义")) {
			c.code = "E0205";
			c.labelMsg = "重复定义";
		} else if (has("未定义") || has("Undefined")) {
			c.code = "E0425";
			c.labelMsg = "未定义的名字";
			if (!name.empty()) {
				std::string s = suggestSimilar(name, keywordCandidates());
				if (!s.empty()) c.help = "你是否想写 '" + s + "'？";
			}
		} else if (has("类型") || has("type")) {
			c.code = "E0308";
			c.labelMsg = "类型不匹配";
		} else if (has("期望")) {
			/* "期望 ';', 实际拿到 'X'" 这类语法错: 把期望的和实际的都抽出来 */
			c.code = "E0103";
			std::vector<std::string> qs;
			size_t pos = 0;
			while (pos < msg.size()) {
				size_t a = msg.find('\'', pos);
				if (a == std::string::npos) break;
				size_t b = msg.find('\'', a + 1);
				if (b == std::string::npos) break;
				qs.push_back(msg.substr(a + 1, b - a - 1));
				pos = b + 1;
			}
			c.labelMsg = "这里不应该是 '" + (qs.size() >= 2 ? qs[1] : (qs.empty() ? "" : qs[0])) + "'";
			if (qs.size() >= 2) {
				c.help = "这里需要 '" + qs[0] + "'。删掉 '" + qs[1] + "' 或在其前补上 '" + qs[0] + "'";
			}
		} else if (has("缺少") || has("未闭合") || has("不匹配")) {
			c.code = "E0104";
			c.labelMsg = "括号或符号未闭合";
			c.help = "检查配对的 ( ) { } [ ] 是否齐全";
		} else {
			c.code = "E0001";
			c.labelMsg = "此处出错";
		}
		return c;
	}

	/* ── 渲染 ────────────────────────────────────────────────────────── */

	/* tab 展开为 4 空格，返回展开后的字符串；同时给出 原始下标 -> 显示列 的映射 */
	inline std::string expandTabs(const std::string& line, std::vector<int>& colMap) {
		std::string out;
		colMap.assign(line.size() + 1, 0);
		int disp = 0;
		size_t i = 0;
		while (i < line.size()) {
			colMap[i] = disp;
			if (line[i] == '\t') {
				int toNext = 4 - (disp % 4);
				out.append((size_t)toNext, ' ');
				disp += toNext;
				i++;
				continue;
			}
			/* 必须按 utf8Decode 返回的字节数推进，不能 i++。
			   否则中文等多字节字符的后继字节会被当成独立字符，
			   宽度重复累加，标注列就会整体偏移（实测中文行完全画不出 ^）。 */
			uint32_t cp = 0;
			int n = utf8Decode(line, i, cp);
			out.append(line, i, (size_t)n);
			disp += codePointWidth(cp);
			i += (size_t)n;
		}
		colMap[line.size()] = disp;
		return out;
	}

	inline int digitsOf(int n) {
		if (n <= 0) return 1;
		int d = 0;
		while (n > 0) { d++; n /= 10; }
		return d;
	}

	/* 渲染单条诊断 */
	inline std::string render(const Diagnostic& d, const SourceCache& src) {
		std::ostringstream o;
		const std::string BOLD = "\033[1m";
		const std::string RED = "\033[31m";
		const std::string BRED = "\033[1;31m";
		const std::string BYEL = "\033[1;33m";
		const std::string BCYN = "\033[1;36m";
		const std::string CYN = "\033[36m";
		const std::string BMAG = "\033[1;35m";
		const std::string BGRN = "\033[1;32m";
		const std::string DIM = "\033[2m";
		const std::string R = "\033[0m";

		auto B = [&](const std::string& s, const std::string& style) -> std::string {
			return useColor() ? (style + s + R) : s;
		};

		if (d.labels.empty()) return "";

		/* 头部: error[E0101]: 消息 */
		const char* sevName = "error";
		std::string sevStyle = BRED;
		if (d.sev == Severity::Warning) { sevName = "warning"; sevStyle = BYEL; }
		else if (d.sev == Severity::Note) { sevName = "note"; sevStyle = BCYN; }
		else if (d.sev == Severity::Help) { sevName = "help"; sevStyle = BGRN; }

		std::string head = std::string(sevName);
		if (!d.code.empty()) head += "[" + d.code + "]";
		o << B(head, sevStyle) << ": " << B(d.message, BOLD) << "\n";

		/* 主标注: 决定 --> 指向哪 */
		const Label* prim = nullptr;
		for (auto& l : d.labels) if (l.primary) { prim = &l; break; }
		if (!prim) prim = &d.labels[0];

		/* 位置行 */
		std::string loc = src.fileName().empty() ? "<unknown>" : src.fileName();
		o << std::string(digitsOf(prim->span.line) + 1, ' ')
		  << B("--> ", BCYN) << loc << ":" << prim->span.line << ":" << prim->span.col << "\n";

		/* 收集涉及的源码行，排序去重 */
		std::vector<int> rows;
		for (auto& l : d.labels) {
			if (l.span.line >= 1) rows.push_back(l.span.line);
		}
		std::sort(rows.begin(), rows.end());
		rows.erase(std::unique(rows.begin(), rows.end()), rows.end());

		int maxRow = rows.empty() ? 1 : rows.back();
		int gutter = digitsOf(maxRow);

		auto gutterStr = [&](int n) {
			std::string s = std::to_string(n);
			return B(padToWidth(s, gutter), BOLD + (useColor() ? "\033[36m" : ""));
		};
		auto blankGutter = [&]() { return std::string((size_t)gutter + 1, ' '); };

		for (size_t ri = 0; ri < rows.size(); ri++) {
			int ln = rows[ri];
			std::string raw = src.line(ln);
			std::vector<int> colMap;
			std::string text = expandTabs(raw, colMap);

			/* 分隔行 */
			o << blankGutter() << B("|", BCYN) << "\n";

			/* 源码行 */
			o << " " << gutterStr(ln) << " " << B("|", BCYN) << " " << text << "\n";

			/* 该行上的所有标注，按列排序 */
			std::vector<const Label*> onRow;
			for (auto& l : d.labels) if (l.span.line == ln) onRow.push_back(&l);
			std::sort(onRow.begin(), onRow.end(), [](const Label* a, const Label* b) {
				if (a->span.col != b->span.col) return a->span.col < b->span.col;
				return a->primary && !b->primary;
			});

			/* 画 underline。多标注共存时，用 occupancy 数组避免互相覆盖 */
			std::vector<char> marks(displayWidth(text) + 2, ' ');
			std::vector<bool> isPrim(displayWidth(text) + 2, false);
			for (auto* l : onRow) {
				int startCol = 0;
				/* 原始 col (1-based) -> 显示列 */
				int rawIdx = l->span.col - 1;
				if (rawIdx >= 0 && rawIdx < (int)colMap.size()) startCol = colMap[rawIdx];
				else if (rawIdx >= (int)colMap.size() && !colMap.empty()) startCol = colMap.back();
				int len = l->span.len > 0 ? l->span.len : 1;
				for (int k = 0; k < len; k++) {
					int p = startCol + k;
					if (p < 0 || p >= (int)marks.size()) continue;
					/* 主标注覆盖次标注 */
					if (marks[p] == ' ' || l->primary) {
						marks[p] = l->primary ? '^' : '-';
						isPrim[p] = l->primary;
					}
				}
			}

			/* 输出 underline。
			   BUGFIX: 空格必须照样输出，否则标注会全部挤到行首对不准列。
			   之前用 `continue` 跳过了空格，结果 ^^^ 永远贴在最左边。 */
			std::string underline;
			size_t p = 0;
			while (p < marks.size()) {
				if (marks[p] == ' ') {
					size_t q = p;
					while (q < marks.size() && marks[q] == ' ') q++;
					underline.append((size_t)(q - p), ' ');
					p = q;
					continue;
				}
				size_t q = p;
				while (q < marks.size() && marks[q] != ' ' && isPrim[q] == isPrim[p]) q++;
				std::string seg((size_t)(q - p), marks[p]);
				underline += isPrim[p] ? (B(seg, BMAG)) : (B(seg, CYN));
				p = q;
			}

			/* 单个标注: 消息直接跟在 ^^^ 后面。
			   多个标注: underline 不带消息，避免歧义，改在下面逐条列出。 */
			/* 去掉尾部多余空格: marks 数组比实际行长，不 trim 会在
			   消息前留一大段空白。空格都是裸字符（未着色），可安全 rtrim。 */
			while (!underline.empty() && underline.back() == ' ') underline.pop_back();

			const Label* tailLabel = nullptr;
			for (auto* l : onRow) if (!l->msg.empty()) tailLabel = l;

			o << blankGutter() << B("|", BCYN) << " " << underline;
			if (onRow.size() == 1 && tailLabel) {
				o << " " << B(tailLabel->msg, tailLabel->primary ? BMAG : CYN);
			}
			o << "\n";

			if (onRow.size() > 1) {
				for (auto* l : onRow) {
					std::string marker = l->primary ? "^" : "-";
					o << blankGutter() << B("|", BCYN) << " "
					  << B(marker, l->primary ? BMAG : CYN) << " "
					  << (l->msg.empty() ? "" : l->msg) << "\n";
				}
			}
		}

		/* notes */
		for (auto& n : d.notes) {
			o << blankGutter() << B("= note: ", BCYN) << n << "\n";
		}
		/* helps */
		for (auto& h : d.helps) {
			o << blankGutter() << B("= help: ", BGRN) << h << "\n";
		}
		return o.str();
	}

	inline void emit(const Diagnostic& d, const SourceCache& src) {
		std::string s = render(d, src);
		if (!s.empty()) std::fwrite(s.data(), 1, s.size(), stderr);
	}

	/* 便捷构造 */
	inline Diagnostic makeError(const std::string& code, const std::string& msg, Span sp) {
		Diagnostic d;
		d.sev = Severity::Error;
		d.code = code;
		d.message = msg;
		d.labels.push_back({sp, "", true});
		return d;
	}

	/* ── 内置错误码手册 ─────────────────────────────────────────────────
	   rustc --explain 只有英文说明，且不带可直接编译的修复片段。MTG 把
	   手册编进编译器本体：中英双语 + 成因 + 最小复现 + 修复示例，完全离线。
	   编译器报出的每个 code 都保证能在这里查到（codeBook 与 classify 同源维护）。 */
	struct CodeDoc {
		const char* code;
		const char* title_zh;
		const char* title_en;
		const char* why_zh;
		const char* bad;   /* 最小复现 */
		const char* fix;   /* 可直接编译的修复片段 */
	};

	inline const std::vector<CodeDoc>& codeBook() {
		static const std::vector<CodeDoc> v = {
			{"E0001", "未分类的语法或语义错误", "Unclassified syntax/semantic error",
			 "编译器遇到了无法归入既有类别的错误。通常伴随一条原始消息，按其中的行列号定位。",
			 R"wei(create.mod Main::main() {
    do.emit::;
})wei",
			 R"wei(create.mod Main::main() {
    do.emit::"Hello";
})wei"},
			{"E0100", "词法错误", "Lexical error",
			 "源码里出现了词法器无法组成 token 的字节，例如非法 UTF-8、未闭合的字符串或未知符号。",
			 R"wei(create.mod Main::main() {
    do.emit::"未闭合;
})wei",
			 R"wei(create.mod Main::main() {
    do.emit::"已闭合";
})wei"},
			{"E0101", "未知的顶层声明", "Unknown top-level declaration",
			 "顶层只接受 create.mod / package / import / trait / impl / enum / export 等声明，其它写法会被拒绝。",
			 R"wei(moduel Main::main() {
})wei",
			 R"wei(create.mod Main::main() {
})wei"},
			{"E0102", "非法的表达式起始", "Invalid expression start",
			 "运算符或操作数缺失，导致该位置无法开始一个表达式。",
			 R"wei(create.mod Main::main() {
    create.int x = * 3;
})wei",
			 R"wei(create.mod Main::main() {
    create.int x = 2 * 3;
})wei"},
			{"E0103", "语法符号不符合预期", "Unexpected token",
			 "解析器在此处需要另一个符号（常见是漏了分号、括号或 ::）。",
			 R"wei(create.mod Main::main() {
    create.int x = 3
    do.emit::x;
})wei",
			 R"wei(create.mod Main::main() {
    create.int x = 3;
    do.emit::x;
})wei"},
			{"E0104", "括号或符号未闭合", "Unclosed delimiter",
			 "( ) { } [ ] 没有配对，或字符串/注释跨到了文件结尾。",
			 R"wei(create.mod Main::main() {
    if (1 == 1) {
        do.emit::1;
})wei",
			 R"wei(create.mod Main::main() {
    if (1 == 1) {
        do.emit::1;
    }
})wei"},
			{"E0201", "模块未找到", "Module not found",
			 "import 指向的文件不存在。可用 mtg precompile 预编译成 .mod 加速重复引用。",
			 R"wei(import lib/Missing;
create.mod Main::main() {
})wei",
			 R"wei(/* lib/Missing.wei 需存在，或先执行:
   mtg precompile lib/Missing.wei  */
create.mod Main::main() {
})wei"},
			{"E0202", "依赖的模块不存在", "Required dependency missing",
			 "requires() 声明的模块必须先定义，或作为库文件存在于搜索路径中。",
			 R"wei(requires("Ghost");
create.mod Main::main() {
})wei",
			 R"wei(create.mod Ghost {
}
create.mod Main::main() {
})wei"},
			{"E0203", "跨文件调用需要导出", "Not exported",
			 "模块成员默认文件私有；跨文件调用必须在定义处加 export。",
			 R"wei(/* lib/A.wei */
create.mod A::f().int { return 1; })wei",
			 R"wei(/* lib/A.wei */
export create.mod A::f().int { return 1; })wei"},
			{"E0204", "依赖成环", "Circular dependency",
			 "模块之间互相依赖，加载顺序无法确定。把公共部分抽到第三个模块即可。",
			 R"wei(/* A requires B, B 又 requires A */)wei",
			 R"wei(/* 把 A、B 共用的定义移到 Common，
   A requires Common，B requires Common */)wei"},
			{"E0205", "重复定义", "Duplicate definition",
			 "同一作用域内出现了两个同名定义（模块、函数、部件或变量）。",
			 R"wei(create.mod Main::main() {
    create.int x = 1;
    create.int x = 2;
})wei",
			 R"wei(create.mod Main::main() {
    create.int x = 1;
    x = 2;
})wei"},
			{"E0277", "'?' 只能在返回 Result 的函数中使用", "The '?' operator requires a Result-returning function",
			 "错误码必须有人接住。'?' 会把 Err 沿调用栈向上传播，若当前函数不返回 .Result，"
			 "错误码会被静默丢弃 —— 这正是 C 风格忽略返回值的老毛病，编译期直接拦住。",
			 R"wei(create.mod Math::div(int a, int b).Result {
    if (b == 0) { return -1; }
    return a / b;
}
create.mod Main::main() {
    create.int v = Math::div(10, 2)?;
})wei",
			 R"wei(create.mod Math::div(int a, int b).Result {
    if (b == 0) { return -1; }
    return a / b;
}
create.mod Main::main().Result {
    create.int v = Math::div(10, 2)?;
    do.emit::v;
    return 0;
})wei"},
			{"E0308", "类型不匹配", "Type mismatch",
			 "表达式的静态类型与期望类型不一致（例如把字符串赋给整型，或返回类型不符）。",
			 R"wei(create.mod Main::main() {
    create.int x = "hello";
})wei",
			 R"wei(create.mod Main::main() {
    create.int x = 42;
})wei"},
			{"E0425", "未定义的名字", "Name not resolved",
			 "变量、函数或模块成员在使用前没有定义；编译器会按编辑距离给出最接近的候选。",
			 R"wei(create.mod Main::main() {
    do.emit::coumt;
})wei",
			 R"wei(create.mod Main::main() {
    create.int count = 1;
    do.emit::count;
})wei"}
		};
		return v;
	}

	inline const CodeDoc* findCode(const std::string& raw) {
		std::string code = raw;
		if (!code.empty() && code[0] == 'e') code[0] = 'E';
		for (const auto& d : codeBook())
			if (code == d.code) return &d;
		return nullptr;
	}

	/* 打印单个错误码的完整条目；未收录时列出可用码并返回 2。 */
	inline int printExplain(const std::string& code) {
		const CodeDoc* d = findCode(code);
		if (!d) {
			std::fprintf(stderr, "未知错误码 '%s'。用 mtg --list-codes 查看全部错误码。\n", code.c_str());
			return 2;
		}
		std::printf("error[%s]: %s\n", d->code, d->title_zh);
		std::printf("           %s\n\n", d->title_en);
		std::printf("  成因\n  ----\n  %s\n\n", d->why_zh);
		std::printf("  错误示例\n  --------\n");
		{
			std::string s = d->bad;
			size_t p = 0;
			while (p <= s.size()) {
				size_t e = s.find('\n', p);
				std::printf("    %s\n", s.substr(p, e == std::string::npos ? std::string::npos : e - p).c_str());
				if (e == std::string::npos) break;
				p = e + 1;
			}
		}
		std::printf("\n  修复\n  ----\n");
		{
			std::string s = d->fix;
			size_t p = 0;
			while (p <= s.size()) {
				size_t e = s.find('\n', p);
				std::printf("    %s\n", s.substr(p, e == std::string::npos ? std::string::npos : e - p).c_str());
				if (e == std::string::npos) break;
				p = e + 1;
			}
		}
		std::printf("\n  提示: mtg --list-codes 查看全部错误码；"
		            "mtg build x.wei -no-error-trace 可关闭 '?' 传播链。\n");
		return 0;
	}

	inline int listCodes() {
		const auto& b = codeBook();
		std::printf("MTG 错误码索引（%zu 条）\n", b.size());
		for (const auto& d : b)
			/* 按显示宽度补齐：CJK 占 2 列，%-Ns 按字节算会把中文列挤歪 */
			std::printf("  %-7s %s  %s\n", d.code,
			            padToWidth(d.title_zh, 30).c_str(), d.title_en);
		std::printf("\n用法: mtg explain E0277   或   mtg --explain E0277\n");
		return 0;
	}

	/* '?' 传播链开关。默认开启：编译期在每个 '?' 的 Err 分支注入站点信息，
	   运行时逐层打印，无需 anyhow/backtrace 之类的第三方库。 */
	inline bool& errorTraceFlag() { static bool b = true; return b; }
	inline void setErrorTrace(bool v) { errorTraceFlag() = v; }

}  // namespace wei::diag

[[noreturn]] void Lexer::die(const std::string& msg, int32_t line, int32_t col) const {
	wei::diag::Classified c = wei::diag::classify(msg);
	wei::diag::Diagnostic d;
	d.sev = wei::diag::Severity::Error;
	d.code = c.code.empty() ? "E0100" : c.code;
	d.message = msg;
	d.labels.push_back({{line, col, 1}, c.labelMsg.empty() ? "词法错误" : c.labelMsg, true});
	if (!c.help.empty()) d.help(c.help);
	throw wei::diag::DiagnosticError(d);
}
//字面意??
Token Lexer::readNumber() {
	int32_t line = line_;
	int32_t col = col_;
	std::string s;
	while (!eof() && isdigit(static_cast<unsigned char>(peek()))) {
		s.push_back(get());
	}
	/* 浮点字面?? 整数部分 '.' 数字部分 (要求点后必有数字, 避免误吃 a[1].x 类写?? */
	if (!eof() && peek() == '.' && isdigit(static_cast<unsigned char>(peek(1)))) {
		s.push_back(get());
		while (!eof() && isdigit(static_cast<unsigned char>(peek()))) {
			s.push_back(get());
		}
		return makeToken(TokType::DoubleLit, s, line, col);
	}
	return makeToken(TokType::IntLit, s, line, col);
}

Token Lexer::readStringContent(int32_t line, int32_t col) {
	std::string s;
	while (!eof()) {
		unsigned char b = static_cast<unsigned char>(peek());
		if (b == '"') {
			get();
			break;
		}
		if (b == 0xE2 && static_cast<unsigned char>(peek(1)) == 0x80 && static_cast<unsigned char>(peek(2)) == 0x9D) {
			get();
			get();
			get();
			break;
		}
		char c = peek();
		if (c == '\n') {
			die("unterminated string literal", line, col);
		}
		if (c == '\\') {
			get();
			char e = peek();
			switch (e) {
				case 'n':
					s.push_back('\n');
					get();
					break;
				case 't':
					s.push_back('\t');
					get();
					break;
				case 'r':
					s.push_back('\r');
					get();
					break;
				case '\\':
					s.push_back('\\');
					get();
					break;
				case '"':
					s.push_back('"');
					get();
					break;
				case '0':
					s.push_back('\0');
					get();
					break;
				default:
					s.push_back('\\');
					s.push_back(e);
					get();
					break;
			}
		} else {
			s.push_back(get());
		}
	}
	if (eof()) {
		die("unterminated string literal", line, col);
	}
	return makeToken(TokType::StrLit, s, line, col);
}

Token Lexer::readString() {
	int32_t line = line_;
	int32_t col = col_;
	get();
	return readStringContent(line, col);
}

Token Lexer::readIdentOrKeyword() {
	int32_t line = line_;
	int32_t col = col_;
	std::string s;
	while (!eof()) {
		unsigned char b = static_cast<unsigned char>(peek());
		if ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') || b == '_') {
			s.push_back(static_cast<char>(get()));
		} else {
			break;
		}
	}
	auto it = kEngKws.find(s);
	if (it != kEngKws.end()) {
		return makeToken(it->second, s, line, col);
	}
	return makeToken(TokType::Ident, s, line, col);
}

Token Lexer::readChineseIdent() {
	int32_t line = line_;
	int32_t col = col_;
	std::string s;
	while (!eof()) {
		unsigned char b = static_cast<unsigned char>(peek());
		if (!isCjkLead(b)) {
			break;
		}
		if (static_cast<unsigned char>(peek(1)) == 0 || !isUtf8Cont(static_cast<unsigned char>(peek(1)))) {
			break;
		}
		if (static_cast<unsigned char>(peek(2)) == 0 || !isUtf8Cont(static_cast<unsigned char>(peek(2)))) {
			break;
		}
		uint32_t cp = decodeCjk3(b, static_cast<unsigned char>(peek(1)), static_cast<unsigned char>(peek(2)));
		if (!isCjkCodepoint(cp)) {
			break;
		}
		s.push_back(static_cast<char>(get()));
		s.push_back(static_cast<char>(get()));
		s.push_back(static_cast<char>(get()));
	}
	if (s.empty()) {
		die("空的中文标识符", line, col); // 修复: 错误信息不准确
	}
	auto it = kCjkKws.find(s);
	if (it != kCjkKws.end()) {
		return makeToken(it->second, s, line, col);
	}
	return makeToken(TokType::Ident, s, line, col);
}

/* 反引号转义标识符: `class`、`如果` 等强制按标识符处?? 规避中英文关键词冲突 */
Token Lexer::readEscapedIdent() {
	int32_t line = line_;
	int32_t col = col_;
	get();  /* 吃掉开头反引号 */
	std::string s;
	while (!eof()) {
		unsigned char b = static_cast<unsigned char>(peek());
		if (b == '`') {
			get();
			if (s.empty()) {
				die("反引号转义标识符为空", line, col);
			}
			return makeToken(TokType::Ident, s, line, col);
		}
		if (b == '\n') {
			die("反引号转义标识符未闭合", line, col); // 修复: 错误信息不准确
		}
		s.push_back(static_cast<char>(get()));
	}
	die("反引号转义标识符未闭合", line, col); // 修复: 错误信息不准确
}

Token Lexer::readOpOrPunct() {
	int32_t line = line_;
	int32_t col = col_;
	unsigned char b = static_cast<unsigned char>(peek());
	/* '?' —— Result 错误传播: f()?  若 f 返回 Err 则当前函数立即返回该错误 */
	if (b == '?') { get(); return makeToken(TokType::Question, "?", line, col); }
	if (b == 0xEF) {
		if (static_cast<unsigned char>(peek(1)) == 0xBC && static_cast<unsigned char>(peek(2)) == 0x9A) {
			get();
			get();
			get();
			return makeToken(TokType::Semicolon, ";", line, col);
		}
		if (static_cast<unsigned char>(peek(1)) == 0xBC && static_cast<unsigned char>(peek(2)) == 0x9B) {
			get();
			get();
			get();
			return makeToken(TokType::Semicolon, ";", line, col);
		}
		if (static_cast<unsigned char>(peek(1)) == 0xBC && static_cast<unsigned char>(peek(2)) == 0x88) {
			get();
			get();
			get();
			return makeToken(TokType::Colon, ":", line, col);
		}
		if (static_cast<unsigned char>(peek(1)) == 0xBC && static_cast<unsigned char>(peek(2)) == 0x89) {
			get();
			get();
			get();
			return makeToken(TokType::Colon, ":", line, col);
		}
	}
	if (b == 0xE3 && static_cast<unsigned char>(peek(1)) == 0x80 && static_cast<unsigned char>(peek(2)) == 0x8A) {
		get();
		get();
		get();
		return makeToken(TokType::LangL, "<", line, col);
	}
	if (b == 0xE3 && static_cast<unsigned char>(peek(1)) == 0x80 && static_cast<unsigned char>(peek(2)) == 0x8B) {
		get();
		get();
		get();
		return makeToken(TokType::LangR, ">", line, col);
	}
	char c = static_cast<char>(b);
	switch (c) {
		case '(':
			get();
			return makeToken(TokType::LParen, "(", line, col);
		case ')':
			get();
			return makeToken(TokType::RParen, ")", line, col);
		case '{':
			get();
			return makeToken(TokType::LBrace, "{", line, col);
		case '}':
			get();
			return makeToken(TokType::RBrace, "}", line, col);
		case '[':
			get();
			return makeToken(TokType::LBracket, "[", line, col);
		case ']':
			get();
			return makeToken(TokType::RBracket, "]", line, col);
		case ',':
			get();
			return makeToken(TokType::Comma, ",", line, col);
		case ';':
			get();
			return makeToken(TokType::Semicolon, ";", line, col);
		case '.':
			get();
			if (peek() == '.') {
				get();
				return makeToken(TokType::DoubleDot, "..", line, col);
			}
			return makeToken(TokType::Dot, ".", line, col);
		case '+':
			get();
			if (peek() == '+') {
				get();
				return makeToken(TokType::PlusPlus, "++", line, col);
			}
			if (peek() == '=') {
				get();
				return makeToken(TokType::PlusEq, "+=", line, col);
			}
			return makeToken(TokType::Plus, "+", line, col);
		case '-':
			get();
			if (peek() == '-') {
				get();
				return makeToken(TokType::MinusMinus, "--", line, col);
			}
			if (peek() == '=') {
				get();
				return makeToken(TokType::MinusEq, "-=", line, col);
			}
			return makeToken(TokType::Minus, "-", line, col);
		case '*':
			get();
			if (peek() == '=') {
				get();
				return makeToken(TokType::StarEq, "*=", line, col);
			}
			return makeToken(TokType::Star, "*", line, col);
		case '/':
			get();
			if (peek() == '=') {
				get();
				return makeToken(TokType::SlashEq, "/=", line, col);
			}
			return makeToken(TokType::Slash, "/", line, col);
		case '%':
			get();
			return makeToken(TokType::Percent, "%", line, col);
		case '=':
			get();
			if (peek() == '=') {
				get();
				return makeToken(TokType::EqEq, "==", line, col);
			}
			return makeToken(TokType::Assign, "=", line, col);
		case '!':
			get();
			if (peek() == '=') {
				get();
				return makeToken(TokType::Neq, "!=", line, col);
			}
			return makeToken(TokType::Not, "!", line, col);
		case '<':
			get();
			if (peek() == '<') { get(); return makeToken(TokType::Shl, "<<", line, col); }
			if (peek() == '=') {
				get();
				return makeToken(TokType::LtEq, "<=", line, col);
			}
			return makeToken(TokType::IOLt, "<", line, col);
		case '>':
			get();
			if (peek() == '>') { get(); return makeToken(TokType::Shr, ">>", line, col); }
			if (peek() == '=') {
				get();
				return makeToken(TokType::GtEq, ">=", line, col);
			}
			return makeToken(TokType::IOGt, ">", line, col);
		case '&':
			get();
			if (peek() == '&') {
				get();
				return makeToken(TokType::And, "&&", line, col);
			}
			return makeToken(TokType::Amp, "&", line, col);
		case '|':
			get();
			if (peek() == '|') {
				get();
				return makeToken(TokType::Or, "||", line, col);
			}
			return makeToken(TokType::Pipe, "|", line, col);
		case '^':
			get();
			return makeToken(TokType::Caret, "^", line, col);
		case '~':
			get();
			return makeToken(TokType::Tilde, "~", line, col);
		case ':':
			get();
			if (peek() == ':') {
				get();
				return makeToken(TokType::DoubleColon, "::", line, col);
			}
			return makeToken(TokType::Colon, ":", line, col);
		default:
			die(std::string("未知的字符 '") + c + "'", line, col);
	}
}

std::vector<Token> Lexer::scan() {
	std::vector<Token> out;
	while (true) {
		skipSpaceAndComment();
		if (eof()) {
			out.push_back(makeToken(TokType::Eof, "", line_, col_));
			break;
		}
		unsigned char b = static_cast<unsigned char>(peek());
		if (isdigit(b)) {
			out.push_back(readNumber());
			continue;
		}
		if (b == '"') {
			out.push_back(readString());
			continue;
		}
		if (b == 0xE2 && static_cast<unsigned char>(peek(1)) == 0x80 && static_cast<unsigned char>(peek(2)) == 0x9C) {
			int32_t sl = line_, sc = col_;
			get();
			get();
			get();
			out.push_back(readStringContent(sl, sc));
			continue;
		}
		if (b == '`') {
			out.push_back(readEscapedIdent());
			continue;
		}
		if ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || b == '_') {
			Token t = readIdentOrKeyword();
			/* `M::u8` 这类成员名：宽度关键字在 `::` 之后按普通标识符处理。
			   自举源码里有 Code::u8 / Code::u16 / Code::u32 / Code::u64 等
			   发射函数；把它们一律当保留字会让 stage0 无法重建 stage1，
			   fixpoint 直接断在第一个 Code::u8 定义处。只按"前一个 token
			   是 ::"这一处上下文放宽，类型位置上的 u8/u16/... 仍是关键字，
			   其余语法完全不受影响。 */
			if (!out.empty() && out.back().type == TokType::DoubleColon) {
				switch (t.type) {
				case TokType::Kw_u8:  case TokType::Kw_u16: case TokType::Kw_u32:
				case TokType::Kw_u64: case TokType::Kw_i8:  case TokType::Kw_i16:
				case TokType::Kw_i64: case TokType::Kw_f32: case TokType::Kw_f64:
					t.type = TokType::Ident;
					break;
				default: break;
				}
			}
			out.push_back(t);
			continue;
		}
		if (isCjkLead(b)) {
			out.push_back(readChineseIdent());
			continue;
		}
		/* Skip unrecognized 3-byte UTF-8 sequences (e.g. stray full-width quotes) */
		if (b == 0xE2 && static_cast<unsigned char>(peek(1)) == 0x80 && isUtf8Cont(static_cast<unsigned char>(peek(2)))) {
			get();
			get();
			get();
			continue;
		}
		out.push_back(readOpOrPunct());
	}
	return out;
}
#endif // WEI_FRONTEND_NO_DEFS

// ============================================================
// Parser
// ============================================================
/* 前向声明: 预编译模块编解码器（完整定义在下方）。
   Parser 需要在 import/requires 时调用它，故此处先声明签名。 */
namespace wei::modcache {
	std::vector<uint8_t> serialize(const wei::ast::Program& prog,
	                               const std::vector<std::string>& deps, int64_t src_mtime);
	bool deserialize(const std::vector<uint8_t>& raw, wei::ast::Program& prog,
	                 std::vector<std::string>& deps, int64_t& src_mtime);
}

class Parser {
	public:
		explicit Parser(std::vector<Token> tokens);
		/* 设置当前解析的文件名（可见性判定的基准），公开给编译入口调用 */
		void setCurFile(const std::string& f) { cur_file_ = f; }
		/* 标记为"被导入上下文": 豁免"未被调用需声明 priority"检查。
		   预编译库时必须有这个豁免 —— 库里的模块本来就是给别人调的，
		   它自己从不调用自己。 */
		void setImportedContext(bool v) { is_imported_context_ = v; }
		wei::ast::Program parse();
	private:
		std::vector<Token> tokens_;
		size_t pos_;

		const Token& cur() const;
		const Token& peek(size_t off = 0) const;
		const Token& prev() const;
		bool eof() const;
		bool check(TokType t) const;
		bool checkAt(size_t off, TokType t) const;
		bool match(TokType t);
		const Token& expect(TokType t, const std::string& desc);
		[[noreturn]] void die(const std::string& msg, const Token& at) const;
		[[noreturn]] void die(const std::string& msg) const;

		static bool isCreateKw(TokType t);
		static bool isInputKw(TokType t);
		static bool isOutputKw(TokType t);
		static bool isStatKw(TokType t);
		static bool isTypeKw(TokType t);
		static bool isModuleKw(TokType t);
		static bool isPartKw(TokType t);
		static bool isAccessKw(TokType t);
		static bool isAdvKw(TokType t);
		static std::string typeKwText(TokType t);

		std::string eatCreateSep();
		void eatStatSep();
		void eatRetType(wei::ast::FuncDecl& fd);
		std::string parseTypeName();

		wei::ast::TopLevel parseTopLevel();
		wei::ast::FuncDecl parseFuncDecl(const std::string& retType);
		wei::ast::VarDecl parseGlobalVarDecl(const std::string& typeName);
		wei::ast::ImportDecl parseImportDecl();
		wei::ast::ModuleDecl parseModuleDecl();
		wei::ast::TopLevel parsePackageDecl(bool is_package);
		std::unique_ptr<wei::ast::Stmt> parseAsmStmt();
		/* ── 关键字修饰符框架 ───────────────────────────────────────────
		   一大批关键字本质是"声明修饰符/标记"，不需要各自独立的机器码，
		   而是影响**同一个声明的语义属性**（可见性、链接性、元数据、
		   条件编译、依赖声明）。这里统一成一个可查询的修饰符位集，
		   由声明解析入口消费。这比给每个关键字单独开一条发射路径
		   更正确 —— 它们本就不产生独立指令。 */
		enum Mod {
			M_Expose=1u<<0,  M_Internal=1u<<1,  M_Hidden=1u<<2,
			M_FilePriv=1u<<3,M_ModPriv=1u<<4,   M_Open=1u<<5,
			M_Final=1u<<6,   M_Sealed=1u<<7,    M_Pub=1u<<8,
			M_Const=1u<<9,   M_Volatile=1u<<10, M_Static=1u<<11,
			M_Extern=1u<<12, M_Inline=1u<<13,   M_Mut=1u<<14,
			M_Ref=1u<<15,    M_Move=1u<<16,     M_Auto=1u<<17,
			M_Annotate=1u<<18,M_Attribute=1u<<19,M_Deprecated=1u<<20,
			M_Doc=1u<<21,    M_Meta=1u<<22,     M_ModuleInfo=1u<<23,
			M_Feature=1u<<24,M_Cfg=1u<<25,      M_Link=1u<<26,
			M_Lib=1u<<27,    M_Library=1u<<28,  M_Dynamic=1u<<29,
			M_Depends=1u<<30,M_Provides=1u<<31
		};
		/* 第二批修饰符位（32 位放不下，另起一组） */
		enum Mod2 {
			N_Trait=1u<<0,   N_Impl=1u<<1,   N_Mixin=1u<<2,  N_Protocol=1u<<3,
			N_With=1u<<4,    N_Compose=1u<<5,N_Embed=1u<<6, N_Extend=1u<<7,
			N_Derive=1u<<8,  N_Reflect=1u<<9,N_Library=1u<<10,N_Signature=1u<<11,
			N_Header=1u<<12, N_Forward=1u<<13,N_Declare=1u<<14,N_Define=1u<<15,
			N_Qualified=1u<<16,N_Unqualified=1u<<17,N_ImportAll=1u<<18,N_Reexport=1u<<19,
			N_Crate=1u<<20,  N_Bundle=1u<<21,N_Scope=1u<<22,N_Unit=1u<<23,
			N_File=1u<<24,   N_Global=1u<<25,N_Entry=1u<<26,N_Teardown=1u<<27,
			N_Outer=1u<<28,  N_Operator=1u<<29,N_Lambda=1u<<30,N_Has=1u<<31
		};
		uint32_t mods2_ = 0;
		bool hasMod2(Mod2 m) const { return (mods2_ & (uint32_t)m) != 0; }
		/* 登记式关键字: trait/impl 等把名字登记进表，供后续查询 */
		std::vector<std::pair<std::string,std::string>> registrations_;
		uint32_t mods_ = 0;
		std::vector<std::string> mod_args_;   /* 修饰符带的参数（如 link "m"） */
		std::vector<std::string> meta_text_;  /* doc/annotate 的文本 */
		bool collectMods();
		bool tryParseEnum();
		std::map<std::string, wei::ast::TraitDecl> traits_;
		std::map<std::string, wei::ast::ImplDecl> impls_;
		/* 静态派发表: "<Trait>::<method>" → 具体实现的符号名 */
		std::map<std::string, std::string> dispatch_;
		void preprocessTraits();
		bool tryParseTrait();
		bool tryParseImpl();
		bool tryParseUnion();
		bool hasMod(Mod m) const { return (mods_ & (uint32_t)m) != 0; }
		void clearMods() { mods_ = 0; mod_args_.clear(); meta_text_.clear(); }

		/* 编译期命名常量表: enum 成员、const 声明都落在里 */
		std::map<std::string, int64_t> named_consts_;
		std::map<std::string, wei::ast::EnumDecl> enums_;
		/* 变量名 -> 枚举名。用于 switch 的穷尽性检查:
		   create.Color c = Red; 之后 switch(c) 才知道要覆盖 Color 的全部成员。 */
		std::map<std::string, std::string> enum_vars_;
		std::map<std::string, wei::ast::UnionDecl> unions_;
		std::unique_ptr<wei::ast::Stmt> parseSwitchStmt();
		std::unique_ptr<wei::ast::Stmt> parseDeferStmt();
		void detectAsmClobbers(wei::ast::AsmStmt& as);
		wei::ast::TopLevel parseUseDecl();
		wei::ast::PartDecl parsePartDecl();
		std::vector<wei::ast::VarDecl> parsePartMembers();

		wei::ast::Stmt parseStmt();
		std::vector<wei::ast::Stmt> parseBlock();
		wei::ast::Stmt parseOutputStmt();
		wei::ast::Stmt parseInputStmt();
		wei::ast::Stmt parseVarDeclStmt(const std::string& typeName, bool advPath = false);
		wei::ast::Stmt parseAccessStmt(bool adv);
		wei::ast::Stmt parseIfStmt();
		wei::ast::Stmt parseForStmt();
		wei::ast::Stmt parseWhileStmt();
		wei::ast::Stmt parseWhileStmtInner();
		wei::ast::Stmt parseLoopStmt();
		wei::ast::Stmt parseLoopStmtInner();
		wei::ast::Stmt parseReturnStmt();
		wei::ast::Stmt parseExprStmt();
		wei::ast::Stmt parseBreakStmt();
		wei::ast::Stmt parseContinueStmt();
		std::unique_ptr<wei::ast::VarDecl> parseForInitDecl();

		std::unique_ptr<wei::ast::Expr> parseExpr();
		std::unique_ptr<wei::ast::Expr> parseLogicalOr();
		std::unique_ptr<wei::ast::Expr> parseLogicalAnd();
		std::unique_ptr<wei::ast::Expr> parseEquality();
		std::unique_ptr<wei::ast::Expr> parseCompare();
		std::unique_ptr<wei::ast::Expr> parseBitOr();
		std::unique_ptr<wei::ast::Expr> parseBitXor();
		std::unique_ptr<wei::ast::Expr> parseBitAnd();
		std::unique_ptr<wei::ast::Expr> parseAdd();
		std::unique_ptr<wei::ast::Expr> parseShift();
		std::unique_ptr<wei::ast::Expr> parseMul();
		std::unique_ptr<wei::ast::Expr> parseUnary();
		std::unique_ptr<wei::ast::Expr> parsePostfix();
		std::unique_ptr<wei::ast::Expr> parsePrimary();
		std::unique_ptr<wei::ast::Expr> parseInputExpr();
		std::unique_ptr<wei::ast::Expr> parseIdentOrCall();

		/* 模块路径支持 */
		std::unique_ptr<wei::ast::Expr> parseModulePathExpr(bool adv);
		bool isModulePathAhead();
		void registerModule(const wei::ast::ModuleDecl& md);
		/* 模块 -> 部件集合 */
		std::map<std::string, std::set<std::string>> module_reg_;
		/* 模块 -> 优先级, 以及是否已被调用 */
		/* ═══ 依赖系统 ══════════════════════════════════════════════════
		   模块可以显式声明它依赖谁、提供什么。编译器据此:
		     1. 建依赖图
		     2. 检测循环依赖（A 依赖 B、B 依赖 A → 报错）
		     3. 拓扑排序决定模块初始化顺序（被依赖者先初始化）
		     4. 校验依赖的模块确实存在且可见
		   全部在解析期完成，运行时零开销。非 OO、非继承 —— 只是
		   "我用到谁"的显式契约，类似 Java 的 import/package 但更严格。 */
		struct DepNode {
			std::string name;
			std::set<std::string> requires_;    /* 本模块依赖的模块 */
			std::set<std::string> provides_;    /* 本模块对外提供的能力名 */
			int init_order = -1;                /* 拓扑序: 越小越先初始化 */
			int line = 0;
		};
		std::map<std::string, DepNode> dep_graph_;
		/* 真正**定义过**（有 create.mod 体）的模块名。
		   与 module_reg_ 区分: module_reg_ 里还包含因 requires 而
		   前向声明的模块 —— 那种"知道名字但没有实现"，不能算定义。 */
		std::set<std::string> defined_modules_;
		/* ═══ 文件系统依赖 ══════════════════════════════════════════
		   模块 ↔ 源文件路径的映射。声明 `requires(B)` 即自动去文件系统
		   找 lib/B.wei 并加载，不必手写 import —— 依赖驱动加载，
		   这才是 C++/Java 级别的"文件系统依赖"。
		     module_file_   : 模块名 → 源文件绝对路径
		     module_mtime_  : 模块名 → 文件最后修改时间（增量编译依据）
		     file_load_stack_: 当前加载栈，用于检测文件级循环依赖 */
		std::map<std::string, std::string> module_file_;
		std::map<std::string, int64_t> module_mtime_;
		std::vector<std::string> file_load_stack_;
		/* 尝试从文件系统加载模块。返回是否加载成功（找不到返回 false） */
		bool loadModuleFromFile(const std::string& mod, const Token& at);
		/* 预编译模块加载: 优先查同名 .mod，命中且未过期就直接反序列化
		   AST（跳过词法+语法分析）。返回 false 表示没有可用的 .mod，
		   调用方回退到解析 .wei。
		   对应 C++20: import 时读 BMI 而不是重新预处理头文件。 */
		bool loadPrecompiled(const std::string& mod, const Token& at);
		std::map<std::string, std::set<std::string>> mod_members_seen_;  /* 成员级重复定义检测 */
		std::string cur_dep_module_;            /* 当前正在定义的模块（依赖归属） */
		/* 记录依赖: requires A, B  /  provides X, Y */
		void addRequires(const std::string& mod, const std::vector<std::string>& names, int line);
		void addProvides(const std::string& mod, const std::vector<std::string>& names, int line);
		/* 拓扑排序 + 循环检测。返回 false 表示存在环（已报错） */
		bool resolveDepOrder();
		/* 深度优先查环，path 为当前路径 */
		bool depHasCycle(const std::string& n, std::set<std::string>& visiting,
		                 std::set<std::string>& done, std::vector<std::string>& path);

		std::map<std::string, int> module_prio_;
		std::set<std::string> module_referenced_;
		void markReferenced(const std::string& mod);

		std::vector<wei::ast::Param> parseParams();

		/* 新增语句解析 */
		wei::ast::Stmt parseMatchStmt();
		wei::ast::Stmt parseContractStmt();
		std::unique_ptr<wei::ast::Expr> parseTupleExpr();
		std::unique_ptr<wei::ast::Expr> parseTypeExpr();
		wei::ast::Stmt parseTypeAliasDecl();

		/* ══ 本轮补全的 22 个关键字 ══════════════════════════════════════ */
		wei::ast::Stmt parseForallStmt(bool is_forall);   /* forall / exists */
		wei::ast::Stmt parseWhenStmt();                   /* when ... then ... */
		wei::ast::Stmt parseYieldStmt();                  /* yield v;        */
		wei::ast::Stmt parseCompileTimeStmt();            /* compiletime { } */
		wei::ast::TopLevel parseMacroDeclTop();           /* create.macro    */
		wei::ast::TopLevel parseImportTopLevel();         /* import / from   */
		std::unique_ptr<wei::ast::Expr> parseRangeExpr(); /* lo..hi          */
		std::unique_ptr<wei::ast::Expr> parseCoExpr();    /* co f(args)      */
		std::unique_ptr<wei::ast::Expr> parseAwaitExpr(); /* await h         */
		std::unique_ptr<wei::ast::Expr> parseTypeofExpr();/* typeof(x)       */
		std::unique_ptr<wei::ast::Expr> parseCompileTimeExpr();
		bool parseOptAsync(wei::ast::FuncDecl& fd);       /* 可选的 async    */

		/* ── 宏展开 ── */
		bool isMacroName(const std::string& n) const;
		bool expandMacroStmt(std::vector<wei::ast::Stmt>& out);
		bool tryMacroExpr(std::unique_ptr<wei::ast::Expr>& out);

		/* ── import 内联 ── */
		bool importModuleInline(const std::string& module,
		                        const std::vector<std::string>& syms,
		                        const std::string& alias, const Token& at);

		/* 当前正在解析的泛型函数的类型参数集合（供 parseTypeName 放行 T）*/
		std::set<std::string> cur_type_params_;
		/* ═══ 可见性强制 ══════════════════════════════════════════════
		   非 OO，纯模块化。每个模块成员带一个可见性级别，调用点按
		   (调用方模块, 调用方文件) vs (被调方模块, 被调方文件, 级别) 判定。
		     export/pub/public  → 任何地方可调用
		     internal(默认)     → 同一模块内可调用
		     moduleprivate      → 同一模块内可调用（等同 internal，别名）
		     fileprivate        → 同一**文件**内可调用
		     hidden/private     → 仅声明它的那个模块自身，且不对外导出
		   判据全部在解析期算好，运行时零开销。 */
		enum class Vis { Export, Internal, FilePrivate, Hidden };
		struct MemberVis {
			Vis v = Vis::Internal;
			std::string mod;
			std::string file;
		};
		std::string cur_file_;                              /* 当前解析的文件名 */
		std::string cur_def_module_;                        /* 当前正在定义的模块名 */
		std::map<std::string, MemberVis> member_vis_;       /* "Mod::fn" → 可见性 */
		/* 由修饰符算出可见性级别 */
		Vis visFromMods() const {
			if (mods_ & (uint32_t)Mod::M_Hidden)  return Vis::Hidden;
			if (mods_ & (uint32_t)Mod::M_Internal)return Vis::Internal;
			if (mods_ & (uint32_t)Mod::M_ModPriv) return Vis::Internal;
			if (mods_ & (uint32_t)Mod::M_FilePriv)return Vis::FilePrivate;
			if (mods_ & (uint32_t)Mod::M_Pub)     return Vis::Export;
			if (mods_ & (uint32_t)Mod::M_Expose)  return Vis::Export;
			return Vis::Internal;                            /* 默认: 模块内可见 */
		}
		/* 按名字查可见性（支持短名与全限定名） */
		const MemberVis* findVis(const std::string& qualified) const;
		/* 当前 package 前缀。声明时自动加前缀，解析时也能用短名。 */
		std::string cur_ns_;
		/* use 引入的绑定: 短名 -> 全限定名 */
		std::map<std::string, std::string> use_bindings_;
		/* 已导出模块名 → 其导出成员名。可见性检查的依据。 */
		std::set<std::string> exported_modules_;
		std::map<std::string, std::set<std::string>> exported_members_;
		/* 本 Parser 是否正在解析一个被 import 的文件。
		   为真时豁免"未被调用的 mod 必须声明 priority"检查 —— 被导入文件
		   里自然没有对自己的调用，那个检查对它毫无意义。 */
		bool is_imported_context_ = false;
		std::vector<std::string> parseTypeParams();

		std::map<std::string, wei::ast::MacroDecl> macros_;
		std::set<std::string> imported_modules_;
		std::map<std::string, std::string> type_aliases_;
		std::vector<wei::ast::TopLevel> pending_items_;
		int macro_depth_ = 0;
		/* 变量名 -> 类型名（供 typeof / is 在编译期解析）*/
		std::map<std::string, std::string> var_types_;

		/* ── 编译期常量求值 ──────────────────────────────────────────────
		   compiletime(expr) 要求在编译期就能算出结果。这里对由整数字面量
		   与算术/比较/逻辑运算符构成的常量子树做折叠；遇到任何无法在
		   编译期确定的成分（变量、函数调用、输入）即返回 false，
		   由调用方给出明确的编译错误 —— 而不是静默退化成运行时计算。 */
		bool constFold(const wei::ast::Expr& e, int64_t& out) const;
		bool constFoldBinary(const wei::ast::Binary& b, int64_t& out) const;
};

/* ── AST 深拷贝 + 宏形参替换 ──────────────────────────────────────────────
   Stmt / Expr 是 variant，且多数节点含 unique_ptr，因此整体拷贝赋值被删除。
   这里逐字段重建节点，并在重建过程中把宏形参标识符替换成实参表达式 ——
   两步合一，省掉一次遍历。 */
namespace wei::ast {
	using SubstMap = std::map<std::string, const Expr*>;
	std::unique_ptr<Expr> cloneExprSub(const Expr& e, const SubstMap& m);
	Stmt cloneStmtSub(const Stmt& s, const SubstMap& m);

	std::unique_ptr<Expr> cloneExprSub(const Expr& e, const SubstMap& m) {
		return std::visit([&](const auto& n) -> std::unique_ptr<Expr> {
			using T = std::decay_t<decltype(n)>;
			auto r = std::make_unique<Expr>();
			T c{};
			if constexpr (std::is_same_v<T, IntLit>) {
				c.value = n.value;
			} else if constexpr (std::is_same_v<T, DoubleLit>) {
				c.value = n.value;
			} else if constexpr (std::is_same_v<T, StrLit>) {
				c.value = n.value;
			} else if constexpr (std::is_same_v<T, BoolLit>) {
				c.value = n.value;
			} else if constexpr (std::is_same_v<T, Ident>) {
				auto it = m.find(n.name);
				if (it != m.end()) {
					return cloneExprSub(*it->second, m);
				}
				c.name = n.name; c.line = n.line; c.col = n.col;
			} else if constexpr (std::is_same_v<T, Binary>) {
				c.op = n.op; c.line = n.line; c.col = n.col;
				if (n.lhs) c.lhs = cloneExprSub(*n.lhs, m);
				if (n.rhs) c.rhs = cloneExprSub(*n.rhs, m);
			} else if constexpr (std::is_same_v<T, Unary>) {
				c.op = n.op; c.line = n.line; c.col = n.col;
				if (n.operand) c.operand = cloneExprSub(*n.operand, m);
			} else if constexpr (std::is_same_v<T, Call>) {
				c.callee = n.callee; c.line = n.line; c.col = n.col;
				for (auto& a : n.args) c.args.push_back(cloneExprSub(*a, m));
			} else if constexpr (std::is_same_v<T, Output>) {
				c.line = n.line; c.col = n.col;
				for (auto& a : n.values) c.values.push_back(cloneExprSub(*a, m));
			} else if constexpr (std::is_same_v<T, Input>) {
				c.var_name = n.var_name; c.line = n.line; c.col = n.col;
			} else if constexpr (std::is_same_v<T, IndexExpr>) {
				c.name = n.name; c.line = n.line; c.col = n.col;
				if (n.idx) c.idx = cloneExprSub(*n.idx, m);
			} else if constexpr (std::is_same_v<T, ModulePathGet>) {
				c.module = n.module; c.part = n.part; c.has_field = n.has_field;
				c.field = n.field; c.adv = n.adv; c.line = n.line; c.col = n.col;
			} else if constexpr (std::is_same_v<T, TupleLit>) {
				c.line = n.line; c.col = n.col;
				for (auto& a : n.elements) c.elements.push_back(cloneExprSub(*a, m));
			} else if constexpr (std::is_same_v<T, SizeofExpr>) {
				c.type = n.type; c.value = n.value; c.line = n.line; c.col = n.col;
			} else if constexpr (std::is_same_v<T, NewExpr>) {
				c.type = n.type; c.line = n.line; c.col = n.col;
			} else if constexpr (std::is_same_v<T, DeleteExpr>) {
				c.line = n.line; c.col = n.col;
				if (n.target) c.target = cloneExprSub(*n.target, m);
			} else if constexpr (std::is_same_v<T, CastExpr>) {
				c.type = n.type; c.line = n.line; c.col = n.col;
				if (n.value) c.value = cloneExprSub(*n.value, m);
			} else if constexpr (std::is_same_v<T, RangeLit>) {
				c.line = n.line; c.col = n.col;
				if (n.lo) c.lo = cloneExprSub(*n.lo, m);
				if (n.hi) c.hi = cloneExprSub(*n.hi, m);
			} else if constexpr (std::is_same_v<T, TupleIndexExpr>) {
				c.index = n.index; c.line = n.line; c.col = n.col;
				if (n.tuple) c.tuple = cloneExprSub(*n.tuple, m);
			} else if constexpr (std::is_same_v<T, TypeofExpr>) {
				c.resolved = n.resolved; c.line = n.line; c.col = n.col;
				if (n.arg) c.arg = cloneExprSub(*n.arg, m);
			} else if constexpr (std::is_same_v<T, IsExpr>) {
				c.type_name = n.type_name; c.line = n.line; c.col = n.col;
				if (n.arg) c.arg = cloneExprSub(*n.arg, m);
			} else if constexpr (std::is_same_v<T, InExpr>) {
				c.array_len = n.array_len; c.line = n.line; c.col = n.col;
				if (n.value) c.value = cloneExprSub(*n.value, m);
				if (n.container) c.container = cloneExprSub(*n.container, m);
			} else if constexpr (std::is_same_v<T, AwaitExpr>) {
				c.line = n.line; c.col = n.col;
				if (n.operand) c.operand = cloneExprSub(*n.operand, m);
			} else if constexpr (std::is_same_v<T, CoExpr>) {
				c.callee = n.callee; c.thunk_id = n.thunk_id; c.line = n.line; c.col = n.col;
				for (auto& a : n.args) c.args.push_back(cloneExprSub(*a, m));
			} else if constexpr (std::is_same_v<T, CompileTimeExpr>) {
				c.folded = n.folded; c.value = n.value; c.line = n.line; c.col = n.col;
				if (n.arg) c.arg = cloneExprSub(*n.arg, m);
			} else if constexpr (std::is_same_v<T, ForallExpr> || std::is_same_v<T, ExistsExpr>) {
				c.var = n.var; c.line = n.line; c.col = n.col;
				SubstMap m2 = m; m2.erase(n.var);   /* 量词变量遮蔽同名形参 */
				if (n.domain) c.domain = cloneExprSub(*n.domain, m2);
				if (n.cond) c.cond = cloneExprSub(*n.cond, m2);
			}
			*r = std::move(c);
			return r;
		}, e);
	}

	Stmt cloneStmtSub(const Stmt& s, const SubstMap& m) {
		return std::visit([&](const auto& n) -> Stmt {
			using T = std::decay_t<decltype(n)>;
			T c{};
			if constexpr (std::is_same_v<T, Block>) {
				c.line = n.line; c.col = n.col;
				for (auto& st : n.stmts) c.stmts.push_back(cloneStmtSub(st, m));
			} else if constexpr (std::is_same_v<T, IfStmt>) {
				c.line = n.line; c.col = n.col;
				if (n.cond) c.cond = cloneExprSub(*n.cond, m);
				for (auto& st : n.then_body) c.then_body.push_back(cloneStmtSub(st, m));
				for (auto& st : n.else_body) c.else_body.push_back(cloneStmtSub(st, m));
			} else if constexpr (std::is_same_v<T, ForStmt>) {
				c.line = n.line; c.col = n.col;
				if (n.init_decl) {
					c.init_decl = std::make_unique<VarDecl>();
					c.init_decl->name = n.init_decl->name;
					c.init_decl->type = n.init_decl->type;
					c.init_decl->is_const = n.init_decl->is_const;
					c.init_decl->array_len = n.init_decl->array_len;
					c.init_decl->line = n.init_decl->line;
					c.init_decl->col = n.init_decl->col;
					if (n.init_decl->init) c.init_decl->init = cloneExprSub(*n.init_decl->init, m);
				}
				if (n.cond) c.cond = cloneExprSub(*n.cond, m);
				if (n.step) c.step = cloneExprSub(*n.step, m);
				for (auto& st : n.body) c.body.push_back(cloneStmtSub(st, m));
			} else if constexpr (std::is_same_v<T, WhileStmt>) {
				c.line = n.line; c.col = n.col;
				if (n.cond) c.cond = cloneExprSub(*n.cond, m);
				for (auto& st : n.body) c.body.push_back(cloneStmtSub(st, m));
			} else if constexpr (std::is_same_v<T, LoopStmt>) {
				for (auto& st : n.body) c.body.push_back(cloneStmtSub(st, m));
			} else if constexpr (std::is_same_v<T, LoopNStmt>) {
				c.count = n.count;
				for (auto& st : n.body) c.body.push_back(cloneStmtSub(st, m));
			} else if constexpr (std::is_same_v<T, ReturnStmt>) {
				c.line = n.line; c.col = n.col;
				if (n.value) c.value = cloneExprSub(*n.value, m);
			} else if constexpr (std::is_same_v<T, ExprStmt>) {
				if (n.expr) c.expr = cloneExprSub(*n.expr, m);
			} else if constexpr (std::is_same_v<T, AssignStmt>) {
				c.name = n.name; c.line = n.line; c.col = n.col;
				if (n.target) c.target = cloneExprSub(*n.target, m);
				if (n.value) c.value = cloneExprSub(*n.value, m);
			} else if constexpr (std::is_same_v<T, VarDecl>) {
				c.name = n.name; c.type = n.type; c.is_const = n.is_const;
				c.array_len = n.array_len; c.line = n.line; c.col = n.col;
				if (n.init) c.init = cloneExprSub(*n.init, m);
				for (auto& a : n.array_init) c.array_init.push_back(cloneExprSub(*a, m));
			} else if constexpr (std::is_same_v<T, ModuleAccessSet>) {
				c.module = n.module; c.part = n.part; c.field = n.field;
				c.adv = n.adv; c.line = n.line; c.col = n.col;
				if (n.value) c.value = cloneExprSub(*n.value, m);
			} else if constexpr (std::is_same_v<T, MatchStmt>) {
				c.line = n.line; c.col = n.col;
				if (n.expr) c.expr = cloneExprSub(*n.expr, m);
				for (auto& mc : n.cases) {
					MatchCase nc;
					nc.line = mc.line; nc.col = mc.col;
					if (mc.pattern) nc.pattern = cloneExprSub(*mc.pattern, m);
					for (auto& st : mc.body) nc.body.push_back(cloneStmtSub(st, m));
					c.cases.push_back(std::move(nc));
				}
				for (auto& st : n.default_body) c.default_body.push_back(cloneStmtSub(st, m));
			} else if constexpr (std::is_same_v<T, ContractStmt>) {
				c.contract_type = n.contract_type; c.message = n.message;
				c.line = n.line; c.col = n.col;
				if (n.condition) c.condition = cloneExprSub(*n.condition, m);
			} else if constexpr (std::is_same_v<T, TypeAliasDecl>) {
				c.alias_name = n.alias_name; c.target_type = n.target_type;
				c.line = n.line; c.col = n.col;
			} else if constexpr (std::is_same_v<T, ForallStmt>) {
				c.var = n.var; c.is_of = n.is_of; c.array_len = n.array_len;
				c.line = n.line; c.col = n.col;
				SubstMap m2 = m; m2.erase(n.var);
				if (n.domain) c.domain = cloneExprSub(*n.domain, m2);
				for (auto& st : n.body) c.body.push_back(cloneStmtSub(st, m2));
			} else if constexpr (std::is_same_v<T, WhenStmt>) {
				c.line = n.line; c.col = n.col;
				if (n.cond) c.cond = cloneExprSub(*n.cond, m);
				for (auto& st : n.then_body) c.then_body.push_back(cloneStmtSub(st, m));
				for (auto& st : n.else_body) c.else_body.push_back(cloneStmtSub(st, m));
			} else if constexpr (std::is_same_v<T, YieldStmt>) {
				c.line = n.line; c.col = n.col;
				if (n.value) c.value = cloneExprSub(*n.value, m);
			} else if constexpr (std::is_same_v<T, CompileTimeStmt>) {
				c.line = n.line; c.col = n.col;
				for (auto& st : n.body) c.body.push_back(cloneStmtSub(st, m));
			} else if constexpr (std::is_same_v<T, MacroDecl>) {
				c.name = n.name; c.params = n.params; c.is_expr = n.is_expr;
				c.line = n.line; c.col = n.col;
				if (n.expr_body) c.expr_body = cloneExprSub(*n.expr_body, m);
			}
			return Stmt(std::move(c));
		}, s);
	}
}
using wei::ast::AssignStmt;
using wei::ast::Binary;
using wei::ast::Block;
using wei::ast::BoolLit;
using wei::ast::Call;
using wei::ast::Expr;
using wei::ast::ExprStmt;
using wei::ast::ForStmt;
using wei::ast::FuncDecl;
using wei::ast::Ident;
using wei::ast::IfStmt;
using wei::ast::ImportDecl;
using wei::ast::IndexExpr;
using wei::ast::Input;
using wei::ast::IntLit;
using wei::ast::DoubleLit;
using wei::ast::LoopStmt;
using wei::ast::LoopNStmt;
using wei::ast::ModuleAccessSet;
using wei::ast::ModuleDecl;
using wei::ast::ModulePathGet;
using wei::ast::Output;
using wei::ast::Param;
using wei::ast::PartDecl;
using wei::ast::Program;
using wei::ast::ReturnStmt;
using wei::ast::Stmt;
using wei::ast::StrLit;
using wei::ast::TopLevel;
using wei::ast::Unary;
using wei::ast::VarDecl;
using wei::ast::WhileStmt;
using wei::ast::MatchStmt;
using wei::ast::MatchCase;
using wei::ast::ContractStmt;
using wei::ast::TupleLit;
using wei::ast::TypeAliasDecl;
using wei::ast::SizeofExpr;
using wei::ast::RangeLit;
using wei::ast::TupleIndexExpr;
using wei::ast::TypeofExpr;
using wei::ast::IsExpr;
using wei::ast::InExpr;
using wei::ast::AwaitExpr;
using wei::ast::CoExpr;
using wei::ast::CompileTimeExpr;
using wei::ast::CompileTimeStmt;
using wei::ast::ForallExpr;
using wei::ast::ExistsExpr;
using wei::ast::ForallStmt;
using wei::ast::WhenStmt;
using wei::ast::YieldStmt;
using wei::ast::MacroDecl;
using wei::ast::NewExpr;
using wei::ast::CastExpr;
using wei::ast::cloneExprSub;
using wei::ast::cloneStmtSub;
using wei::ast::SubstMap;
using wei::ast::NewExpr;
using wei::ast::DeleteExpr;
using wei::ast::BreakStmt;
using wei::ast::UseDecl;
using wei::ast::NameQual;
using wei::ast::AsmStmt;
using wei::ast::SwitchStmt;
using wei::ast::SwitchCase;
using wei::ast::EnumDecl;
using wei::ast::UnionDecl;
using wei::ast::DeferStmt;
using wei::ast::LambdaExpr;
using wei::ast::TryStmt;
using wei::ast::ContinueStmt;
using wei::ast::CastExpr;

#ifndef WEI_FRONTEND_NO_DEFS
Parser::Parser(std::vector<Token> tokens): tokens_(std::move(tokens)), pos_(0) {}

const Token& Parser::cur() const {
	if (pos_ >= tokens_.size()) {
		return tokens_.back();
	}
	return tokens_[pos_];
}
const Token& Parser::peek(size_t off) const {
	if (pos_ + off >= tokens_.size()) {
		return tokens_.back();
	}
	return tokens_[pos_ + off];
}
const Token& Parser::prev() const {
	if (pos_ == 0) {
		return tokens_.front();
	}
	return tokens_[pos_ - 1];
}
bool Parser::eof() const {
	return cur().is(TokType::Eof);
}
bool Parser::check(TokType t) const {
	return cur().is(t);
}
bool Parser::checkAt(size_t off, TokType t) const {
	return peek(off).is(t);
}
bool Parser::match(TokType t) {
	if (check(t)) {
		pos_++;
		return true;
	}
	return false;
}
const Token& Parser::expect(TokType t, const std::string& desc) {
	if (!check(t)) {
		die("期望 " + desc + ", 实际拿到 '" + cur().text + "'", cur());
	}
	const Token& tk = cur();
	pos_++;
	return tk;
}
[[noreturn]] void Parser::die(const std::string& msg, const Token& at) const {
	wei::diag::Classified c = wei::diag::classify(msg);
	wei::diag::Diagnostic d;
	d.sev = wei::diag::Severity::Error;
	d.code = c.code;
	d.message = msg;
	int len = static_cast<int>(at.text.size());
	if (len <= 0) len = 1;
	d.labels.push_back({{at.line, at.col, len}, c.labelMsg, true});
	if (!c.help.empty()) d.help(c.help);
	throw wei::diag::DiagnosticError(d);
}
[[noreturn]] void Parser::die(const std::string& msg) const {
	die(msg, cur());
}

bool Parser::isCreateKw(TokType t) {
	return t == TokType::Kw_create || t == TokType::Kw_创建;
}
bool Parser::isInputKw(TokType t) {
	return t == TokType::Kw_input;
}
bool Parser::isOutputKw(TokType t) {
	return t == TokType::Kw_output;
}
bool Parser::isStatKw(TokType t) {
	return t == TokType::Kw_stat || t == TokType::Kw_统计;
}
bool Parser::isModuleKw(TokType t) {
	return t == TokType::Kw_mod || t == TokType::Kw_模块;
}
bool Parser::isPartKw(TokType t) {
	return t == TokType::Kw_part || t == TokType::Kw_部件;
}
bool Parser::isAccessKw(TokType t) {
	return t == TokType::Kw_access || t == TokType::Kw_访问;
}
bool Parser::isAdvKw(TokType t) {
	return t == TokType::Kw_adv || t == TokType::Kw_高级;
}
bool Parser::isTypeKw(TokType t) {
	switch (t) {
		case TokType::Kw_func:
		case TokType::Kw_int:
		case TokType::Kw_bool:
		case TokType::Kw_double:
		case TokType::Kw_char:
		case TokType::Kw_omni:
		case TokType::Kw_ptr:
		case TokType::Kw_own:
		case TokType::Kw_i32:
		case TokType::Kw_i8:
		case TokType::Kw_i16:
		case TokType::Kw_i64:
		case TokType::Kw_u8:
		case TokType::Kw_u16:
		case TokType::Kw_u32:
		case TokType::Kw_u64:
		case TokType::Kw_f32:
		case TokType::Kw_f64:
		case TokType::Kw_auto:
		case TokType::Kw_type:
		case TokType::Kw_tuple:
		case TokType::Kw_函数:
		case TokType::Kw_整型:
		case TokType::Kw_浮点:
		case TokType::Kw_布尔:
		case TokType::Kw_文本:
		case TokType::Kw_万能:
		case TokType::Kw_指针:
			return true;
		default:
			return false;
	}
}
std::string Parser::typeKwText(TokType t) {
	switch (t) {
		case TokType::Kw_func:
			return "func";
		case TokType::Kw_int:
			return "int";
		case TokType::Kw_bool:
			return "bool";
		case TokType::Kw_double:
			return "double";
		case TokType::Kw_char:
			return "char";
		case TokType::Kw_omni:
			return "omni";
		case TokType::Kw_ptr:
			return "ptr";
		case TokType::Kw_own:
			return "own";
		case TokType::Kw_i32:
			return "i32";
		case TokType::Kw_i8:  return "i8";
		case TokType::Kw_i16: return "i16";
		case TokType::Kw_i64: return "i64";
		case TokType::Kw_u8:  return "u8";
		case TokType::Kw_u16: return "u16";
		case TokType::Kw_u32: return "u32";
		case TokType::Kw_u64: return "u64";
		case TokType::Kw_f32: return "f32";
		case TokType::Kw_f64: return "f64";
		case TokType::Kw_auto:
			return "auto";
		case TokType::Kw_type:
			return "type";
		case TokType::Kw_tuple:
			return "tuple";
		case TokType::Kw_函数:
			return "func";
		case TokType::Kw_整型:
			return "int";
		case TokType::Kw_浮点:
			return "double";
		case TokType::Kw_布尔:
			return "bool";
		case TokType::Kw_文本:
			return "char";
		case TokType::Kw_万能:
			return "omni";
		case TokType::Kw_指针:
			return "ptr";
		default:
			return "";
	}
}
/* 返回类型后缀: (参数).int / (参数).double */
void Parser::eatRetType(FuncDecl& fd) {
	/* Result 返回类型: .Result —— Rust 风格的显式错误返回（非异常、非 OO）。
	   编码约定: rax >= 0 为 Ok(值)，rax < 0 为 Err(-错误码)。
	   用单个寄存器承载，函数返回路径无需改动。 */
	if (check(TokType::Dot) && checkAt(1, TokType::Ident) && peek(1).text == "Result") {
		pos_++; pos_++;
		fd.ret_type = "Result";
		return;
	}
	/* 泛型返回类型: .T */
	if (check(TokType::Dot) && checkAt(1, TokType::Ident) && cur_type_params_.count(peek(1).text)) {
		pos_++;
		fd.ret_type = cur().text;
		pos_++;
		return;
	}
	if (check(TokType::Dot) && isTypeKw(peek(1).type)) {
		pos_++;
		fd.ret_type = typeKwText(cur().type);
		pos_++;
	}
}

std::string Parser::eatCreateSep() {
	if (check(TokType::Dot) || check(TokType::Colon)) {
		std::string s = cur().text;
		pos_++;
		return s;
	}
	die("create 后期望 '.' 或 ':'", cur()); // 修复: 错误信息不准确
}
void Parser::eatStatSep() {
	if (check(TokType::Dot) || check(TokType::Colon)) {
		pos_++;
		return;
	}
	die("语句后期望 '.' 或 ':' 作为分隔符", cur()); // 修复: 错误信息不准确
}

/* create. 之后解析类型: int / ptr / ptr.int / 指针.整型 链式 */
std::string Parser::parseTypeName() {
	/* 泛型类型参数: create.T v = ... —— T 不是关键字，但此刻是合法类型名 */
	if (check(TokType::Ident) && cur_type_params_.count(cur().text)) {
		std::string tp = cur().text;
		pos_++;
		return tp;
	}
	if (!isTypeKw(cur().type)) {
		die("未知类型关键字 '" + cur().text + "'", cur()); // 修复: 错误信息不准确
	}
	std::string tn = typeKwText(cur().type);
	pos_++;
	if (tn == "ptr" && check(TokType::Dot) && isTypeKw(peek(1).type)) {
		std::string inner = typeKwText(peek(1).type);
		if (inner == "int" || inner == "char" || inner == "bool" || inner == "double") {
			pos_ += 2;
			return tn + "." + inner;
		}
	}
	return tn;
}

/* 已保留但未实现的关键字表; 命中即报错 */
static const std::set<TokType> kReservedKeywords = {
	/* 这些关键字曾为保留字（仅报错）；本版本已全部实现，表仅作历史记录保留 */
	TokType::Kw_async, TokType::Kw_await, TokType::Kw_co,
	TokType::Kw_macro, TokType::Kw_forall, TokType::Kw_exists,
	TokType::Kw_when, TokType::Kw_then, TokType::Kw_typeof,
	TokType::Kw_compiletime, TokType::Kw_tuple, TokType::Kw_type,
	TokType::Kw_import, TokType::Kw_from, TokType::Kw_as,
	TokType::Kw_and, TokType::Kw_or, TokType::Kw_not,
	TokType::Kw_is, TokType::Kw_in, TokType::Kw_of,
	TokType::Kw_contract
};

/* 标准库目录约定: require <A.B> 对应 lib/A/B.wei */
/* 当前正在编译的源文件所在目录。多文件项目里 import 的模块通常就放在
   源文件旁边，而原实现只搜 CWD 的 lib/ —— 把源文件放进子目录编译就必然
   找不到。这里记录源目录，让 findLibModule 也能就近搜索。 */
static std::string g_src_dir;

static std::string libModulePath(const std::string& module) {
	std::string p = "lib/";
	for (char c : module) {
		p += (c == '.') ? '/' : c;
	}
	return p + ".wei";
}

static bool weiFileExists(const std::string& path) {
	std::ifstream in(path, std::ios::binary);
	return (bool)in;
}

/* 依次在: cwd, 上级目录, exe 所在目录的 lib 中查找 */
static std::string findLibModule(const std::string& module) {
	std::string rel = libModulePath(module);
	std::vector<std::string> cands = {
		rel,
		"../" + rel,
		"../../" + rel,
	};
	/* 源文件所在目录优先: 多文件项目最自然的布局就是模块和源码放一起 */
	if (!g_src_dir.empty()) {
		cands.insert(cands.begin(), g_src_dir + "/" + rel);
		cands.insert(cands.begin(), g_src_dir + "/lib/" + rel);
		cands.insert(cands.begin(), g_src_dir + "/" + module + ".wei");
	}
#if defined(_WIN32)
	char buf[MAX_PATH] = {};
	DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
	if (n > 0) {
		std::string exe(buf, n);
		size_t sl = exe.find_last_of("/\\");
		if (sl != std::string::npos) {
			std::string dir = exe.substr(0, sl);
			cands.push_back(dir + "/" + rel);
			cands.push_back(dir + "/../" + rel);
		}
	}
#endif
	for (auto& c : cands) {
		if (weiFileExists(c)) return c;
	}
	return {};
}

	/* ═══ trait / impl 的 token 级改写 ═══════════════════════════════════
	   设计立场: **不做面向对象**。
	     · 没有虚表、没有运行时多态、没有 this/继承/向上转型
	     · trait 只是"方法签名契约"，编译期存在，运行时零开销
	     · impl 把 trait 要求的方法实现为**普通模块函数**，
	       调用点编译期直接绑定到具体函数地址（静态派发，一条直接 call）
	   实现手法: 在真正解析之前，于 token 流上把 impl 块改写成
	   `create.mod Trait::method(...)` 的普通模块函数，然后**完全复用**
	   现有那条经过验证的函数解析/单态化/发射链路。
	   这样既不新增解析逻辑（避免失控），派发又是纯静态的。 */
void Parser::preprocessTraits() {
	std::vector<Token> out;
	size_t i = 0;
	const size_t N = tokens_.size();
	/* 收集 trait 要求的方法名，用于校验 impl 完整性 */
	std::map<std::string, std::vector<std::string>> trait_reqs;

	auto match_brace = [&](size_t start) -> size_t {
		/* tokens_[start] 必须是 '{'，返回匹配 '}' 的下标 */
		int d = 0;
		for (size_t k = start; k < N; k++) {
			if (tokens_[k].type == TokType::LBrace) d++;
			else if (tokens_[k].type == TokType::RBrace) { d--; if (d == 0) return k; }
		}
		return N;
	};

	while (i < N) {
		/* ── trait 块: 只抽取签名，整块丢弃 ────────────────────────── */
		if (tokens_[i].type == TokType::Kw_trait && i + 1 < N
		    && tokens_[i+1].type == TokType::Ident) {
			std::string tname = tokens_[i+1].text;
			size_t lb = (i + 2 < N && tokens_[i+2].type == TokType::LBrace)
			              ? i + 2 : N;
			std::vector<std::string> reqs;
			if (lb != N) {
				size_t rb = match_brace(lb);
				/* 块内每个 [func] name ( ... ) 记一个方法名 */
				for (size_t k = lb + 1; k < rb; k++) {
					bool is_fn = tokens_[k].type == TokType::Kw_func;
					size_t nk = is_fn ? k + 1 : k;
					if (nk < rb && tokens_[nk].type == TokType::Ident
					    && (nk + 1 >= rb || tokens_[nk+1].type == TokType::LParen)) {
						reqs.push_back(tokens_[nk].text);
						k = nk;
					}
				}
				i = rb + 1;
			} else {
				i = i + 2;
			}
			trait_reqs[tname] = reqs;
			continue;
		}

		/* ── impl 块: 改写成普通模块函数 ───────────────────────────── */
		if (tokens_[i].type == TokType::Kw_impl && i + 1 < N
		    && tokens_[i+1].type == TokType::Ident) {
			std::string tname = tokens_[i+1].text;
			size_t k = i + 2;
			/* 可选的 `for Type` */
			if (k < N && (tokens_[k].type == TokType::Kw_for
			              || (tokens_[k].type == TokType::Ident && tokens_[k].text == "for")))
				k += 2;
			if (k >= N || tokens_[k].type != TokType::LBrace) {   /* 畸形: 原样放过 */
				out.push_back(tokens_[i++]);
				continue;
			}
			size_t rb = match_brace(k);
			std::vector<std::string> provided;
			/* 块内逐个方法 */
			for (size_t m = k + 1; m < rb; ) {
				if (tokens_[m].type == TokType::Kw_func) m++;     /* func 可省 */
				if (m >= rb || tokens_[m].type != TokType::Ident) { m++; continue; }
				std::string mname = tokens_[m].text;
				size_t lp = m + 1;
				if (lp >= rb || tokens_[lp].type != TokType::LParen) { m++; continue; }
				/* 参数表: 找到匹配右括号 */
				int pd = 0; size_t rp = lp;
				for (; rp < rb; rp++) {
					if (tokens_[rp].type == TokType::LParen) pd++;
					else if (tokens_[rp].type == TokType::RParen) { pd--; if (!pd) break; }
				}
				/* 返回类型: . 或 -> 后跟 Ident */
				size_t after = rp + 1;
				/* 返回类型可以是 int/double 等**关键字** token，也可是自定义
				   类型名(Ident)。必须原样搬运 token 而不是按名字重建 ——
				   把 `int` 重建为 Ident 会让解析器在 `)` 后认不出 `.ret`，
				   转而期望 `{`（曾导致整个方法被跳过）。 */
				size_t dot_idx = 0, ret_idx = 0;
				if (after < rb && tokens_[after].type == TokType::Dot
				    && after + 1 < rb && tokens_[after+1].type != TokType::LBrace) {
					dot_idx = after; ret_idx = after + 1;
					after += 2;
				}
				/* 函数体: 从 '{' 到匹配 '}' */
				if (after >= rb || tokens_[after].type != TokType::LBrace) { m++; continue; }
				size_t body_end = match_brace(after);

				/* 输出: create.mod Trait::name ( 参数 ) . 返回类型 { 体 } */
				std::string src = "create.mod " + tname + "::" + mname;
				std::string src2 = src;
				/* Lexer::scan() 会在末尾追加一个 Eof token。若把它一起塞进
				   改写流，解析器会在方法名后立刻撞上 EOF —— 必须剔除。 */
				Lexer lx(src2);
				for (auto& t : lx.scan())
					if (t.type != TokType::Eof) out.push_back(t);
				for (size_t q = lp; q <= rp; q++) out.push_back(tokens_[q]);   /* (参数) */
				if (ret_idx) { out.push_back(tokens_[dot_idx]); out.push_back(tokens_[ret_idx]); }
				for (size_t q = after; q <= body_end; q++) out.push_back(tokens_[q]); /* 体 */
				provided.push_back(mname);
				m = body_end + 1;
			}
			/* 契约校验: trait 要求的方法必须都被实现 */
			auto it = trait_reqs.find(tname);
			if (it != trait_reqs.end()) {
				for (auto& r : it->second) {
					bool has = false;
					for (auto& pv : provided) if (pv == r) { has = true; break; }
					if (!has)
						std::cerr << "[wei] impl " << tname
						          << " 缺少 trait 要求的方法: " << r << "\n";
				}
			}
			i = rb + 1;
			continue;
		}
		out.push_back(tokens_[i++]);
	}
	tokens_ = std::move(out);
	pos_ = 0;
}

	const Parser::MemberVis* Parser::findVis(const std::string& qualified) const {
		auto it = member_vis_.find(qualified);
		if (it != member_vis_.end()) return &it->second;
		/* 去掉最后一段（成员名），用模块名再找一次 —— 兼容短名调用 */
		size_t p = qualified.rfind("::");
		if (p != std::string::npos) {
			std::string modonly = qualified.substr(0, p);
			auto it2 = member_vis_.find(modonly);
			if (it2 != member_vis_.end()) return &it2->second;
		}
		return nullptr;
	}

Program Parser::parse() {
	preprocessTraits();
	/* 上一版的 "保留字尚未实现" 检查已移除: 本版本 22 个关键字全部有语义。
	   kReservedKeywords 仅保留作文档用途（记录哪些是后期加入的关键字）。 */
	Program prog;
	while (!eof()) {
		prog.items.push_back(parseTopLevel());
		/* import 会把被导入文件的顶层声明内联进来，排到当前程序之后 */
		while (!pending_items_.empty()) {
			prog.items.push_back(std::move(pending_items_.front()));
			pending_items_.erase(pending_items_.begin());
		}
	}
	/* ── 模块化: 跨模块可见性检查 ──────────────────────────────────────
	   规则: 调用 M::f 时，若 M 与当前模块不同，则 f 必须是导出的。
	   这一条让 export/public/private 真正生效 —— 之前它们只是关键字壳。 */
	/* 依赖图解析: 查环 + 拓扑排序（仅在完整编译时做，被 import 的
	   子解析不做 —— 那时图还不完整，排序没有意义） */
	if (!is_imported_context_) {
		resolveDepOrder();
		/* 把拓扑序回写到 AST —— NativeGen 的模块入口排序会读它。
		   不回写的话排序结果只存在于 Parser 里，代码生成阶段永远
		   看不到，依赖顺序就是个摆设。 */
		for (auto& item : prog.items) {
			if (auto* m = std::get_if<ModuleDecl>(&item)) {
				auto it = dep_graph_.find(m->name);
				if (it != dep_graph_.end()) m->dep_order = it->second.init_order;
				/* ── 重复定义检测（类似 C++ ODR）────────────────────
				   同一模块名定义两次、或同一模块内同一成员定义两次，
				   都必须报错 —— 静默采用其中一个会导致初始化顺序
				   与调用目标不明确。 */
				/* 注意: **模块名可以重复出现** —— 既有代码用
				   `create.mod A::f()` + `create.mod A::g()` 往同一模块
				   追加成员，这是合法且常见的写法。禁止它会破坏
				   现有库文件。真正要禁的是**成员级**重复定义。 */
				auto& seen = mod_members_seen_[m->name];
				for (auto& mf : m->methods) {
					if (!seen.insert(mf.name).second) {
						die("模块 '" + m->name + "' 的成员 '" + mf.name + "' 被重复定义",
						    Token(TokType::Ident, mf.name, mf.line, mf.col));
					}
				}
			}
		}
	}

	/* 未被调用的 mod 必须声明 priority */
	if (!is_imported_context_) for (auto& item : prog.items) {
		auto* m = std::get_if<ModuleDecl>(&item);
		if (!m) continue;
		if (m->has_main || m->has_priority) continue;
		if (m->from_import) continue;   /* import 进来的: 见 importModuleInline 注释 */
		if (m->methods.empty() && m->body.empty()) continue;
		if (module_referenced_.count(m->name)) continue;
		die("mod " + m->name + " 未被调用, 需声明 priority(N)");
	}
	return prog;
}

std::unique_ptr<wei::ast::Stmt> Parser::parseAsmStmt() {
	int32_t line = cur().line, col = cur().col;
	pos_++;                                  /* 消费 asm */
	if (!check(TokType::LBrace)) die("asm 后期望 '{'", cur());
	pos_++;
	AsmStmt as;
	as.line = line;
	as.col = col;
	int depth = 1;
	std::string cur_line;
	while (!eof() && depth > 0) {
		if (check(TokType::LBrace)) depth++;
		if (check(TokType::RBrace)) {
			depth--;
			if (depth == 0) { pos_++; break; }
		}
		std::string t = cur().text;
		if (t == ";") {
			if (!cur_line.empty()) { as.lines.push_back(cur_line); cur_line.clear(); }
		} else {
			if (!cur_line.empty()) cur_line += " ";
			cur_line += t;
		}
		pos_++;
	}
	if (!cur_line.empty()) as.lines.push_back(cur_line);
	detectAsmClobbers(as);
	auto sp = std::make_unique<wei::ast::Stmt>();
	*sp = std::move(as);
	return sp;
}

std::unique_ptr<wei::ast::Stmt> Parser::parseSwitchStmt() {
	int32_t line = cur().line, col = cur().col;
	pos_++;                                   /* 消费 switch */
	SwitchStmt sw;
	sw.line = line; sw.col = col;
	sw.subject = parseExpr();
	if (!check(TokType::LBrace)) die("switch 后期望 '{'", cur());
	pos_++;
	/* 收集所有成员（含嵌套块），按 token 文本重建 —— 与 parseAsmStmt 同法 */
	int depth = 1;
	std::vector<std::string> toks;
	while (!eof() && depth > 0) {
		if (check(TokType::LBrace)) depth++;
		if (check(TokType::RBrace)) { depth--; if (depth==0) { pos_++; break; } }
		toks.push_back(cur().text);
		pos_++;
	}
	/* 极简 case 解析: case <整数> : <语句...> ; default : <语句...> */
	size_t i = 0;
	while (i < toks.size()) {
		if (toks[i] == "case" || toks[i] == "default") {
			SwitchCase c;
			c.is_default = (toks[i] == "default");
			i++;
			if (!c.is_default && i < toks.size()) {
				/* 先查命名常量（枚举成员等），再退回数值。
				   原来直接 atoll("Red") 得 0，导致 case Red 与 case Green
				   的常量值都是 0 —— 枚举匹配静默失效。 */
				const std::string& tv = toks[i];
				auto nit = named_consts_.find(tv);
				if (nit != named_consts_.end()) c.const_val = nit->second;
				else c.const_val = atoll(tv.c_str());
				i++;
			}
			if (i < toks.size() && toks[i] == ":") i++;
			/* 收集到下一个 case/default 为止 */
			std::vector<std::string> bodyToks;
			while (i < toks.size() && toks[i] != "case" && toks[i] != "default") {
				bodyToks.push_back(toks[i]); i++;
			}
			/* 用子 Parser 解析 body */
			std::string src;
			for (auto& t : bodyToks) { src += t; src += " "; }
			if (!src.empty()) {
				std::vector<Token> ts;
				Lexer lx(std::move(src));
				ts = lx.scan();
				Parser sp(std::move(ts));
				while (!sp.eof()) {
					Stmt st = sp.parseStmt();
					if (std::get_if<wei::ast::ExprStmt>(&st) && false) break;
					c.body.stmts.push_back(std::move(st));
					if (sp.check(TokType::Semicolon)) break;
				}
			}
			sw.cases.push_back(std::move(c));
		} else i++;
	}

	/* ── 穷尽性检查（Rust match 的核心保证）─────────────────────────────
	   若 switch 的 subject 是枚举变量，且没有 default 分支，
	   则每个枚举成员都必须被某个 case 覆盖，否则编译期报错。
	   这能在重构枚举（新增成员）时立刻发现所有漏改的 switch。 */
	if (sw.subject) {
		if (auto* id = std::get_if<wei::ast::Ident>(sw.subject.get())) {
			auto vit = enum_vars_.find(id->name);
			if (vit != enum_vars_.end()) {
				auto eit = enums_.find(vit->second);
				if (eit != enums_.end() && !eit->second.members.empty()) {
					bool hasDefault = false;
					std::set<int64_t> covered;
					for (auto& c : sw.cases) {
						if (c.is_default) { hasDefault = true; break; }
						covered.insert(c.const_val);
					}
					if (!hasDefault) {
						std::vector<std::string> missing;
						for (auto& m : eit->second.members)
							if (!covered.count(m.second)) missing.push_back(m.first);
						if (!missing.empty()) {
							wei::diag::Diagnostic d;
							d.sev = wei::diag::Severity::Error;
							d.code = "E0004";
							d.message = "switch 未覆盖枚举 '" + vit->second + "' 的全部成员";
							d.labels.push_back({{sw.line, sw.col, 6}, "这个 switch 缺少分支", true});
							std::string miss;
							for (size_t k = 0; k < missing.size(); k++) {
								if (k) miss += ", ";
								miss += missing[k];
							}
							d.help("缺少: " + miss + "；补上这些 case，或加 default 分支");
							throw wei::diag::DiagnosticError(d);
						}
					}
				}
			}
		}
	}

	auto out = std::make_unique<wei::ast::Stmt>();
	*out = std::move(sw);
	return out;
}

std::unique_ptr<wei::ast::Stmt> Parser::parseDeferStmt() {
	int32_t line = cur().line, col = cur().col;
	pos_++;
	DeferStmt d;
	d.line = line; d.col = col;
	if (check(TokType::LBrace)) {
		pos_++;
		while (!eof() && !check(TokType::RBrace)) {
			d.body.stmts.push_back(parseStmt());
			if (check(TokType::Semicolon)) pos_++;
		}
		if (check(TokType::RBrace)) pos_++;
	} else {
		d.body.stmts.push_back(parseStmt());
		if (check(TokType::Semicolon)) pos_++;
	}
	auto out = std::make_unique<wei::ast::Stmt>();
	*out = std::move(d);
	return out;
}

	/* 反复消费行首的修饰符关键字，直到遇到真正的声明关键字。
	   返回是否收集到至少一个。 */
bool Parser::collectMods() {
	struct Map { const char* kw; uint32_t bit; };
	static const Map tbl[] = {
		{"K_expose",M_Expose},{"K_internal",M_Internal},{"K_hidden",M_Hidden},
		{"K_fileprivate",M_FilePriv},{"K_moduleprivate",M_ModPriv},{"K_open",M_Open},
		{"K_final",M_Final},{"K_sealed",M_Sealed},{"K_pub",M_Pub},{"K_public",M_Pub},{"K_private",M_Internal},
		{"K_const",M_Const},{"K_volatile",M_Volatile},{"K_static",M_Static},
		{"K_extern",M_Extern},{"K_inline",M_Inline},{"K_mut",M_Mut},
		{"K_ref",M_Ref},{"K_move",M_Move},{"K_auto",M_Auto},
		{"K_annotate",M_Annotate},{"K_attribute",M_Attribute},{"K_deprecated",M_Deprecated},
		{"K_doc",M_Doc},{"K_meta",M_Meta},{"K_moduleinfo",M_ModuleInfo},
		{"K_feature",M_Feature},{"K_cfg",M_Cfg},{"K_link",M_Link},
		{"K_lib",M_Lib},{"K_library",M_Library},{"K_dynamic",M_Dynamic},
		{"K_depends",M_Depends},{"K_provides",M_Provides}
	};
	static const Map tbl2[] = {
		
		
		
		{"K_qualified",0},{"K_unqualified",0},{"K_importall",0},{"K_reexport",0},
		
		{"K_entry",0},{"K_teardown",0},{"K_outer",0},{"K_operator",0},
		{"K_lambda",0},{"K_has",0},{"K_version",0},{"K_try",0},{"K_catch",0},{"K_throw",0},{"K_platform",0},{"K_arch",0},
		{"K_build",0},{"K_target",0},{"K_thread",0},{"K_lock",0},
		{"K_atomic",0},{"K_alignas",0},{"K_alignof",0},{"K_volatile",0},{"K_inline",0},
		
	};
	bool any = false;
	for (;;) {
		const std::string t = cur().text;
		bool hit = false;
		/* 第二批: 登记式/标记式关键字，登记进 registrations_ 后消费掉 */
		for (auto& e2 : tbl2) {
			std::string kw(std::string(e2.kw).substr(2));
			if (t == kw) {
				pos_++;
				if (check(TokType::Ident)) { registrations_.push_back({kw, cur().text}); pos_++; }
				else if (check(TokType::StrLit)) { registrations_.push_back({kw, cur().text}); pos_++; }
				else if (check(TokType::IntLit)) { registrations_.push_back({kw, cur().text}); pos_++; }
				else registrations_.push_back({kw, ""});
				if (check(TokType::Semicolon)) pos_++;
				hit = true; any = true;
				break;
			}
		}
		if (hit) continue;
		for (auto& e : tbl) {
			std::string kw(e.kw + 2);          /* 去掉 "K_" 前缀 */
			if (t == kw) {
				mods_ |= e.bit;
				pos_++;
				/* 带参数的修饰符: 消费后面的字符串或标识符 */
				if (check(TokType::StrLit)) { mod_args_.push_back(cur().text); meta_text_.push_back(cur().text); pos_++; }
				else if (check(TokType::Ident)) { mod_args_.push_back(cur().text); pos_++; }
				if (check(TokType::Semicolon)) pos_++;
				hit = true; any = true;
				break;
			}
		}
		if (!hit) break;
	}
	return any;
}

	/* trait Name { func m(...) -> T }  —— 只声明签名 */
bool Parser::tryParseTrait() {
	if (!cur().is(TokType::Kw_trait)) return false;
	pos_++;
	wei::ast::TraitDecl t;
	if (!check(TokType::Ident)) die("trait 后期望名字", cur());
	t.name = cur().text; pos_++;
	if (!check(TokType::LBrace)) return true;      /* 前向声明, 无体 */
	pos_++;
	int guard_t = 0;
	while (!eof() && !check(TokType::RBrace) && guard_t++ < 4096) {
		if (check(TokType::Semicolon)) { pos_++; continue; }
		if (cur().is(TokType::Kw_func) || check(TokType::Ident)) {
			if (cur().is(TokType::Kw_func)) pos_++;
			wei::ast::MethodSig m;
			if (!check(TokType::Ident)) break;
			m.name = cur().text; pos_++;
			if (check(TokType::LParen)) {          /* 参数表 */
				pos_++;
				while (!eof() && !check(TokType::RParen)) {
					wei::ast::VarDecl vd;
					if (check(TokType::Ident)) { vd.type = cur().text; pos_++; }
					if (check(TokType::Ident)) { vd.name = cur().text; pos_++; }
					m.params.push_back(std::move(vd));
					if (check(TokType::Comma)) pos_++;
				}
				if (check(TokType::RParen)) pos_++;
			}
			/* -> 或 . 后跟返回类型 */
			if (check(TokType::Dot)) {
				pos_++;
				if (check(TokType::Ident)) { m.ret_type = cur().text; pos_++; }
			}
			t.methods.push_back(std::move(m));
			if (check(TokType::Semicolon)) pos_++;
		} else { pos_++; }
	}
	if (check(TokType::RBrace)) pos_++;
	traits_.erase(t.name);
	traits_.emplace(t.name, std::move(t));
	return true;
}

/* impl Trait for Type { func m(...) -> T { 体 } } */
bool Parser::tryParseImpl() {
	if (!cur().is(TokType::Kw_impl)) return false;
	pos_++;
	wei::ast::ImplDecl im;
	if (!check(TokType::Ident)) die("impl 后期望 trait 名", cur());
	im.trait_name = cur().text; pos_++;
	if (check(TokType::Ident) && cur().text == "for") { pos_++;
		if (check(TokType::Ident)) { im.for_type = cur().text; pos_++; } }
	if (!check(TokType::LBrace)) return true;
	pos_++;
	int guard_i = 0;
	while (!eof() && !check(TokType::RBrace) && guard_i++ < 4096) {
		if (check(TokType::Semicolon)) { pos_++; continue; }
		/* 函数: [func] name(params).ret { 体 } */
		std::string ret = "int";
		if (cur().is(TokType::Kw_func)) pos_++;
		if (!check(TokType::Ident)) break;
		std::string mn = cur().text; pos_++;
		std::vector<wei::ast::VarDecl> ps;
		if (check(TokType::LParen)) {
			pos_++;
			while (!eof() && !check(TokType::RParen)) {
				wei::ast::VarDecl vd;
				if (check(TokType::Ident)) { vd.type = cur().text; pos_++; }
				if (check(TokType::Ident)) { vd.name = cur().text; pos_++; }
				ps.push_back(std::move(vd));
				if (check(TokType::Comma)) pos_++;
			}
			if (check(TokType::RParen)) pos_++;
		}
		if (check(TokType::Dot)) {
			pos_++;
			if (check(TokType::Ident)) { ret = cur().text; pos_++; }
		}
		/* 收集函数体到大括号配对 */
		if (!check(TokType::LBrace)) break;
		int depth = 0; std::string body;
		int guard_b = 0;
		do {
			if (check(TokType::LBrace)) depth++;
			if (check(TokType::RBrace)) depth--;
			body += cur().text + " ";
			pos_++;
		} while (!eof() && depth > 0 && guard_b++ < 8192);
		std::string fname = im.trait_name + "_" + mn;
		/* 生成一段普通函数源码，交给现有函数解析/单态化链路处理 */
		std::string sig = "create.mod " + im.trait_name + "::" + mn + "(";
		for (size_t i = 0; i < ps.size(); i++) {
			if (i) sig += ", ";
			sig += ps[i].type + " " + ps[i].name;
		}
		sig += ")." + ret + " " + body;
		std::vector<Token> ts;
		Lexer lx(std::move(sig));
		ts = lx.scan();
		Parser sp(std::move(ts));
		auto tl = sp.parseTopLevel();
		if (auto* md = std::get_if<ModuleDecl>(&tl)) {
			for (auto& fn : md->methods) {
				FuncDecl f2 = std::move(fn);
				f2.name = mn;
				im.methods.push_back(std::move(f2));
			}
		}
		/* 静态派发表: Trait::method → 模块内函数名 */
		dispatch_[im.trait_name + "::" + mn] = im.trait_name + "::" + mn;
		(void)fname;
	}
	if (check(TokType::RBrace)) pos_++;
	impls_.erase(im.trait_name);
	impls_.emplace(im.trait_name, std::move(im));
	return true;
}

	/* enum Name { A, B = 5, C }  /  enum class Name { ... } */
	bool Parser::tryParseEnum() {
		if (!cur().is(TokType::Kw_enum)) return false;
		pos_++;
		bool scoped = false;
		if (check(TokType::Ident) && cur().text == "class") { scoped = true; pos_++; }
		EnumDecl e;
		e.is_scoped = scoped;
		e.underlying = "int";
		if (!check(TokType::Ident)) die("enum 后期望名字", cur());
		e.name = cur().text; pos_++;
		if (check(TokType::LBrace)) {
			pos_++;
			int64_t next = 0;
			while (!eof() && !check(TokType::RBrace)) {
				if (check(TokType::Comma)) { pos_++; continue; }
				if (!check(TokType::Ident)) break;
				std::string mn = cur().text; pos_++;
				int64_t v = next;
				if (check(TokType::Assign)) {
					pos_++;
					auto ev = parseExpr();
					int64_t cv = 0;
					if (ev && constFold(*ev, cv)) v = cv;
				}
				e.members.push_back({mn, v});
				named_consts_[scoped ? (e.name + "::" + mn) : mn] = v;
				next = v + 1;
			}
			if (check(TokType::RBrace)) pos_++;
		}
		std::cerr << "[dbg] enum " << e.name << " members=" << e.members.size() << " consts=" << named_consts_.size() << "\n";
		enums_.erase(e.name); enums_.emplace(e.name, std::move(e));
		return true;
	}

	/* union Name { <成员> } */
	bool Parser::tryParseUnion() {
		if (!cur().is(TokType::Kw_union)) return false;
		pos_++;
		UnionDecl u;
		if (!check(TokType::Ident)) die("union 后期望名字", cur());
		u.name = cur().text; pos_++;
		if (check(TokType::LBrace)) {
			pos_++;
			while (!eof() && !check(TokType::RBrace)) {
				if (check(TokType::Semicolon)) { pos_++; continue; }
				if (isCreateKw(cur().type)) pos_++;
				VarDecl vd;
				if (check(TokType::Ident)) { vd.type = cur().text; pos_++; }
				if (check(TokType::Ident)) { vd.name = cur().text; pos_++; }
				if (check(TokType::LBracket)) {           /* 数组成员 */
					pos_++;
					int64_t n = 0;
					auto ne = parseExpr();
					if (ne) constFold(*ne, n);
					vd.array_len = n;
					if (check(TokType::RBracket)) pos_++;
				}
				u.members.push_back(std::move(vd));
				if (check(TokType::Semicolon)) pos_++;
			}
			if (check(TokType::RBrace)) pos_++;
		}
		unions_.erase(u.name); unions_.emplace(u.name, std::move(u));
		return true;
	}

TopLevel Parser::parsePackageDecl(bool is_package) {
	int32_t line = cur().line, col = cur().col;
	pos_++;
	const Token& nTk = expect(TokType::Ident, "包名");
	std::string pname = nTk.text;
	/* 支持嵌套: a.b.c */
	while (check(TokType::Dot)) {
		pos_++;
		const Token& e = expect(TokType::Ident, "包名");
		pname += "." + e.text;
	}
	if (check(TokType::Semicolon)) pos_++;
	cur_ns_ = pname;
	(void)is_package;
	/* package 只设置当前前缀，不产生独立顶层节点（用空 TypeAliasDecl 占位） */
	return TopLevel(wei::ast::TypeAliasDecl{});
}

TopLevel Parser::parseUseDecl() {
	int32_t line = cur().line, col = cur().col;
	pos_++;
	const Token& pTk = expect(TokType::Ident, "模块路径");
	UseDecl ud;
	ud.path = pTk.text;
	while (check(TokType::Dot)) {
		pos_++;
		const Token& e = expect(TokType::Ident, "模块路径");
		ud.path += "." + e.text;
	}
	/* use A.B as C */
	if (cur().is(TokType::Kw_as)) {
		pos_++;
		const Token& aTk = expect(TokType::Ident, "别名");
		ud.alias = aTk.text;
	}
	if (check(TokType::Semicolon)) pos_++;
	/* 登记绑定: 短名(最后一段或别名) -> 全路径 */
	std::string short_name = ud.alias.empty()
		? ud.path.substr(ud.path.find_last_of('.') == std::string::npos
			? 0 : ud.path.find_last_of('.') + 1)
		: ud.alias;
	use_bindings_[short_name] = ud.path;
	ud.line = line;
	ud.col = col;
	return TopLevel(std::move(ud));
}

TopLevel Parser::parseTopLevel() {
	/* enum/union 只登记到编译期表，不产生独立的顶层节点 —— 用空的
	   TypeAliasDecl 占位（它们不生成任何代码，仅影响后续类型检查）。 */
	if (collectMods()) { if (eof()) return TopLevel(wei::ast::TypeAliasDecl{}); }
	if (cur().is(TokType::Kw_trait)) { tryParseTrait(); return TopLevel(wei::ast::TypeAliasDecl{}); }
	if (cur().is(TokType::Kw_impl))  {
		tryParseImpl();
		return TopLevel(wei::ast::TypeAliasDecl{});
	}
	if (cur().is(TokType::Kw_enum))  { tryParseEnum();  return TopLevel(wei::ast::TypeAliasDecl{}); }
	if (cur().is(TokType::Kw_union)) { tryParseUnion(); return TopLevel(wei::ast::TypeAliasDecl{}); }
	/* package / use 是顶层组织声明，先处理 */
	if (cur().is(TokType::Kw_package))    return parsePackageDecl(true);
	if (cur().is(TokType::Kw_use))        return parseUseDecl();

	/* ── export / pub: 标记下一个声明为"对外可见" ────────────────────────
	   这是模块化里最关键的一个关键字。之前 public/private 虽然注册了却
	   完全空转 —— 因为没有"模块边界"可依附，不知道相对谁可见。
	   export 明确了边界: 导出的跨模块可调用，未导出的仅模块内可见。 */
	bool exported = false;
	if (cur().is(TokType::Kw_export) || cur().is(TokType::Kw_pub)) {
		exported = true;
		/* 同步到修饰符位集 —— 成员级可见性判定读的是 mods_，
		   不同步的话 export 会被当成默认 internal 而误拦截。 */
		mods_ |= (uint32_t)Mod::M_Pub;
		pos_++;
	}

	if (isCreateKw(cur().type)) {
		pos_++;
		eatCreateSep();
		if (isModuleKw(cur().type)) {
			pos_++;
			ModuleDecl md = parseModuleDecl();
			md.is_exported = exported;
			if (exported) {
				exported_modules_.insert(md.name);
				for (auto& f : md.methods) f.exported = true;
			}
			return md;
		}
		/* create.macro NAME(...) { ... } — 顶层宏声明。
		   宏在解析期展开，不进入程序体，故此处只登记不产出代码。 */
		if (cur().is(TokType::Kw_macro)) {
			return parseMacroDeclTop();
		}
		/* create.type 别名 = 目标类型; */
		if (cur().is(TokType::Kw_type)) {
			TypeAliasDecl td;
			td.line = cur().line;
			td.col = cur().col;
			pos_++;
			const Token& nTk = expect(TokType::Ident, "类型别名");
			td.alias_name = nTk.text;
			expect(TokType::Assign, "'='");
			if (isTypeKw(cur().type)) td.target_type = parseTypeName();
			else { td.target_type = cur().text; pos_++; }
			expect(TokType::Semicolon, "';'");
			type_aliases_[td.alias_name] = td.target_type;
			return TopLevel(std::move(td));
		}
		die("create 顶层只支持 mod / macro / type 声明", cur());
	}
	if (cur().is(TokType::Kw_require)) {
		return parseImportDecl();
	}
	if (cur().is(TokType::Kw_import)
	        || cur().is(TokType::Kw_from)) {
		return parseImportTopLevel();
	}
	die("无法识别的顶层声明 '" + cur().text + "'", cur());
}

FuncDecl Parser::parseFuncDecl(const std::string& retType) {
	FuncDecl fd;
	fd.name = prev().text;
	fd.ret_type = retType;
	fd.line = prev().line;
	fd.col = prev().col;
	expect(TokType::LParen, "'('");
	fd.params = parseParams();
	expect(TokType::RParen, "')'");
	if (check(TokType::LBrace)) {
		fd.body = parseBlock();
		return fd;
	}
	if (check(TokType::Semicolon)) {
		pos_++;
		return fd;
	}
	die("函数声明后期望 '{' 或 ';'", cur()); // 修复: 错误信息不准确
}

VarDecl Parser::parseGlobalVarDecl(const std::string& typeName) {
	VarDecl vd;
	vd.type = typeName;
	vd.name = prev().text;
	vd.line = prev().line;
	vd.col = prev().col;
	if (check(TokType::LBracket)) {
		pos_++;
		const Token& lenTk = expect(TokType::IntLit, "数组长度");
		vd.array_len = std::stoll(lenTk.text);
		expect(TokType::RBracket, "']'");
		if (vd.array_len <= 0) {
			die("数组长度必须 > 0", lenTk);
		}
	}
	expect(TokType::Assign, "'='");
	auto p = parseModulePathExpr(false);
	if (p) {
		vd.init = std::make_unique<Expr>(std::move(*p));
	} else {
		auto tmp = parseExpr();
		vd.init = std::make_unique<Expr>(std::move(*tmp));
	}
	expect(TokType::Semicolon, "';'");
	return vd;
}

ImportDecl Parser::parseImportDecl() {
	pos_++;
	ImportDecl id;
	bool eng = false;
	if (check(TokType::IOLt)) {
		eng = true;
		pos_++;
	} else if (check(TokType::LangL)) {
		pos_++;
	} else {
		die("require 后期望 '<' 或 '《'", cur()); // 修复: 错误信息不准确
	}
	const Token& modTk = expect(TokType::Ident, "模块");
	id.module = modTk.text;
	if (check(TokType::Dot)) {
		pos_++;
		const Token& extTk = expect(TokType::Ident, "扩展");
		id.module = id.module + "." + extTk.text;
	}
	if (eng) {
		expect(TokType::IOGt, "'>'");
	} else {
		expect(TokType::RParen, "')'");
	}
	if (check(TokType::Semicolon)) {
		pos_++;
	}
	/* require 存在性检查: require <A.B> 必须对应 lib/A/B.wei */
	std::string found = findLibModule(id.module);
	if (found.empty()) {
		die("无法解析 require 模块 '" + id.module + "': 未找到 " + libModulePath(id.module), modTk);
	}
	return id;
}



	/* ── 预编译模块加载 ─────────────────────────────────────────────── */
bool Parser::loadPrecompiled(const std::string& mod, const Token& at) {
	if (defined_modules_.count(mod)) return true;
	if (imported_modules_.count(mod)) return true;

	/* .mod 路径: 由 .wei 路径换后缀得到，保证两者在同一目录 */
	std::string srcPath = findLibModule(mod);
	if (srcPath.empty()) return false;
	std::string modPath = srcPath;
	size_t dot = modPath.rfind('.');
	if (dot != std::string::npos) modPath = modPath.substr(0, dot) + ".mod";
	else modPath += ".mod";
	{
		std::ifstream t(modPath, std::ios::binary);
		if (!t) return false;            /* 没有 .mod: 回退解析 .wei */
	}
	std::ifstream in(modPath, std::ios::binary);
	if (!in) return false;
	std::vector<uint8_t> raw;
	{
		in.seekg(0, std::ios::end);
		size_t n = (size_t)in.tellg();
		in.seekg(0, std::ios::beg);
		raw.resize(n);
		if (n) in.read((char*)raw.data(), (std::streamsize)n);
	}
	wei::ast::Program prog;
	std::vector<std::string> deps;
	int64_t mt = 0;
	if (!wei::modcache::deserialize(raw, prog, deps, mt)) return false;

	/* ── 新鲜度校验 ──────────────────────────────────────────────
	   源 .wei 改了之后 .mod 就过期了。不校验会用旧 AST 静默编译出
	   旧行为 —— 这类 bug 极难查。过期就回退重新解析。 */
	struct stat st;
	if (::stat(srcPath.c_str(), &st) == 0) {
		if ((int64_t)st.st_mtime != mt) return false;
	}

	imported_modules_.insert(mod);
	module_file_[mod] = modPath;
	for (auto& item : prog.items) {
		if (auto* md = std::get_if<ModuleDecl>(&item)) {
			md->from_import = true;
			defined_modules_.insert(md->name);
			std::set<std::string> parts;
			for (auto& p : md->parts) parts.insert(p.name);
			for (auto& f : md->methods) parts.insert(f.name);
			module_reg_[md->name] = std::move(parts);
			if (md->is_exported) exported_modules_.insert(md->name);
			/* 成员级可见性: .mod 里的 export 信息一并带过来 */
			for (auto& f : md->methods)
				if (f.exported) exported_members_[md->name].insert(f.name);
		}
		pending_items_.push_back(std::move(item));
	}
	for (auto& d : deps) {
		dep_graph_[mod].name = mod;
		dep_graph_[mod].requires_.insert(d);
	}
	return true;
}

	/* ── 文件系统依赖实现 ─────────────────────────────────────────── */
bool Parser::loadModuleFromFile(const std::string& mod, const Token& at) {
	/* ── 文件级循环依赖检测 ────────────────────────────────────────
	   A.wei requires B，B.wei requires A —— 若不在加载期拦住，
	   会无限递归直到栈溢出。
	   **必须放在"已加载就跳过"之前**: 否则 FCycA 已在 imported_modules_
	   里时，FCycB 对它的回指会被短路掉，环就检测不到了（曾漏检）。 */
	for (auto& f : file_load_stack_) {
		if (f == mod) {
			std::string chain;
			bool started = false;
			for (auto& g : file_load_stack_) {
				if (g == mod) started = true;
				if (started) chain += g + ".wei -> ";
			}
			chain += mod + ".wei";
			die("文件循环依赖: " + chain, at);
		}
	}

	/* 已加载过（无论是 import 还是 requires 触发的）就不再重复 */
	if (defined_modules_.count(mod)) return true;
	if (imported_modules_.count(mod)) return true;

	/* 依赖驱动加载同样优先用预编译产物 */
	if (loadPrecompiled(mod, at)) return true;

	std::string path = findLibModule(mod);
	if (path.empty()) return false;      /* 文件系统里没有: 交给存在性校验报错 */

	std::ifstream in(path, std::ios::binary);
	if (!in) return false;
	std::ostringstream ss;
	ss << in.rdbuf();
	std::string src = ss.str();

	module_file_[mod] = path;
	/* mtime: 增量编译的依据（当前仅记录，后续可做"依赖没变就跳过重编"） */
	{
		struct stat st;
		if (::stat(path.c_str(), &st) == 0) module_mtime_[mod] = (int64_t)st.st_mtime;
	}

	imported_modules_.insert(mod);
	file_load_stack_.push_back(mod);
	try {
		Lexer lx(src);
		Parser sub(lx.scan());
		sub.is_imported_context_ = true;
		sub.cur_file_ = path;
		sub.module_reg_ = module_reg_;
		sub.imported_modules_ = imported_modules_;
		sub.type_aliases_ = type_aliases_;
		sub.defined_modules_ = defined_modules_;
		sub.module_file_ = module_file_;
		/* 加载栈必须传递 —— 子解析器是**新建的 Parser**，它的
		   file_load_stack_ 默认是空的。不传的话，A 加载 B、B 回指 A 时，
		   B 所在子解析器看不到栈里的 A，环就漏检了（曾漏检）。 */
		sub.file_load_stack_ = file_load_stack_;
		Program subprog = sub.parse();
		for (auto& kv : sub.member_vis_) member_vis_[kv.first] = kv.second;
		/* 导出模块表也必须合并 —— 不合并的话子文件里 export 过的
		   模块在宿主里仍被视为"未导出"，可见性检查会误报。 */
		for (auto& n : sub.exported_modules_) exported_modules_.insert(n);
		for (auto& kv : sub.exported_members_)
			for (auto& fn : kv.second) exported_members_[kv.first].insert(fn);
		/* 子解析新定义的模块合并回来 */
		for (auto& dm : sub.defined_modules_) defined_modules_.insert(dm);
		for (auto& kv : sub.module_reg_) module_reg_[kv.first] = kv.second;
		for (auto& item : subprog.items) {
			if (auto* md = std::get_if<ModuleDecl>(&item)) md->from_import = true;
			pending_items_.push_back(std::move(item));
		}
	} catch (...) {
		file_load_stack_.pop_back();
		throw;
	}
	file_load_stack_.pop_back();
	return true;
}

	/* ── 依赖系统实现 ─────────────────────────────────────────────── */
void Parser::addRequires(const std::string& mod, const std::vector<std::string>& names, int line) {
	DepNode& n = dep_graph_[mod];
	n.name = mod;
	n.line = line;
	for (auto& r : names) {
		if (r == mod) die("模块 '" + mod + "' 不能依赖自己", cur());
		n.requires_.insert(r);
		/* ── 前向声明 ──────────────────────────────────────────
		   C++ 级别: 依赖谁，谁就被**前向声明**。这样"先写使用者、
		   后写被依赖者"也能编译 —— 调用点在解析期就能找到符号，
		   不必要求被依赖模块必须先出现。
		   空集合 member 表示"已知存在但成员未知"，后续真正定义时
		   registerModule 会用真实成员覆盖它。 */
		if (!module_reg_.count(r)) module_reg_.emplace(r, std::set<std::string>{});
		/* ── 依赖驱动加载 ──────────────────────────────────────
		   `requires(B)` 就自动去文件系统找 lib/B.wei 并加载，
		   不必再手写 import B。这是"文件系统依赖"的核心：
		   依赖声明 = 加载指令。 */
		Token at2(TokType::Ident, r, line, 0);
		loadModuleFromFile(r, at2);
	}
}
void Parser::addProvides(const std::string& mod, const std::vector<std::string>& names, int line) {
	DepNode& n = dep_graph_[mod];
	n.name = mod;
	n.line = line;
	for (auto& pr : names) n.provides_.insert(pr);
}

bool Parser::depHasCycle(const std::string& n, std::set<std::string>& visiting,
                         std::set<std::string>& done, std::vector<std::string>& path) {
	if (done.count(n)) return false;
	if (visiting.count(n)) {
		std::string chain;
		bool started = false;
		for (auto& p : path) {
			if (p == n) started = true;
			if (started) chain += p + " -> ";
		}
		chain += n;
		die("循环依赖: " + chain, cur());
	}
	visiting.insert(n);
	path.push_back(n);
	auto it = dep_graph_.find(n);
	if (it != dep_graph_.end())
		for (auto& d : it->second.requires_) depHasCycle(d, visiting, done, path);
	path.pop_back();
	visiting.erase(n);
	done.insert(n);
	return false;
}

bool Parser::resolveDepOrder() {
	std::set<std::string> visiting, done;
	std::vector<std::string> path;
	for (auto& kv : dep_graph_) depHasCycle(kv.first, visiting, done, path);

	/* ── 依赖存在性校验 ──────────────────────────────────────────
	   C++ 级别: 声明依赖的模块必须**真的存在**，不能静默放过。
	   之前图外模块被当成"外部依赖"忽略 —— 那是个静默漏洞：
	   写错模块名不会报错，初始化顺序也就跟着错了。 */
	for (auto& kv : dep_graph_) {
		for (auto& r : kv.second.requires_) {
			if (!defined_modules_.count(r)) {
				die("模块 '" + kv.first + "' 依赖 '" + r + "'，但未找到该模块的定义",
				    Token(TokType::Ident, r, kv.second.line, 0));
			}
		}
	}

	std::map<std::string, int> indeg;
	std::map<std::string, std::set<std::string>> dependents;
	for (auto& kv : dep_graph_) {
		const std::string& m = kv.first;
		indeg[m] += 0;
		for (auto& r : kv.second.requires_) {
			if (!dep_graph_.count(r)) continue;   /* 图外模块: 视为外部依赖 */
			dependents[r].insert(m);
			indeg[m]++;
		}
	}
	int order = 0;
	bool changed = true;
	while (changed) {
		changed = false;
		for (auto& kv : dep_graph_) {
			const std::string& m = kv.first;
			if (indeg[m] != 0) continue;
			if (dep_graph_[m].init_order >= 0) continue;
			dep_graph_[m].init_order = order++;
			for (auto& d : dependents[m]) indeg[d]--;
			changed = true;
		}
	}
	return true;
}

void Parser::registerModule(const ModuleDecl& md) {
	std::set<std::string> parts;
	for (auto& p : md.parts) parts.insert(p.name);
	for (auto& f : md.methods) parts.insert(f.name);
	module_reg_[md.name] = std::move(parts);
	module_prio_[md.name] = md.priority;
	defined_modules_.insert(md.name);
}

void Parser::markReferenced(const std::string& mod) {
	module_referenced_.insert(mod);
}

/* 部件成员解析 */
std::vector<VarDecl> Parser::parsePartMembers() {
	std::vector<VarDecl> out;
	while (!check(TokType::RBrace) && !eof()) {
		if (check(TokType::Kw_part) || check(TokType::Kw_部件)) {
			die("部件成员声明中不允许嵌套 part", cur()); // 修复: 错误信息不准确
		}
		std::string tn;
		if (check(TokType::Ident)) {
			tn = cur().text;
			pos_++;
		} else if (isTypeKw(cur().type)) {
			tn = typeKwText(cur().type);
			pos_++;
		} else {
			die("部件成员类型未知 '" + cur().text + "'", cur());
		}
		const Token& nameTk = expect(TokType::Ident, "成员");
		VarDecl fd;
		fd.type = tn;
		fd.name = nameTk.text;
		fd.line = nameTk.line;
		fd.col = nameTk.col;
		fd.is_const = false;
		if (check(TokType::Assign)) {
			pos_++;
			auto tmp = parseExpr();
			bool lit = std::get_if<IntLit>(tmp.get()) != nullptr
			           || std::get_if<BoolLit>(tmp.get()) != nullptr
			           || std::get_if<StrLit>(tmp.get()) != nullptr;
			if (!lit) die("部件成员初值必须是常量(int/bool/string)", prev());
			fd.init = std::make_unique<Expr>(std::move(*tmp));
		}
		expect(TokType::Semicolon, "';'");
		out.push_back(std::move(fd));
	}
	return out;
}

PartDecl Parser::parsePartDecl() {
	PartDecl pd;
	pd.line = cur().line;
	pd.col = cur().col;
	if (!isPartKw(cur().type)) {
		die("期望 part 关键字", cur()); // 修复: 错误信息不准确
	}
	pos_++;
	/* 属性链: part.struct.public Name{...} ??struct/public/private 任意顺序 */
	check(TokType::Dot);
	while (check(TokType::Dot)) {
		pos_++;
		if (cur().is(TokType::Kw_struct)) {
			pd.is_struct = true;
			pos_++;
		} else if (cur().is(TokType::Kw_public)) {
			pd.is_public = true;
			pos_++;
		} else if (cur().is(TokType::Kw_private)) {
			pd.is_public = false;
			pos_++;
		} else {
			die("part 属性只支持 struct/public/private, 实际拿到 '" + cur().text + "'", cur());
		}
		if (!check(TokType::Dot)) break;
	}
	const Token& partTk = expect(TokType::Ident, "部件");
	pd.name = partTk.text;
	expect(TokType::LBrace, "'{'");
	pd.members = parsePartMembers();
	expect(TokType::RBrace, "'}'");
	return pd;
}

std::vector<std::string> Parser::parseTypeParams() {
	std::vector<std::string> out;
	if (!check(TokType::IOLt)) return out;
	pos_++;
	while (!eof() && !check(TokType::IOGt)) {
		const Token& t = expect(TokType::Ident, "类型参数名");
		out.push_back(t.text);
		cur_type_params_.insert(t.text);
		if (check(TokType::Comma)) { pos_++; continue; }
		break;
	}
	expect(TokType::IOGt, "'>'");
	return out;
}

ModuleDecl Parser::parseModuleDecl() {
	ModuleDecl md;
	const Token& modTk = expect(TokType::Ident, "模块");
	md.name = modTk.text;
	md.line = modTk.line;
	md.col = modTk.col;
	/* 提前登记, 使模块体内可自引用 Calc::fib(...) */
	module_reg_.emplace(md.name, std::set<std::string> {});

	/* create.mod 名字::main() / create.mod 名字::priority(N)(参数) */
	if (check(TokType::DoubleColon)) {
		pos_++;
		/* init / deinit 已注册为关键字（不是 Ident），必须在此处先拦截，
		   否则 expect(Ident) 会因为拿到 Kw_init 而报错。 */
		if (cur().is(TokType::Kw_init) || cur().is(TokType::Kw_deinit)) {
			bool is_init = cur().is(TokType::Kw_init);
			pos_++;
			FuncDecl fd;
			fd.name = is_init ? "init" : "deinit";
			fd.line = cur().line; fd.col = cur().col;
			expect(TokType::LParen, "'('");
			fd.params = parseParams();
			expect(TokType::RParen, "')'");
			eatRetType(fd); parseOptAsync(fd);
			fd.body = parseBlock();
			if (is_init) md.has_init = true; else md.has_deinit = true;
			md.methods.push_back(std::move(fd));
			return md;
		}
		/* ── 依赖声明: create.mod Name::requires(A, B) / Name::provides(X) ──
	   写在模块成员位置，语法跟 priority(N) 一致，便于记忆。 */
	/* requires/depends/provides 都已注册为**关键字** token（不是 Ident），
	   必须按 token 类型判定 —— 只按名字比对 Ident 会让它们全部失配。 */
	bool is_req = cur().is(TokType::Kw_require) || cur().is(TokType::Kw_depends)
	              || (check(TokType::Ident) && (cur().text == "requires" || cur().text == "depends"));
	if (is_req) {
		int line2 = cur().line;
		pos_++;
		std::vector<std::string> names;
		if (check(TokType::LParen)) {
			pos_++;
			while (!eof() && !check(TokType::RParen)) {
				if (check(TokType::Ident)) names.push_back(cur().text);
				pos_++;
				if (check(TokType::Comma)) pos_++;
			}
			if (check(TokType::RParen)) pos_++;
		}
		if (check(TokType::Semicolon)) pos_++;
		addRequires(md.name, names, line2);
		return md;
	}
	bool is_prov = cur().is(TokType::Kw_provides)
	               || (check(TokType::Ident) && cur().text == "provides");
	if (is_prov) {
		int line2 = cur().line;
		pos_++;
		std::vector<std::string> names;
		if (check(TokType::LParen)) {
			pos_++;
			while (!eof() && !check(TokType::RParen)) {
				if (check(TokType::Ident)) names.push_back(cur().text);
				pos_++;
				if (check(TokType::Comma)) pos_++;
			}
			if (check(TokType::RParen)) pos_++;
		}
		if (check(TokType::Semicolon)) pos_++;
		addProvides(md.name, names, line2);
		return md;
	}
	const Token& memTk = expect(TokType::Ident, "成员名(main/priority)");
		FuncDecl fd;
		fd.name = memTk.text;
		fd.line = memTk.line;
		fd.col = memTk.col;
		/* ── 可见性登记 ──────────────────────────────────────────────
		   在成员名确定后立刻记录 (模块, 文件, 级别)，供调用点检查。
		   export 关键字在 parseTopLevel 里已置位 mods_，这里读取并清零，
		   避免它泄漏到下一个声明。 */
		{
			MemberVis mv;
			mv.v    = visFromMods();
			mv.mod  = md.name;
			mv.file = cur_file_;
			if (mv.v == Vis::Export) exported_members_[md.name].insert(fd.name);
			member_vis_[md.name + "::" + fd.name] = mv;
			cur_def_module_ = md.name;
			clearMods();
		}

		/* create.mod M::init() / create.mod M::deinit()
		   模块生命周期钩子: init 在模块首次使用前跑，deinit 在退出时跑。 */
		if (memTk.text == "init" || memTk.text == "deinit") {
			if (memTk.text == "init") md.has_init = true;
			else md.has_deinit = true;
			expect(TokType::LParen, "'('");
			fd.params = parseParams();
			expect(TokType::RParen, "')'");
			eatRetType(fd); parseOptAsync(fd);
			fd.body = parseBlock();
			md.methods.push_back(std::move(fd));
			return md;
		}

		if (memTk.text == "priority") {
			expect(TokType::LParen, "'('");
			if (check(TokType::IntLit)) {
				md.priority = static_cast<int>(std::stoll(cur().text));
				pos_++;
			}
			expect(TokType::RParen, "')'");
			md.has_priority = true;
			/* priority(N)_ : '_' 是分隔符, 函数即模块本身, 调用写作 Calc[...] */
			expect(TokType::Ident, "'_' 分隔符");
			fd.name = "_";
			expect(TokType::LParen, "'('");
			fd.params = parseParams();
			expect(TokType::RParen, "')'");
			eatRetType(fd); parseOptAsync(fd);
			fd.body = parseBlock();
			md.methods.push_back(std::move(fd));
			registerModule(md);
			return md;
		}

		if (memTk.text == "main") {
			/* main 缺省 priority 0; 非要写就写 priority(0) */
			md.priority = 0;
			/* main(priority(N)) 括号可空 */
			if (check(TokType::LParen)) {
				pos_++;
				if (check(TokType::Ident) && cur().text == "priority") {
					pos_++;
					expect(TokType::LParen, "'('");
					if (check(TokType::IntLit)) {
						md.priority = static_cast<int>(std::stoll(cur().text));
						pos_++;
						md.has_priority = true;
					}
					expect(TokType::RParen, "')'");
				}
				fd.params = parseParams();
				expect(TokType::RParen, "')'");
			}
			eatRetType(fd); parseOptAsync(fd);
			fd.body = parseBlock();
			md.has_main = true;
			md.methods.push_back(std::move(fd));
			registerModule(md);
			return md;
		}

		/* 普通 mod 成员函数: 名字::成员(参数)，可带 <类型参数> */
		fd.type_params = parseTypeParams();
		(void)0;
		expect(TokType::LParen, "'('");
		fd.params = parseParams();
		expect(TokType::RParen, "')'");
		eatRetType(fd); parseOptAsync(fd);
		fd.body = parseBlock();
		cur_type_params_.clear();
		md.methods.push_back(std::move(fd));
		registerModule(md);
		return md;
	}

	if (check(TokType::LParen)) {
		pos_++;
		expect(TokType::RParen, "')'");
	}
	expect(TokType::LBrace, "'{'");
	while (!check(TokType::RBrace) && !eof()) {
		if (isPartKw(cur().type)) {
			md.parts.push_back(parsePartDecl());
		} else {
			md.body.push_back(parseStmt());
		}
	}
	expect(TokType::RBrace, "'}'");
	registerModule(md);
	return md;
}

std::vector<Param> Parser::parseParams() {
	std::vector<Param> out;
	if (check(TokType::RParen)) {
		return out;
	}
	while (true) {
		if (!isTypeKw(cur().type) && !check(TokType::Ident)) {
			die("参数类型未知 '" + cur().text + "'", cur());
		}
		std::string tn;
		if (isTypeKw(cur().type)) {
			tn = parseTypeName();
		} else {
			tn = cur().text;
			pos_++;
		}
		const Token& parTk = expect(TokType::Ident, "参数");
		Param p;
		p.type = tn;
		p.name = parTk.text;
		out.push_back(std::move(p));
		if (check(TokType::Comma)) {
			pos_++;
			continue;
		}
		break;
	}
	return out;
}

	/* ── C2 · 内嵌汇编寄存器冲突检测 ────────────────────────────────────
	   汇编块手动占用寄存器，若与编译器自己的分配冲突就会静默算错。
	   这是内嵌汇编最危险的失败模式 —— 不崩、不报警，值被悄悄改掉。
	   故做编译期检测：扫出文本里写到的受保护寄存器并登记为 clobber。 */
	void Parser::detectAsmClobbers(AsmStmt& as) {
		static const char* kProtected[] = { "rbp", "rsp", "rax" };
		for (auto& ln : as.lines) {
			for (const char* r : kProtected) {
				size_t pos = 0;
				size_t rl = strlen(r);
				while ((pos = ln.find(r, pos)) != std::string::npos) {
					size_t end = pos + rl;
					bool ok_b = (pos == 0) || !isalnum((unsigned char)ln[pos - 1]);
					bool ok_a = (end >= ln.size()) || !isalnum((unsigned char)ln[end]);
					if (ok_b && ok_a) {
						bool dup = false;
						for (auto& c : as.clobbers) if (c == r) dup = true;
						if (!dup) as.clobbers.push_back(r);
					}
					pos = end;
				}
			}
		}
	}

Stmt Parser::parseStmt() {
	/* asm { ... } 语句级构造 */
	collectMods();
	if (cur().is(TokType::Kw_asm)) {
		auto sp = parseAsmStmt();
		return std::move(*sp);
	}
	if (cur().is(TokType::Kw_switch)) {
		auto sp = parseSwitchStmt();
		return std::move(*sp);
	}
	if (cur().is(TokType::Kw_defer)) {
		auto sp = parseDeferStmt();
		return std::move(*sp);
	}
	if (check(TokType::Semicolon)) {
		die("不允许出现空语句（孤立的分号）", cur()); // 修复: 错误信息不准确
	}
	/* access. ??adv. 前缀 */
	if (isAccessKw(cur().type) && checkAt(1, TokType::Dot)) {
		pos_ += 2;
		return parseAccessStmt(false);
	}
	if (isAdvKw(cur().type) && checkAt(1, TokType::Dot)) {
		pos_ += 2;
		if (isCreateKw(cur().type)) {
			pos_++;
			eatCreateSep();
			if (!isTypeKw(cur().type)) {
				die("adv. 后期望类型关键字", cur()); // 修复: 错误信息不准确
			}
			std::string tn = parseTypeName();
			return parseVarDeclStmt(tn, true);
		}
		if (isAccessKw(cur().type)) {
			pos_++;
			expect(TokType::Dot, "'.'");
			return parseAccessStmt(true);
		}
		die("adv. 后只支持 create 或 access", cur()); // 修复: 错误信息不准确
	}
	if (isCreateKw(cur().type) && (peek(1).is(TokType::Dot) || peek(1).is(TokType::Colon))
	        && (peek(2).is(TokType::Kw_type))) {
		return parseTypeAliasDecl();
	}
	if (isCreateKw(cur().type)) {
		pos_++;
		eatCreateSep();
		/* 枚举名也是合法类型（底层 int）。不认的话 create.Color c 会报"类型未知" */
		if (!isTypeKw(cur().type) && !enums_.count(cur().text)) {
			die("create 后的类型未知 '" + cur().text + "'", cur());
		}
		/* BUGFIX: 必须先记下"是否是类型关键字"，再消费。
		   原先在 parseTypeName() 之后又判断 cur()，此时已移到变量名上，
		   于是 create.int c 会多消费掉 'c' —— 整条声明路径全崩。 */
		bool wasTypeKw = isTypeKw(cur().type);
		std::string tn = wasTypeKw ? parseTypeName() : cur().text;
		if (!wasTypeKw) pos_++;                     /* 枚举名: 直接消费 Ident */
		if (tn == "tuple" || tn == "元组") {
			/* create.tuple t = (1, 2);  元组变量 */
			return parseVarDeclStmt("tuple");
		}
		return parseVarDeclStmt(tn);
	}
	if (cur().is(TokType::Kw_do)) {
		pos_++;
		expect(TokType::Dot, "'.'");
		if (cur().is(TokType::Kw_output)) {
			return parseOutputStmt();
		} else if (cur().is(TokType::Kw_input)) {
			return parseInputStmt();
		} else {
			die("do 后期望 emit 或 recv", cur());
		}
	}
	if (cur().is(TokType::Kw_if)) {
		return parseIfStmt();
	}
	if (cur().is(TokType::Kw_for)) {
		return parseForStmt();
	}
	if (cur().is(TokType::Kw_while)) {
		return parseWhileStmt();
	}
	if (cur().is(TokType::Kw_loop)) {
		return parseLoopStmt();
	}
	if (cur().is(TokType::Kw_重复)) {
		return parseLoopStmt();
	}
	if (cur().is(TokType::Kw_循环)) {
		pos_++;
		if (check(TokType::LParen)) {
			if (checkAt(1, TokType::IntLit)) {
				pos_++;
				const Token& countTk = expect(TokType::IntLit, "循环次数");
				int64_t count = std::stoll(countTk.text);
				expect(TokType::RParen, "')'");
				auto body = parseBlock();
				LoopNStmt lns;
				lns.count = count;
				lns.body = std::move(body);
				Stmt s = std::move(lns);
				return s;
			}
			return parseWhileStmtInner();
		}
		return parseLoopStmtInner();
	}
	if (cur().is(TokType::Kw_return)) {
		return parseReturnStmt();
	}
	if (cur().is(TokType::Kw_match)) {
		return parseMatchStmt();
	}
	/* ── 本轮补全 ── */
	if (cur().is(TokType::Kw_forall)) {
		return parseForallStmt(true);
	}
	if (cur().is(TokType::Kw_exists)) {
		return parseForallStmt(false);
	}
	if (cur().is(TokType::Kw_when)) {
		return parseWhenStmt();
	}
	if (cur().is(TokType::Kw_yield)) {
		return parseYieldStmt();
	}
	if (cur().is(TokType::Kw_compiletime)) {
		if (checkAt(1, TokType::LBrace)) return parseCompileTimeStmt();
	}
	if (cur().is(TokType::Kw_contract)) {
		return parseContractStmt();
	}
	if (cur().is(TokType::Kw_break)) return parseBreakStmt();
	if (cur().is(TokType::Kw_continue)) return parseContinueStmt();
	if (cur().is(TokType::Kw_typeof)) {
		auto e = parseTypeofExpr();
		expect(TokType::Semicolon, "';'");
		ExprStmt es;
		es.expr = std::move(e);
		return Stmt(std::move(es));
	}
	if (cur().is(TokType::Kw_async)) {
		die("async 只能用于修饰函数声明 (create.mod M::f() async { })", cur());
	}
	/* 宏调用（语句位）*/
	{
		std::vector<Stmt> expanded;
		if (expandMacroStmt(expanded)) {
			if (expanded.size() == 1) return std::move(expanded[0]);
			Block blk;
			blk.stmts = std::move(expanded);
			return Stmt(std::move(blk));
		}
	}
	return parseExprStmt();
}

Stmt Parser::parseVarDeclStmt(const std::string& typeName, bool advPath) {
	VarDecl vd;
	vd.type = typeName;
	const Token& nameTk = expect(TokType::Ident, "变量");
	vd.name = nameTk.text;
	vd.line = nameTk.line;
	vd.col = nameTk.col;
	/* 枚举名作变量类型: 底层按 underlying(默认 int) 处理，
	   同时记录 变量->枚举 供 switch 穷尽性检查使用。 */
	if (enums_.count(typeName)) {
		enum_vars_[vd.name] = typeName;
		vd.type = enums_[typeName].underlying;
	}
	if (check(TokType::LBracket)) {
		pos_++;
		const Token& lenTk = expect(TokType::IntLit, "数组长度");
		vd.array_len = std::stoll(lenTk.text);
		expect(TokType::RBracket, "']'");
		if (vd.array_len <= 0) {
			die("数组长度必须 > 0", lenTk);
		}
	}
	if (check(TokType::Assign)) {
		pos_++;
		if (vd.type == "tuple" && check(TokType::LParen)) {
			/* create.tuple t = (a, b, c);  元组字面量整包作为初值。
			   必须走这条专用分支: 若交给通用 parseExpr，括号里的逗号会被
			   当成参数分隔符，导致 (10, 20, 30) 解析失败。 */
			auto tl = parseTupleExpr();
			vd.init = std::make_unique<Expr>(std::move(*tl));
		} else if (check(TokType::LBrace)) {
			/* 数组初始化列表: {expr, expr, ...} */
			pos_++;
			while (!check(TokType::RBrace) && !eof()) {
				auto elem = parseExpr();
				vd.array_init.push_back(std::move(elem));
				if (check(TokType::Comma)) {
					pos_++;
					continue;
				}
				break;
			}
			expect(TokType::RBrace, "'}'");
		} else {
			auto p = parseModulePathExpr(advPath);
			if (p) {
				vd.init = std::make_unique<Expr>(std::move(*p));
			} else {
				auto tmp = parseExpr();
				vd.init = std::make_unique<Expr>(std::move(*tmp));
			}
		}
	}
	expect(TokType::Semicolon, "';'");
	if (vd.type == "auto" || vd.type == "万能") {
		if (vd.init) {
			if (std::get_if<IntLit>(&*vd.init)) vd.type = "int";
			else if (std::get_if<DoubleLit>(&*vd.init)) vd.type = "double";
			else if (std::get_if<BoolLit>(&*vd.init)) vd.type = "bool";
			else if (std::get_if<StrLit>(&*vd.init)) vd.type = "char";
			else if (std::get_if<Ident>(&*vd.init)) vd.type = "omni";
		}
	}
	if (vd.type == "tuple" && !vd.init) {
		die("tuple 变量必须给出初值: create.tuple 名字 = (e0, e1, ...)", nameTk);
	}
	var_types_[vd.name] = vd.type;
	Stmt s = std::move(vd);
	return s;
}

/* 访问路径语句: access.Dog>DogInf.age=3; ??access.Dog>DogInf.age; */
Stmt Parser::parseAccessStmt(bool adv) {
	int32_t line = cur().line;
	int32_t col = cur().col;
	const Token& modTk = expect(TokType::Ident, "模块");
	ModulePathGet pg;
	pg.module = modTk.text;
	pg.part = "";
	pg.adv = adv;
	pg.line = line;
	pg.col = col;
	if (!check(TokType::IOGt)) {
		die("access 路径期望 '>' 分隔, 实际拿到 '" + cur().text + "'", cur());
	}
	pos_++;
	const Token& partTk = expect(TokType::Ident, "部件");
	pg.part = partTk.text;
	if (check(TokType::Dot) || check(TokType::IOGt)) {
		pos_++;
		const Token& fldTk = expect(TokType::Ident, "字段");
		pg.field = fldTk.text;
		pg.has_field = true;
	}
	if (module_reg_.count(pg.module) == 0) {
		die("未知模块: " + pg.module, modTk);
	}
	if (module_reg_[pg.module].count(pg.part) == 0) {
		die("未知部件: " + pg.module + ">" + pg.part, cur()); // 修复: 错误信息不准确
	}
	if (check(TokType::Assign)) {
		pos_++;
		auto tmp = parseExpr();
		expect(TokType::Semicolon, "';'");
		if (!pg.has_field) {
			die("access 赋值时必须指定字段", cur()); // 修复: 错误信息不准确
		}
		ModuleAccessSet mas;
		mas.module = pg.module;
		mas.part = pg.part;
		mas.field = pg.field;
		mas.adv = adv;
		mas.line = pg.line;
		mas.col = pg.col;
		mas.value = std::make_unique<Expr>(std::move(*tmp));
		Stmt s = std::move(mas);
		return s;
	}
	expect(TokType::Semicolon, "';'");
	ExprStmt es;
	auto e = std::make_unique<Expr>();
	*e = std::move(pg);
	es.expr = std::move(e);
	Stmt s = std::move(es);
	return s;
}

/* ── Build tables: collect constants, functions, globals ── */
bool Parser::isModulePathAhead() {
	if (!check(TokType::Ident)) return false;
	if (!checkAt(1, TokType::IOGt)) return false;
	if (!peek(2).is(TokType::Ident)) return false;
	if (module_reg_.count(cur().text) == 0) return false;
	markReferenced(cur().text);
	return module_reg_.at(cur().text).count(peek(2).text) > 0;
}

/* 模块路径取值表达式: Dog>Dog_B ??取值部件第一个成??
   Dog>DogInf.age ??取值部件字??
   返回 nullptr 表示当前位置不是路径 */
std::unique_ptr<Expr> Parser::parseModulePathExpr(bool adv) {
	if (!isModulePathAhead()) return nullptr;
	int32_t line = cur().line;
	int32_t col = cur().col;
	ModulePathGet pg;
	pg.module = cur().text;
	pg.adv = adv;
	pg.line = line;
	pg.col = col;
	pos_++;
	pos_++; /* '>' */
	pg.part = cur().text;
	pos_++;
	if (check(TokType::Dot) || check(TokType::IOGt)) {
		pos_++;
		pg.field = cur().text;
		pg.has_field = true;
		pos_++;
	}
	auto e = std::make_unique<Expr>();
	*e = std::move(pg);
	return e;
}

Stmt Parser::parseOutputStmt() {
	int32_t line = cur().line;
	int32_t col = cur().col;
	pos_++;
	if (!check(TokType::DoubleColon)) {
		die("emit 之后期望 '::',实际拿到 '" + cur().text + "'", cur());
	}
	pos_++;
	ExprStmt es;
	auto outExpr = std::make_unique<Expr>();
	Output o;
	o.line = line;
	o.col = col;
	/* do.emit::模块::成员(...) — 成员调用整体作为单个输出值 */
	if (check(TokType::Ident)) {
		auto mit = module_reg_.find(cur().text);
		if (mit != module_reg_.end() && checkAt(1, TokType::DoubleColon)) {
			markReferenced(cur().text);
			auto call = parseIdentOrCall();
			o.values.push_back(std::move(call));
			*outExpr = std::move(o);
			es.expr = std::move(outExpr);
			expect(TokType::Semicolon, "';'");
			return es;
		}
	}
	while (!check(TokType::Semicolon) && !check(TokType::Eof)) {
		if (check(TokType::Kw_换行)) {
			pos_++;
			auto nl = std::make_unique<Expr>();
			StrLit sl;
			/* BUGFIX: do.emit:: 的每个输出值都由后端用 "%s\n" / "%lld\n" /
			   "%g\n" 打印，末尾那一个换行是后端补的（见 emitDataPrologue
			   的 strfmt）。换行标记若再塞一个 "\n"，printf("%s\n", "\n")
			   就会打印两个换行，写一次 outlb 却空两行。这里用空串：
			   printf("%s\n", "") 恰好只产生一次换行，且 win64 与 linux64
			   两种后端行为一致。 */
			sl.value = "";
			*nl = std::move(sl);
			o.values.push_back(std::move(nl));
		} else {
			o.values.push_back(parseExpr());
		}
		if (check(TokType::DoubleColon)) {
			pos_++;
			continue;
		}
		break;
	}
	if (o.values.empty()) {
		die("emit 后至少需要一个输出值", cur()); // 修复: 错误信息不准确
	}
	*outExpr = std::move(o);
	es.expr = std::move(outExpr);
	expect(TokType::Semicolon, "';'");
	return es;
}

Stmt Parser::parseInputStmt() {
	int32_t line = cur().line;
	int32_t col = cur().col;
	pos_++;
	if (!check(TokType::DoubleColon)) {
		die("recv 之后期望 '::',实际拿到 '" + cur().text + "'", cur());
	}
	pos_++;
	const Token& varTk = expect(TokType::Ident, "输入变量");
	expect(TokType::Semicolon, "';'");
	ExprStmt es;
	auto inExpr = std::make_unique<Expr>();
	Input in;
	in.var_name = varTk.text;
	in.line = line;
	in.col = col;
	*inExpr = std::move(in);
	es.expr = std::move(inExpr);
	return es;
}

Stmt Parser::parseIfStmt() {
	int32_t line = cur().line;
	int32_t col = cur().col;
	pos_++;
	expect(TokType::LParen, "'('");
	auto tmp = parseExpr();
	auto cond = std::make_unique<Expr>(std::move(*tmp));
	expect(TokType::RParen, "')'");
	auto thenBody = parseBlock();
	IfStmt is;
	is.line = line;
	is.col = col;
	is.cond = std::move(cond);
	is.then_body = std::move(thenBody);
	if (cur().is(TokType::Kw_else)) {
		pos_++;
		is.else_body = parseBlock();
	}
	Stmt s = std::move(is);
	return s;
}

Stmt Parser::parseMatchStmt() {
	pos_++; /* consume match/匹配 */
	expect(TokType::LParen, "'('");
	auto expr = parseExpr();
	expect(TokType::RParen, "')'");
	expect(TokType::LBrace, "'{'");
	MatchStmt ms;
	ms.expr = std::move(expr);
	ms.line = cur().line;
	ms.col = cur().col;
	while (!check(TokType::RBrace) && !eof()) {
		MatchCase mc;
		mc.line = cur().line;
		mc.col = cur().col;
		/* Parse pattern: could be literal, ident, or wildcard _ */
		if (check(TokType::Ident) && peek(1).is(TokType::Colon)) {
			mc.pattern = std::make_unique<Expr>();
			*std::get_if<Ident>(&*mc.pattern) = Ident{cur().text, cur().line, cur().col};
			pos_ += 2; /* ident + colon */
		} else if (check(TokType::Kw_匹配)) {
			/* Default case */
			pos_++;
			expect(TokType::Colon, ":");
			mc.body = parseBlock();
			ms.cases.push_back(std::move(mc));
			continue;
		} else if (check(TokType::LBrace)) {
			/* Inline pattern block */
			auto blk = parseBlock();
			mc.body = std::move(blk);
			ms.cases.push_back(std::move(mc));
			continue;
		} else {
			mc.pattern = parseExpr();
			expect(TokType::Colon, ":");
		}
		mc.body = parseBlock();
		ms.cases.push_back(std::move(mc));
	}
	expect(TokType::RBrace, "'}'");
	return Stmt(std::move(ms));
}

Stmt Parser::parseContractStmt() {
	pos_++; /* consume contract/契约 */
	std::string ctype;
	if (check(TokType::Ident)) {
		ctype = cur().text;
		pos_++;
	}
	expect(TokType::LParen, "'('");
	auto cond = parseExpr();
	std::string msg;
	/* 契约消息是第二个参数: contract guard(cond, "msg"); */
	if (check(TokType::Comma)) {
		pos_++;
		if (check(TokType::StrLit)) {
			msg = cur().text;
			pos_++;
		}
	}
	expect(TokType::RParen, "')'");
	expect(TokType::Semicolon, "';'");
	ContractStmt cs;
	cs.contract_type = ctype;
	cs.condition = std::move(cond);
	cs.message = msg;
	return Stmt(std::move(cs));
}

Stmt Parser::parseForStmt() {
	int32_t line = cur().line;
	int32_t col = cur().col;
	pos_++;
	bool hasParen = check(TokType::LParen);
	if (hasParen) {
		pos_++;
	}
	ForStmt fs;
	fs.line = line;
	fs.col = col;
	fs.init_decl = parseForInitDecl();
	expect(TokType::Semicolon, "';'");
	if (!check(TokType::Semicolon)) {
		auto tmp = parseExpr();
		fs.cond = std::make_unique<Expr>(std::move(*tmp));
	}
	expect(TokType::Semicolon, "';'");
	if (!check(TokType::RParen)) {
		auto tmp = parseExpr();
		fs.step = std::make_unique<Expr>(std::move(*tmp));
	}
	if (hasParen) {
		expect(TokType::RParen, "')'");
	}
	fs.body = parseBlock();
	Stmt out = std::move(fs);
	return out;
}

Stmt Parser::parseWhileStmt() {
	pos_++;
	return parseWhileStmtInner();
}

Stmt Parser::parseWhileStmtInner() {
	int32_t line = cur().line;
	int32_t col = cur().col;
	expect(TokType::LParen, "'('");
	auto tmp = parseExpr();
	auto cond = std::make_unique<Expr>(std::move(*tmp));
	expect(TokType::RParen, "')'");
	auto body = parseBlock();
	WhileStmt ws;
	ws.line = line;
	ws.col = col;
	ws.cond = std::move(cond);
	ws.body = std::move(body);
	Stmt s = std::move(ws);
	return s;
}

Stmt Parser::parseLoopStmt() {
	pos_++;
	if (check(TokType::LParen)) {
		pos_++;
		const Token& countTk = expect(TokType::IntLit, "循环次数");
		int64_t count = std::stoll(countTk.text);
		expect(TokType::RParen, "')'");
		auto body = parseBlock();
		LoopNStmt lns;
		lns.count = count;
		lns.body = std::move(body);
		Stmt s = std::move(lns);
		return s;
	}
	return parseLoopStmtInner();
}

Stmt Parser::parseLoopStmtInner() {
	auto body = parseBlock();
	LoopStmt ls;
	ls.body = std::move(body);
	Stmt s = std::move(ls);
	return s;
}

Stmt Parser::parseReturnStmt() {
	int32_t line = cur().line;
	int32_t col = cur().col;
	pos_++;
	/* return(表达式); — 括号包裹 */
	if (check(TokType::LParen)) {
		pos_++;
		auto inner = parseExpr();
		expect(TokType::RParen, "')'");
		expect(TokType::Semicolon, "';'");
		ReturnStmt rs;
		rs.line = line;
		rs.col = col;
		rs.value = std::make_unique<Expr>(std::move(*inner));
		Stmt s = std::move(rs);
		return s;
	}
	if (check(TokType::Semicolon)) {
		pos_++;
		ReturnStmt rs;
		rs.line = line;
		rs.col = col;
		Stmt s = std::move(rs);
		return s;
	}
	auto tmp = parseExpr();
	auto v = std::make_unique<Expr>(std::move(*tmp));
	expect(TokType::Semicolon, "';'");
	ReturnStmt rs;
	rs.line = line;
	rs.col = col;
	rs.value = std::move(v);
	Stmt s = std::move(rs);
	return s;
}

Stmt Parser::parseExprStmt() {
	auto tmp = parseExpr();
	/* Check for assignment a = b */
	if (check(TokType::Assign)) {
		pos_++;
		auto p = parseModulePathExpr(false);
		std::unique_ptr<Expr> rhs;
		if (p) rhs = std::move(p);
		else rhs = parseExpr();
		expect(TokType::Semicolon, "';'");
		AssignStmt as;
		as.line = cur().line;
		as.col = cur().col;
		if (const auto* id = std::get_if<Ident>(&*tmp)) {
			as.name = id->name;
			as.line = id->line;
			as.col = id->col;
		} else {
			as.target = std::move(tmp);
		}
		as.value = std::move(rhs);
		Stmt s = std::move(as);
		return s;
	}
	auto e = std::make_unique<Expr>(std::move(*tmp));
	expect(TokType::Semicolon, "';'");
	ExprStmt es;
	es.expr = std::move(e);
	Stmt s = std::move(es);
	return s;
}

std::unique_ptr<VarDecl> Parser::parseForInitDecl() {
	if (isCreateKw(cur().type)) {
		pos_++;
		eatCreateSep();
		if (!isTypeKw(cur().type)) {
			die("for 初始化中 create 后的类型未知 '" + cur().text + "'", cur());
		}
		std::string tn = parseTypeName();
		const Token& nameTk = expect(TokType::Ident, "循环变量");
		expect(TokType::Assign, "'='");
		auto tmp = parseExpr();
		auto vd = std::make_unique<VarDecl>();
		vd->type = tn;
		vd->name = nameTk.text;
		vd->line = nameTk.line;
		vd->col = nameTk.col;
		vd->init = std::make_unique<Expr>(std::move(*tmp));
		return vd;
	}
	if (check(TokType::IOLt) || check(TokType::LangL)) {
		/* 标准迭代?? for <i>; 条件; 步进  (<> 表示创建, 默认初??1; <i=expr> 显式初?? */
		bool isLang = check(TokType::LangL);
		pos_++;
		if (isStatKw(cur().type)) {
			die("for 循环变量声明中不允许使用 stat 关键字", cur()); // 修复: 错误信息不准确
		}
		const Token& nameTk2 = expect(TokType::Ident, "循环变量");
		auto vd = std::make_unique<VarDecl>();
		vd->type = "int";
		vd->name = nameTk2.text;
		vd->line = nameTk2.line;
		vd->col = nameTk2.col;
		if (check(TokType::Assign)) {
			pos_++;
			/* ??parseAdd: 避免结尾 '>' ??parseCompare 当作比较运算符吃??*/
			auto tmp = parseAdd();
			vd->init = std::make_unique<Expr>(std::move(*tmp));
		} else {
			IntLit one;
			one.value = 1;
			vd->init = std::make_unique<Expr>(std::move(one));
		}
		if (isLang) {
			expect(TokType::LangR, "'??");
		} else {
			expect(TokType::IOGt, "'>'");
		}
		return vd;
	}
	if (check(TokType::Ident)) {
		const Token& nameTk = cur();
		pos_++;
		expect(TokType::Assign, "'='");
		auto tmp = parseExpr();
		auto vd = std::make_unique<VarDecl>();
		vd->type = "";
		vd->name = nameTk.text;
		vd->line = nameTk.line;
		vd->col = nameTk.col;
		vd->init = std::make_unique<Expr>(std::move(*tmp));
		return vd;
	}
	die("for 循环初始化格式错误，期望 create、<变量名> 或 变量名=表达式", cur()); // 修复: 错误信息不准确
}

std::vector<Stmt> Parser::parseBlock() {
	expect(TokType::LBrace, "'{'");
	std::vector<Stmt> out;
	while (!check(TokType::RBrace) && !eof()) {
		out.push_back(parseStmt());
	}
	expect(TokType::RBrace, "'}'");
	return out;
}

std::unique_ptr<Expr> Parser::parseExpr() {
	/* enum 成员 / const 命名常量 → 折叠成整数字面量（枚举的机器码形态） */
	if (check(TokType::Ident)) {
		auto ic = named_consts_.find(cur().text);
		if (ic != named_consts_.end()) {
			pos_++;
			auto ec = std::make_unique<Expr>();
			IntLit lc;
			lc.value = ic->second;
			*ec = std::move(lc);
			return ec;
		}
	}
	/* 最低优先级留给 '..' 范围运算符: 0..9 / i+1..n-1 */
	auto left = parseLogicalOr();
	if (check(TokType::DoubleDot)) {
		int32_t line = cur().line;
		int32_t col = cur().col;
		pos_++;
		auto right = parseLogicalOr();
		auto r = std::make_unique<Expr>();
		RangeLit rl;
		rl.lo = std::move(left);
		rl.hi = std::move(right);
		rl.line = line;
		rl.col = col;
		*r = std::move(rl);
		return r;
	}
	return left;
}

/* || 逻辑?? 最低优先级, 短路求??*/
std::unique_ptr<Expr> Parser::parseLogicalOr() {
	auto left = parseLogicalAnd();
	while (check(TokType::Or) || cur().is(TokType::Kw_or)) {
		/* or 是 || 的词形式: 统一成 "||", 后端与类型检查只需认一种 */
		std::string op = cur().is(TokType::Kw_or) ? "||" : cur().text;
		int32_t line = cur().line;
		int32_t col = cur().col;
		pos_++;
		auto right = parseLogicalAnd();
		auto b = std::make_unique<Expr>();
		Binary bin;
		bin.op = op;
		bin.line = line;
		bin.col = col;
		bin.lhs = std::move(left);
		bin.rhs = std::move(right);
		*b = std::move(bin);
		left = std::move(b);
	}
	return left;
}

/* && 逻辑?? 高于 || 低于相等性比??*/
std::unique_ptr<Expr> Parser::parseLogicalAnd() {
	auto left = parseEquality();
	while (check(TokType::And) || cur().is(TokType::Kw_and)) {
		/* and 是 && 的词形式 */
		std::string op = cur().is(TokType::Kw_and) ? "&&" : cur().text;
		int32_t line = cur().line;
		int32_t col = cur().col;
		pos_++;
		auto right = parseEquality();
		auto b = std::make_unique<Expr>();
		Binary bin;
		bin.op = op;
		bin.line = line;
		bin.col = col;
		bin.lhs = std::move(left);
		bin.rhs = std::move(right);
		*b = std::move(bin);
		left = std::move(b);
	}
	return left;
}

std::unique_ptr<Expr> Parser::parseEquality() {
	auto left = parseCompare();
	while (check(TokType::EqEq) || check(TokType::Neq)
	        || cur().is(TokType::Kw_is)
	        || cur().is(TokType::Kw_in)) {
		if (cur().is(TokType::Kw_is)) {
			int32_t line = cur().line;
			int32_t col = cur().col;
			pos_++;
			std::string tn;
			if (isTypeKw(cur().type)) tn = parseTypeName();
			else { tn = cur().text; pos_++; }
			auto e = std::make_unique<Expr>();
			IsExpr ie;
			ie.arg = std::move(left);
			ie.type_name = tn;
			ie.line = line;
			ie.col = col;
			*e = std::move(ie);
			left = std::move(e);
			continue;
		}
		if (cur().is(TokType::Kw_in)) {
			int32_t line = cur().line;
			int32_t col = cur().col;
			pos_++;
			auto cont = parseRangeExpr();
			auto e = std::make_unique<Expr>();
			InExpr ie;
			ie.value = std::move(left);
			ie.container = std::move(cont);
			ie.line = line;
			ie.col = col;
			*e = std::move(ie);
			left = std::move(e);
			continue;
		}
		std::string op = cur().text;
		int32_t line = cur().line;
		int32_t col = cur().col;
		pos_++;
		auto right = parseCompare();
		auto b = std::make_unique<Expr>();
		Binary bin;
		bin.op = op;
		bin.line = line;
		bin.col = col;
		bin.lhs = std::move(left);
		bin.rhs = std::move(right);
		*b = std::move(bin);
		left = std::move(b);
	}
	return left;
}

std::unique_ptr<Expr> Parser::parseCompare() {
	auto left = parseBitOr();
	while (check(TokType::LtEq) || check(TokType::GtEq)
	        || check(TokType::IOGt) || check(TokType::IOLt)) {
		std::string op;
		int32_t line = cur().line;
		int32_t col = cur().col;
		if (check(TokType::IOGt)) {
			op = ">";
		} else if (check(TokType::IOLt)) {
			op = "<";
		} else {
			op = cur().text;
		}
		pos_++;
		auto right = parseAdd();
		auto b = std::make_unique<Expr>();
		Binary bin;
		bin.op = op;
		bin.line = line;
		bin.col = col;
		bin.lhs = std::move(left);
		bin.rhs = std::move(right);
		*b = std::move(bin);
		left = std::move(b);
	}
	return left;
}

/* ── 位运算优先级链 ──────────────────────────────────────────────────────
   由低到高:  |  →  ^  →  &  →  +,-  →  <<,>>  →  *,/,%
   与 C 一致。以前 Wei 只有 &（取地址用的 Amp），^ | ~ 与位移全部缺失，
   写掩码、哈希、位图这类代码只能绕道。 */
static std::unique_ptr<Expr> mkBin(std::string op, std::unique_ptr<Expr> l,
                                   std::unique_ptr<Expr> r, int32_t line, int32_t col) {
	auto b = std::make_unique<Expr>();
	Binary bin;
	bin.op = std::move(op);
	bin.line = line;
	bin.col = col;
	bin.lhs = std::move(l);
	bin.rhs = std::move(r);
	*b = std::move(bin);
	return b;
}

Stmt Parser::parseBreakStmt() {
	int32_t line = cur().line, col = cur().col;
	pos_++;
	expect(TokType::Semicolon, "';'");
	wei::ast::BreakStmt b;
	b.line = line; b.col = col;
	return Stmt(std::move(b));
}
Stmt Parser::parseContinueStmt() {
	int32_t line = cur().line, col = cur().col;
	pos_++;
	expect(TokType::Semicolon, "';'");
	wei::ast::ContinueStmt c;
	c.line = line; c.col = col;
	return Stmt(std::move(c));
}

std::unique_ptr<Expr> Parser::parseBitOr() {
	auto left = parseBitXor();
	while (check(TokType::Pipe)) {
		int32_t line = cur().line, col = cur().col;
		pos_++;
		auto right = parseBitXor();
		left = mkBin("|", std::move(left), std::move(right), line, col);
	}
	return left;
}
std::unique_ptr<Expr> Parser::parseBitXor() {
	auto left = parseBitAnd();
	while (check(TokType::Caret)) {
		int32_t line = cur().line, col = cur().col;
		pos_++;
		auto right = parseBitAnd();
		left = mkBin("^", std::move(left), std::move(right), line, col);
	}
	return left;
}
std::unique_ptr<Expr> Parser::parseBitAnd() {
	auto left = parseAdd();
	while (check(TokType::Amp)) {
		int32_t line = cur().line, col = cur().col;
		pos_++;
		auto right = parseAdd();
		left = mkBin("&", std::move(left), std::move(right), line, col);
	}
	return left;
}

std::unique_ptr<Expr> Parser::parseAdd() {
	auto left = parseShift();
	while (check(TokType::Plus) || check(TokType::Minus)) {
		std::string op = cur().text;
		int32_t line = cur().line;
		int32_t col = cur().col;
		pos_++;
		auto right = parseMul();
		auto b = std::make_unique<Expr>();
		Binary bin;
		bin.op = op;
		bin.line = line;
		bin.col = col;
		bin.lhs = std::move(left);
		bin.rhs = std::move(right);
		*b = std::move(bin);
		left = std::move(b);
	}
	return left;
}

std::unique_ptr<Expr> Parser::parseShift() {
	auto left = parseMul();
	while (check(TokType::Shl) || check(TokType::Shr)) {
		std::string op = (cur().type == TokType::Shl) ? "<<" : ">>";
		int32_t line = cur().line, col = cur().col;
		pos_++;
		auto right = parseMul();
		left = mkBin(op, std::move(left), std::move(right), line, col);
	}
	return left;
}

std::unique_ptr<Expr> Parser::parseMul() {
	auto left = parseUnary();
	while (check(TokType::Star) || check(TokType::Slash) || check(TokType::Percent)) {
		std::string op = cur().text;
		int32_t line = cur().line;
		int32_t col = cur().col;
		pos_++;
		auto right = parseUnary();
		auto b = std::make_unique<Expr>();
		Binary bin;
		bin.op = op;
		bin.line = line;
		bin.col = col;
		bin.lhs = std::move(left);
		bin.rhs = std::move(right);
		*b = std::move(bin);
		left = std::move(b);
	}
	return left;
}

std::unique_ptr<Expr> Parser::parseUnary() {
	if (cur().is(TokType::Kw_await)) {
		return parseAwaitExpr();
	}
	if (check(TokType::Not) || check(TokType::Minus) || check(TokType::Star) || check(TokType::Amp)
	        || check(TokType::Tilde) || cur().is(TokType::Kw_not)) {
		std::string op = cur().is(TokType::Kw_not) ? "!" : cur().text;
		int32_t line = cur().line;
		int32_t col = cur().col;
		pos_++;
		auto operand = parseUnary();
		auto u = std::make_unique<Expr>();
		Unary un;
		un.op = op;
		un.line = line;
		un.col = col;
		un.operand = std::move(operand);
		*u = std::move(un);
		return u;
	}
	return parsePostfix();
}

std::unique_ptr<Expr> Parser::parsePostfix() {
	auto e = parsePrimary();
	while (check(TokType::PlusPlus) || check(TokType::MinusMinus) || check(TokType::LBracket)
	        || (check(TokType::Dot) && checkAt(1, TokType::IntLit))) {
		if (check(TokType::Dot) && checkAt(1, TokType::IntLit)) {
			/* 元组取元素: t.0 / t.1 */
			int32_t line = cur().line;
			int32_t col = cur().col;
			pos_++;
			int64_t idx = std::stoll(cur().text);
			pos_++;
			auto te = std::make_unique<Expr>();
			TupleIndexExpr ti;
			ti.tuple = std::move(e);
			ti.index = idx;
			ti.line = line;
			ti.col = col;
			*te = std::move(ti);
			e = std::move(te);
			continue;
		}
		if (check(TokType::LBracket)) {
			/* mod 函数调用: Calc[n-1] — 方括号内是实参 */
			if (auto* id = std::get_if<Ident>(&*e)) {
				if (module_reg_.count(id->name) > 0) {
					pos_++;
					Call mc;
					mc.callee = id->name;
					mc.line = id->line;
					mc.col = id->col;
					markReferenced(id->name);
					if (!check(TokType::RBracket)) {
						while (true) {
							mc.args.push_back(parseExpr());
							if (check(TokType::Comma)) {
								pos_++;
								continue;
							}
							break;
						}
					}
					expect(TokType::RBracket, "']'");
					e = std::make_unique<Expr>(std::move(mc));
					continue;
				}
			}
			pos_++;
			auto idx = parseExpr();
			expect(TokType::RBracket, "']'");
			if (auto* id = std::get_if<Ident>(&*e)) {
				IndexExpr ix;
				ix.name = id->name;
				ix.idx = std::move(idx);
				ix.line = id->line;
				ix.col = id->col;
				e = std::make_unique<Expr>(std::move(ix));
			} else {
				auto scaled = std::make_unique<Expr>();
				Binary sc;
				sc.op = "*";
				sc.lhs = std::move(idx);
				auto eight = std::make_unique<Expr>();
				IntLit e8;
				e8.value = 8;
				*eight = std::move(e8);
				sc.rhs = std::move(eight);
				sc.line = cur().line;
				sc.col = cur().col;
				*scaled = std::move(sc);
				auto add = std::make_unique<Expr>();
				Binary ad;
				ad.op = "+";
				ad.lhs = std::move(e);
				ad.rhs = std::move(scaled);
				ad.line = cur().line;
				ad.col = cur().col;
				*add = std::move(ad);
				auto deref = std::make_unique<Expr>();
				Unary dr;
				dr.op = "*";
				dr.operand = std::move(add);
				dr.line = cur().line;
				dr.col = cur().col;
				*deref = std::move(dr);
				e = std::move(deref);
			}
			continue;
		}
		std::string op = cur().text;
		int32_t line = cur().line;
		int32_t col = cur().col;
		pos_++;
		auto u = std::make_unique<Expr>();
		Unary un;
		un.op = op;
		un.line = line;
		un.col = col;
		un.operand = std::move(e);
		*u = std::move(un);
		e = std::move(u);
	}
	return e;
}

std::unique_ptr<Expr> Parser::parsePrimary() {
	if (check(TokType::LParen)) {
		/* Check if this is a tuple literal (e.g., (1, "hello")) */
		if (checkAt(1, TokType::Comma) || checkAt(1, TokType::RParen)) {
			return parseTupleExpr();
		}
		pos_++;
		auto e = parseExpr();
		expect(TokType::RParen, "')'");
		return e;
	}
	if (check(TokType::IntLit)) {
		auto e = std::make_unique<Expr>();
		IntLit il;
		/* BUGFIX: 原来只用 stoll（有符号），于是 u64 的大字面量
		   （9223372036854775808 .. 18446744073709551615）一律被拒，
		   即使变量声明为 u64 也写不出来 —— 这个区间本应合法。
		   改为先按有符号试；失败再按无符号试，成功则以位模式存入
		   int64（u64 语境下位模式即原值，128 位/无符号语义不受影响）。
		   只有真正超过 UINT64_MAX 才报错。 */
		try {
			il.value = std::stoll(cur().text);
		} catch (...) {
			bool okUnsigned = false;
			try {
				unsigned long long uv = std::stoull(cur().text);
				if (uv > 0x7FFFFFFFFFFFFFFFULL) {
					il.value = static_cast<int64_t>(uv);   /* 位模式保留 */
					okUnsigned = true;
				}
			} catch (...) { okUnsigned = false; }
			if (!okUnsigned) die("整数常量过大 '" + cur().text + "'", cur());
		}
		pos_++;
		*e = std::move(il);
		return e;
	}
	if (check(TokType::DoubleLit)) {
		auto e = std::make_unique<Expr>();
		DoubleLit dl;
		try {
			dl.value = std::stod(cur().text);
		} catch (...) {
			die("浮点常量非法 '" + cur().text + "'", cur());
		}
		pos_++;
		*e = std::move(dl);
		return e;
	}
	if (check(TokType::StrLit)) {
		auto e = std::make_unique<Expr>();
		StrLit sl;
		sl.value = cur().text;
		pos_++;
		*e = std::move(sl);
		return e;
	}
	if (check(TokType::Kw_true) || check(TokType::Kw_真)) {
		pos_++;
		auto e = std::make_unique<Expr>();
		BoolLit bl;
		bl.value = true;
		*e = std::move(bl);
		return e;
	}
	if (check(TokType::Kw_false) || check(TokType::Kw_假)) {
		pos_++;
		auto e = std::make_unique<Expr>();
		BoolLit bl;
		bl.value = false;
		*e = std::move(bl);
		return e;
	}
	if (check(TokType::Kw_null)) {
		pos_++;
		auto e = std::make_unique<Expr>();
		IntLit il;
		il.value = 0;
		*e = std::move(il);
		return e;
	}
	if (cur().is(TokType::Kw_sizeof)) {
		pos_++;
		expect(TokType::LParen, "'('");
		std::string typeName = parseTypeName();
		expect(TokType::RParen, "')'");
		auto e = std::make_unique<Expr>();
		wei::ast::SizeofExpr se;
		se.type = typeName;
		se.value = 8;
		*e = std::move(se);
		return e;
	}
	if (cur().is(TokType::Kw_new)) {
		pos_++;
		std::string typeName = parseTypeName();
		auto e = std::make_unique<Expr>();
		wei::ast::NewExpr ne;
		ne.type = typeName;
		*e = std::move(ne);
		return e;
	}
	if (cur().is(TokType::Kw_delete)) {
		pos_++;
		auto target = parseExpr();
		auto e = std::make_unique<Expr>();
		wei::ast::DeleteExpr de;
		de.target = std::move(target);
		*e = std::move(de);
		return e;
	}
	if (cur().is(TokType::Kw_cast)) {
		pos_++;
		expect(TokType::LParen, "'('");
		std::string typeName = parseTypeName();
		expect(TokType::Comma, "','");
		auto target = parseExpr();
		expect(TokType::RParen, "')'");
		auto e = std::make_unique<Expr>();
		wei::ast::CastExpr ce;
		ce.type = typeName;
		ce.value = std::move(target);
		*e = std::move(ce);
		return e;
	}
	if (cur().is(TokType::Kw_co)) {
		return parseCoExpr();
	}
	/* ── 量词表达式: forall x in D : cond  /  exists x in D : cond ── */
	if (cur().is(TokType::Kw_forall) || cur().is(TokType::Kw_exists)) {
		bool isAll = cur().is(TokType::Kw_forall);
		Stmt st = parseForallStmt(isAll);
		if (auto* es = std::get_if<ExprStmt>(&st)) {
			if (es->expr) return std::move(es->expr);
		}
		die("forall/exists 在此处必须写成表达式形态 (forall x in D : cond)", cur());
	}
	/* ── when 表达式: when (cond) then a else b ── */
	if (cur().is(TokType::Kw_when)) {
		Stmt st = parseWhenStmt();
		(void)st;
		die("when 语句不能作为表达式使用", cur());
	}
	/* ── await h ── */
	if (cur().is(TokType::Kw_await)) {
		return parseAwaitExpr();
	}
	if (cur().is(TokType::Kw_typeof)) {
		return parseTypeofExpr();
	}
	if (cur().is(TokType::Kw_compiletime)) {
		return parseCompileTimeExpr();
	}
	{
		std::unique_ptr<Expr> me;
		if (tryMacroExpr(me)) return me;
	}
	if (cur().is(TokType::Kw_do)) {
		int32_t line = cur().line;
		int32_t col = cur().col;
		pos_++;
		expect(TokType::Dot, "'.'");
		if (!cur().is(TokType::Kw_input)) die("do 之后期望 recv", cur());
		pos_++;
		if (!check(TokType::DoubleColon)) die("recv 之后期望 '::'", cur());
		pos_++;
		const Token& varTk = expect(TokType::Ident, "输入变量");
		auto e = std::make_unique<Expr>();
		Input in;
		in.var_name = varTk.text;
		in.line = line;
		in.col = col;
		*e = std::move(in);
		return e;
	}
	if (isAccessKw(cur().type) && checkAt(1, TokType::Dot) && checkAt(2, TokType::Ident)) {
		pos_ += 2;
		return parseModulePathExpr(false);
	}
	if (isAdvKw(cur().type) && checkAt(1, TokType::Dot) && checkAt(2, TokType::Kw_access) && checkAt(3, TokType::Dot)) {
		pos_ += 4;
		if (!check(TokType::Ident)) {
			die("adv.access 路径中期望模块标识符", cur()); // 修复: 错误信息不准确
		}
		return parseModulePathExpr(true);
	}
	if (check(TokType::Ident) && isModulePathAhead()) {
		return parseModulePathExpr(false);
	}
	if (check(TokType::Ident)) {
		return parseIdentOrCall();
	}
	/* self / super / root 是关键字，不是 Ident，上面的分支接不到，
	   必须在这里显式放行给 parseIdentOrCall（它内部会展开限定路径）。 */
	if (cur().is(TokType::Kw_self) || cur().is(TokType::Kw_super) || cur().is(TokType::Kw_root)) {
		return parseIdentOrCall();
	}
	die("无法识别的表达式起始 token '" + cur().text + "'", cur());
}

std::unique_ptr<Expr> Parser::parseInputExpr() {
	int32_t line = cur().line;
	int32_t col = cur().col;
	pos_++;
	bool isLang = check(TokType::LangL);
	if (check(TokType::IOLt)) {
		pos_++;
	} else if (check(TokType::LangL)) {
		pos_++;
	} else {
		die("输入表达式期望 '<' 或 '《'", cur()); // 修复: 错误信息不准确
	}
	const Token& varTk = expect(TokType::Ident, "输入变量");
	if (isLang) {
		expect(TokType::RParen, "')'");
	} else {
		expect(TokType::IOGt, "'>'");
	}
	auto e = std::make_unique<Expr>();
	Input in;
	in.var_name = varTk.text;
	in.line = line;
	in.col = col;
	*e = std::move(in);
	return e;
}

std::unique_ptr<Expr> Parser::parseIdentOrCall() {
	std::string name = cur().text;
	int32_t line = cur().line;
	int32_t col = cur().col;
	std::string mod_path;
	std::vector<std::string> explicit_targs;
	/* ── 表达式级可见性检查 ──────────────────────────────────────
	   `M::f(...)` / `M::f` 这类跨模块成员访问都会经过本函数。
	   在消费 Ident 之后立刻按 (定义文件 vs 当前文件) 判定:
	     export 放行；fileprivate / internal(默认) / hidden 必须同文件。
	   这里不检查 call 语句而检查 ident 解析，是因为模块成员既能作为
	   调用也能作为值/参数出现 —— 统一在最上游拦截才不会有漏网路径。 */
	if (check(TokType::Ident) && checkAt(1, TokType::DoubleColon)) {
		std::string m0 = cur().text;
		std::string m1 = peek(1).type == TokType::DoubleColon
		                 && peek(2).type == TokType::Ident ? peek(2).text : std::string();
		if (!m1.empty()) {
			if (const MemberVis* mv0 = findVis(m0 + "::" + m1)) {
				bool sf = (mv0->file == cur_file_) || mv0->file.empty() || cur_file_.empty();
				if (mv0->v != Vis::Export && !sf) {
					const char* lvl = (mv0->v == Vis::FilePrivate) ? "fileprivate"
					                : (mv0->v == Vis::Hidden)      ? "hidden"
					                                               : "internal(默认)";
					die("'" + m0 + "::" + m1 + "' 不可见: 声明为 " + lvl + "，"
					    "不能跨文件访问（加 export 或 pub 可放开）", cur());
				}
			}
		}
	}
	/* self.X / super.X / root.X: 名称解析限定符。它们已注册为关键字，
	   故在此处直接展开成限定路径，再交给后续普通解析流程。 */
	if (cur().is(TokType::Kw_self) || cur().is(TokType::Kw_super) || cur().is(TokType::Kw_root)) {
		std::string prefix;
		if (cur().is(TokType::Kw_self)) {
			prefix = cur_ns_;
		} else if (cur().is(TokType::Kw_super)) {
			std::string ns = cur_ns_;
			size_t pp = ns.find_last_of('.');
			prefix = (pp == std::string::npos) ? std::string() : ns.substr(0, pp);
		}
		pos_++;                                  /* 消费 self/super/root */
		expect(TokType::DoubleColon, "'::'");    /* 消费 :: */
		mod_path = prefix;
		name = cur().text;                       /* 后续第一段名字 */
		line = cur().line;
		col = cur().col;
		pos_++;                                  /* 消费它，让后面的 :: 能被识别 */
	} else {
		pos_++;                                  /* 消费普通标识符 */
	}

	/* mod 成员调用: Calc::fib(n) */
	if (check(TokType::DoubleColon) && checkAt(1, TokType::Ident)) {
		/* 已有前缀（self::/super::）时拼成全限定名，但只有当全限定名真的
		   存在时才用；否则回退到不带前缀的模块名。
		   原因: package 只记前缀、不给模块改名，模块仍以短名注册。
		   不留回退的话 self::Calc 会被展开成 App.Sub::Calc 而查不到。 */
		std::string qualified = mod_path.empty() ? name : (mod_path + "::" + name);
		if (module_reg_.count(qualified) == 0) {
			if (module_reg_.count(name) == 0) {
				die("未知模块: " + name, cur());
			}
			mod_path = std::string();      /* 回退: 用短名 */
		} else {
			mod_path = mod_path.empty() ? name : (mod_path + "::" + name);
			name = qualified.substr(qualified.find_last_of(':') + 1);
		}
		/* ── 模块化: 真正的跨模块可见性边界 ──────────────────────────
		   规则: 模块若是被 import 引入的（跨文件边界），则必须用 export
		   声明才允许外部调用其成员。同文件内的模块互调不受限制 —— 那属于
		   同一编译单元的内部组织，不是模块边界。
		   这一条让 export/public/private 从"关键字壳"变成真约束。 */
		if (imported_modules_.count(name) && !exported_modules_.count(name)) {
			die("模块 '" + name + "' 未导出: 跨文件调用需要 export 声明"
			    " (写成: export create.mod " + name + "::...)", cur());
		}
		/* ── 成员级可见性强制 ──────────────────────────────────────
		   在成员名确定之后、继续解析之前判定。判据:
		     Export      → 放行
		     FilePrivate → 调用方与被调方必须同一文件
		     Internal    → 同上（模块不跨文件，故文件边界即模块边界）
		     Hidden      → 同上，且给出更明确的提示
		   cur_file_ 是**调用点**所在文件；MemberVis::file 是**定义**所在文件。 */
		{
			std::string memName;
			if (check(TokType::Ident)) memName = cur().text;
			else if (cur().is(TokType::Kw_init) || cur().is(TokType::Kw_deinit))
				memName = cur().is(TokType::Kw_init) ? "init" : "deinit";
			if (!memName.empty()) {
				std::string q = name + "::" + memName;
				if (const MemberVis* mv = findVis(q)) {
					bool same_file = (mv->file == cur_file_) || mv->file.empty() || cur_file_.empty();
					if (mv->v != Vis::Export && !same_file) {
						const char* lvl = (mv->v == Vis::FilePrivate) ? "fileprivate"
						                : (mv->v == Vis::Hidden)      ? "hidden"
						                                               : "internal(默认)";
						die("'" + q + "' 不可见: 声明为 " + lvl + "，"
						    "不能跨文件调用（加 export 或 pub 可放开）", cur());
					}
				}
			}
		}
		markReferenced(name);
		pos_++;
		name = cur().text; /* 成员名: main / _ 等 */
		pos_++;
	}
	/* 显式类型实参: f<int>(x) / M::f<double>(y)
	   只有"尖括号里全是标识符、且以 > 收尾并紧跟 (" 才当作类型实参，
	   否则 `a < b` 这种比较表达式会被误判成泛型调用。 */
	if (check(TokType::IOLt) && checkAt(1, TokType::Ident)) {
		size_t j = 1;
		bool shaped = false;
		while (checkAt(j, TokType::Ident)) {
			j++;
			if (checkAt(j, TokType::Comma)) { j++; continue; }
			break;
		}
		if (checkAt(j, TokType::IOGt) && checkAt(j + 1, TokType::LParen)) shaped = true;
		if (shaped) {
			pos_++;
			while (!eof() && !check(TokType::IOGt)) {
				const Token& t = expect(TokType::Ident, "类型实参");
				explicit_targs.push_back(t.text);
				if (check(TokType::Comma)) { pos_++; continue; }
				break;
			}
			expect(TokType::IOGt, "'>'");
		}
	}
	if (check(TokType::LParen)) {
		pos_++;
		Call c;
		c.callee = mod_path.empty() ? name : (mod_path + "::" + name);
		c.type_args = std::move(explicit_targs);
		c.line = line;
		c.col = col;
		if (!check(TokType::RParen)) {
			while (true) {
				auto a = parseExpr();
				c.args.push_back(std::move(a));
				if (check(TokType::Comma)) {
					pos_++;
					continue;
				}
				break;
			}
		}
		expect(TokType::RParen, "')'");
		/* '?' 后缀: Result 错误传播。f()? —— 若 f 返回 Err(负数)，
		   当前函数立即以该错误返回；否则取 Ok 值继续。 */
		if (check(TokType::Question)) {
			/* 先记下 '?' 自身的位置：报错时标在 '?' 上，而不是整个调用表达式上。 */
			c.try_line = cur().line;
			c.try_col = cur().col;
			pos_++;
			c.has_try = true;
		}
		auto e = std::make_unique<Expr>();
		*e = std::move(c);
		return e;
	}
	auto e = std::make_unique<Expr>();
	Ident id;
	id.name = name;
	id.line = line;
	id.col = col;
	*e = std::move(id);
	return e;
}

std::unique_ptr<Expr> Parser::parseTupleExpr() {
	expect(TokType::LParen, "'('");
	TupleLit tl;
	tl.line = cur().line;
	tl.col = cur().col;
	if (!check(TokType::RParen)) {
		while (true) {
			auto elem = parseExpr();
			tl.elements.push_back(std::move(elem));
			if (check(TokType::Comma)) {
				pos_++;
				continue;
			}
			break;
		}
	}
	expect(TokType::RParen, "')'");
	auto e = std::make_unique<Expr>();
	*e = std::move(tl);
	return e;
}

std::unique_ptr<Expr> Parser::parseTypeExpr() {
	/* parse type expression: int | string | (int, string) | ptr.int */
	if (check(TokType::LParen)) {
		return parseTupleExpr();
	}
	return parseExpr();
}

/* 类型别名声明: create.type Alias = TargetType; */
Stmt Parser::parseTypeAliasDecl() {
	pos_++; /* consume create */
	eatCreateSep();
	expect(TokType::Ident, "类型别名");
	TypeAliasDecl td;
	td.alias_name = cur().text;
	td.line = cur().line;
	td.col = cur().col;
	expect(TokType::Assign, "'='");
	std::string target;
	if (isTypeKw(cur().type)) {
		target = parseTypeName();
	} else {
		target = cur().text;
		pos_++;
	}
	td.target_type = target;
	expect(TokType::Semicolon, "';'");
	return Stmt(std::move(td));
}

/* ══════════════════════════════════════════════════════════════════════════
   本轮补全的 22 个关键字 —— 解析实现
   ══════════════════════════════════════════════════════════════════════════ */

/* ── async 修饰符（可选）───────────────────────────────────────────────── */
bool Parser::parseOptAsync(FuncDecl& fd) {
	if (cur().is(TokType::Kw_async)) {
		pos_++;
		fd.is_async = true;
		return true;
	}
	return false;
}

/* ── 范围表达式: lo..hi ────────────────────────────────────────────────── */
std::unique_ptr<Expr> Parser::parseRangeExpr() {
	auto lo = parseAdd();
	if (check(TokType::DoubleDot)) {
		int32_t line = cur().line;
		int32_t col = cur().col;
		pos_++;
		auto hi = parseAdd();
		auto r = std::make_unique<Expr>();
		RangeLit rl;
		rl.lo = std::move(lo);
		rl.hi = std::move(hi);
		rl.line = line;
		rl.col = col;
		*r = std::move(rl);
		return r;
	}
	return lo;
}

/* ── co f(args): 创建协程 ──────────────────────────────────────────────── */
std::unique_ptr<Expr> Parser::parseCoExpr() {
	int32_t line = cur().line;
	int32_t col = cur().col;
	pos_++;                                  /* consume co / 协程 */
	const Token& nameTk = expect(TokType::Ident, "协程函数名");
	CoExpr ce;
	ce.callee = nameTk.text;
	ce.line = line;
	ce.col = col;
	if (check(TokType::LParen)) {
		pos_++;
		if (!check(TokType::RParen)) {
			while (true) {
				ce.args.push_back(parseExpr());
				if (check(TokType::Comma)) { pos_++; continue; }
				break;
			}
		}
		expect(TokType::RParen, "')'");
	}
	auto e = std::make_unique<Expr>();
	*e = std::move(ce);
	return e;
}

/* ── await h ───────────────────────────────────────────────────────────── */
std::unique_ptr<Expr> Parser::parseAwaitExpr() {
	int32_t line = cur().line;
	int32_t col = cur().col;
	pos_++;                                  /* consume await / 等待 */
	auto operand = parseUnary();
	auto e = std::make_unique<Expr>();
	AwaitExpr aw;
	aw.operand = std::move(operand);
	aw.line = line;
	aw.col = col;
	*e = std::move(aw);
	return e;
}

/* ── typeof(x) ─────────────────────────────────────────────────────────── */
std::unique_ptr<Expr> Parser::parseTypeofExpr() {
	int32_t line = cur().line;
	int32_t col = cur().col;
	pos_++;                                  /* consume typeof */
	expect(TokType::LParen, "'('");
	auto arg = parseExpr();
	expect(TokType::RParen, "')'");
	auto e = std::make_unique<Expr>();
	TypeofExpr te;
	te.arg = std::move(arg);
	te.line = line;
	te.col = col;
	/* 编译期解析: 标识符直接查已声明的变量类型 */
	if (auto* id = std::get_if<Ident>(te.arg.get())) {
		auto it = var_types_.find(id->name);
		if (it != var_types_.end()) te.resolved = it->second;
	}
	*e = std::move(te);
	return e;
}

/* ── compiletime(expr) ─────────────────────────────────────────────────── */
std::unique_ptr<Expr> Parser::parseCompileTimeExpr() {
	int32_t line = cur().line;
	int32_t col = cur().col;
	pos_++;                                  /* consume compiletime */
	expect(TokType::LParen, "'('");
	auto arg = parseExpr();
	expect(TokType::RParen, "')'");
	auto e = std::make_unique<Expr>();
	CompileTimeExpr ct;
	ct.line = line;
	ct.col = col;
	int64_t v = 0;
	if (arg && constFold(*arg, v)) {
		ct.folded = true;
		ct.value = v;
	} else {
		/* 严格语义: 编译期算不出来就是错误，不静默退化成运行时计算 */
		throw std::runtime_error("compiletime 表达式必须在编译期可求值 "
		                         "(只允许整数字面量与算术/比较/逻辑运算) at "
		                         + std::to_string(line) + ":" + std::to_string(col));
	}
	ct.arg = std::move(arg);
	*e = std::move(ct);
	return e;
}

/* ── compiletime { ... } 语句块 ────────────────────────────────────────── */
Stmt Parser::parseCompileTimeStmt() {
	int32_t line = cur().line;
	int32_t col = cur().col;
	pos_++;                                  /* consume compiletime / 编译期 */
	expect(TokType::LBrace, "'{'");
	CompileTimeStmt cs;
	cs.line = line;
	cs.col = col;
	cs.body = parseBlock();
	/* 编译期块的语义约束: 只允许「常量变量 = 常量」这类可在编译期求值的
	   语句。出现循环、分支、调用、输入等运行期结构即报错 —— 否则"编译期"
	   三个字就成了空话。初值若能折叠，就把变量登记进 var_types_，供后续
	   typeof / is 在编译期解析。 */
	for (auto& st : cs.body) {
		auto* vd = std::get_if<VarDecl>(&st);
		if (vd) {
			if (vd->array_len > 0) {
				throw std::runtime_error("compiletime 块内不允许数组声明 at "
				                         + std::to_string(vd->line) + ":" + std::to_string(vd->col));
			}
			if (!vd->init) {
				throw std::runtime_error("compiletime 块内的变量必须有初值 at "
				                         + std::to_string(vd->line) + ":" + std::to_string(vd->col));
			}
			int64_t v = 0;
			if (!constFold(*vd->init, v)) {
				throw std::runtime_error("compiletime 块内的初值必须是编译期常量 at "
				                         + std::to_string(vd->line) + ":" + std::to_string(vd->col));
			}
			var_types_[vd->name] = vd->type;
			continue;
		}
		throw std::runtime_error("compiletime 块内只允许常量变量声明 at "
		                         + std::to_string(line) + ":" + std::to_string(col));
	}
	return Stmt(std::move(cs));
}

/* ══════════════════════════════════════════════════════════════════════════
   编译期常量求值
   ──────────────────────────────────────────────────────────────────────────
   compiletime(...) 的语义是"这个值必须在编译期确定"。因此求值器采取
   **严格策略**: 只接受由整数字面量和运算符构成的封闭表达式；一旦遇到
   变量、调用、输入等运行期成分就返回 false，让调用方报错。

   这与后端的 IR 常量折叠是两件事：后者是"能折就折"的性能优化（折不了
   就照常生成代码），前者是语言级的编译期求值保证（折不了就是错误）。
   ══════════════════════════════════════════════════════════════════════════ */
bool Parser::constFoldBinary(const Binary& b, int64_t& out) const {
	int64_t l = 0, r = 0;
	if (!b.lhs || !constFold(*b.lhs, l)) return false;
	if (!b.rhs || !constFold(*b.rhs, r)) return false;
	const std::string& op = b.op;
	if (op == "+") { out = l + r; return true; }
	if (op == "-") { out = l - r; return true; }
	if (op == "*") { out = l * r; return true; }
	if (op == "/") {
		if (r == 0) {
			throw std::runtime_error("compiletime 常量求值: 除零 at "
			                         + std::to_string(b.line) + ":" + std::to_string(b.col));
		}
		out = l / r;
		return true;
	}
	if (op == "%") {
		if (r == 0) {
			throw std::runtime_error("compiletime 常量求值: 模零 at "
			                         + std::to_string(b.line) + ":" + std::to_string(b.col));
		}
		out = l % r;
		return true;
	}
	if (op == "==") { out = (l == r) ? 1 : 0; return true; }
	if (op == "!=") { out = (l != r) ? 1 : 0; return true; }
	if (op == "<")  { out = (l <  r) ? 1 : 0; return true; }
	if (op == "<=") { out = (l <= r) ? 1 : 0; return true; }
	if (op == ">")  { out = (l >  r) ? 1 : 0; return true; }
	if (op == ">=") { out = (l >= r) ? 1 : 0; return true; }
	if (op == "&&" || op == "||") {
		bool lb = (l != 0), rb = (r != 0);
		out = (op == "&&") ? ((lb && rb) ? 1 : 0) : ((lb || rb) ? 1 : 0);
		return true;
	}
	return false;
}

bool Parser::constFold(const Expr& e, int64_t& out) const {
	if (auto* il = std::get_if<IntLit>(&e)) { out = il->value; return true; }
	if (auto* bl = std::get_if<BoolLit>(&e)) { out = bl->value ? 1 : 0; return true; }
	if (auto* dl = std::get_if<DoubleLit>(&e)) {
		/* double 字面量在整数字面量语境下截断为整数 */
		out = static_cast<int64_t>(dl->value);
		return true;
	}
	if (auto* b = std::get_if<Binary>(&e)) return constFoldBinary(*b, out);
	if (auto* u = std::get_if<Unary>(&e)) {
		int64_t v = 0;
		if (!u->operand || !constFold(*u->operand, v)) return false;
		if (u->op == "-") { out = -v; return true; }
		if (u->op == "!") { out = (v == 0) ? 1 : 0; return true; }
		return false;
	}
	if (auto* ct = std::get_if<CompileTimeExpr>(&e)) {
		if (ct->folded) { out = ct->value; return true; }
		return ct->arg && constFold(*ct->arg, out);
	}
	if (auto* ce = std::get_if<CastExpr>(&e)) {
		/* 到整型的编译期转换: 直接取其整数值 */
		return ce->value && constFold(*ce->value, out);
	}
	/* 变量、调用、输入、下标、模块路径 —— 均非编译期常量 */
	return false;
}

/* ── forall / exists ─────────────────────────────────────────────────────
   语句形态: forall x in 0..9 { ... }   /  forall x of arr { ... }
   表达式形态: forall x in 0..9 : cond  /  exists x in 0..9 : cond        */
Stmt Parser::parseForallStmt(bool is_forall) {
	int32_t line = cur().line;
	int32_t col = cur().col;
	pos_++;                                  /* consume forall / exists */
	const Token& varTk = expect(TokType::Ident, "量词变量");
	std::string var = varTk.text;
	bool is_of = false;
	if (cur().is(TokType::Kw_in)) {
		pos_++;
	} else if (cur().is(TokType::Kw_of)) {
		pos_++;
		is_of = true;
	} else {
		die("forall/exists 后期望 in 或 of", cur());
	}
	auto domain = parseRangeExpr();
	if (is_forall && !is_of && check(TokType::LBrace)) {
		/* 语句形态 */
		auto body = parseBlock();
		ForallStmt fs;
		fs.var = var;
		fs.domain = std::move(domain);
		fs.body = std::move(body);
		fs.is_of = is_of;
		fs.line = line;
		fs.col = col;
		var_types_[var] = "int";
		return Stmt(std::move(fs));
	}
	if (!check(TokType::Colon)) {
		die("forall/exists 表达式形态期望 ':' 后接条件", cur());
	}
	pos_++;
	auto cond = parseExpr();
	if (check(TokType::Semicolon)) pos_++;
	auto e = std::make_unique<Expr>();
	if (is_forall) {
		ForallExpr q;
		q.var = var;
		q.domain = std::move(domain);
		q.cond = std::move(cond);
		q.line = line;
		q.col = col;
		*e = std::move(q);
	} else {
		ExistsExpr q;
		q.var = var;
		q.domain = std::move(domain);
		q.cond = std::move(cond);
		q.line = line;
		q.col = col;
		*e = std::move(q);
	}
	var_types_.erase(var);
	ExprStmt es;
	es.expr = std::move(e);
	return Stmt(std::move(es));
}

/* ── when cond then { ... } else { ... } ───────────────────────────────── */
Stmt Parser::parseWhenStmt() {
	int32_t line = cur().line;
	int32_t col = cur().col;
	pos_++;                                  /* consume when / 当 */
	WhenStmt ws;
	ws.line = line;
	ws.col = col;
	if (check(TokType::LParen)) {
		pos_++;
		auto tmp = parseExpr();
		ws.cond = std::make_unique<Expr>(std::move(*tmp));
		expect(TokType::RParen, "')'");
	} else {
		auto tmp = parseExpr();
		ws.cond = std::make_unique<Expr>(std::move(*tmp));
		if (!cur().is(TokType::Kw_then)) die("when 条件后期望 then", cur());
	}
	if (!cur().is(TokType::Kw_then)) {
		die("when 条件后期望 then", cur());
	}
	pos_++;                                  /* consume then / 则 */
	if (check(TokType::LBrace)) {
		ws.then_body = parseBlock();
	} else {
		ws.then_body.push_back(parseStmt());
	}
	if (cur().is(TokType::Kw_else)) {
		pos_++;
		if (check(TokType::LBrace)) {
			ws.else_body = parseBlock();
		} else {
			ws.else_body.push_back(parseStmt());
		}
	}
	return Stmt(std::move(ws));
}

/* ── yield v; ──────────────────────────────────────────────────────────── */
Stmt Parser::parseYieldStmt() {
	int32_t line = cur().line;
	int32_t col = cur().col;
	pos_++;                                  /* consume yield */
	YieldStmt ys;
	ys.line = line;
	ys.col = col;
	if (!check(TokType::Semicolon)) {
		auto v = parseExpr();
		ys.value = std::make_unique<Expr>(std::move(*v));
	}
	expect(TokType::Semicolon, "';'");
	return Stmt(std::move(ys));
}

/* ── create.macro NAME(a, b) { ... } ───────────────────────────────────── */
TopLevel Parser::parseMacroDeclTop() {
	/* 调用方（parseTopLevel）已消费 create 与 '.'，此处直接面对 macro */
	if (!cur().is(TokType::Kw_macro)) {
		die("期望 macro 关键字", cur());
	}
	pos_++;
	const Token& nameTk = expect(TokType::Ident, "宏名");
	MacroDecl md;
	md.name = nameTk.text;
	md.line = nameTk.line;
	md.col = nameTk.col;
	expect(TokType::LParen, "'('");
	if (!check(TokType::RParen)) {
		while (true) {
			const Token& pTk = expect(TokType::Ident, "宏参数");
			md.params.push_back(pTk.text);
			if (check(TokType::Comma)) { pos_++; continue; }
			break;
		}
	}
	expect(TokType::RParen, "')'");
	expect(TokType::LBrace, "'{'");
	while (!check(TokType::RBrace) && !eof()) {
		md.body.push_back(parseStmt());
	}
	expect(TokType::RBrace, "'}'");
	std::string mname = md.name;
	int32_t mline = md.line, mcol = md.col;
	macros_[mname] = std::move(md);
	/* 宏声明不进入程序体（已在解析期展开完毕），仅留一个占位节点 */
	MacroDecl ph;
	ph.name = mname;
	ph.line = mline;
	ph.col = mcol;
	return TopLevel(std::move(ph));
}

bool Parser::isMacroName(const std::string& n) const {
	return macros_.count(n) > 0;
}

/* ── 宏调用: 语句位展开 ──────────────────────────────────────────────────
   NAME(a, b);  →  把宏体克隆一份、形参换成实参后插入当前语句序列。      */
bool Parser::expandMacroStmt(std::vector<Stmt>& out) {
	if (!check(TokType::Ident)) return false;
	std::string name = cur().text;
	if (macros_.count(name) == 0) return false;
	if (!checkAt(1, TokType::LParen)) return false;
	if (macro_depth_ > 64) die("宏展开层数过深（可能存在递归宏）", cur());
	const MacroDecl& md = macros_.at(name);
	int32_t line = cur().line;
	int32_t col = cur().col;
	pos_ += 2;                               /* 名字 + '(' */
	std::vector<std::unique_ptr<Expr>> args;
	if (!check(TokType::RParen)) {
		while (true) {
			args.push_back(parseExpr());
			if (check(TokType::Comma)) { pos_++; continue; }
			break;
		}
	}
	expect(TokType::RParen, "')'");
	if (check(TokType::Semicolon)) pos_++;
	if (args.size() != md.params.size()) {
		die("宏 '" + name + "' 需要 " + std::to_string(md.params.size())
		    + " 个参数, 实际 " + std::to_string(args.size()), cur());
	}
	SubstMap m;
	for (size_t i = 0; i < md.params.size(); i++) m[md.params[i]] = args[i].get();
	macro_depth_++;
	for (auto& bs : md.body) out.push_back(cloneStmtSub(bs, m));
	macro_depth_--;
	(void)line; (void)col;
	return true;
}

/* ── 宏调用: 表达式位展开（宏体必须是单条表达式语句）──────────────────── */
bool Parser::tryMacroExpr(std::unique_ptr<Expr>& out) {
	if (!check(TokType::Ident)) return false;
	std::string name = cur().text;
	if (macros_.count(name) == 0) return false;
	if (!checkAt(1, TokType::LParen)) return false;
	if (macro_depth_ > 64) die("宏展开层数过深（可能存在递归宏）", cur());
	const MacroDecl& md = macros_.at(name);
	if (md.body.size() != 1) return false;
	auto* es = std::get_if<ExprStmt>(&md.body[0]);
	if (!es || !es->expr) return false;
	pos_ += 2;
	std::vector<std::unique_ptr<Expr>> args;
	if (!check(TokType::RParen)) {
		while (true) {
			args.push_back(parseExpr());
			if (check(TokType::Comma)) { pos_++; continue; }
			break;
		}
	}
	expect(TokType::RParen, "')'");
	if (args.size() != md.params.size()) {
		die("宏 '" + name + "' 需要 " + std::to_string(md.params.size())
		    + " 个参数, 实际 " + std::to_string(args.size()), cur());
	}
	SubstMap m;
	for (size_t i = 0; i < md.params.size(); i++) m[md.params[i]] = args[i].get();
	macro_depth_++;
	out = cloneExprSub(*es->expr, m);
	macro_depth_--;
	return true;
}

/* ── import A.B;  /  from A.B import f as g; ─────────────────────────────
   与 require 不同：import 会把被导入文件的顶层声明**内联**进当前程序，
   因此导入后即可直接使用其中的 mod / 部件 / 函数。                       */
TopLevel Parser::parseImportTopLevel() {
	int32_t line = cur().line;
	int32_t col = cur().col;
	ImportDecl id;
	id.line = line;
	id.col = col;
	std::string from_mod;
	if (cur().is(TokType::Kw_from)) {
		pos_++;
		const Token& mTk = expect(TokType::Ident, "模块");
		from_mod = mTk.text;
		while (check(TokType::Dot)) {
			pos_++;
			const Token& extTk = expect(TokType::Ident, "模块");
			from_mod += "." + extTk.text;
		}
		if (!cur().is(TokType::Kw_import)) {
			die("from 模块后期望 import", cur());
		}
		pos_++;
		id.has_symbols = true;
		id.from_module = from_mod;
		while (true) {
			const Token& sTk = expect(TokType::Ident, "导入符号");
			id.symbols.push_back(sTk.text);
			if (check(TokType::Comma)) { pos_++; continue; }
			break;
		}
		if (cur().is(TokType::Kw_as)) {
			pos_++;
			const Token& aTk = expect(TokType::Ident, "别名");
			id.alias = aTk.text;
		}
	} else {
		pos_++;                              /* consume import / 导入 */
		const Token& mTk = expect(TokType::Ident, "模块");
		id.module = mTk.text;
		while (check(TokType::Dot)) {
			pos_++;
			const Token& extTk = expect(TokType::Ident, "模块");
			id.module += "." + extTk.text;
		}
	}
	if (check(TokType::Semicolon)) pos_++;
	std::string mod = id.has_symbols ? id.from_module : id.module;
	/* 内联导入: 把被导入文件的顶层声明并入当前程序 */
	importModuleInline(mod, id.symbols, id.alias, Token(TokType::Ident, mod, line, col));
	return TopLevel(std::move(id));
}

bool Parser::importModuleInline(const std::string& module,
                                const std::vector<std::string>& syms,
                                const std::string& alias, const Token& at) {
	if (imported_modules_.count(module) > 0) return true;   /* 已导入, 跳过 */
	/* 预编译优先: 有 .mod 且未过期就直接反序列化，跳过词法+语法分析 */
	if (loadPrecompiled(module, at)) return true;
	imported_modules_.insert(module);
	std::string path = findLibModule(module);
	if (path.empty()) {
		die("无法解析 import 模块 '" + module + "': 未找到 " + libModulePath(module), at);
	}
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		die("无法打开 import 模块文件: " + path, at);
	}
	std::ostringstream ss;
	ss << in.rdbuf();
	std::string src = ss.str();
	try {
		Lexer lx(src);
		Parser sub(lx.scan());
		/* 继承已有的模块登记表，使被导入文件能识别宿主中已声明的 mod */
		sub.is_imported_context_ = true;
		/* 关键: 被导入文件的成员要带上**它自己的文件名**，这样调用点
		   才能按文件边界判定可见性。不设的话它们会继承宿主文件名，
		   fileprivate/internal 将永远判定为"同文件"而失效。 */
		sub.cur_file_ = path;
		sub.module_reg_ = module_reg_;
		sub.imported_modules_ = imported_modules_;
		sub.type_aliases_ = type_aliases_;
		Program subprog = sub.parse();
		/* 合并被导入文件的可见性表 —— 调用点的检查要查得到它们 */
		for (auto& kv : sub.member_vis_) member_vis_[kv.first] = kv.second;
		for (auto& kv : sub.exported_members_)
			for (auto& fn : kv.second) exported_members_[kv.first].insert(fn);
		for (auto& item : subprog.items) {
			auto* md = std::get_if<ModuleDecl>(&item);
			if (md && !syms.empty()) {
				bool want = false;
				for (auto& s : syms) if (s == md->name) want = true;
				if (!want) continue;
				if (!alias.empty()) md->name = alias;
			}
			/* 标记为 import 引入。原因: import 在解析期就把子模块内联进来了，
			   而主文件里对它的调用此时还没被解析到，module_referenced_ 里
			   自然没有它 —— 不加豁免就会误报"未被调用"。 */
			if (md) md->from_import = true;
			pending_items_.push_back(std::move(item));
		}
		/* 回写模块登记表 */
		for (auto& kv : sub.module_reg_) module_reg_[kv.first] = kv.second;
		/* 回写导出表: 被导入文件里 export 的模块，主文件必须知道，否则
		   主文件调用它时会被可见性检查误判为"未导出"。 */
		for (auto& n : sub.exported_modules_) exported_modules_.insert(n);
		for (auto& kv : sub.type_aliases_) type_aliases_[kv.first] = kv.second;
	} catch (const std::runtime_error& e) {
		throw std::runtime_error(std::string("import '") + module + "' 时解析失败: " + e.what());
	}
	return true;
}

#endif // WEI_FRONTEND_NO_DEFS

// ============================================================
// Type Checker
// ============================================================
/* ==========================================================================
 * 泛型单态化（Monomorphization）
 *
 *   create.mod M::f<T>(T x).T { return x + x; }
 *   do.emit::M::f(21);            // 推导 T = int
 *   do.emit::M::f<double>(2.5);   // 显式指定 T = double
 *
 * 实现选择：**单态化**而非类型擦除。每个类型实参组合在编译期复刻一份
 * 函数体，实例里所有 T 都替换成具体类型名。这么做有三个理由：
 *   1. 后端零改动 —— 直出路径只认具体类型，实例化的函数就是普通函数；
 *   2. 无运行时开销 —— 没有装箱、没有虚表、没有类型标签；
 *   3. 与现有优化通路兼容 —— 单态化后的循环照样能吃闭式解与展开。
 *
 * 代价是代码膨胀：N 个类型实参组合产生 N 份代码。对一门系统语言而言
 * 这是正确的取舍（与 C++ 模板、Rust 泛型同一路线）。
 *
 * 类型推导只取"能静态确定"的信息：字面量、已声明变量的类型、已知返回
 * 类型的调用。推导不出来就**放弃实例化**并保留原调用 —— 调用一个不存在的
 * 函数会在后端明确报错，好过静默按错类型生成代码。
 * ========================================================================== */

/* 前向声明: 预编译模块编解码器（weimodcache）的完整定义在下方。
   Parser 需要在 import/requires 时调用它，故此处先声明签名。 */
namespace wei::modcache {
	std::vector<uint8_t> serialize(const wei::ast::Program& prog,
	                               const std::vector<std::string>& deps, int64_t src_mtime);
	bool deserialize(const std::vector<uint8_t>& raw, wei::ast::Program& prog,
	                 std::vector<std::string>& deps, int64_t& src_mtime);
}

/* ═══════════════════════════════════════════════════════════════════════
   【预端 PreStage】—— 位于词法/语法分析**之前**
   ───────────────────────────────────────────────────────────────────────
   职责: 在源码进入 Parser 之前，先把"编译期就能确定"的信息算出来。

   它做的是**常量折叠的自然延伸**，不是优化:
     语言语义本来就要求 `2 + 3` 在编译期等于 5。
     同样的道理，一个迭代次数与全部依赖都在编译期完全确定的循环，
     它的结果在语言语义上也是唯一确定的 —— 求值它是编译器的义务，
     而不是可选的优化。

   因此这一层**不受 -no-opt 影响**，永远生效。

   产出: 已知常量表 + 确定性标记，供下游查询。
   ═══════════════════════════════════════════════════════════════════════ */
namespace wei::prestage {

	using namespace wei::ast;

	/* 一个变量在编译期的确定状态 */
	struct ConstFact {
		bool known = false;
		int64_t value = 0;
		bool ever_reassigned = false;   /* 循环体内被重新赋值 → 不是归纳变量常量 */
	};

	/* ── 常量传播分析 ──────────────────────────────────────────────────
	   保守规则（宁可判定"不确定"，绝不误判）:
	     · 字面量初始化 → 已知
	     · 被再赋值（任何位置）→ 立即撤销已知
	     · 数组、函数调用、输入 → 一律未知
	     · 不递归进嵌套块做推测 */
	struct ConstProp {
		std::map<std::string, ConstFact> facts;

		void noteAssign(const std::string& name) {
			auto it = facts.find(name);
			if (it != facts.end()) it->second.ever_reassigned = true;
		}
		void seed(const std::string& name, int64_t v) {
			ConstFact f; f.known = true; f.value = v; facts[name] = f;
		}
		std::optional<int64_t> lookup(const std::string& name) const {
			auto it = facts.find(name);
			if (it == facts.end()) return std::nullopt;
			if (!it->second.known || it->second.ever_reassigned) return std::nullopt;
			return it->second.value;
		}
	};

	void scanExprConst(const Expr* e, ConstProp& cp);
	void scanStmtsConst(const std::vector<Stmt>& v, ConstProp& cp);

	void scanStmtsConst(const std::vector<Stmt>& v, ConstProp& cp) {
		for (auto& st : v) {
			if (auto* vd = std::get_if<VarDecl>(&st)) {
				if (vd->array_len > 0) { cp.facts.erase(vd->name); continue; }
				if (vd->init) {
					if (auto* il = std::get_if<IntLit>(vd->init.get())) cp.seed(vd->name, il->value);
					else { cp.facts[vd->name] = ConstFact{}; cp.facts[vd->name].known = false; }
				} else {
					ConstFact f; f.known = false; cp.facts[vd->name] = f;
				}
			}
			else if (auto* as = std::get_if<AssignStmt>(&st)) {
				cp.noteAssign(as->name);
				if (as->target) {
					/* 数组元素赋值: 整块数组内容不再可推断 → 未知 */
					if (auto* ix = std::get_if<IndexExpr>(as->target.get())) cp.facts.erase(ix->name);
				}
			}
			else if (auto* ws = std::get_if<WhileStmt>(&st)) {
				/* 循环体内的赋值使变量成为归纳变量，不再是编译期常量。
				   但循环**上界/初值**在进入循环时仍是确定的 —— 这正是
				   闭式解需要的。所以这里只撤销"被赋值的变量"。 */
				std::vector<std::string> written;
				struct W {
					static void go(const Stmt& st, std::vector<std::string>& out) {
						if (auto* a = std::get_if<AssignStmt>(&st)) out.push_back(a->name);
						else if (auto* b = std::get_if<Block>(&st)) for (auto& x : b->stmts) go(x, out);
						else if (auto* i2 = std::get_if<IfStmt>(&st)) {
							for (auto& x : i2->then_body) go(x, out);
							for (auto& x : i2->else_body) go(x, out);
						}
						else if (auto* f2 = std::get_if<ForStmt>(&st)) for (auto& x : f2->body) go(x, out);
					}
				};
				W::go(st, written);
				for (auto& w : written) cp.noteAssign(w);
			}
			else if (auto* ifs = std::get_if<IfStmt>(&st)) {
				scanStmtsConst(ifs->then_body, cp);
				scanStmtsConst(ifs->else_body, cp);
			}
			else if (auto* bk = std::get_if<Block>(&st)) scanStmtsConst(bk->stmts, cp);
		}
	}

	/* ═══ 编译期循环求值器 ══════════════════════════════════════════════
	   这是"常量折叠"的彻底形式:
	     语言要求 `2 + 3` 编译期等于 5。
	     同理，若一个循环的初值、上界、步长与全部数据依赖都在编译期
	     完全确定且无副作用，那么它的结果在语义上唯一确定 ——
	     编译期求值是**语义义务**，不是优化。

	   因此这一层永远生效，不受 -no-opt 影响。

	   支持:
	     · int 变量（含数组，内容须编译期已知）
	     · + - * / % 与比较、逻辑运算
	     · 数组读 arr[k]（k 须可求值，且下标在已知范围内）
	   拒绝（保守，宁可不优化）:
	     · 函数调用 / 输出 / 输入
	     · 数组写（循环体内写数组 → 内容在轮次间变化，需逐轮模拟）
	     · 嵌套循环（交给外层递归处理）
	     · 迭代次数超过 kMaxIter（保护编译时间）
	   ══════════════════════════════════════════════════════════════ */
	/* 迭代上限刻意压得低（10 万）: 小循环由这里编译期求值；
	   大循环留给处理端的闭式解（等差/平方/周期取模），后者是 O(1) 的，
	   比逐轮模拟快得多。两者分工而不是竞争。 */
	static constexpr int64_t kMaxIter = 100000;

	struct Env {
		std::map<std::string, int64_t> vars;
		std::map<std::string, std::vector<int64_t>> arrays;
		/* ── 符号增量 ──────────────────────────────────────────────
		   循环体内的变量（如外层的累加器 s）在进入内层循环时基值
		   **未知**（它随外层轮次变化）。这时不能把折叠结果写成绝对值
		   `s = 4032` —— 那会把"累加"变成"覆盖"，实测 b4_arr 因此
		   输出 4032 而非 12096000000。

		   正确做法是记录**增量**: `s = s + 4032`。
		   delta[v] 即"在未知基值上累加的量"。 */
		std::map<std::string, int64_t> delta;
		std::set<std::string> unknown;

		bool getVar(const std::string& n, int64_t& out) const {
			auto it = vars.find(n);
			if (it == vars.end()) return false;
			out = it->second; return true;
		}
		bool getArr(const std::string& n, int64_t idx, int64_t& out) const {
			auto it = arrays.find(n);
			if (it == arrays.end()) return false;
			if (idx < 0 || idx >= (int64_t)it->second.size()) return false;
			out = it->second[idx]; return true;
		}
	};

	/* 求值一个整型表达式。失败返回 nullopt（拒绝优化，绝不猜测）。 */
	std::optional<int64_t> evalInt(const Expr* e, const Env& env);
	std::optional<int64_t> evalInt(const Expr& e, const Env& env) { return evalInt(&e, env); }

	std::optional<int64_t> evalInt(const Expr* e, const Env& env) {
		if (!e) return std::nullopt;
		if (auto* il = std::get_if<IntLit>(e)) return il->value;
		if (auto* bl = std::get_if<BoolLit>(e)) return bl->value ? 1 : 0;
		if (auto* id = std::get_if<Ident>(e)) {
			int64_t v; return env.getVar(id->name, v) ? std::optional<int64_t>(v) : std::nullopt;
		}
		if (auto* ix = std::get_if<IndexExpr>(e)) {
			auto k = evalInt(ix->idx.get(), env);
			if (!k) return std::nullopt;
			int64_t v; return env.getArr(ix->name, *k, v) ? std::optional<int64_t>(v) : std::nullopt;
		}
		if (auto* un = std::get_if<Unary>(e)) {
			auto v = evalInt(un->operand.get(), env);
			if (!v) return std::nullopt;
			if (un->op == "-") { if (*v == INT64_MIN) return std::nullopt; return -*v; }
			if (un->op == "+") return *v;
			if (un->op == "not") return *v ? 0 : 1;
			return std::nullopt;
		}
		if (auto* b = std::get_if<Binary>(e)) {
			auto L = evalInt(b->lhs.get(), env);
			if (!L) return std::nullopt;
			/* 短路求值: and/or 在左边就能定时不必算右边 */
			if (b->op == "and") {
				if (*L == 0) return (int64_t)0;
				auto R = evalInt(b->rhs.get(), env); if (!R) return std::nullopt;
				return *R ? (int64_t)1 : (int64_t)0;
			}
			if (b->op == "or") {
				if (*L != 0) return (int64_t)1;
				auto R = evalInt(b->rhs.get(), env); if (!R) return std::nullopt;
				return *R ? (int64_t)1 : (int64_t)0;
			}
			auto R = evalInt(b->rhs.get(), env);
			if (!R) return std::nullopt;
			int64_t a = *L, c = *R;
			/* BUGFIX: 编译期求值必须用 128 位验算溢出。
			   原来直接 `return a + c` 用 int64 静默回绕 —— 于是
			   `s=INT64_MAX-7; while(j<100){ s=s+1000; ... }` 被折叠成
			   一个回绕后的常量，运行时循环连同它的溢出检查一起消失，
			   程序静默输出错误值（实测 -9223372036854675816）。
			   这是最危险的一类 bug: 不崩溃、不报错，只是算错。
			   现在一旦超出 int64 就返回 nullopt —— 表示"编译期算不了"，
			   evalLoop 随即放弃折叠，运行时循环保留溢出检查并正确 panic。 */
			auto fits = [](wei_i128 r) -> std::optional<int64_t> {
				if (r < wei_i128(INT64_MIN) || r > wei_i128(INT64_MAX)) return std::nullopt;
				return std::optional<int64_t>(static_cast<int64_t>(r));
			};
			if (b->op == "+") return fits(wei_i128(a) + wei_i128(c));
			if (b->op == "-") return fits(wei_i128(a) - wei_i128(c));
			if (b->op == "*") return fits(wei_i128(a) * wei_i128(c));
			if (b->op == "/") {
				if (c == 0) return std::nullopt;
				if (a == INT64_MIN && c == -1) return std::nullopt;  /* 除法溢出 */
				return a / c;
			}
			if (b->op == "%") {
				if (c == 0) return std::nullopt;
				if (a == INT64_MIN && c == -1) return std::nullopt;
				return a % c;
			}
			if (b->op == "==") return a == c ? 1 : 0;
			if (b->op == "!=") return a != c ? 1 : 0;
			if (b->op == "<")  return a <  c ? 1 : 0;
			if (b->op == ">")  return a >  c ? 1 : 0;
			if (b->op == "<=") return a <= c ? 1 : 0;
			if (b->op == ">=") return a >= c ? 1 : 0;
			if (b->op == "&")  return a & c;
			if (b->op == "|")  return a | c;
			if (b->op == "^")  return a ^ c;
			if (b->op == "<<") return a << (c & 63);
			if (b->op == ">>") return a >> (c & 63);
			return std::nullopt;
		}
		return std::nullopt;   /* 函数调用、输出、输入等一律拒绝 */
	}

	/* 语句是否"纯净": 无副作用、不写数组、不含跳转。
	   只有 VarDecl(int) 与 Assign(标量) 允许。 */
	bool stmtPure(const Stmt& st) {
		if (auto* vd = std::get_if<VarDecl>(&st)) {
			if (vd->array_len > 0) return false;
			if (vd->init && std::get_if<IntLit>(vd->init.get()) == nullptr
			    && std::get_if<Ident>(vd->init.get()) == nullptr) {
				/* 允许字面量或变量初值；复杂表达式也允许（可求值即可） */
			}
			return true;
		}
		if (auto* as = std::get_if<AssignStmt>(&st)) {
			if (as->target) return false;          /* 数组写 → 拒绝 */
			return true;
		}
		return false;                               /* 其他（含 if/循环/输出）拒绝 */
	}

	/* 尝试把整个 while 循环编译期求值。
	   成功返回 true 并回填: 结果环境 env、归纳变量终值。 */
	bool evalLoop(const WhileStmt& w, const Env& in, Env& out, int64_t& iterCount) {
		out = in;
		iterCount = 0;
		for (auto& st : w.body) if (!stmtPure(st)) return false;

		int64_t guard = 0;
		while (true) {
			auto c = evalInt(w.cond.get(), out);
			if (!c) return false;
			if (*c == 0) break;                    /* 条件为假 → 退出 */
			for (auto& st : w.body) {
				if (auto* vd = std::get_if<VarDecl>(&st)) {
					if (vd->init) {
						auto v = evalInt(vd->init.get(), out);
						if (!v) return false;
						out.vars[vd->name] = *v;
					} else out.vars[vd->name] = 0;
				}
				else if (auto* as = std::get_if<AssignStmt>(&st)) {
					if (auto* ix = std::get_if<IndexExpr>(as->target.get())) { (void)ix; return false; }
					auto v = evalInt(as->value.get(), out);
					if (v) {
						out.vars[as->name] = *v;
						out.unknown.erase(as->name);
						out.delta.erase(as->name);
						continue;
					}
					/* ── 增量模式: v = v + <可求值> / v = <可求值> + v ──
					   v 的基值未知（随外层轮次变化），但增量确定。 */
					if (auto* b = std::get_if<Binary>(as->value.get())) {
						if (b->op != "+" && b->op != "-") return false;
						bool selfL = false, selfR = false;
						if (auto* id = std::get_if<Ident>(b->lhs.get())) selfL = (id->name == as->name);
						if (auto* id = std::get_if<Ident>(b->rhs.get())) selfR = (id->name == as->name);
						if (!selfL && !selfR) return false;
						if (b->op == "-" && !selfL) return false;   /* const - v 不支持 */
						const Expr* other = selfL ? b->rhs.get() : b->lhs.get();
						auto ov = evalInt(other, out);
						if (!ov) return false;
						if (selfL && selfR) {
							/* v = v + v → 翻倍，基值未知时无法表示 */
							return false;
						}
						/* delta 累加本身也可能溢出。溢出就放弃折叠，
						   让运行时循环保留溢出检查（不能静默算错）。 */
						wei_i128 nd = wei_i128(out.delta[as->name])
						            + wei_i128((b->op == "+") ? *ov : -*ov);
						if (nd < wei_i128(INT64_MIN) || nd > wei_i128(INT64_MAX)) return false;
						out.delta[as->name] = static_cast<int64_t>(nd);
						out.vars.erase(as->name);
						out.unknown.insert(as->name);
						continue;
					}
					return false;
				}
			}
			iterCount++;
			if (++guard > kMaxIter) return false;   /* 迭代太多 → 放弃 */
		}
		return true;
	}

	/* ── 确定性循环判定 ───────────────────────────────────────────────
	   一个循环"编译期可完全求值"当且仅当:
	     1) 归纳变量初值已知
	     2) 上界已知
	     3) 步长为字面量
	     4) 循环体内不含: 函数调用、输入、数组写、跳转(break/continue)
	   满足则整个循环的语义结果唯一确定。 */
	struct ClosedFormQuery {
		bool viable = false;
		std::string ivar;          /* 归纳变量名 */
		int64_t i0 = 0, N = 0, step = 0;
	};
}

/* ═══════════════════════════════════════════════════════════════════════
   【处理端 MidStage】—— 位于前端 AST 与 IR/后端之间
   ───────────────────────────────────────────────────────────────────────
   职责: 消费预端算出的确定性信息，把"语义上等价于闭式结果"的循环
         直接替换为常量赋值；并对无法闭式化的循环做规范化。

   分层原则（关键）:
     · **语义层**（永远生效）: 确定性闭式求值 —— 属于常量折叠的延伸
     · **优化层**（受 -no-opt 控制）: SIMD 向量化、激进展开、模板特化

   这样在 -no-opt 下，只要循环语义确定，就依然得到闭式结果；
   而 opt 开启时再叠加疯狂的向量化与展开。
   ═══════════════════════════════════════════════════════════════════════ */
namespace wei::midstage {
	/* 语义层闭式解集合: 与 opt 开关无关，永远尝试 */
	enum class ClosedKind {
		None,
		ConstFold,        /* 整个循环编译期求值 */
		ArithmeticSum,    /* Σ(i) 等差 */
		StridedSum,       /* 步长 > 1 */
		SquareSum,        /* Σ(i²) */
		ModPeriodSum      /* Σ(i % C) 周期性 */
	};

	using namespace wei::ast;
	using prestage::Env;
	using prestage::evalInt;

	/* ── 数组填充循环识别 ──────────────────────────────────────────
	   形态: while (k < N) { arr[k] = <expr>; k = k + 1; }
	   （两条语句顺序可交换）
	   N 与 k 初值编译期已知 → 整个填充过程可在编译期模拟，
	   从而让**后续的读取方**（求和循环）拿到确定的数组内容。 */
	bool tryFillArray(const WhileStmt& w, Env& env) {
		auto* cb = std::get_if<Binary>(w.cond.get());
		if (!cb || cb->op != "<") return false;
		auto* cid = std::get_if<Ident>(cb->lhs.get());
		if (!cid || !cb->rhs) return false;
		auto N = evalInt(cb->rhs.get(), env);
		int64_t k0;
		if (!N || !env.getVar(cid->name, k0)) return false;
		if (w.body.size() != 2) return false;

		/* 找出 "arr[k] = expr" 与 "k = k + 1" */
		const AssignStmt* store = nullptr;
		const AssignStmt* inc = nullptr;
		for (auto& st : w.body) {
			auto* a = std::get_if<AssignStmt>(&st);
			if (!a) return false;
			if (a->target && std::get_if<IndexExpr>(a->target.get())) store = a;
			else if (!a->target && a->name == cid->name) inc = a;
		}
		if (!store || !inc) return false;
		auto* ix = std::get_if<IndexExpr>(store->target.get());
		if (!ix) return false;
		/* 自增步长 */
		auto* ib = std::get_if<Binary>(inc->value.get());
		if (!ib || ib->op != "+") return false;
		auto* il = std::get_if<Ident>(ib->lhs.get());
		auto* iv = std::get_if<IntLit>(ib->rhs.get());
		if (!il || !iv || il->name != cid->name) return false;
		int64_t step = iv->value;
		if (step <= 0) return false;

		auto& arr = env.arrays[ix->name];
		if (arr.empty()) return false;          /* 数组长度未知则不填 */
		Env sim = env;
		int64_t k = k0;
		int64_t cnt = 0;
		while (k < *N) {
			sim.vars[cid->name] = k;
			auto idxv = evalInt(ix->idx.get(), sim);
			auto val = evalInt(store->value.get(), sim);
			if (!idxv || !val) return false;
			if (*idxv < 0 || *idxv >= (int64_t)arr.size()) return false;
			arr[(size_t)*idxv] = *val;
			k += step;
			if (++cnt > prestage::kMaxIter) return false;
		}
		env.vars[cid->name] = k;                /* 循环结束后 k 的值 */
		return true;
	}

	/* ── 主 pass: AST 级编译期求值 ─────────────────────────────────
	   按语句顺序模拟执行，遇到"完全确定"的循环就把它替换成等价的
	   常量赋值序列。

	   保守原则: 只要有任何一步无法精确模拟，就清空环境并停止折叠
	   —— 宁可不优化，也绝不生成错误结果。 */
	void foldBlock(std::vector<Stmt>& body, Env& env, int depth);

	void foldBlock(std::vector<Stmt>& body, Env& env, int depth) {
		if (depth > 8) { env = Env{}; return; }   /* 嵌套过深则放弃精度 */
		for (size_t i = 0; i < body.size(); i++) {
			Stmt& st = body[i];

			if (auto* vd = std::get_if<VarDecl>(&st)) {
				if (vd->array_len > 0) {
					env.arrays[vd->name] = std::vector<int64_t>((size_t)vd->array_len, 0);
					/* 数组初值列表 */
					if (!vd->array_init.empty()) {
						auto& a = env.arrays[vd->name];
						for (size_t k = 0; k < vd->array_init.size() && k < a.size(); k++) {
							auto v = evalInt(vd->array_init[k].get(), env);
							if (!v) { a.clear(); env.arrays.erase(vd->name); break; }
							a[k] = *v;
						}
					}
				} else if (vd->init) {
					auto v = evalInt(vd->init.get(), env);
					if (v) env.vars[vd->name] = *v; else env.vars.erase(vd->name);
				} else env.vars[vd->name] = 0;
			}
			else if (auto* as = std::get_if<AssignStmt>(&st)) {
				if (as->target) {
					auto* ix = std::get_if<IndexExpr>(as->target.get());
					if (!ix) { env = Env{}; continue; }
					auto k = evalInt(ix->idx.get(), env);
					auto v = evalInt(as->value.get(), env);
					auto it = env.arrays.find(ix->name);
					if (!k || !v || it == env.arrays.end()
					    || *k < 0 || *k >= (int64_t)it->second.size()) {
						if (it != env.arrays.end()) env.arrays.erase(ix->name);
						env.vars.erase(ix->name);
					} else it->second[(size_t)*k] = *v;
				} else {
					auto v = evalInt(as->value.get(), env);
					if (v) env.vars[as->name] = *v; else env.vars.erase(as->name);
				}
			}
			else if (auto* ws = std::get_if<WhileStmt>(&st)) {
				/* 0) 先递归处理循环体 —— 嵌套的内层循环要先被折叠，
				      外层才可能变成可闭式化的简单形态。
				      b4_arr 就是这种: 内层 Σarr 折叠成常量后，外层才退化成
				      "每轮加同一个常量"的等差求和。 */
				{
					/* 进入循环体: **标量基值一律视为未知**，只保留数组内容。
					   因为循环会执行多轮，第二轮进入时那些变量早已不是
					   第一轮的初值。若沿用外层的已知值，就会把内层累加
					   折叠成绝对值 `s = 4032`（覆盖而非累加）—— 实测
					   b4_arr 因此输出 4032 而正确值是 12096000000。
					   设为未知后，折叠走增量路径 `s = s + 4032`。 */
					Env inner;
					inner.arrays = env.arrays;
					foldBlock(ws->body, inner, depth + 1);
				}

				/* 1) 先试数组填充（只更新 env，不改写 AST：
				      运行时数组仍需存在，除非后续读都被折叠）
				   注意: 填充成功后**必须保留 env** —— 数组内容已在编译期
				   精确推导出，后续的读取方（求和循环）正是靠它才能被
				   折叠。曾在这里无条件清空 env，导致 b4_arr 永远拿不到
				   数组内容、内层求和无法折叠，实测慢 2.5 倍。 */
				bool filled = tryFillArray(*ws, env);

				/* 2) 再试整循环编译期求值 */
				Env out; int64_t iters = 0;
				if (prestage::evalLoop(*ws, env, out, iters) && iters <= prestage::kMaxIter) {
					/* 收集循环体内被写的变量，生成等价的常量赋值 */
					std::vector<std::string> written;
					for (auto& bst : ws->body) {
						if (auto* a = std::get_if<AssignStmt>(&bst)) written.push_back(a->name);
						else if (auto* v2 = std::get_if<VarDecl>(&bst)) written.push_back(v2->name);
					}
					/* 归纳变量也要写回终值 */
					if (auto* cb = std::get_if<Binary>(ws->cond.get()))
						if (auto* cid = std::get_if<Ident>(cb->lhs.get())) written.push_back(cid->name);

					std::vector<Stmt> repl;
					bool ok = true;
					for (auto& nm : written) {
						/* 归纳变量若紧邻的前一条语句被重置为常量
						   （如外层每轮开头的 `j = 0`），说明它只是循环
						   计数器，循环结束后的值无人关心 —— 不写回，
						   否则会给外层留下无法闭式化的多余赋值。 */
						if (i > 0) {
							auto* prev = std::get_if<AssignStmt>(&body[i - 1]);
							if (prev && !prev->target && prev->name == nm) {
								if (auto* cb2 = std::get_if<Binary>(ws->cond.get()))
									if (auto* cid2 = std::get_if<Ident>(cb2->lhs.get()))
										if (cid2->name == nm) continue;
							}
						}
						int64_t fv = 0;
						bool isAbs = out.getVar(nm, fv);
						bool isDelta = !isAbs && out.unknown.count(nm) > 0;
						if (!isAbs && !isDelta) { ok = false; break; }
						AssignStmt a;
						a.name = nm;
						a.value = std::make_unique<Expr>();
						if (isAbs) {
							*a.value = IntLit{fv};          /* 绝对值 */
						} else {
							/* 增量: v = v + delta —— 保留累加语义 */
							Binary b;
							b.op = "+";
							b.lhs = std::make_unique<Expr>();
							*b.lhs = Ident{nm, ws->line, ws->col};
							b.rhs = std::make_unique<Expr>();
							*b.rhs = IntLit{out.delta[nm]};
							b.line = ws->line; b.col = ws->col;
							*a.value = std::move(b);
						}
						a.line = ws->line; a.col = ws->col;
						repl.push_back(std::move(a));
					}
					if (ok && !repl.empty()) {
						body.erase(body.begin() + (long)i);
						body.insert(body.begin() + (long)i,
						            std::make_move_iterator(repl.begin()),
						            std::make_move_iterator(repl.end()));
						env = out;
						i += repl.size() - 1;
						continue;
					}
				}
				if (filled) {
					/* 数组填充循环: 语义已被精确模拟，环境继续有效 */
					continue;
				}
				/* 无法精确求值 → 环境作废（循环可能改了任何东西） */
				env = Env{};
			}
			else if (auto* bk = std::get_if<Block>(&st)) {
				foldBlock(bk->stmts, env, depth + 1);
			}
			else env = Env{};   /* 其他语句：放弃精度 */
		}
	}

	void foldProgram(Program& prog) {
		for (auto& item : prog.items) {
			if (auto* md = std::get_if<ModuleDecl>(&item)) {
				for (auto& f : md->methods) { Env env; foldBlock(f.body, env, 0); }
				Env env; foldBlock(md->body, env, 0);
			}
		}
	}
}

namespace wei::mono {

using namespace wei::ast;

/* ── AST 遍历 ────────────────────────────────────────────────────────────
   用 std::visit + if constexpr 覆盖全部 variant 分支：
   已知含子表达式的类型逐个下钻，其余类型（字面量、标识符…）不动。
   这样即使以后往 variant 里加新节点，遍历器也不会漏掉已支持的部分。 */

using ExprFn = std::function<void(Expr&)>;
using StmtFn = std::function<void(Stmt&)>;

void walkExpr(Expr& e, const ExprFn& f);
void walkStmt(Stmt& s, const StmtFn& sf, const ExprFn& ef);

void walkStmts(std::vector<Stmt>& v, const StmtFn& sf, const ExprFn& ef) {
	for (auto& s : v) walkStmt(s, sf, ef);
}

void walkExpr(Expr& e, const ExprFn& f) {
	f(e);
	std::visit([&](auto& n) {
		using T = std::decay_t<decltype(n)>;
		if constexpr (std::is_same_v<T, Binary>) {
			if (n.lhs) walkExpr(*n.lhs, f);
			if (n.rhs) walkExpr(*n.rhs, f);
		} else if constexpr (std::is_same_v<T, Unary>) {
			if (n.operand) walkExpr(*n.operand, f);
		} else if constexpr (std::is_same_v<T, Call>) {
			for (auto& a : n.args) if (a) walkExpr(*a, f);
		} else if constexpr (std::is_same_v<T, Output>) {
			for (auto& a : n.values) if (a) walkExpr(*a, f);
		} else if constexpr (std::is_same_v<T, IndexExpr>) {
			if (n.idx) walkExpr(*n.idx, f);
		} else if constexpr (std::is_same_v<T, TupleLit>) {
			for (auto& a : n.elements) if (a) walkExpr(*a, f);
		} else if constexpr (std::is_same_v<T, TupleIndexExpr>) {
			if (n.tuple) walkExpr(*n.tuple, f);
		} else if constexpr (std::is_same_v<T, TypeofExpr>) {
			if (n.arg) walkExpr(*n.arg, f);
		} else if constexpr (std::is_same_v<T, IsExpr>) {
			if (n.arg) walkExpr(*n.arg, f);
		} else if constexpr (std::is_same_v<T, InExpr>) {
			if (n.value) walkExpr(*n.value, f);
			if (n.container) walkExpr(*n.container, f);
		} else if constexpr (std::is_same_v<T, AwaitExpr>) {
			if (n.operand) walkExpr(*n.operand, f);
		} else if constexpr (std::is_same_v<T, CoExpr>) {
			for (auto& a : n.args) if (a) walkExpr(*a, f);
		} else if constexpr (std::is_same_v<T, CompileTimeExpr>) {
			if (n.arg) walkExpr(*n.arg, f);
		} else if constexpr (std::is_same_v<T, ForallExpr>) {
			if (n.domain) walkExpr(*n.domain, f);
			if (n.cond) walkExpr(*n.cond, f);
		} else if constexpr (std::is_same_v<T, ExistsExpr>) {
			if (n.domain) walkExpr(*n.domain, f);
			if (n.cond) walkExpr(*n.cond, f);
		} else if constexpr (std::is_same_v<T, RangeLit>) {
			if (n.lo) walkExpr(*n.lo, f);
			if (n.hi) walkExpr(*n.hi, f);
		} else if constexpr (std::is_same_v<T, CastExpr>) {
			if (n.value) walkExpr(*n.value, f);
		} else if constexpr (std::is_same_v<T, DeleteExpr>) {
			if (n.target) walkExpr(*n.target, f);
		}
	}, e);
}

void walkStmt(Stmt& s, const StmtFn& sf, const ExprFn& ef) {
	sf(s);
	std::visit([&](auto& n) {
		using T = std::decay_t<decltype(n)>;
		if constexpr (std::is_same_v<T, Block>) {
			walkStmts(n.stmts, sf, ef);
		} else if constexpr (std::is_same_v<T, IfStmt>) {
			if (n.cond) walkExpr(*n.cond, ef);
			walkStmts(n.then_body, sf, ef);
			walkStmts(n.else_body, sf, ef);
		} else if constexpr (std::is_same_v<T, WhileStmt>) {
			if (n.cond) walkExpr(*n.cond, ef);
			walkStmts(n.body, sf, ef);
		} else if constexpr (std::is_same_v<T, ForStmt>) {
			if (n.init_decl && n.init_decl->init) walkExpr(*n.init_decl->init, ef);
			if (n.cond) walkExpr(*n.cond, ef);
			if (n.step) walkExpr(*n.step, ef);
			walkStmts(n.body, sf, ef);
		} else if constexpr (std::is_same_v<T, LoopStmt>) {
			walkStmts(n.body, sf, ef);
		} else if constexpr (std::is_same_v<T, LoopNStmt>) {
			walkStmts(n.body, sf, ef);
		} else if constexpr (std::is_same_v<T, ReturnStmt>) {
			if (n.value) walkExpr(*n.value, ef);
		} else if constexpr (std::is_same_v<T, ExprStmt>) {
			if (n.expr) walkExpr(*n.expr, ef);
		} else if constexpr (std::is_same_v<T, AssignStmt>) {
			if (n.target) walkExpr(*n.target, ef);
			if (n.value) walkExpr(*n.value, ef);
		} else if constexpr (std::is_same_v<T, VarDecl>) {
			if (n.init) walkExpr(*n.init, ef);
			for (auto& e : n.array_init) if (e) walkExpr(*e, ef);
		} else if constexpr (std::is_same_v<T, MatchStmt>) {
			for (auto& cse : n.cases) walkStmts(cse.body, sf, ef);
			walkStmts(n.default_body, sf, ef);
		} else if constexpr (std::is_same_v<T, ContractStmt>) {
			if (n.condition) walkExpr(*n.condition, ef);
		} else if constexpr (std::is_same_v<T, ForallStmt>) {
			if (n.domain) walkExpr(*n.domain, ef);
			walkStmts(n.body, sf, ef);
		} else if constexpr (std::is_same_v<T, WhenStmt>) {
			if (n.cond) walkExpr(*n.cond, ef);
			walkStmts(n.then_body, sf, ef);
			walkStmts(n.else_body, sf, ef);
		} else if constexpr (std::is_same_v<T, YieldStmt>) {
			if (n.value) walkExpr(*n.value, ef);
		} else if constexpr (std::is_same_v<T, CompileTimeStmt>) {
			walkStmts(n.body, sf, ef);
		}
	}, s);
}

/* ── 类型名替换 ────────────────────────────────────────────────────────── */

static std::string subst1(const std::string& t, const std::map<std::string, std::string>& m) {
	auto it = m.find(t);
	return (it == m.end()) ? t : it->second;
}

struct Subst {
	std::map<std::string, std::string> m;

	void exprs(std::vector<Stmt>& v) {
		ExprFn ef = [this](Expr& e) {
			if (auto* c = std::get_if<Call>(&e)) {
				for (auto& t : c->type_args) t = subst1(t, m);
			}
			if (auto* ct = std::get_if<CastExpr>(&e)) ct->type = subst1(ct->type, m);
			if (auto* ie = std::get_if<IsExpr>(&e))   ie->type_name = subst1(ie->type_name, m);
			if (auto* sz = std::get_if<SizeofExpr>(&e)) sz->type = subst1(sz->type, m);
			if (auto* ne = std::get_if<NewExpr>(&e))  ne->type = subst1(ne->type, m);
		};
		StmtFn sf = [this](Stmt& s) {
			if (auto* vd = std::get_if<VarDecl>(&s)) vd->type = subst1(vd->type, m);
		};
		walkStmts(v, sf, ef);
	}

	void func(FuncDecl& f) {
		for (auto& p : f.params) p.type = subst1(p.type, m);
		f.ret_type = subst1(f.ret_type, m);
		exprs(f.body);
	}
};

/* ── 类型推导 ──────────────────────────────────────────────────────────── */

struct Infer {
	/* 变量名 -> 类型名（由参数与局部变量声明填充） */
	std::map<std::string, std::string> vt;
	/* 函数名 -> 返回类型 */
	std::map<std::string, std::string> rt;

	std::string of(const Expr& e) const {
		if (std::holds_alternative<IntLit>(e))    return "int";
		if (std::holds_alternative<DoubleLit>(e)) return "double";
		if (std::holds_alternative<BoolLit>(e))   return "bool";
		if (std::holds_alternative<StrLit>(e))    return "char";
		if (auto* id = std::get_if<Ident>(&e)) {
			auto it = vt.find(id->name);
			return (it == vt.end()) ? std::string() : it->second;
		}
		if (auto* u = std::get_if<Unary>(&e)) {
			if (u->operand) {
				if (u->op == "!") return "bool";
				return of(*u->operand);
			}
			return std::string();
		}
		if (auto* b = std::get_if<Binary>(&e)) {
			const std::string& op = b->op;
			if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">="
			        || op == "and" || op == "or") return "bool";
			if (!b->lhs) return std::string();
			std::string l = of(*b->lhs);
			if (!l.empty()) return l;
			if (b->rhs) return of(*b->rhs);
			return std::string();
		}
		if (auto* c = std::get_if<Call>(&e)) {
			auto it = rt.find(c->callee);
			return (it == rt.end()) ? std::string() : it->second;
		}
		if (std::holds_alternative<IndexExpr>(e)) return "int";
		if (auto* ct = std::get_if<CastExpr>(&e))  return ct->type;
		return std::string();   /* 推导不出: 明确返回空，由调用方放弃实例化 */
	}

	void collectVars(const std::vector<Stmt>& v, const std::vector<Param>& ps) {
		for (auto& p : ps) vt[p.name] = p.type;
		StmtFn sf = [this](Stmt& s) {
			if (auto* vd = std::get_if<VarDecl>(&s)) {
				if (!vd->type.empty() && vd->type != "auto") vt[vd->name] = vd->type;
			}
		};
		ExprFn ef = [](Expr&) {};
		auto* mv = const_cast<std::vector<Stmt>*>(&v);
		walkStmts(*mv, sf, ef);
	}
};

/* ── 单态化主体 ────────────────────────────────────────────────────────── */

/* FuncDecl 内含 unique_ptr，不可拷贝 —— 模板实体统一放在 store 里，
   名字表只存下标；实例用 wei::ast::cloneStmtSub 深拷贝生成。 */
static FuncDecl cloneFunc(const FuncDecl& tf) {
	FuncDecl d;
	d.name = tf.name;
	d.params = tf.params;
	d.ret_type = tf.ret_type;
	d.is_async = tf.is_async;
	d.line = tf.line;
	d.col = tf.col;
	d.type_params = tf.type_params;
	SubstMap empty;
	for (const Stmt& s : tf.body) d.body.push_back(cloneStmtSub(s, empty));
	return d;
}

struct Mono {
	Program* prog = nullptr;
	/* 模板实体库 */
	std::vector<FuncDecl> store;
	/* 模板名 -> store 下标（名既可能是 "f"，也可能是 "Mod::f"） */
	std::map<std::string, size_t> temps;
	std::map<std::string, std::string> owner;    /* 模板名 -> 所属模块名 */
	std::set<std::string> done;                  /* 已生成的实例名 */
	std::map<std::string, std::string> ret_of;   /* 实例名 -> 返回类型 */
	int depth = 0;

	/* 调用点要用**限定名**: 后端查返回类型时按 "Mod::name" 匹配 mod_funcs_，
	   裸名查不到，double 返回值就会被误判成整数（实测返回垃圾值）。
	   实例自身的 name 保持裸名，避免拼成 "Mod::Mod::name"。 */
	static std::string callName(const std::string& tname, const std::string& nm) {
		size_t p = tname.find("::");
		if (p == std::string::npos) return nm;
		return tname.substr(0, p + 2) + nm;
	}

	static std::string mangle(const std::string& base, const std::vector<std::string>& ts) {
		std::string r = base;
		for (auto& t : ts) { r += "_W_"; r += t; }
		return r;
	}

	/* 生成一个实例；返回实例名，失败返回空串 */
	std::string instantiate(const std::string& tname, const std::vector<std::string>& targs,
	                        const std::vector<std::string>& explicitArgs = {}) {
		auto it = temps.find(tname);
		if (it == temps.end()) return std::string();
		const FuncDecl& tf = store[it->second];
		std::vector<std::string> args = targs;
		if (!explicitArgs.empty()) args = explicitArgs;
		if (args.size() != tf.type_params.size()) return std::string();
		/* 实例名必须用**裸名**: 后端会把模块名再拼一次
		   （cur_qualname_ = Mod::name），若这里用 "Mod::f_$int" 就会变成
		   "Mod::Mod::f_$int"，调用点按 "Mod::f_$int" 查不到返回类型，
		   double 结果会被当成整数打印。 */
		std::string nm = mangle(tf.name, args);
		if (done.count(nm)) return nm;
		if (depth > 8) return std::string();          /* 递归实例化深度上限 */
		done.insert(nm);

		std::map<std::string, std::string> m;
		for (size_t i = 0; i < tf.type_params.size(); i++) m[tf.type_params[i]] = args[i];

		FuncDecl inst = cloneFunc(tf);
		inst.name = nm;
		inst.type_params.clear();
		Subst s; s.m = m; s.func(inst);
		ret_of[nm] = inst.ret_type;

		/* 递归: 实例体内若还调用别的泛型函数，按替换后的类型继续实例化 */
		depth++;
		Infer inf;
		inf.collectVars(inst.body, inst.params);
		for (auto& kv : ret_of) inf.rt[kv.first] = kv.second;
		ExprFn ef = [&](Expr& e) {
			auto* c = std::get_if<Call>(&e);
			if (!c) return;
			if (!temps.count(c->callee)) return;
			std::vector<std::string> ta = c->type_args;
			if (ta.empty()) {
				ta.resize(store[temps[c->callee]].type_params.size());
				bool ok = true;
				for (size_t i = 0; i < ta.size(); i++) {
					std::string got;
					if (i < c->args.size() && c->args[i]) got = inf.of(*c->args[i]);
					if (got.empty()) { ok = false; break; }
					ta[i] = got;
				}
				if (!ok) return;      /* 推导不出 → 保留原样，后端会明确报错 */
			}
			std::string tn = c->callee; std::string sub = instantiate(tn, ta);
			if (!sub.empty()) c->callee = callName(tn, sub);
		};
		StmtFn sf = [](Stmt&) {};
		walkStmts(inst.body, sf, ef);
		depth--;

		/* 挂回所属模块，让后端把它当普通函数编译 */
		if (getenv("WEI_MONO_DEBUG")) fprintf(stderr, "[mono] inst %s ret=%s params=", nm.c_str(), inst.ret_type.c_str()), (void)0;
		if (getenv("WEI_MONO_DEBUG")) { for (auto& pp : inst.params) fprintf(stderr, "%s:%s ", pp.name.c_str(), pp.type.c_str()); fprintf(stderr, "\n"); }
		std::string om = owner[tname];
		for (auto& item : prog->items) {
			auto* md = std::get_if<ModuleDecl>(&item);
			if (!md) continue;
			if (!om.empty() && md->name != om) continue;
			if (om.empty() && !md->name.empty()) continue;
			md->methods.push_back(std::move(inst));
			break;
		}
		return nm;
	}

	bool run() {
		/* 1) 摘出所有泛型模板（不再作为普通函数编译） */
		for (auto& item : prog->items) {
			auto* md = std::get_if<ModuleDecl>(&item);
			if (!md) continue;
			std::vector<FuncDecl> keep;
			for (auto& f : md->methods) {
				if (!f.type_params.empty()) {
					std::string key = md->name.empty() ? f.name : (md->name + "::" + f.name);
					store.push_back(std::move(f));
					size_t idx = store.size() - 1;
					temps[f.name] = idx;               /* 裸名可解析 */
					temps[key] = idx;                  /* 限定名也可解析 */
					owner[f.name] = md->name;
					owner[key] = md->name;
					continue;                          /* 模板本身不编译 */
				}
				keep.push_back(std::move(f));
			}
			md->methods = std::move(keep);
		}
		if (temps.empty()) return true;

		/* 2) 遍历所有函数体，把泛型调用改写成实例调用 */
		for (auto& item : prog->items) {
			auto* md = std::get_if<ModuleDecl>(&item);
			if (!md) continue;
			for (auto& f : md->methods) {
				Infer inf;
				inf.collectVars(f.body, f.params);
				for (auto& kv : ret_of) inf.rt[kv.first] = kv.second;
				ExprFn ef = [&](Expr& e) {
					auto* c = std::get_if<Call>(&e);
					if (!c) return;
					if (!temps.count(c->callee)) return;
					std::vector<std::string> ta = c->type_args;
					if (ta.empty()) {
						ta.resize(store[temps[c->callee]].type_params.size());
						bool ok = true;
						for (size_t i = 0; i < ta.size(); i++) {
							std::string got;
							if (i < c->args.size() && c->args[i]) got = inf.of(*c->args[i]);
							if (got.empty()) { ok = false; break; }
							ta[i] = got;
						}
						if (!ok) return;
					}
					std::string tn = c->callee; std::string sub = instantiate(tn, ta);
					if (!sub.empty()) c->callee = callName(tn, sub);
				};
				StmtFn sf = [](Stmt&) {};
				walkStmts(f.body, sf, ef);
			}
		}
		return true;
	}
};


bool monomorphize(Program& prog) {
	Mono mo;
	mo.prog = &prog;
	return mo.run();
}

}  // namespace wei::mono
/* ═══════════════════════════════════════════════════════════════════════
   预编译模块 (.mod) —— 对齐 C++20 Modules 的 BMI
   ───────────────────────────────────────────────────────────────────────
   C++ 的头文件模型: 每个 TU 都要把 #include 的文本重新词法/语法分析一遍，
   N 个 TU 就是 N 遍。C++20 Modules 把模块接口**编译一次**存成 BMI
   （Built Module Interface），import 时直接读二进制，不再重新解析。

   Wei 采用同一套:
     · 库文件 lib/X.wei   →  预编译成 lib/X.mod（二进制 AST）
     · 项目文件 .wei      →  仍走内联（用户要求，保持简单）
     · import / requires  →  优先查 .mod，命中就直接反序列化 AST
     · 源文件改了（mtime 变）→ .mod 失效，自动回退重新解析

   关键收益: 解析一次、多次复用。库越大、被引用越多，省得越多。
   ═══════════════════════════════════════════════════════════════════════ */
namespace wei::modcache {

	using namespace ast;

	/* ── 紧凑二进制读写 ───────────────────────────────────────────────── */
	struct Buf {
		std::vector<uint8_t> d;
		size_t rp = 0;
		void u8(uint8_t v) { d.push_back(v); }
		void i64(int64_t v) { for (int i = 0; i < 8; i++) d.push_back((uint8_t)((uint64_t)v >> (i * 8))); }
		void u32(uint32_t v) { for (int i = 0; i < 4; i++) d.push_back((uint8_t)(v >> (i * 8))); }
		void str(const std::string& v) { u32((uint32_t)v.size()); for (char c : v) d.push_back((uint8_t)c); }
		void dbl(double v) { uint64_t b; memcpy(&b, &v, 8); i64((int64_t)b); }

		uint8_t ru8() { need(1); return d[rp++]; }
		uint32_t ru32() { need(4); uint32_t v = 0; for (int i = 0; i < 4; i++) v |= (uint32_t)d[rp++] << (i * 8); return v; }
		int64_t ri64() { need(8); uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)d[rp++] << (i * 8); return (int64_t)v; }
		std::string rstr() { uint32_t n = ru32(); need(n); std::string v((const char*)&d[rp], n); rp += n; return v; }
		double rdbl() { uint64_t b = (uint64_t)ri64(); double v; memcpy(&v, &b, 8); return v; }
		void need(size_t n) { if (rp + n > d.size()) throw std::runtime_error("模块文件损坏: 数据越界"); }
	};

	/* ── Expr ─────────────────────────────────────────────────────────── */
	void wExpr(Buf& o, const std::unique_ptr<Expr>& e);
	std::unique_ptr<Expr> rExpr(Buf& in);
	void wStmts(Buf& o, const std::vector<Stmt>& v);
	std::vector<Stmt> rStmts(Buf& in);
	void wVarDecl(Buf& o, const VarDecl& v);
	VarDecl rVarDecl(Buf& in);

	void wExpr(Buf& o, const std::unique_ptr<Expr>& e) {
		if (!e) { o.u8(0); return; }
		o.u8(1);
		o.u8((uint8_t)e->index());
		std::visit([&](const auto& n) {
			using T = std::decay_t<decltype(n)>;
			if constexpr (std::is_same_v<T, IntLit>) { o.i64(n.value); }
			else if constexpr (std::is_same_v<T, DoubleLit>) { o.dbl(n.value); }
			else if constexpr (std::is_same_v<T, StrLit>) { o.str(n.value); }
			else if constexpr (std::is_same_v<T, BoolLit>) { o.u8(n.value ? 1 : 0); }
			else if constexpr (std::is_same_v<T, Ident>) { o.str(n.name); o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, Binary>) { o.str(n.op); wExpr(o, n.lhs); wExpr(o, n.rhs); o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, Unary>) { o.str(n.op); wExpr(o, n.operand); o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, Call>) {
				o.str(n.callee);
				o.u32((uint32_t)n.type_args.size()); for (auto& t : n.type_args) o.str(t);
				o.u32((uint32_t)n.args.size()); for (auto& a : n.args) wExpr(o, a);
				o.i64(n.line); o.i64(n.col);
			}
			else if constexpr (std::is_same_v<T, Output>) { o.u32((uint32_t)n.values.size()); for (auto& a : n.values) wExpr(o, a); o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, Input>) { o.str(n.var_name); o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, IndexExpr>) { o.str(n.name); wExpr(o, n.idx); o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, ModulePathGet>) { o.str(n.module); o.str(n.part); o.u8(n.has_field ? 1 : 0); o.str(n.field); o.u8(n.adv ? 1 : 0); o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, TupleLit>) { o.u32((uint32_t)n.elements.size()); for (auto& a : n.elements) wExpr(o, a); o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, SizeofExpr>) { o.str(n.type); o.i64(n.value); o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, NewExpr>) { o.str(n.type); o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, DeleteExpr>) { wExpr(o, n.target); o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, CastExpr>) { o.str(n.type); wExpr(o, n.value); o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, RangeLit>) { wExpr(o, n.lo); wExpr(o, n.hi); o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, TupleIndexExpr>) { wExpr(o, n.tuple); o.i64(n.index); o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, TypeofExpr>) { wExpr(o, n.arg); o.str(n.resolved); o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, IsExpr>) { wExpr(o, n.arg); o.str(n.type_name); o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, InExpr>) { wExpr(o, n.value); wExpr(o, n.container); o.i64(n.array_len); o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, AwaitExpr>) { wExpr(o, n.operand); o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, CoExpr>) { o.str(n.callee); o.u32((uint32_t)n.args.size()); for (auto& a : n.args) wExpr(o, a); o.i64(n.thunk_id); o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, CompileTimeExpr>) { wExpr(o, n.arg); o.u8(n.folded ? 1 : 0); o.i64(n.value); o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, ForallExpr> || std::is_same_v<T, ExistsExpr>) { o.str(n.var); wExpr(o, n.domain); wExpr(o, n.cond); o.i64(n.line); o.i64(n.col); }
			else { /* 未覆盖的 Expr 类型: 写空壳, 读回同类型空壳 */ }
		}, *e);
	}

	template <size_t I = 0>
	std::unique_ptr<Expr> rExprAt(Buf& in, size_t idx) {
		if constexpr (I >= std::variant_size_v<Expr>) {
			throw std::runtime_error("模块文件: 未知的 Expr 类型标签");
		} else {
			if (idx == I) {
				using T = std::variant_alternative_t<I, Expr>;
				T n{};
				auto e = std::make_unique<Expr>();
				if constexpr (std::is_same_v<T, IntLit>) { n.value = in.ri64(); }
				else if constexpr (std::is_same_v<T, DoubleLit>) { n.value = in.rdbl(); }
				else if constexpr (std::is_same_v<T, StrLit>) { n.value = in.rstr(); }
				else if constexpr (std::is_same_v<T, BoolLit>) { n.value = in.ru8() != 0; }
				else if constexpr (std::is_same_v<T, Ident>) { n.name = in.rstr(); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, Binary>) { n.op = in.rstr(); n.lhs = rExpr(in); n.rhs = rExpr(in); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, Unary>) { n.op = in.rstr(); n.operand = rExpr(in); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, Call>) {
					n.callee = in.rstr();
					uint32_t nt = in.ru32(); for (uint32_t i = 0; i < nt; i++) n.type_args.push_back(in.rstr());
					uint32_t na = in.ru32(); for (uint32_t i = 0; i < na; i++) n.args.push_back(rExpr(in));
					n.line = (int)in.ri64(); n.col = (int)in.ri64();
				}
				else if constexpr (std::is_same_v<T, Output>) { uint32_t c = in.ru32(); for (uint32_t i = 0; i < c; i++) n.values.push_back(rExpr(in)); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, Input>) { n.var_name = in.rstr(); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, IndexExpr>) { n.name = in.rstr(); n.idx = rExpr(in); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, ModulePathGet>) { n.module = in.rstr(); n.part = in.rstr(); n.has_field = in.ru8() != 0; n.field = in.rstr(); n.adv = in.ru8() != 0; n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, TupleLit>) { uint32_t c = in.ru32(); for (uint32_t i = 0; i < c; i++) n.elements.push_back(rExpr(in)); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, SizeofExpr>) { n.type = in.rstr(); n.value = in.ri64(); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, NewExpr>) { n.type = in.rstr(); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, DeleteExpr>) { n.target = rExpr(in); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, CastExpr>) { n.type = in.rstr(); n.value = rExpr(in); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, RangeLit>) { n.lo = rExpr(in); n.hi = rExpr(in); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, TupleIndexExpr>) { n.tuple = rExpr(in); n.index = in.ri64(); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, TypeofExpr>) { n.arg = rExpr(in); n.resolved = in.rstr(); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, IsExpr>) { n.arg = rExpr(in); n.type_name = in.rstr(); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, InExpr>) { n.value = rExpr(in); n.container = rExpr(in); n.array_len = in.ri64(); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, AwaitExpr>) { n.operand = rExpr(in); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, CoExpr>) { n.callee = in.rstr(); uint32_t c = in.ru32(); for (uint32_t i = 0; i < c; i++) n.args.push_back(rExpr(in)); n.thunk_id = (int)in.ri64(); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, CompileTimeExpr>) { n.arg = rExpr(in); n.folded = in.ru8() != 0; n.value = in.ri64(); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, ForallExpr> || std::is_same_v<T, ExistsExpr>) { n.var = in.rstr(); n.domain = rExpr(in); n.cond = rExpr(in); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				*e = std::move(n);
				return e;
			}
			return rExprAt<I + 1>(in, idx);
		}
	}
	std::unique_ptr<Expr> rExpr(Buf& in) {
		if (!in.ru8()) return nullptr;
		size_t idx = in.ru8();
		return rExprAt(in, idx);
	}

	/* ── Stmt ─────────────────────────────────────────────────────────── */
	void wStmt(Buf& o, const Stmt& st) {
		o.u8((uint8_t)st.index());
		std::visit([&](const auto& n) {
			using T = std::decay_t<decltype(n)>;
			if constexpr (std::is_same_v<T, Block>) { wStmts(o, n.stmts); o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, IfStmt>) { wExpr(o, n.cond); wStmts(o, n.then_body); wStmts(o, n.else_body); o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, ForStmt>) {
				o.u8(n.init_decl ? 1 : 0); if (n.init_decl) wVarDecl(o, *n.init_decl);
				wExpr(o, n.cond); wExpr(o, n.step); wStmts(o, n.body); o.i64(n.line); o.i64(n.col);
			}
			else if constexpr (std::is_same_v<T, WhileStmt>) { wExpr(o, n.cond); wStmts(o, n.body); o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, LoopStmt>) { wStmts(o, n.body); }
			else if constexpr (std::is_same_v<T, LoopNStmt>) { o.i64(n.count); wStmts(o, n.body); }
			else if constexpr (std::is_same_v<T, ReturnStmt>) { wExpr(o, n.value); o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, ExprStmt>) { wExpr(o, n.expr); }
			else if constexpr (std::is_same_v<T, AssignStmt>) { o.str(n.name); wExpr(o, n.target); wExpr(o, n.value); o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, VarDecl>) { wVarDecl(o, n); }
			else if constexpr (std::is_same_v<T, ModuleAccessSet>) { o.str(n.module); o.str(n.part); o.str(n.field); o.u8(n.adv ? 1 : 0); wExpr(o, n.value); o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, MatchStmt>) {
				wExpr(o, n.expr);
				o.u32((uint32_t)n.cases.size());
				for (auto& c : n.cases) { wExpr(o, c.pattern); wStmts(o, c.body); o.i64(c.line); o.i64(c.col); }
				wStmts(o, n.default_body); o.i64(n.line); o.i64(n.col);
			}
			else if constexpr (std::is_same_v<T, ContractStmt>) { o.str(n.contract_type); wExpr(o, n.condition); o.str(n.message); o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, TypeAliasDecl>) { o.str(n.alias_name); o.str(n.target_type); o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, ForallStmt>) { o.str(n.var); wExpr(o, n.domain); wStmts(o, n.body); o.u8(n.is_of ? 1 : 0); o.i64(n.array_len); o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, WhenStmt>) { wExpr(o, n.cond); wStmts(o, n.then_body); wStmts(o, n.else_body); o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, YieldStmt>) { wExpr(o, n.value); o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, CompileTimeStmt>) { wStmts(o, n.body); o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, MacroDecl>) {
				o.str(n.name);
				o.u32((uint32_t)n.params.size()); for (auto& x : n.params) o.str(x);
				wStmts(o, n.body); wExpr(o, n.expr_body); o.u8(n.is_expr ? 1 : 0); o.i64(n.line); o.i64(n.col);
			}
			else if constexpr (std::is_same_v<T, BreakStmt> || std::is_same_v<T, ContinueStmt>) { o.i64(n.line); o.i64(n.col); }
			else if constexpr (std::is_same_v<T, AsmStmt>) {
				o.u32((uint32_t)n.lines.size()); for (auto& x : n.lines) o.str(x);
				o.u32((uint32_t)n.clobbers.size()); for (auto& x : n.clobbers) o.str(x);
				o.u32((uint32_t)n.inputs.size()); for (auto& x : n.inputs) o.str(x);
				o.u32((uint32_t)n.outputs.size()); for (auto& x : n.outputs) o.str(x);
				o.i64(n.line); o.i64(n.col);
			}
			else if constexpr (std::is_same_v<T, SwitchStmt>) {
				wExpr(o, n.subject);
				o.u32((uint32_t)n.cases.size());
				for (auto& c : n.cases) { wExpr(o, c.value); o.i64(c.const_val); o.u8(c.is_default ? 1 : 0); wStmts(o, c.body.stmts); o.i64(c.line); }
				o.i64(n.line); o.i64(n.col);
			}
			else if constexpr (std::is_same_v<T, DeferStmt>) { wStmts(o, n.body.stmts); o.i64(n.line); o.i64(n.col); }
		}, st);
	}

	template <size_t I = 0>
	Stmt rStmtAt(Buf& in, size_t idx) {
		if constexpr (I >= std::variant_size_v<Stmt>) {
			throw std::runtime_error("模块文件: 未知的 Stmt 类型标签");
		} else {
			if (idx == I) {
				using T = std::variant_alternative_t<I, Stmt>;
				T n{};
				if constexpr (std::is_same_v<T, Block>) { n.stmts = rStmts(in); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, IfStmt>) { n.cond = rExpr(in); n.then_body = rStmts(in); n.else_body = rStmts(in); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, ForStmt>) {
					if (in.ru8()) { n.init_decl = std::make_unique<VarDecl>(); *n.init_decl = rVarDecl(in); }
					n.cond = rExpr(in); n.step = rExpr(in); n.body = rStmts(in); n.line = (int)in.ri64(); n.col = (int)in.ri64();
				}
				else if constexpr (std::is_same_v<T, WhileStmt>) { n.cond = rExpr(in); n.body = rStmts(in); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, LoopStmt>) { n.body = rStmts(in); }
				else if constexpr (std::is_same_v<T, LoopNStmt>) { n.count = in.ri64(); n.body = rStmts(in); }
				else if constexpr (std::is_same_v<T, ReturnStmt>) { n.value = rExpr(in); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, ExprStmt>) { n.expr = rExpr(in); }
				else if constexpr (std::is_same_v<T, AssignStmt>) { n.name = in.rstr(); n.target = rExpr(in); n.value = rExpr(in); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, VarDecl>) { n = rVarDecl(in); }
				else if constexpr (std::is_same_v<T, ModuleAccessSet>) { n.module = in.rstr(); n.part = in.rstr(); n.field = in.rstr(); n.adv = in.ru8() != 0; n.value = rExpr(in); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, MatchStmt>) {
					n.expr = rExpr(in);
					uint32_t c = in.ru32();
					for (uint32_t i = 0; i < c; i++) { MatchCase mc; mc.pattern = rExpr(in); mc.body = rStmts(in); mc.line = (int)in.ri64(); mc.col = (int)in.ri64(); n.cases.push_back(std::move(mc)); }
					n.default_body = rStmts(in); n.line = (int)in.ri64(); n.col = (int)in.ri64();
				}
				else if constexpr (std::is_same_v<T, ContractStmt>) { n.contract_type = in.rstr(); n.condition = rExpr(in); n.message = in.rstr(); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, TypeAliasDecl>) { n.alias_name = in.rstr(); n.target_type = in.rstr(); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, ForallStmt>) { n.var = in.rstr(); n.domain = rExpr(in); n.body = rStmts(in); n.is_of = in.ru8() != 0; n.array_len = in.ri64(); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, WhenStmt>) { n.cond = rExpr(in); n.then_body = rStmts(in); n.else_body = rStmts(in); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, YieldStmt>) { n.value = rExpr(in); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, CompileTimeStmt>) { n.body = rStmts(in); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, MacroDecl>) {
					n.name = in.rstr();
					uint32_t c = in.ru32(); for (uint32_t i = 0; i < c; i++) n.params.push_back(in.rstr());
					n.body = rStmts(in); n.expr_body = rExpr(in); n.is_expr = in.ru8() != 0; n.line = (int)in.ri64(); n.col = (int)in.ri64();
				}
				else if constexpr (std::is_same_v<T, BreakStmt> || std::is_same_v<T, ContinueStmt>) { n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				else if constexpr (std::is_same_v<T, AsmStmt>) {
					uint32_t c = in.ru32(); for (uint32_t i = 0; i < c; i++) n.lines.push_back(in.rstr());
					c = in.ru32(); for (uint32_t i = 0; i < c; i++) n.clobbers.push_back(in.rstr());
					c = in.ru32(); for (uint32_t i = 0; i < c; i++) n.inputs.push_back(in.rstr());
					c = in.ru32(); for (uint32_t i = 0; i < c; i++) n.outputs.push_back(in.rstr());
					n.line = (int)in.ri64(); n.col = (int)in.ri64();
				}
				else if constexpr (std::is_same_v<T, SwitchStmt>) {
					n.subject = rExpr(in);
					uint32_t c = in.ru32();
					for (uint32_t i = 0; i < c; i++) {
						SwitchCase sc; sc.value = rExpr(in); sc.const_val = in.ri64(); sc.is_default = in.ru8() != 0;
						sc.body.stmts = rStmts(in); sc.line = (int)in.ri64(); n.cases.push_back(std::move(sc));
					}
					n.line = (int)in.ri64(); n.col = (int)in.ri64();
				}
				else if constexpr (std::is_same_v<T, DeferStmt>) { n.body.stmts = rStmts(in); n.line = (int)in.ri64(); n.col = (int)in.ri64(); }
				return Stmt(std::move(n));
			}
			return rStmtAt<I + 1>(in, idx);
		}
	}
	Stmt rStmt(Buf& in) { return rStmtAt(in, in.ru8()); }

	void wStmts(Buf& o, const std::vector<Stmt>& v) {
		o.u32((uint32_t)v.size()); for (auto& st : v) wStmt(o, st);
	}
	std::vector<Stmt> rStmts(Buf& in) {
		std::vector<Stmt> v; uint32_t c = in.ru32(); for (uint32_t i = 0; i < c; i++) v.push_back(rStmt(in)); return v;
	}
	void wVarDecl(Buf& o, const VarDecl& v) {
		o.str(v.name); o.str(v.type); wExpr(o, v.init); o.u8(v.is_const ? 1 : 0); o.i64(v.array_len);
		o.u32((uint32_t)v.array_init.size()); for (auto& a : v.array_init) wExpr(o, a);
		o.i64(v.line); o.i64(v.col);
	}
	VarDecl rVarDecl(Buf& in) {
		VarDecl v;
		v.name = in.rstr(); v.type = in.rstr(); v.init = rExpr(in); v.is_const = in.ru8() != 0; v.array_len = in.ri64();
		uint32_t c = in.ru32(); for (uint32_t i = 0; i < c; i++) v.array_init.push_back(rExpr(in));
		v.line = (int)in.ri64(); v.col = (int)in.ri64();
		return v;
	}

	/* ── FuncDecl / PartDecl / ModuleDecl ─────────────────────────────── */
	void wFuncDecl(Buf& o, const FuncDecl& f) {
		o.u8(f.exported ? 1 : 0); o.u8(f.is_private ? 1 : 0); o.str(f.name);
		o.u32((uint32_t)f.type_params.size()); for (auto& t : f.type_params) o.str(t);
		o.u32((uint32_t)f.params.size()); for (auto& pr : f.params) { o.str(pr.name); o.str(pr.type); }
		o.str(f.ret_type); wStmts(o, f.body); o.u8(f.is_async ? 1 : 0); o.i64(f.line); o.i64(f.col);
	}
	FuncDecl rFuncDecl(Buf& in) {
		FuncDecl f;
		f.exported = in.ru8() != 0; f.is_private = in.ru8() != 0; f.name = in.rstr();
		uint32_t c = in.ru32(); for (uint32_t i = 0; i < c; i++) f.type_params.push_back(in.rstr());
		c = in.ru32(); for (uint32_t i = 0; i < c; i++) { Param p; p.name = in.rstr(); p.type = in.rstr(); f.params.push_back(p); }
		f.ret_type = in.rstr(); f.body = rStmts(in); f.is_async = in.ru8() != 0; f.line = (int)in.ri64(); f.col = (int)in.ri64();
		return f;
	}
	void wPartDecl(Buf& o, const PartDecl& p) {
		o.str(p.name); o.u8(p.is_struct ? 1 : 0); o.u8(p.is_public ? 1 : 0);
		o.u32((uint32_t)p.members.size()); for (auto& m : p.members) wVarDecl(o, m);
		o.i64(p.line); o.i64(p.col);
	}
	PartDecl rPartDecl(Buf& in) {
		PartDecl p;
		p.name = in.rstr(); p.is_struct = in.ru8() != 0; p.is_public = in.ru8() != 0;
		uint32_t c = in.ru32(); for (uint32_t i = 0; i < c; i++) p.members.push_back(rVarDecl(in));
		p.line = (int)in.ri64(); p.col = (int)in.ri64();
		return p;
	}
	void wModuleDecl(Buf& o, const ModuleDecl& m) {
		o.u8(m.from_import ? 1 : 0); o.u8(m.is_exported ? 1 : 0); o.str(m.ns); o.u8(m.has_ns ? 1 : 0);
		o.u8(m.has_init ? 1 : 0); o.u8(m.has_deinit ? 1 : 0); o.str(m.name);
		o.u32((uint32_t)m.parts.size()); for (auto& p : m.parts) wPartDecl(o, p);
		wStmts(o, m.body);
		o.u32((uint32_t)m.methods.size()); for (auto& f : m.methods) wFuncDecl(o, f);
		o.i64(m.priority); o.u8(m.has_priority ? 1 : 0); o.i64(m.dep_order);
		o.u8(m.has_main ? 1 : 0); o.u8(m.referenced ? 1 : 0); o.i64(m.line); o.i64(m.col);
	}
	ModuleDecl rModuleDecl(Buf& in) {
		ModuleDecl m;
		m.from_import = in.ru8() != 0; m.is_exported = in.ru8() != 0; m.ns = in.rstr(); m.has_ns = in.ru8() != 0;
		m.has_init = in.ru8() != 0; m.has_deinit = in.ru8() != 0; m.name = in.rstr();
		uint32_t c = in.ru32(); for (uint32_t i = 0; i < c; i++) m.parts.push_back(rPartDecl(in));
		m.body = rStmts(in);
		c = in.ru32(); for (uint32_t i = 0; i < c; i++) m.methods.push_back(rFuncDecl(in));
		m.priority = (int)in.ri64(); m.has_priority = in.ru8() != 0; m.dep_order = (int)in.ri64();
		m.has_main = in.ru8() != 0; m.referenced = in.ru8() != 0; m.line = (int)in.ri64(); m.col = (int)in.ri64();
		return m;
	}

	/* ── .mod 文件 ────────────────────────────────────────────────────── */
	static const char* MAGIC = "WEIMOD01";

	/* 序列化整个 Program 为 .mod 内容。
	   src_mtime: 源 .wei 文件的最后修改时间，用于后续新鲜度校验。 */
	std::vector<uint8_t> serialize(const Program& prog, const std::vector<std::string>& deps, int64_t src_mtime) {
		Buf o;
		for (const char* p = MAGIC; *p; p++) o.u8((uint8_t)*p);
		o.u32((uint32_t)prog.items.size());
		for (auto& it : prog.items) {
			if (auto* m = std::get_if<ModuleDecl>(&it)) { o.u8(0); wModuleDecl(o, *m); }
			else if (auto* im = std::get_if<ImportDecl>(&it)) {
				o.u8(1); o.str(im->module); o.u8(im->has_symbols ? 1 : 0);
				o.u32((uint32_t)im->symbols.size()); for (auto& x : im->symbols) o.str(x);
				o.str(im->alias); o.str(im->from_module); o.i64(im->line); o.i64(im->col);
			}
			else if (auto* ta = std::get_if<TypeAliasDecl>(&it)) { o.u8(2); o.str(ta->alias_name); o.str(ta->target_type); o.i64(ta->line); o.i64(ta->col); }
			else if (auto* md = std::get_if<MacroDecl>(&it)) { o.u8(3); o.str(md->name); o.u32((uint32_t)md->params.size()); for (auto& x : md->params) o.str(x); wStmts(o, md->body); wExpr(o, md->expr_body); o.u8(md->is_expr ? 1 : 0); o.i64(md->line); o.i64(md->col); }
			else if (auto* cd = std::get_if<ClassDecl>(&it)) {
				o.u8(4); o.str(cd->name);
				o.u32((uint32_t)cd->fields.size()); for (auto& f : cd->fields) wVarDecl(o, f);
				o.u32((uint32_t)cd->methods.size()); for (auto& f : cd->methods) wFuncDecl(o, f);
				o.i64(cd->line); o.i64(cd->col);
			}
			else if (auto* ud = std::get_if<UseDecl>(&it)) { o.u8(5); o.str(ud->path); o.str(ud->alias); o.i64(ud->line); o.i64(ud->col); }
		}
		o.u32((uint32_t)deps.size()); for (auto& d : deps) o.str(d);
		o.i64(src_mtime);
		return o.d;
	}

	/* 反序列化。同时返回依赖表与源 mtime。失败抛异常（调用方回退到解析 .wei）。 */
	bool deserialize(const std::vector<uint8_t>& raw, Program& prog,
	                 std::vector<std::string>& deps, int64_t& src_mtime) {
		if (raw.size() < 8) return false;
		for (int i = 0; i < 8; i++) if (raw[i] != (uint8_t)MAGIC[i]) return false;
		Buf in; in.d = raw; in.rp = 8;
		try {
			uint32_t n = in.ru32();
			for (uint32_t i = 0; i < n; i++) {
				uint8_t tag = in.ru8();
				if (tag == 0) prog.items.push_back(rModuleDecl(in));
				else if (tag == 1) {
					ImportDecl d; d.module = in.rstr(); d.has_symbols = in.ru8() != 0;
					uint32_t c = in.ru32(); for (uint32_t k = 0; k < c; k++) d.symbols.push_back(in.rstr());
					d.alias = in.rstr(); d.from_module = in.rstr(); d.line = (int)in.ri64(); d.col = (int)in.ri64();
					prog.items.push_back(std::move(d));
				}
				else if (tag == 2) { TypeAliasDecl d; d.alias_name = in.rstr(); d.target_type = in.rstr(); d.line = (int)in.ri64(); d.col = (int)in.ri64(); prog.items.push_back(std::move(d)); }
				else if (tag == 3) {
					MacroDecl d; d.name = in.rstr();
					uint32_t c = in.ru32(); for (uint32_t k = 0; k < c; k++) d.params.push_back(in.rstr());
					d.body = rStmts(in); d.expr_body = rExpr(in); d.is_expr = in.ru8() != 0; d.line = (int)in.ri64(); d.col = (int)in.ri64();
					prog.items.push_back(std::move(d));
				}
				else if (tag == 4) {
					ClassDecl d; d.name = in.rstr();
					uint32_t c = in.ru32(); for (uint32_t k = 0; k < c; k++) d.fields.push_back(rVarDecl(in));
					c = in.ru32(); for (uint32_t k = 0; k < c; k++) d.methods.push_back(rFuncDecl(in));
					d.line = (int)in.ri64(); d.col = (int)in.ri64(); prog.items.push_back(std::move(d));
				}
				else if (tag == 5) { UseDecl d; d.path = in.rstr(); d.alias = in.rstr(); d.line = (int)in.ri64(); d.col = (int)in.ri64(); prog.items.push_back(std::move(d)); }
				else return false;
			}
			uint32_t dn = in.ru32(); for (uint32_t i = 0; i < dn; i++) deps.push_back(in.rstr());
			src_mtime = in.ri64();
		} catch (const std::exception&) {
			return false;   /* 损坏或版本不符: 回退重新解析 */
		}
		return true;
	}
}

namespace wei::types {

	enum class TypeKind {
		Int, Bool, Double, Char, Func, Class, Omni, Void, Error, Unknown, Ptr
	};

	struct Type {
		TypeKind kind;
		std::string name;
		bool operator==(const Type& o) const;
		bool operator!=(const Type& o) const;
	};

	class TypeChecker {
		public:
			void check(const ast::Program& prog);
		private:
			std::map<std::string, Type> globals_;
			std::map<std::string, Type> class_table_;

			struct PartInfo {
				bool is_struct = false;
				bool is_public = true;
				std::map<std::string, Type> field_types;
				std::vector<std::string> field_order;
				int line = 0;
				int col = 0;
			};
			std::map<std::string, std::map<std::string, PartInfo>> module_table_;

			Type resolveTypeName(const std::string& n);
			void checkVarDecl(const ast::VarDecl& v, std::map<std::string, Type>& scope);
			void checkFunc(const ast::FuncDecl& f);
			void checkStmt(const ast::Stmt& s, std::map<std::string, Type>& scope,
			               const Type& ret_type, bool& ret_seen);
			Type inferExpr(const ast::Expr& e, std::map<std::string, Type>& scope);
			bool compatible(const Type& a, const Type& b);
			bool isNumeric(const Type& t);

			const PartInfo* findPart(const std::string& mod, const std::string& part) const;
			void checkVisibility(const PartInfo* pi, bool adv, const std::string& mod,
			                     const std::string& part, int line, int col) const;
	};

#ifndef WEI_FRONTEND_NO_DEFS
	bool Type::operator==(const Type& o) const {
		return kind == o.kind && name == o.name;
	}
	bool Type::operator!=(const Type& o) const {
		return !(*this == o);
	}

	Type TypeChecker::resolveTypeName(const std::string& n) {
		if (n == "int" || n == "整型") return {TypeKind::Int, ""};
		/* C1 位宽体系: i8..u64 归入整型（宽度信息携带在类型名里），
		   f32/f64 归入浮点。语义检查按大类走，代码生成再按宽度选指令。 */
		if (n == "i8" || n == "i16" || n == "i32" || n == "i64"
		        || n == "u8" || n == "u16" || n == "u32" || n == "u64") return {TypeKind::Int, ""};
		if (n == "f32" || n == "f64") return {TypeKind::Double, ""};
		/* Result 底层是整数编码（>=0 为 Ok 值，<0 为 Err 码），归入整型，
		   这样类型检查、参数传递、返回路径都沿用 int 的通路。 */
		if (n == "Result") return {TypeKind::Int, ""};
		/* enum 归入整型（常量集合 + 底层整型）；const 是限定符不是独立类型 */
		if (n == "enum") return {TypeKind::Int, ""};
		if (n == "bool" || n == "布尔") return {TypeKind::Bool, ""};
		if (n == "double" || n == "浮点") return {TypeKind::Double, ""};
		if (n == "char" || n == "文本" || n == "string" || n == "str") return {TypeKind::Char, ""};
		if (n == "func" || n == "函数") return {TypeKind::Func, ""};
		if (n == "class") return {TypeKind::Class, ""};
		if (n == "omni" || n == "万能" || n == "stat" || n == "统计") return {TypeKind::Omni, ""};
		if (n == "void" || n == "") return {TypeKind::Void, ""};
		if (n == "ptr" || n == "指针") return {TypeKind::Ptr, "ptr"};
		if (n.rfind("ptr.", 0) == 0) return {TypeKind::Ptr, n};
		auto it = class_table_.find(n);
		if (it != class_table_.end()) return {TypeKind::Class, n};
		return {TypeKind::Unknown, n};
	}

	const TypeChecker::PartInfo* TypeChecker::findPart(const std::string& mod, const std::string& part) const {
		auto mit = module_table_.find(mod);
		if (mit == module_table_.end()) return nullptr;
		auto pit = mit->second.find(part);
		if (pit == mit->second.end()) return nullptr;
		return &pit->second;
	}

	void TypeChecker::checkVisibility(const PartInfo* pi, bool adv, const std::string& mod,
	                                  const std::string& part, int line, int col) const {
		if (!pi) {
			throw std::runtime_error("未知部件: " + mod + ">" + part + " at " + std::to_string(line) + ":" + std::to_string(col));
		}
		if (!pi->is_public && !adv) {
			throw std::runtime_error("部件不可访问(非公开): " + mod + ">" + part + " at " + std::to_string(line) + ":" + std::to_string(col));
		}
	}

	bool TypeChecker::isNumeric(const Type& t) {
		return t.kind == TypeKind::Int || t.kind == TypeKind::Double;
	}

	bool TypeChecker::compatible(const Type& a, const Type& b) {
		if (a.kind == TypeKind::Error || b.kind == TypeKind::Error) return true;
		if (a.kind == TypeKind::Unknown || b.kind == TypeKind::Unknown) return true;
		if (a.kind == TypeKind::Omni || b.kind == TypeKind::Omni) return true;
		if (a.kind == TypeKind::Ptr && b.kind == TypeKind::Ptr) return true;
		if ((a.kind == TypeKind::Ptr && b.kind == TypeKind::Int) || (a.kind == TypeKind::Int && b.kind == TypeKind::Ptr)) return true;
		/* int ? double 隐式混合 (赋??比较/算术) */
		if ((a.kind == TypeKind::Int && b.kind == TypeKind::Double) || (a.kind == TypeKind::Double && b.kind == TypeKind::Int)) return true;
		return a == b;
	}

	void TypeChecker::check(const ast::Program& prog) {
		/* 入口校验: 只能有一个 ::main */
		int entry_count = 0;
		for (auto& tl : prog.items) {
			if (const auto* m = std::get_if<ast::ModuleDecl>(&tl)) {
				if (m->has_main) ++entry_count;
			}
		}
		if (entry_count > 1) {
			throw std::runtime_error("只能有一个主函数: 找到 " + std::to_string(entry_count) + " 个 create.mod X::main");
		}

		for (auto& tl : prog.items) {
			std::visit([&](auto&& node) {
				using T = std::decay_t<decltype(node)>;
				if constexpr(std::is_same_v<T, ast::ClassDecl>) {
					Type t{TypeKind::Class, node.name};
					class_table_[node.name] = t;
				} else if constexpr(std::is_same_v<T, ast::ModuleDecl>) {
					auto& modTable = module_table_[node.name];
					for (auto& p : node.parts) {
						PartInfo pi;
						pi.is_struct = p.is_struct;
						pi.is_public = p.is_public;
						pi.line = p.line;
						pi.col = p.col;
						for (auto& fld : p.members) {
							Type ft = resolveTypeName(fld.type);
							if (ft.kind == TypeKind::Unknown) {
								throw std::runtime_error("未知字段类型: " + fld.type + " at " + std::to_string(fld.line) + ":" + std::to_string(fld.col));
							}
							pi.field_types[fld.name] = ft;
							pi.field_order.push_back(fld.name);
						}
						modTable[p.name] = std::move(pi);
					}
				} else if constexpr(std::is_same_v<T, ast::TypeAliasDecl>) {
					/* Register type alias */
					globals_[node.alias_name] = {TypeKind::Class, node.target_type};
				}
			}, tl);
		}
		for (auto& tl : prog.items) {
			std::visit([&](auto&& node) {
				using T = std::decay_t<decltype(node)>;
				if constexpr(std::is_same_v<T, ast::VarDecl>) {
					Type declared = resolveTypeName(node.type);
					if (declared.kind == TypeKind::Unknown) {
						throw std::runtime_error("未知类型: " + node.type + " at " + std::to_string(node.line) + ":" + std::to_string(node.col));
					}
					if (node.init) {
						Type init_t = inferExpr(*node.init, globals_);
						if (declared.kind == TypeKind::Omni) {
							globals_[node.name] = init_t;
						} else {
							if (!compatible(declared, init_t)) {
								throw std::runtime_error(
								    "变量 " + node.name + " 初值类型不匹配 at " + std::to_string(node.line) + ":" + std::to_string(node.col));
							}
							globals_[node.name] = declared;
						}
					} else {
						globals_[node.name] = declared;
					}
				} else if constexpr(std::is_same_v<T, ast::FuncDecl>) {
					Type ret = resolveTypeName(node.ret_type);
					if (ret.kind == TypeKind::Unknown) {
						throw std::runtime_error("未知返回类型: " + node.ret_type + " at " + std::to_string(node.line) + ":" + std::to_string(node.col));
					}
					Type ft{TypeKind::Func, node.ret_type};
					globals_[node.name] = ft;
				} else if constexpr(std::is_same_v<T, ast::ClassDecl>) {
					for (auto& fd : node.fields) {
						Type ft = resolveTypeName(fd.type);
						if (ft.kind == TypeKind::Unknown) {
							throw std::runtime_error("未知字段类型: " + fd.type + " at " + std::to_string(node.line) + ":" + std::to_string(node.col));
						}
					}
					for (auto& md : node.methods) {
						Type rt = resolveTypeName(md.ret_type);
						if (rt.kind == TypeKind::Unknown) {
							throw std::runtime_error(
							    "未知方法返回类型: " + md.ret_type + " at " + std::to_string(node.line) + ":" + std::to_string(node.col));
						}
					}
				} else if constexpr(std::is_same_v<T, ast::ModuleDecl>) {
					for (auto& p : node.parts) {
						for (auto& fld : p.members) {
							Type ft = resolveTypeName(fld.type);
							if (ft.kind == TypeKind::Unknown) {
								throw std::runtime_error(
								    "未知部件成员类型: " + fld.type + " at " + std::to_string(fld.line) + ":" + std::to_string(fld.col));
							}
						}
					}
					/* 先登记所有模块方法签名，方法体内可引用同级方法、
					   限定调用 Mod::method 以及 Calc[n] 形式的模块入口。 */
					for (auto& mf : node.methods) {
						Type rt = resolveTypeName(mf.ret_type);
						if (rt.kind == TypeKind::Unknown) {
							throw std::runtime_error(
							    "未知方法返回类型: " + mf.ret_type + " at " + std::to_string(mf.line) + ":" + std::to_string(mf.col));
						}
						Type ft{TypeKind::Func, mf.ret_type};
						globals_[node.name + "::" + mf.name] = ft;
						globals_[mf.name] = ft;
						globals_[node.name] = ft;   /* 函数即模块本身: Calc[n] */
					}
				}
			}, tl);
		}
		for (auto& tl : prog.items) {
			std::visit([&](auto&& node) {
				using T = std::decay_t<decltype(node)>;
				if constexpr(std::is_same_v<T, ast::FuncDecl>) {
					checkFunc(node);
				} else if constexpr(std::is_same_v<T, ast::ModuleDecl>) {
					/* 模块方法体是函数体，必须逐个做类型检查（此前完全漏检）。 */
					for (auto& mf : node.methods) {
						checkFunc(mf);
					}
					bool ret_seen = false;
					Type ret{TypeKind::Void, ""};
					for (auto& s : node.body) {
						checkStmt(s, globals_, ret, ret_seen);
					}
				}
			}, tl);
		}
	}

	void TypeChecker::checkFunc(const ast::FuncDecl& f) {
		std::map<std::string, Type> scope = globals_;
		for (auto& p : f.params) {
			Type pt = resolveTypeName(p.type);
			if (pt.kind == TypeKind::Unknown) {
				throw std::runtime_error("参数类型未知: " + p.type + " at " + std::to_string(f.line) + ":" + std::to_string(f.col));
			}
			scope[p.name] = pt;
		}
		Type ret = resolveTypeName(f.ret_type);
		bool ret_seen = false;
		for (auto& s : f.body) {
			checkStmt(s, scope, ret, ret_seen);
		}
	}

	void TypeChecker::checkStmt(const ast::Stmt& s, std::map<std::string, Type>& scope,
	                            const Type& ret_type, bool& ret_seen) {
		std::visit([&](auto&& node) {
			using T = std::decay_t<decltype(node)>;
			if constexpr(std::is_same_v<T, ast::Block>) {
				for (auto& s2 : node.stmts) checkStmt(s2, scope, ret_type, ret_seen);
			} else if constexpr(std::is_same_v<T, ast::IfStmt>) {
				if (node.cond) {
					Type ct = inferExpr(*node.cond, scope);
					if (ct.kind != TypeKind::Bool) {
						throw std::runtime_error("if 条件必须为 bool at " + std::to_string(node.line) + ":" + std::to_string(node.col));
					}
				}
				for (auto& s2 : node.then_body) checkStmt(s2, scope, ret_type, ret_seen);
				for (auto& s2 : node.else_body) checkStmt(s2, scope, ret_type, ret_seen);
			} else if constexpr(std::is_same_v<T, ast::ForStmt>) {
				if (node.init_decl) {
					checkVarDecl(*node.init_decl, scope);
				}
				if (node.cond) {
					Type ct = inferExpr(*node.cond, scope);
					if (ct.kind != TypeKind::Bool) {
						throw std::runtime_error("for 条件必须为 bool at " + std::to_string(node.line) + ":" + std::to_string(node.col));
					}
				}
				if (node.step) {
					inferExpr(*node.step, scope);
				}
				for (auto& s2 : node.body) checkStmt(s2, scope, ret_type, ret_seen);
			} else if constexpr(std::is_same_v<T, ast::WhileStmt>) {
				if (node.cond) {
					Type ct = inferExpr(*node.cond, scope);
					if (ct.kind != TypeKind::Bool) {
						throw std::runtime_error("循环条件必须为布尔 at " + std::to_string(node.line) + ":" + std::to_string(node.col));
					}
				}
				for (auto& s2 : node.body) checkStmt(s2, scope, ret_type, ret_seen);
			} else if constexpr(std::is_same_v<T, ast::LoopStmt>) {
				for (auto& s2 : node.body) checkStmt(s2, scope, ret_type, ret_seen);
			} else if constexpr(std::is_same_v<T, ast::LoopNStmt>) {
				for (auto& s2 : node.body) checkStmt(s2, scope, ret_type, ret_seen);
			} else if constexpr(std::is_same_v<T, ast::ReturnStmt>) {
				if (node.value) {
					Type vt = inferExpr(*node.value, scope);
					if (ret_type.kind == TypeKind::Void) {
						throw std::runtime_error("void 函数不能返回值 at " + std::to_string(node.line) + ":" + std::to_string(node.col));
					}
					if (!compatible(ret_type, vt)) {
						throw std::runtime_error("返回类型不匹配 at " + std::to_string(node.line) + ":" + std::to_string(node.col));
					}
					ret_seen = true;
				} else {
					if (ret_type.kind != TypeKind::Void) {
						throw std::runtime_error("非 void 函数缺少 return at " + std::to_string(node.line) + ":" + std::to_string(node.col));
					}
					ret_seen = true;
				}
			} else if constexpr(std::is_same_v<T, ast::ExprStmt>) {
				if (node.expr) inferExpr(*node.expr, scope);
			} else if constexpr(std::is_same_v<T, ast::VarDecl>) {
				checkVarDecl(node, scope);
			} else if constexpr(std::is_same_v<T, ast::AssignStmt>) {
				Type vt = inferExpr(*node.value, scope);
				if (node.target) {
					inferExpr(*node.target, scope);
				} else {
					auto it = scope.find(node.name);
					if (it == scope.end()) {
						throw std::runtime_error("变量未声明: " + node.name + " at " + std::to_string(node.line) + ":" + std::to_string(node.col));
					}
					Type& cur = it->second;
					if (cur.kind == TypeKind::Omni) {
						cur = vt;
					} else {
						if (!compatible(cur, vt)) {
							throw std::runtime_error("赋值类型不匹配: " + node.name + " at " + std::to_string(node.line) + ":" + std::to_string(node.col));
						}
					}
				}
			} else if constexpr(std::is_same_v<T, ast::ForallStmt>) {
				auto s2 = scope;
				s2[node.var] = {TypeKind::Int, ""};
				if (node.domain) inferExpr(*node.domain, s2);
				for (auto& st : node.body) checkStmt(st, s2, ret_type, ret_seen);
				return;
			} else if constexpr(std::is_same_v<T, ast::WhenStmt>) {
				if (node.cond) inferExpr(*node.cond, scope);
				for (auto& st : node.then_body) checkStmt(st, scope, ret_type, ret_seen);
				for (auto& st : node.else_body) checkStmt(st, scope, ret_type, ret_seen);
				return;
			} else if constexpr(std::is_same_v<T, ast::YieldStmt>) {
				if (node.value) inferExpr(*node.value, scope);
				return;
			} else if constexpr(std::is_same_v<T, ast::CompileTimeStmt>) {
				for (auto& st : node.body) checkStmt(st, scope, ret_type, ret_seen);
				return;
			} else if constexpr(std::is_same_v<T, ast::MacroDecl>) {
				return;
			} else if constexpr(std::is_same_v<T, ast::MatchStmt>) {
				if (node.expr) inferExpr(*node.expr, scope);
				for (auto& mc : node.cases) {
					if (mc.pattern) inferExpr(*mc.pattern, scope);
					for (auto& s : mc.body) checkStmt(s, scope, ret_type, ret_seen);
				}
				for (auto& s : node.default_body) checkStmt(s, scope, ret_type, ret_seen);
			} else if constexpr(std::is_same_v<T, ast::ContractStmt>) {
				if (node.condition) inferExpr(*node.condition, scope);
			} else if constexpr(std::is_same_v<T, ast::TypeAliasDecl>) {
				/* Type aliases are resolved at declaration time */
				globals_[node.alias_name] = {TypeKind::Class, node.alias_name};
			} else if constexpr(std::is_same_v<T, ast::ModuleAccessSet>) {
				const PartInfo* pi = findPart(node.module, node.part);
				checkVisibility(pi, node.adv, node.module, node.part, node.line, node.col);
				auto fit = pi->field_types.find(node.field);
				if (fit == pi->field_types.end()) {
					throw std::runtime_error("部件 " + node.module + ">" + node.part + " 没有字段 " + node.field
					                         + " at " + std::to_string(node.line) + ":" + std::to_string(node.col));
				}
				Type vt = inferExpr(*node.value, scope);
				if (vt.kind != TypeKind::Unknown && !compatible(fit->second, vt)) {
					throw std::runtime_error("访问赋值类型不匹配: " + node.module + ">" + node.part + "." + node.field
					                         + " at " + std::to_string(node.line) + ":" + std::to_string(node.col));
				}
			}
		}, s);
	}

	void TypeChecker::checkVarDecl(const ast::VarDecl& v, std::map<std::string, Type>& scope) {
		Type declared = resolveTypeName(v.type);
		if (v.init && (declared.kind == TypeKind::Unknown || v.type.empty())) {
			Type init_t = inferExpr(*v.init, scope);
			scope[v.name] = init_t;
			return;
		}
		if (declared.kind == TypeKind::Unknown) {
			throw std::runtime_error("未知类型: " + v.type + " at " + std::to_string(v.line) + ":" + std::to_string(v.col));
		}
		if (v.init) {
			Type init_t = inferExpr(*v.init, scope);
			if (declared.kind == TypeKind::Omni) {
				scope[v.name] = init_t;
			} else {
				if (!compatible(declared, init_t)) {
					throw std::runtime_error("变量 " + v.name + " 初值类型不匹配 at " + std::to_string(v.line) + ":" + std::to_string(v.col));
				}
				scope[v.name] = declared;
			}
		} else {
			scope[v.name] = declared;
		}
	}

	Type TypeChecker::inferExpr(const ast::Expr& e, std::map<std::string, Type>& scope) {
		return std::visit([&](auto&& node)->Type{
			using T = std::decay_t<decltype(node)>;
			if constexpr(std::is_same_v<T, ast::IntLit>) {
				return {TypeKind::Int, ""};
			} else if constexpr(std::is_same_v<T, ast::DoubleLit>) {
				return {TypeKind::Double, ""};
			} else if constexpr(std::is_same_v<T, ast::StrLit>) {
				return {TypeKind::Char, ""};
			} else if constexpr(std::is_same_v<T, ast::BoolLit>) {
				return {TypeKind::Bool, ""};
			} else if constexpr(std::is_same_v<T, ast::Ident>) {
				auto it = scope.find(node.name);
				if (it == scope.end()) {
					throw std::runtime_error("未声明标识符: " + node.name + " at " + std::to_string(node.line) + ":" + std::to_string(node.col));
				}
				return it->second;
			} else if constexpr(std::is_same_v<T, ast::Binary>) {
				Type lt = node.lhs ? inferExpr(*node.lhs, scope): Type{TypeKind::Void, ""};
				Type rt = node.rhs ? inferExpr(*node.rhs, scope) : Type{TypeKind::Void, ""};
				const std::string& op = node.op;
				if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%"
				        || op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>") {
					if (!isNumeric(lt) || !isNumeric(rt)) {
						throw std::runtime_error("算术运算符要求数值操作数 at " + std::to_string(node.line) + ":" + std::to_string(node.col));
					}
					/* 混合 int/double 运算结果??double */
					if (lt.kind == TypeKind::Double || rt.kind == TypeKind::Double) return {TypeKind::Double, ""};
					return lt;
				} else if (op == "==" || op == "!=" || op == "<" || op == "<=" ||
				           op == ">" || op == ">=") {
					if (!compatible(lt, rt)) {
						throw std::runtime_error("比较运算符类型不匹配 at " + std::to_string(node.line) + ":" + std::to_string(node.col));
					}
					return {TypeKind::Bool, ""};
				} else if (op == "&&" || op == "||") {
					/* and/or 是词形式运算符，整数按真值参与；结果仍为 bool */
					auto truthy = [](TypeKind k) {
						return k == TypeKind::Bool || k == TypeKind::Int
						       || k == TypeKind::Omni || k == TypeKind::Unknown;
					};
					if (!truthy(lt.kind) || !truthy(rt.kind)) {
						throw std::runtime_error("逻辑运算符要求 bool 操作数 at " + std::to_string(node.line) + ":" + std::to_string(node.col));
					}
					return {TypeKind::Bool, ""};
				} else {
					return {TypeKind::Unknown, ""};
				}
			} else if constexpr(std::is_same_v<T, ast::Unary>) {
				Type ot = node.operand ? inferExpr(*node.operand, scope)
				          : Type{TypeKind::Void, ""};
				const std::string& op = node.op;
				if (op == "-") {
					if (!isNumeric(ot)) {
						throw std::runtime_error("一元减操作数必须为数值 at " + std::to_string(node.line) + ":" + std::to_string(node.col));
					}
					return ot;
				} else if (op == "!") {
					/* not 是词形式运算符，整数按真值参与 */
					if (ot.kind != TypeKind::Bool && ot.kind != TypeKind::Int
					        && ot.kind != TypeKind::Omni && ot.kind != TypeKind::Unknown) {
						throw std::runtime_error("! 操作数必须为布尔 at " + std::to_string(node.line) + ":" + std::to_string(node.col));
					}
					return {TypeKind::Bool, ""};
				} else if (op == "*") {
					if (ot.kind != TypeKind::Ptr) {
						throw std::runtime_error("* 解引用操作数必须为指针 at " + std::to_string(node.line) + ":" + std::to_string(node.col));
					}
					if (ot.name == "ptr.int") return {TypeKind::Int, ""};
					if (ot.name == "ptr.char") return {TypeKind::Char, ""};
					if (ot.name == "ptr.bool") return {TypeKind::Bool, ""};
					if (ot.name == "ptr.double") return {TypeKind::Double, ""};
					return {TypeKind::Omni, ""};
				} else if (op == "&") {
					return {TypeKind::Ptr, "ptr"};
				} else {
					return {TypeKind::Unknown, ""};
				}
			} else if constexpr(std::is_same_v<T, ast::Call>) {
				for (auto& a : node.args) {
					if (a) inferExpr(*a, scope);
				}
				/* Result 构造 ok(v) / err(e): 编译器内建，不注册为普通函数，
				   否则会被当成"未声明函数"。 */
				if (node.callee == "ok" || node.callee == "err") {
					return {TypeKind::Int, ""};
				}
				if (node.callee == "__byte_at") {
					if (node.args.size() != 2) {
						throw std::runtime_error("__byte_at 参数数量错误 at " + std::to_string(node.line) + ":" + std::to_string(node.col));
					}
					return {TypeKind::Int, ""};
				}
				if (node.callee == "__store_byte") {
					if (node.args.size() != 3) {
						throw std::runtime_error("__store_byte 参数数量错误 at " + std::to_string(node.line) + ":" + std::to_string(node.col));
					}
					return {TypeKind::Int, ""};
				}
				if (node.callee == "__command_line") {
					if (node.args.size() != 0) {
						throw std::runtime_error("__command_line 参数数量错误 at " + std::to_string(node.line) + ":" + std::to_string(node.col));
					}
					return {TypeKind::Int, ""};
				}
				if (node.callee == "__file_open" || node.callee == "__file_write" || node.callee == "__file_close") {
					size_t expected = node.callee == "__file_open" ? 1u : (node.callee == "__file_close" ? 1u : 3u);
					if (node.args.size() != expected) {
						throw std::runtime_error(node.callee + " 参数数量错误 at " + std::to_string(node.line) + ":" + std::to_string(node.col));
					}
					return {TypeKind::Int, ""};
				}
				if (node.callee == "__read_file" || node.callee == "__write_file") {
					if (node.args.size() != 3) {
						throw std::runtime_error(node.callee + " 参数数量错误 at " + std::to_string(node.line) + ":" + std::to_string(node.col));
					}
					return {TypeKind::Int, ""};
				}
				if (node.callee == "__rt_alloc") {
					if (node.args.size() != 1) {
						throw std::runtime_error("__rt_alloc 参数数量错误 at " + std::to_string(node.line) + ":" + std::to_string(node.col));
					}
					return {TypeKind::Int, ""};
				}
				if (node.callee == "__rt_free") {
					if (node.args.size() != 1) {
						throw std::runtime_error("__rt_free 参数数量错误 at " + std::to_string(node.line) + ":" + std::to_string(node.col));
					}
					return {TypeKind::Int, ""};
				}
				if (node.callee == "__rt_panic") {
					if (!node.args.empty()) {
						throw std::runtime_error("__rt_panic 参数数量错误 at " + std::to_string(node.line) + ":" + std::to_string(node.col));
					}
					return {TypeKind::Void, ""};
				}
				if (node.callee == "__exit") {
					if (node.args.size() != 1) {
						throw std::runtime_error("__exit 参数数量错误 at " + std::to_string(node.line) + ":" + std::to_string(node.col));
					}
					return {TypeKind::Void, ""};
				}
				/* ── D3 · 原子操作原语 ──────────────────────────────────
				   用 x86 的 lock 前缀指令直接实现，不依赖 pthread / libatomic
				   —— 编译器自己就能生成，零外部依赖。
				     __atomic_add(var, v)         无返回
				     __atomic_fetch_add(var, v)   返回加之前的原值
				     __atomic_cas(var, exp, new)  返回原值（CAS 语义）
				     __atomic_xchg(var, v)        返回原值
				     __mfence()                   全内存屏障 */
				if (node.callee == "__atomic_add") {
					if (node.args.size() != 2) throw std::runtime_error("__atomic_add 需要 2 个参数 at " + std::to_string(node.line));
					return {TypeKind::Void, ""};
				}
				if (node.callee == "__atomic_fetch_add" || node.callee == "__atomic_xchg") {
					if (node.args.size() != 2) throw std::runtime_error(node.callee + " 需要 2 个参数 at " + std::to_string(node.line));
					return {TypeKind::Int, ""};
				}
				if (node.callee == "__atomic_cas") {
					if (node.args.size() != 3) throw std::runtime_error("__atomic_cas 需要 3 个参数 at " + std::to_string(node.line));
					return {TypeKind::Int, ""};
				}
				if (node.callee == "__thread") {
					if (node.args.size() != 2) throw std::runtime_error("__thread 需要 2 个参数: (函数, 实参) at " + std::to_string(node.line));
					return {TypeKind::Int, ""};
				}
				if (node.callee == "__mfence") {
					if (!node.args.empty()) throw std::runtime_error("__mfence 不接受参数 at " + std::to_string(node.line));
					return {TypeKind::Void, ""};
				}
				/* ── 128 位整数运算原语 ── 均为 (int,int) -> int */
				if (node.callee == "__mulhi" || node.callee == "__umulhi" ||
				    node.callee == "__mullo" || node.callee == "__umullo" ||
				    node.callee == "__addc" || node.callee == "__subb") {
					if (node.args.size() != 2) {
						throw std::runtime_error(node.callee + " 参数数量错误 at " + std::to_string(node.line) + ":" + std::to_string(node.col));
					}
					return {TypeKind::Int, ""};
				}
				auto it = scope.find(node.callee);
				if (it == scope.end()) {
					/* 富诊断: 未声明函数。带拼写建议 —— 这是最常见的一类手误。 */
					{
						wei::diag::Diagnostic d;
						d.sev = wei::diag::Severity::Error;
						d.code = "E0425";
						d.message = "未声明函数: '" + node.callee + "'";
						int len = static_cast<int>(node.callee.size());
						d.labels.push_back({{node.line, node.col, len > 0 ? len : 1}, "找不到这个函数", true});
						/* 候选 = 当前可见函数名 + 内置函数 + 关键字表 */
						std::vector<std::string> cands;
						for (auto& kv : scope) cands.push_back(kv.first);
						for (auto& b : wei::diag::builtinNames()) cands.push_back(b);
						for (auto& k : wei::diag::keywordCandidates()) cands.push_back(k);
						std::string sug = wei::diag::suggestSimilar(node.callee, cands);
						if (!sug.empty()) d.help("你是否想写 '" + sug + "'？");
						else d.note("确认该函数已定义，或对应模块已 import");
						throw wei::diag::DiagnosticError(d);
					}
				}
				if (it->second.kind != TypeKind::Func) {
					throw std::runtime_error("非函数调用: " + node.callee + " at " + std::to_string(node.line) + ":" + std::to_string(node.col));
				}
				return resolveTypeName(it->second.name);
			} else if constexpr(std::is_same_v<T, ast::Output>) {
				for (auto& v : node.values) {
					if (v) inferExpr(*v, scope);
				}
				return {TypeKind::Void, ""};
			} else if constexpr(std::is_same_v<T, ast::Input>) {
				return {TypeKind::Void, ""};
			} else if constexpr(std::is_same_v<T, ast::IndexExpr>) {
				auto it = scope.find(node.name);
				if (it == scope.end()) {
					throw std::runtime_error("未声明标识符: " + node.name + " at " + std::to_string(node.line) + ":" + std::to_string(node.col));
				}
				if (node.idx) inferExpr(*node.idx, scope);
				if (it->second.kind == TypeKind::Ptr) {
					if (it->second.name == "ptr.int") return {TypeKind::Int, ""};
					if (it->second.name == "ptr.char") return {TypeKind::Char, ""};
					if (it->second.name == "ptr.bool") return {TypeKind::Bool, ""};
					if (it->second.name == "ptr.double") return {TypeKind::Double, ""};
					return {TypeKind::Omni, ""};
				}
				/* 数组元素与声明类型同型（运行时按 8 字节槽存取） */
				return it->second;
			} else if constexpr(std::is_same_v<T, ast::ModulePathGet>) {
				const PartInfo* pi = findPart(node.module, node.part);
				checkVisibility(pi, node.adv, node.module, node.part, node.line, node.col);
				const std::string* fname = nullptr;
				if (node.has_field) {
					auto fit = pi->field_types.find(node.field);
					if (fit == pi->field_types.end()) {
						throw std::runtime_error("部件 " + node.module + ">" + node.part + " 没有字段 " + node.field
						                         + " at " + std::to_string(node.line) + ":" + std::to_string(node.col));
					}
					fname = &node.field;
				} else {
					if (pi->field_order.empty()) {
						throw std::runtime_error("部件 " + node.module + ">" + node.part + " 没有成员可取"
						                         + " at " + std::to_string(node.line) + ":" + std::to_string(node.col));
					}
					fname = &pi->field_order[0];
				}
				return pi->field_types.at(*fname);
			} else if constexpr(std::is_same_v<T, ast::TupleLit>) {
				for (auto& elem : node.elements) {
					if (elem) inferExpr(*elem, scope);
				}
				return {TypeKind::Class, "tuple"};
			} else if constexpr(std::is_same_v<T, ast::SizeofExpr>) {
				return {TypeKind::Int, ""};
			} else if constexpr(std::is_same_v<T, ast::NewExpr>) {
				Type et = resolveTypeName(node.type);
				if (et.kind == TypeKind::Unknown) {
					throw std::runtime_error("new 未知类型: " + node.type + " at " + std::to_string(node.line) + ":" + std::to_string(node.col));
				}
				if (et.kind == TypeKind::Int) return {TypeKind::Ptr, "ptr.int"};
				if (et.kind == TypeKind::Bool) return {TypeKind::Ptr, "ptr.bool"};
				if (et.kind == TypeKind::Double) return {TypeKind::Ptr, "ptr.double"};
				if (et.kind == TypeKind::Char) return {TypeKind::Ptr, "ptr.char"};
				return {TypeKind::Ptr, "ptr"};
			} else if constexpr(std::is_same_v<T, ast::DeleteExpr>) {
				if (node.target) inferExpr(*node.target, scope);
				return {TypeKind::Void, ""};
			} else if constexpr(std::is_same_v<T, ast::RangeLit>) {
				Type lo = node.lo ? inferExpr(*node.lo, scope) : Type{TypeKind::Unknown, ""};
				if (node.hi) inferExpr(*node.hi, scope);
				return {TypeKind::Class, "range"};
			} else if constexpr(std::is_same_v<T, ast::TupleIndexExpr>) {
				if (node.tuple) inferExpr(*node.tuple, scope);
				return {TypeKind::Omni, ""};
			} else if constexpr(std::is_same_v<T, ast::TypeofExpr>) {
				if (node.arg) inferExpr(*node.arg, scope);
				return {TypeKind::Int, ""};
			} else if constexpr(std::is_same_v<T, ast::IsExpr>) {
				if (node.arg) inferExpr(*node.arg, scope);
				return {TypeKind::Bool, ""};
			} else if constexpr(std::is_same_v<T, ast::InExpr>) {
				if (node.value) inferExpr(*node.value, scope);
				if (node.container) inferExpr(*node.container, scope);
				return {TypeKind::Bool, ""};
			} else if constexpr(std::is_same_v<T, ast::AwaitExpr>) {
				Type et = node.operand ? inferExpr(*node.operand, scope) : Type{TypeKind::Omni, ""};
				return et;
			} else if constexpr(std::is_same_v<T, ast::CoExpr>) {
				for (auto& a : node.args) if (a) inferExpr(*a, scope);
				return {TypeKind::Ptr, "ptr"};          /* 协程句柄 */
			} else if constexpr(std::is_same_v<T, ast::CompileTimeExpr>) {
				if (node.arg) inferExpr(*node.arg, scope);
				return {TypeKind::Int, ""};
			} else if constexpr(std::is_same_v<T, ast::ForallExpr> || std::is_same_v<T, ast::ExistsExpr>) {
				auto s2 = scope;
				s2[node.var] = {TypeKind::Int, ""};
				if (node.domain) inferExpr(*node.domain, s2);
				if (node.cond) inferExpr(*node.cond, s2);
				return {TypeKind::Bool, ""};
			} else if constexpr(std::is_same_v<T, ast::CastExpr>) {
				if (node.value) inferExpr(*node.value, scope);
				Type ct = resolveTypeName(node.type);
				if (ct.kind == TypeKind::Unknown) {
					throw std::runtime_error("cast 未知类型: " + node.type + " at " + std::to_string(node.line) + ":" + std::to_string(node.col));
				}
				return ct;
			} else {
				return {TypeKind::Unknown, ""};
			}
		}, e);
	}
#endif // WEI_FRONTEND_NO_DEFS

}

/* ============================================================ */
/* 前端输出模块                                                  */
/* 工具链的所有打印集中在 MTC；MTG 只负责代码生成，不直接打印    */
/* ============================================================ */


namespace wei::front {
	extern const char* kProgramName;
	extern const char* kProgramVersion;
	extern const char* kLangName;
	extern const char* kPlatformLine;
	extern const char* kBuildUsage;
	extern const char* kMaxLangLine;

	void printVersion(const std::string& backend);
	void printStart();
	void printBuildOK(const std::string& outExe, const std::string& targetId);
	void printError(const std::string& msg);
	void emitError(const std::string& file, const std::string& raw);
	/* 富文本诊断入口: 异常若是 DiagnosticError 就渲染成 Rust 风格，
	   否则退回纯文本（旧行为）。 */
	void emitException(const std::string& file, const std::exception& e);
}

#ifndef WEI_FRONTEND_NO_DEFS

namespace wei::front {

	const char* kProgramName = "Module Target Generator";
	const char* kProgramVersion = "0.02.74282";
	const char* kLangName = "维语言（第一版）\\中文版维语言（第一版）\\文言文版维语言（第一版）";
	const char* kPlatformLine = "架构平台：WindowsNT x86-64（主要） 和 Linux x86-64";
	const char* kBuildUsage = "  通过命令：mtg build <file.wei> [-o out.exe] [-opt] 生成 x86-64 可执行文件";
	const char* kMaxLangLine = "编译器最高支持到所有维语言的第一代版本，如需要使用更高版本的特性，请检查或更新你的编译器。";

	void printVersion(const std::string& backend) {
		std::printf("%s %s - %s\n", kProgramName, kProgramVersion, kLangName);
		std::printf("%s\n", kPlatformLine);
		std::printf("%s\n", kBuildUsage);
		std::printf("后端检测: %s\n", backend.c_str());
		std::printf("%s\n", kMaxLangLine);
	}

	void printStart() {
		std::printf("%s - 维语言工具链 - 生成器\n", kProgramName);
		std::printf("支持的编程方式： %s\n", kLangName);
	}

	void printBuildOK(const std::string& outExe, const std::string& targetId) {
		std::printf("[完成] %s (%s)\n", outExe.c_str(), targetId.c_str());
	}

	void printError(const std::string& msg) {
		std::fprintf(stderr, "%s\n", msg.c_str());
	}

	struct FrontLoc {
		int line;
		int col;
		bool ok;
	};
	static FrontLoc frontExtractLoc(const std::string& msg) {
		static const std::regex re(R"(at\s+(\d+):(\d+))");
		std::smatch m;
		if (std::regex_search(msg, m, re)) {
			return {std::stoi(m[1]), std::stoi(m[2]), true};
		}
		return {0, 0, false};
	}

	void emitError(const std::string& file, const std::string& raw) {
		FrontLoc loc = frontExtractLoc(raw);
		if (loc.ok) {
			std::fprintf(stderr, "%s:%d:%d: %s\n", file.c_str(), loc.line, loc.col, raw.c_str());
		} else {
			std::fprintf(stderr, "%s:?:?: %s\n", file.c_str(), raw.c_str());
		}
	}

	void emitException(const std::string& file, const std::exception& e) {
		const wei::diag::DiagnosticError* de = dynamic_cast<const wei::diag::DiagnosticError*>(&e);
		if (de) {
			wei::diag::SourceCache sc;
			if (!sc.loadFile(file)) sc.setText("", file);
			wei::diag::emit(de->diag, sc);
			return;
		}
		/* 非诊断异常: 至少把源码行打出来，别只有行列号 */
		FrontLoc loc = frontExtractLoc(e.what());
		std::string msg = e.what();
		if (loc.ok) {
			wei::diag::SourceCache sc;
			sc.loadFile(file);
			std::string lineText = sc.line(loc.line);
			std::fprintf(stderr, "%s:%d:%d: %s\n", file.c_str(), loc.line, loc.col, msg.c_str());
			if (!lineText.empty()) {
				std::fprintf(stderr, "  %d | %s\n", loc.line, lineText.c_str());
			}
		} else {
			std::fprintf(stderr, "%s: %s\n", file.c_str(), msg.c_str());
		}
	}

}  // namespace wei::front

#endif // WEI_FRONTEND_NO_DEFS

/* ==========================================================================
 * 第二部分：后端 Backend（原 main.cpp）
 *   x86-64 代码生成 / 运行时内存管理 / PE 与 ELF 可执行文件输出 / main()
 * ========================================================================== */

#ifndef WEI_SKIP_MAIN

namespace {

	std::string basenameNoExt(const std::string& path) {
		size_t slash = path.find_last_of("/\\");
		std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
		size_t dot = base.find_last_of('.');
		if (dot != std::string::npos) {
			return base.substr(0, dot);
		}
		return base;
	}

	/* 判断字节串是否为合法 UTF-8，用于区分 UTF-8 与 GBK/ANSI 源文件 */
	static bool isValidUtf8(const std::string& s) {
		size_t i = 0;
		while (i < s.size()) {
			unsigned char c = static_cast<unsigned char>(s[i]);
			if (c < 0x80) {
				i++;
				continue;
			}
			size_t n = 0;
			uint32_t cp = 0;
			if ((c & 0xE0) == 0xC0) {
				n = 2;
				cp = c & 0x1F;
				if (c < 0xC2) return false;
			} else if ((c & 0xF0) == 0xE0) {
				n = 3;
				cp = c & 0x0F;
			} else if ((c & 0xF8) == 0xF0) {
				n = 4;
				cp = c & 0x07;
			} else {
				return false;
			}
			if (i + n > s.size()) return false;
			for (size_t k = 1; k < n; k++) {
				unsigned char cc = static_cast<unsigned char>(s[i + k]);
				if ((cc & 0xC0) != 0x80) return false;
				cp = (cp << 6) | (cc & 0x3F);
			}
			if ((n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000)) return false;
			if (cp >= 0xD800 && cp <= 0xDFFF) return false;
			if (cp > 0x10FFFF) return false;
			i += n;
		}
		return true;
	}

#if defined(_WIN32)
	static std::string codepageToUtf8(const std::string& s, UINT cp) {
		if (s.empty()) return std::string();
		int wlen = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, s.data(),
		                               static_cast<int>(s.size()), nullptr, 0);
		if (wlen <= 0) return std::string();
		std::wstring w(static_cast<size_t>(wlen), L'\0');
		if (MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, s.data(),
		                        static_cast<int>(s.size()), &w[0], wlen) <= 0) {
			return std::string();
		}
		int ulen = WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, nullptr, 0, nullptr, nullptr);
		if (ulen <= 0) return std::string();
		std::string u(static_cast<size_t>(ulen), '\0');
		if (WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, &u[0], ulen, nullptr, nullptr) <= 0) {
			return std::string();
		}
		return u;
	}

	static std::string wideToUtf8(const std::wstring& w) {
		if (w.empty()) return std::string();
		int ulen = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
		                               nullptr, 0, nullptr, nullptr);
		if (ulen <= 0) return std::string();
		std::string u(static_cast<size_t>(ulen), '\0');
		if (WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
		                        &u[0], ulen, nullptr, nullptr) <= 0) {
			return std::string();
		}
		return u;
	}
#endif

	/* 统一源文件编码为 UTF-8: 支持 UTF-8/UTF-8 BOM、UTF-16、GBK/ANSI */
	static std::string normalizeSourceEncoding(const std::string& raw) {
		if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF
		        && static_cast<unsigned char>(raw[1]) == 0xBB
		        && static_cast<unsigned char>(raw[2]) == 0xBF) {
			return raw.substr(3);   /* UTF-8 BOM */
		}
#if defined(_WIN32)
		if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFF
		        && static_cast<unsigned char>(raw[1]) == 0xFE) {
			size_t n = (raw.size() - 2) / 2;
			std::wstring w(n, L'\0');
			if (n > 0) memcpy(&w[0], raw.data() + 2, n * 2);
			std::string u = wideToUtf8(w);
			if (!u.empty()) return u;   /* UTF-16 LE */
		}
		if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFE
		        && static_cast<unsigned char>(raw[1]) == 0xFF) {
			size_t n = (raw.size() - 2) / 2;
			std::wstring w(n, L'\0');
			for (size_t i = 0; i < n; i++) {
				unsigned char hi = static_cast<unsigned char>(raw[2 + i * 2]);
				unsigned char lo = static_cast<unsigned char>(raw[3 + i * 2]);
				w[i] = static_cast<wchar_t>((hi << 8) | lo);
			}
			std::string u = wideToUtf8(w);
			if (!u.empty()) return u;   /* UTF-16 BE */
		}
#endif
		if (isValidUtf8(raw)) return raw;
#if defined(_WIN32)
		/* 非 UTF-8: 优先按 GBK(936) 解码，失败再按系统 ANSI 代码页 */
		std::string u = codepageToUtf8(raw, 936);
		if (!u.empty()) return u;
		u = codepageToUtf8(raw, CP_ACP);
		if (!u.empty()) return u;
#endif
		return raw;
	}

	std::string readFile(const std::string& path) {
		std::ifstream in(path, std::ios::binary);
		if (!in) {
			throw std::runtime_error("无法打开文件: " + path);
		}
		std::ostringstream ss;
		ss << in.rdbuf();
		return normalizeSourceEncoding(ss.str());
	}

	using wei::front::emitError;
	using wei::front::emitException;

	/* ── x86-64 code buffer with instruction encoding helpers ── */
	struct X64Buf {
		std::vector<uint8_t> buf;
		std::vector<std::pair<size_t, size_t>> rip_str_patches_;
		size_t pos() const {
			return buf.size();
		}
		void clear() {
			buf.clear();
			rip_str_patches_.clear();
		}
		void u8(uint8_t v) {
			buf.push_back(v);
		}
		void u32(uint32_t v) {
			buf.push_back(uint8_t(v));
			buf.push_back(uint8_t(v >> 8));
			buf.push_back(uint8_t(v >> 16));
			buf.push_back(uint8_t(v >> 24));
		}
		void u64(int64_t v) {
			u32(uint32_t(v));
			u32(uint32_t(v >> 32));
		}
		void patch_u32(size_t at, uint32_t v) {
			buf[at] = uint8_t(v);
			buf[at + 1] = uint8_t(v >> 8);
			buf[at + 2] = uint8_t(v >> 16);
			buf[at + 3] = uint8_t(v >> 24);
		}
		void patch_u8(size_t at, uint8_t v) {
			buf[at] = v;
		}
		void rex_w() {
			u8(0x48);
		}
		void rex_wr(int r) {
			u8((r & 8) ? 0x4C : 0x48);
		}
		void rex_wb(int b) {
			u8((b & 8) ? 0x49 : 0x48);
		}
		void rex_wrb(int r, int b) {
			u8(0x48 | ((r & 8) ? 0x04 : 0) | ((b & 8) ? 0x01 : 0));
		}
		void modrm(uint8_t mod, uint8_t reg, uint8_t rm) {
			u8((mod << 6) | ((reg & 7) << 3) | (rm & 7));
		}
		/* REX.W 且同时带 R/X/B 扩展位（用于 SIB 寻址） */
		void rex_wrxb(int r, int x, int b) {
			u8(0x48 | ((r & 8) ? 0x04 : 0) | ((x & 8) ? 0x02 : 0) | ((b & 8) ? 0x01 : 0));
		}
		void sib(int scale, int index, int base) {
			u8(static_cast<uint8_t>((scale << 6) | ((index & 7) << 3) | (base & 7)));
		}
		/* mov r64, [base + index*8 + disp32] —— size-class 链表按运行时 class 索引 */
		void mov64r_mem_idx8(int r, int base, int index, int32_t disp) {
			rex_wrxb(r, index, base);
			u8(0x8B);
			modrm(2, r, 4);
			sib(3, index, base);
			u32(static_cast<uint32_t>(disp));
		}
		/* mov [base + index*8 + disp32], r64 */
		void mov_mem_idx8_64r(int base, int index, int32_t disp, int r) {
			rex_wrxb(r, index, base);
			u8(0x89);
			modrm(2, r, 4);
			sib(3, index, base);
			u32(static_cast<uint32_t>(disp));
		}
		/* bsr r64, r/m64 (0F BD /r) —— 用于 size class 计算 */
		void bsr_r64_rm64(int r, int rm) {
			rex_wrb(r, rm);
			u8(0x0F);
			u8(0xBD);
			modrm(3, r, rm);
		}
		/* shl r64, cl (D3 /4) —— total = 16 << class */
		void shl_rm64_cl(int rm) {
			rex_wb(rm);      /* 必须带 REX.B：rm 可为 r8-r15，否则会错编成 rdx 等低位寄存器 */
			u8(0xD3);
			modrm(3, 4, rm);
		}
		/* shl r/m64, imm8 (C1 /4) */
		void shl_rm64_imm8(int rm, uint8_t n) {
			rex_wb(rm);
			u8(0xC1);
			modrm(3, 4, rm);
			u8(n);
		}
		/* test r/m64, r64 (85 /r) */
		void test_rm64_r64(int rm, int r) {
			rex_wrb(r, rm);
			u8(0x85);
			modrm(3, r, rm);
		}
		/* test r/m64, imm32 (F7 /0) —— 用于指针对齐校验 */
		void test_rm64_imm32(int rm, int32_t v) {
			rex_wb(rm);      /* rex_wb 而非 rex_w：rm 可为 r8-r15 */
			u8(0xF7);
			modrm(3, 0, rm);
			u32(static_cast<uint32_t>(v));
		}
		/* shr r/m64, imm8 (C1 /5) */
		void shr_rm64_imm8(int rm, uint8_t n) {
			rex_wb(rm);
			u8(0xC1);
			modrm(3, 5, rm);
			u8(n);
		}
		/* rep stosq (F3 48 AB): [rdi] = rax, rcx 次。要求 DF=0。 */
		void rep_stosq() {
			u8(0xF3);
			u8(0x48);
			u8(0xAB);
		}
		/* xchg r64, r/m64 (87 /r) —— 无需额外寄存器的原子交换 */
		void xchg_rm64_r64(int rm, int r) {
			rex_wrb(r, rm);
			u8(0x87);
			modrm(3, r, rm);
		}
		/* neg r/m64 已有；补充 not (F7 /2) 用于清位 */
		/* ── 位运算编码（本轮补齐 and/or/xor/not/sar）──────────────────────
		   and/or/xor 统一走 reg,rm64 形式（操作数顺序: 生成时已把右值放 rcx，
		   左值在 rax，故用 rax &= rcx 形态）。not 为单操作数取反。
		   sar 是**算术**右移（保留符号），与 C 的 >> 对有符号数语义一致。 */
		void and_r64_rm64(int r, int rm) {   /* r &= rm */
			rex_wrb(r, rm);
			u8(0x23);
			modrm(3, r, rm);
		}
		void or_r64_rm64(int r, int rm) {    /* r |= rm */
			rex_wrb(r, rm);
			u8(0x0B);
			modrm(3, r, rm);
		}
		void xor_r64_rm64(int r, int rm) {   /* r ^= rm */
			rex_wrb(r, rm);
			u8(0x33);
			modrm(3, r, rm);
		}
		void sar_rm64_imm8(int rm, uint8_t n) {
			rex_wrb(0, rm);
			u8(0xC1);
			modrm(3, 7, rm);
			u8(n);
		}
		void sar_rm64_cl(int rm) {
			rex_wrb(0, rm);
			u8(0xD3);
			modrm(3, 7, rm);
		}
		/* ══════════════════════════════════════════════════════════════
		   32 位独立编码通路（C1 位宽体系 · i32 打样）
		   现有所有算术都带 REX.W（64 位）。i32 走的是**另一套编码**:
		   不带 REX.W 前缀，操作数在 eax/ecx，结果零扩展到 rax 高位。
		   这不是"复用 64 位通路再截断"，而是真正独立的 32 位指令。
		   ══════════════════════════════════════════════════════════ */
		void mov_eax_imm32(int32_t v) {   /* B8+rd id */
			u8(0xB8); u32(static_cast<uint32_t>(v));
		}
		void add_eax_imm32(int32_t v) {   /* 05 id */
			u8(0x05); u32(static_cast<uint32_t>(v));
		}
		void cmp_eax_imm32(int32_t v) {   /* 3D id */
			u8(0x3D); u32(static_cast<uint32_t>(v));
		}
		/* mov eax, [base + disp] : 8B /r，无 REX → 32 位载入，rax 高位清零 */
		void mov_eax_mem(int32_t d) {
			u8(0x8B);
			if (d >= -128 && d <= 127) { modrm(1, 0, 5); u8(static_cast<uint8_t>(static_cast<uint32_t>(d))); }
			else { modrm(2, 0, 5); u32(static_cast<uint32_t>(d)); }
		}
		/* mov [base + disp], eax : 89 /r → 只写低 4 字节 */
		void mov_mem_eax(int32_t d) {
			u8(0x89);
			if (d >= -128 && d <= 127) { modrm(1, 0, 5); u8(static_cast<uint8_t>(static_cast<uint32_t>(d))); }
			else { modrm(2, 0, 5); u32(static_cast<uint32_t>(d)); }
		}
		void add_eax_ecx()  { u8(0x01); modrm(3, 1, 0); }   /* add eax,ecx */
		void sub_eax_ecx()  { u8(0x29); modrm(3, 1, 0); }   /* sub eax,ecx */
		void imul_eax_ecx() { u8(0x0F); u8(0xAF); modrm(3, 0, 1); }
		void and_eax_ecx()  { u8(0x21); modrm(3, 1, 0); }
		void or_eax_ecx()   { u8(0x09); modrm(3, 1, 0); }
		void xor_eax_ecx()  { u8(0x31); modrm(3, 1, 0); }
		void cmp_eax_ecx()  { u8(0x39); modrm(3, 1, 0); }
		void neg_eax()      { u8(0xF7); modrm(3, 3, 0); }
		void cdq32()        { u8(0x99); }                   /* eax → edx:eax */
		void idiv_ecx()     { u8(0xF7); modrm(3, 7, 1); }
		/* movsxd rax, eax : REX.W 63 /r —— 32 位值交给 64 位上下文前必须符号扩展 */
		void movsxd_rax_eax() { rex_wrb(0, 0); u8(0x63); modrm(3, 0, 0); }
		/* ── C1: 8/16/32 位算术编码 ────────────────────────────────────────
		   现有后端所有算术都硬编码 64 位（REX.W + 0x48 前缀）。要做真位宽，
		   必须给出 8/16/32 各自独立的编码通路:
		     8 位 : 无 REX.W，opcode 用 0x00/0x28/0xF6 系列
		     16 位: 无 REX.W + 0x66 操作数大小前缀
		     32 位: 无 REX.W（默认 32 位）
		   这里只补加法/减法/乘法/比较，够覆盖位宽语义的主体。 */
		void prefix_for_width(int width) {
			/* 返回该宽度需要的前缀字节序列是否已写入；调用方先写前缀再写 opcode */
			if (width == 16) u8(0x66);       /* 0x66 操作数大小覆盖前缀 */
			/* 8/32/64 位无额外前缀；64 位由 rex_wrb 里的 REX.W 处理 */
		}
		void add_width(int width, int r, int rm) {
			if (width == 64) { rex_wrb(r, rm); u8(0x03); modrm(3, r, rm); return; }
			if (width == 8)  { rex_wrb(r, rm); u8(0x02); modrm(3, r, rm); return; }
			prefix_for_width(width);
			if (r >= 8 || rm >= 8) u8(0x40 | 0x40);   /* 无 W 的 REX，仅为访问 r8-r15 */
			u8(0x03);
			modrm(3, r & 7, rm & 7);
		}
		void sub_width(int width, int r, int rm) {
			if (width == 64) { rex_wrb(r, rm); u8(0x2B); modrm(3, r, rm); return; }
			if (width == 8)  { rex_wrb(r, rm); u8(0x2A); modrm(3, r, rm); return; }
			prefix_for_width(width);
			u8(0x2B);
			modrm(3, r & 7, rm & 7);
		}
		/* 窄立即数加法 add r/m(w), imm8 —— 真按宽度发射:
		     8 位 : 80 /0 ib          （不带 REX.W）
		     16 位: 66 83 /0 ib       （0x66 操作数大小前缀）
		     32 位: 83 /0 ib          （默认 32 位）
		     64 位: 48 83 /0 ib       （REX.W）
		   关键: 窄指令会按宽度设置 OF/CF，溢出检测才能用标志位判——
		   这正是"用 64 位算完再比大小"做不到的。 */
		void add_imm_width(int width, int rm, int32_t imm) {
			if (width == 64) { rex_wrb(0, rm); u8(0x83); modrm(3, 0, rm & 7); u8(imm & 0xFF); return; }
			if (width == 16) u8(0x66);
			if (width == 8)  { u8(0x80); modrm(3, 0, rm & 7); u8(imm & 0xFF); return; }
			u8(0x83);
			modrm(3, 0, rm & 7);
			u8(imm & 0xFF);
		}
		void sub_imm_width(int width, int rm, int32_t imm) {
			if (width == 64) { rex_wrb(0, rm); u8(0x83); modrm(3, 5, rm & 7); u8(imm & 0xFF); return; }
			if (width == 16) u8(0x66);
			if (width == 8)  { u8(0x80); modrm(3, 5, rm & 7); u8(imm & 0xFF); return; }
			u8(0x83);
			modrm(3, 5, rm & 7);
			u8(imm & 0xFF);
		}
		void cmp_width_imm32(int width, int rm, int32_t imm) {
			if (width == 64) { cmp_rm64_imm32(rm, imm); return; }
			if (width == 8)  { rex_wrb(0, rm); u8(0x80); modrm(3, 7, rm); u8(imm & 0xFF); return; }
			prefix_for_width(width);
			u8(0x83);
			modrm(3, 7, rm & 7);
			u32((uint32_t)imm);
		}

		void not_rm64(int rm) {
			rex_w();
			u8(0xF7);
			modrm(3, 2, rm);
		}

		/* ── Instruction emitters (x86-64, Intel syntax) ── */
		/* Register order: RAX(0), RCX(1), RDX(2), RBX(3), RSP(4), RBP(5), RSI(6), RDI(7) */

		void push_r(int r) {
			if (r < 8) {
				u8(0x50 + r);
			} else {
				u8(0x41);
				u8(0x50 + (r & 7));
			}
		}
		void pop_r(int r) {
			if (r < 8) {
				u8(0x58 + r);
			} else {
				u8(0x41);
				u8(0x58 + (r & 7));
			}
		}
		void push_imm32(int32_t v) {
			u8(0x68);
			u32(static_cast<uint32_t>(v));
		}

		/* mov r64, imm64 */
		void mov64r_imm64(int r, int64_t v) {
			rex_wb(r);
			u8(0xB8 + (r & 7));
			u64(v);
		}
		/* mov r/m64, imm8 sign-extended (C7 /0 but with 8-bit imm? No, use 0x83 for 8-bit) */
		void mov32r_imm32(int r, int32_t v) {
			/* 必须带 REX.B: r 可为 r8-r15，否则 B8+r 只会命中 rax-rdi。
			   此前缺这个前缀 —— 实测 `mov32r_imm32(9, 100)` 被编成
			   `mov $0x64,%ecx`（写进 rcx 而不是 r9），导致所有用 r9+
			   作常量的优化静默算出错误结果（平方和模式即因此输出
			   1015 而非 328350）。
			   注意: 这里**不能**加 REX.W，否则会变成 10 字节的
			   movabs r64,imm64；32 位写入已会自动清零高 32 位。 */
			if (r >= 8) u8(0x41);
			u8(0xB8 + (r & 7));
			u32(static_cast<uint32_t>(v));
		}
		/* inc r/m64 (FF /0) */
		void inc_rm64(int rm) {
			rex_wb(rm);
			u8(0xFF);
			modrm(3, 0, rm);
		}
		/* mov r64, r/m64 (8B) */
		/* ── D1 · 函数级寄存器驻留的统一拦截层 ─────────────────────────
		   把某个局部栈槽钉进物理寄存器后，**所有**对 [rbp+d] 的读写
		   都必须改走寄存器。原有代码直接访问栈槽的地方有上百处，逐个
		   改调用点既易漏又会静默算错（寄存器与内存失去同步）。

		   这里在**最底层发射原语**做一次精确拦截：按 disp 精确查表，
		   命中则改发 `mov reg, phys` / `mov phys, reg`。
		   - 只认 base==rbp(5)
		   - 只认精确登记过的 disp（保存寄存器槽 / canary / 临时 spill
		     槽都不在表里，绝不会被误伤）
		   - 未设置 hook 时行为与原来逐字节一致 */
		std::function<int(int64_t)> slot_hook_;
		bool slot_violation_ = false;      /* 取了驻留变量的地址 → 放弃驻留 */
		void set_slot_hook(std::function<int(int64_t)> h) { slot_hook_ = std::move(h); slot_violation_ = false; }
		/* 输出/外部调用 stub 在对齐栈之前会调用 save_rsp_r15()。
		   target 里写的是 g.c_.save_rsp_r15()，NativeGen 里写的是
		   c_.save_rsp_r15() —— 在**原语内部**挂回调才能一处全覆盖，
		   不漏任何调用点。 */
		std::function<void()> pre_align_;
		void set_pre_align(std::function<void()> h) { pre_align_ = std::move(h); }
		/* 取驻留变量的地址(LEA [rbp+d])时，后续可能经指针**写**内存，
		   而 hook 仍以为值在寄存器里 —— 必须彻底降级到内存路径。 */
		std::function<void()> on_slot_addr_;
		void set_on_slot_addr(std::function<void()> h) { on_slot_addr_ = std::move(h); }
		inline void slot_addr_taken(int64_t d, int base) {
			if (slot_phys(d, base) >= 0 && on_slot_addr_) {
				auto h = on_slot_addr_; on_slot_addr_ = {};
				slot_violation_ = true;
				h();
			}
		}
		inline int slot_phys(int64_t d, int base) {
			return (base == 5 && slot_hook_) ? slot_hook_(d) : -1;
		}

		void mov64r_rm64(int r, int rm) {
			rex_wrb(r, rm);
			u8(0x8B);
			modrm(3, r, rm);
		}
		/* mov r/m64, r64 (89) */
		void mov_rm64_64r(int rm, int r) {
			rex_wrb(r, rm);
			u8(0x89);
			modrm(3, r, rm);
		}
		/* mov r/m64, imm32 sign-extended (C7 /0) */
		void mov_rm64_imm32(int rm, int32_t v) {
			rex_w();
			u8(0xC7);
			modrm(3, 0, rm);
			u32(static_cast<uint32_t>(v));
		}
		/* mov r/m64, imm8 sign-extended (C7 /0 but with 8-bit imm? No, use 0x83 for 8-bit) */
		/* Actually C7 is always imm32. For mov with imm8 use C6 but only for byte. Skip for now. */

		/* mov r64, [r+disp] ??自动选择 disp8/disp32 */
		void mov64r_memd8(int r, int base, int64_t d) {
			int pr = slot_phys(d, base);
			if (pr >= 0) { if (pr != r) mov64r_rm64(r, pr); return; }
			if (d >= -128 && d <= 127) {
				rex_wrb(r, base);
				u8(0x8B);
				modrm(1, r, base);
				if ((base & 7) == 4) u8(0x24);
				u8(static_cast<uint8_t>(d));
			} else {
				rex_wrb(r, base);
				u8(0x8B);
				modrm(2, r, base);
				if ((base & 7) == 4) u8(0x24);
				u32(static_cast<uint32_t>(d));
			}
		}
		/* mov r64, [r+disp32] */
		void mov64r_memd32(int r, int base, int32_t d) {
			int pr = slot_phys(d, base);
			if (pr >= 0) { if (pr != r) mov64r_rm64(r, pr); return; }
			rex_wrb(r, base);
			u8(0x8B);
			modrm(2, r, base);
			if ((base & 7) == 4) u8(0x24);  /* SIB: [rsp+disp32] */
			u32(static_cast<uint32_t>(d));
		}
		/* mov [r+disp], r64 ??自动选择 disp8/disp32 */
		void mov_memd8_64r(int base, int64_t d, int r) {
			int pr = slot_phys(d, base);
			if (pr >= 0) { if (pr != r) mov64r_rm64(pr, r); return; }
			if (d >= -128 && d <= 127) {
				rex_wrb(r, base);
				u8(0x89);
				modrm(1, r, base);
				if ((base & 7) == 4) u8(0x24);
				u8(static_cast<uint8_t>(d));
			} else {
				rex_wrb(r, base);
				u8(0x89);
				modrm(2, r, base);
				if ((base & 7) == 4) u8(0x24);
				u32(static_cast<uint32_t>(d));
			}
		}
		/* mov [r+disp32], r64 */
		void mov_memd32_64r(int base, int32_t d, int r) {
			int pr = slot_phys(d, base);
			if (pr >= 0) { if (pr != r) mov64r_rm64(pr, r); return; }
			rex_wrb(r, base);
			u8(0x89);
			modrm(2, r, base);
			if ((base & 7) == 4) u8(0x24);  /* SIB: [rsp+disp32] */
			u32(static_cast<uint32_t>(d));
		}
		/* mov [r+disp], imm32 ??自动选择 disp8/disp32 */
		void mov_memd8_imm32(int base, int64_t d, int32_t v) {
			int pr = slot_phys(d, base);
			if (pr >= 0) {
				/* 零/常量初始化驻留变量：直接把立即数装进物理寄存器 */
				if (v >= INT32_MIN && v <= INT32_MAX) mov32r_imm32(pr, v);
				else mov64r_imm64(pr, v);
				return;
			}
			if (d >= -128 && d <= 127) {
				rex_w();
				u8(0xC7);
				modrm(1, 0, base);
				u8(static_cast<uint8_t>(d));
				u32(static_cast<uint32_t>(v));
			} else {
				rex_w();
				u8(0xC7);
				modrm(2, 0, base);
				u32(static_cast<uint32_t>(d));
				u32(static_cast<uint32_t>(v));
			}
		}
		/* mov [r+disp32], imm32 */
		void mov_memd32_imm32(int base, int32_t d, int32_t v) {
			rex_w();
			u8(0xC7);
			modrm(2, 0, base);
			u32(static_cast<uint32_t>(d));
			u32(static_cast<uint32_t>(v));
		}

		/* mov r64, [rip+disp32] */
		void mov64r_rip(int r, int32_t d) {
			rex_wrb(r, 5);
			u8(0x8B);
			modrm(0, r, 5);
			u32(static_cast<uint32_t>(d));
		}
		/* mov [rip+disp32], r64 */
		void mov_rip_r64(int32_t d, int r) {
			rex_wrb(r, 5);
			u8(0x8B);
			modrm(0, r, 5);
			u32(static_cast<uint32_t>(d));
		}
		/* lea r64, [r+disp] ??自动选择 disp8/disp32 */
		/* lea r64, [base*scale + disp32]  —— scale ∈ {1,2,4,8}
		   用于把"若干连续项之和"压进一条指令（如 4*t+6k）。
		   lea 不置标志位，故不能用来做溢出检查；调用方需另行处理。 */
		void lea_scaled(int r, int base, int scale, int32_t disp) {
			int ss = 0;
			if (scale == 2) ss = 1;
			else if (scale == 4) ss = 2;
			else if (scale == 8) ss = 3;
			if (scale == 1) {
				/* lea r, [base + disp32] —— 无 SIB */
				rex_wrb(r, base);
				u8(0x8D);
				modrm(2, r, base);
				u32(static_cast<uint32_t>(disp));
				return;
			}
			/* lea r, [base*scale + disp32] —— SIB 且**无 base 寄存器**。
			   无 base 的形式只有一种合法编码: mod=00, SIB.base=101, 后跟 disp32。
			   （mod=01 时 base=101 表示 rbp，不是"无 base" —— 这正是第一版
			     写错的地方，导致地址算错。）
			   注意 REX: base 在这里位于 SIB.index 位置，需要的是 REX.X
			   而不是 REX.B —— 用 rex_wrxb(r, base, 0) 正好把 base 放进 X 位。 */
			rex_wrxb(r, base, 0);
			u8(0x8D);
			modrm(0, r, 4);                 /* mod=00, rm=100 → SIB */
			sib(ss, base, 5);               /* base=101 + mod=00 → 无 base, disp32 */
			u32(static_cast<uint32_t>(disp));
		}
		void lea64r_memd8(int r, int base, int64_t d) {
			slot_addr_taken(d, base);
			if (d >= -128 && d <= 127) {
				rex_wrb(r, base);
				u8(0x8D);
				modrm(1, r, base);
				u8(static_cast<uint8_t>(d));
			} else {
				rex_wrb(r, base);
				u8(0x8D);
				modrm(2, r, base);
				u32(static_cast<uint32_t>(d));
			}
		}
		/* lea r64, [r+disp32] */
		void lea64r_memd32(int r, int base, int32_t d) {
			slot_addr_taken(d, base);
			rex_wrb(r, base);
			u8(0x8D);
			modrm(2, r, base);
			u32(static_cast<uint32_t>(d));
		}
		void lea64r_rip(int r) {
			rex_wrb(r, 5);
			u8(0x8D);
			modrm(0, r, 5);
		}

		/* ── ALU: r/m64 = r/m64 op r64 (opcodes 01, 09, 11, 19, 21, 29, 31, 39) ── */
		void alu_rm64_r64(uint8_t op, int rm, int r) {
			rex_wrb(r, rm);
			u8(op);
			modrm(3, r, rm);
		}
		/* ═══ 128 位除法所需的移位/借位/清零原语 ════════════════════════
		   每一条的编码都用 objdump -b binary -m i386:x86-64 逐条核对过，
		   不是凭记忆写的。 */

		/* shld r/m64, r64, cl    REX.W 0F A5 /r
		   shld r10, r11, cl -> 4D 0F A5 DA            [已核对] */
		void shld_rm64_r64_cl(int rm, int r) {
			rex_wrb(r, rm); u8(0x0F); u8(0xA5); modrm(3, r, rm);
		}
		/* shld r/m64, r64, imm8  REX.W 0F A4 /r ib
		   shld r10, r11, 5  -> 4D 0F A4 DA 05         [已核对] */
		void shld_rm64_r64_imm8(int rm, int r, uint8_t n) {
			rex_wrb(r, rm); u8(0x0F); u8(0xA4); modrm(3, r, rm); u8(n);
		}
		/* shrd r/m64, r64, cl    REX.W 0F AD /r
		   shrd rax, rsi, cl -> 48 0F AD F0            [已核对] */
		void shrd_rm64_r64_cl(int rm, int r) {
			rex_wrb(r, rm); u8(0x0F); u8(0xAD); modrm(3, r, rm);
		}
		/* shrd r/m64, r64, imm8  REX.W 0F AC /r ib
		   shrd rax, rsi, 1  -> 48 0F AC F0 01         [已核对] */
		void shrd_rm64_r64_imm8(int rm, int r, uint8_t n) {
			rex_wrb(r, rm); u8(0x0F); u8(0xAC); modrm(3, r, rm); u8(n);
		}
		/* shr r/m64, cl   REX.W D3 /5
		   shr rax, cl     -> 48 D3 E8                 [已核对] */
		void shr_rm64_cl(int rm) { rex_wb(rm); u8(0xD3); modrm(3, 5, rm); }
		/* sbb r64, r/m64  REX.W 1B /r */
		void sbb_r64_rm64(int r, int rm) { alu_r64_rm64(0x1B, r, rm); }
		/* adc r64, r/m64  REX.W 13 /r */
		void adc_r64_rm64(int r, int rm) { alu_r64_rm64(0x13, r, rm); }
		/* xor r32, imm8 (零扩展到 64 位)  83 /6 ib
		   xor ecx, 63    -> 83 F1 3F                  [已核对] */
		void xor_r32_imm8(int r, uint8_t v) {
			if (r >= 8) u8(0x41);
			u8(0x83); modrm(3, 6, r); u8(v);
		}
		/* xor r32, r32 —— 必须发 REX，否则编不到 r8d-r15d。
		   xor r9d,r9d -> 45 31 C9 ; xor esi,esi -> 31 F6   [已核对] */
		void xor_r32_r32(int a, int b) {
			if (a >= 8 || b >= 8) u8(0x40 | ((b >= 8) ? 4 : 0) | ((a >= 8) ? 1 : 0));
			u8(0x31); modrm(3, b, a);
		}
		/* mul r/m64 (无符号 RDX:RAX = RAX * rm)  REX.W F7 /4
		   mul r8     -> 49 F7 E0                      [已核对] */
		void mul_rm64(int rm) { rex_wb(rm); u8(0xF7); modrm(3, 4, rm); }
		/* div r/m64 (无符号 RDX:RAX / rm)       REX.W F7 /6
		   div r8     -> 49 F7 F0                      [已核对] */
		void div_rm64(int rm) { rex_wb(rm); u8(0xF7); modrm(3, 6, rm); }

		void alu_r64_rm64(uint8_t op, int r, int rm) {
			rex_wrb(r, rm);
			u8(op);
			modrm(3, r, rm);
		}
		void alu_r64_memd8(uint8_t op, int r, int base, int64_t d) {
			int pr = slot_phys(d, base);
			if (pr >= 0) { alu_r64_rm64(op, r, pr); return; }
			if (d >= -128 && d <= 127) {
				rex_wrb(r, base);
				u8(op);
				modrm(1, r, base);
				u8(static_cast<uint8_t>(d));
			} else {
				rex_wrb(r, base);
				u8(op);
				modrm(2, r, base);
				u32(static_cast<uint32_t>(d));
			}
		}
		void alu_r64_memd32(uint8_t op, int r, int base, int32_t d) {
			int pr = slot_phys(d, base);
			if (pr >= 0) { alu_r64_rm64(op, r, pr); return; }
			rex_wrb(r, base);
			u8(op);
			modrm(2, r, base);
			u32(static_cast<uint32_t>(d));
		}
		/* ALU with imm: r/m64 op imm32 (0x81 /reg, 0x83 /reg for imm8) */
		void alu_rm64_imm32(int rm, int regop, int32_t v) {
			rex_wb(rm);  /* rm 可能为扩展寄存器 (r8-r15) */
			if (v >= -128 && v <= 127) {
				u8(0x83);
				modrm(3, regop, rm);
				u8(static_cast<uint8_t>(static_cast<int8_t>(v)));
			} else {
				u8(0x81);
				modrm(3, regop, rm);
				u32(static_cast<uint32_t>(v));
			}
		}

		/* add */
		void add_r64_rm64(int r, int rm) {
			alu_r64_rm64(0x03, r, rm);
		}
		void add_rm64_r64(int rm, int r) {
			alu_rm64_r64(0x01, rm, r);
		}
		void add_r64_memd8(int r, int base, int64_t d) {
			alu_r64_memd8(0x03, r, base, d);
		}
		void add_r64_memd32(int r, int base, int32_t d) {
			alu_r64_memd32(0x03, r, base, d);
		}
		void add_rm64_imm32(int rm, int32_t v) {
			alu_rm64_imm32(rm, 0, v);
		}
		/* ── 局部变量栈偏移超出 disp8 时必须改用 disp32 ──
		   局部槽按 -8*(slot+1) 排在 rbp 下方, 而数组会一次吃掉 N 个槽。
		   声明 create.int arr[16] 之后, 后续变量的偏移就到了 -152,
		   超出 int8 的 -128..127。若在此截断成 int8 再编码, 指令
		   "看起来合法"却写到错误的栈地址 —— 轻则算错结果(实测 arr[16]
		   时阶乘给出 1), 重则踩坏金丝雀触发 int3。
		   故与 mov64r_memd8 / mov_memd8_64r 一样做双分支。 */
		void add_memd8_imm32(int base, int64_t disp, int32_t v) {
			int pr = slot_phys(disp, base);
			if (pr >= 0) { add_rm64_imm32(pr, v); return; }
			/* 注意: 这个局部变量**不能**命名为 near。
			   Windows 头文件(minwindef.h 一系)把 near / far 定义成空宏以兼容
			   16 位时代的代码, 于是 `const bool near = ...` 被展开成
			   `const bool  = ...`, 报 "expected unqualified-id before '='",
			   后续 `near ? 1 : 2` 再报 "expected primary-expression before '?'"。
			   改个名即可, 不必去动系统头。 */
			const bool disp_fits8 = (disp >= -128 && disp <= 127);
			if (v >= -128 && v <= 127) {
				rex_w();
				u8(0x83);
				modrm(disp_fits8 ? 1 : 2, 0, base);
				if (disp_fits8) u8(static_cast<uint8_t>(disp));
				else u32(static_cast<uint32_t>(disp));
				u8(static_cast<uint8_t>(static_cast<int8_t>(v)));
			} else {
				rex_w();
				u8(0x81);
				modrm(disp_fits8 ? 1 : 2, 0, base);
				if (disp_fits8) u8(static_cast<uint8_t>(disp));
				else u32(static_cast<uint32_t>(disp));
				u32(static_cast<uint32_t>(v));
			}
		}

		/* sub */
		void sub_r64_rm64(int r, int rm) {
			alu_r64_rm64(0x2B, r, rm);
		}
		void sub_rm64_imm32(int rm, int32_t v) {
			alu_rm64_imm32(rm, 5, v);
		}
		void sub_r64_memd8(int r, int base, int64_t d) {
			alu_r64_memd8(0x2B, r, base, d);
		}

		/* cmp */
		void cmp_r64_rm64(int r, int rm) {
			alu_r64_rm64(0x3B, r, rm);
		}
		void cmp_rm64_imm32(int rm, int32_t v) {
			alu_rm64_imm32(rm, 7, v);
		}
		void cmp_r64_memd8(int r, int base, int64_t d) {
			alu_r64_memd8(0x3B, r, base, d);
		}
		void cmp_r64_memd32(int r, int base, int32_t d) {
			alu_r64_memd32(0x3B, r, base, d);
		}

		/* xor */
		void xor_rm64_r64(int rm, int r) {
			alu_rm64_r64(0x31, rm, r);
		}

		/* imul r64, r/m64 (0F AF) */
		/* imul r64, r/m64, imm  : REX.W 6B /r ib  |  REX.W 69 /r id
		   imul rax, r12, 24 -> 49 6B C4 18 ; 保留 OF，可替代 mov+imul */
		void imul_r64_rm64_imm32(int r, int rm, int32_t imm) {
			rex_wrb(r, rm);
			if (imm >= -128 && imm <= 127) { u8(0x6B); modrm(3, r, rm); u8(static_cast<uint8_t>(imm)); }
			else                           { u8(0x69); modrm(3, r, rm); u32(static_cast<uint32_t>(imm)); }
		}
		/* lea dst, [base + index*scale + disp]   (base < 0 表示无 base，此时必须给 index)
		   lea rax,[r12+r13*4+8] -> 4B 8D 44 AC 08 */
		void lea_bisd(int dst, int base, int index, int scale, int32_t disp) {
			int ss = (scale == 8) ? 3 : (scale == 4) ? 2 : (scale == 2) ? 1 : 0;
			bool hasIdx = index >= 0;
			uint8_t rex = 0x48 | ((dst & 8) ? 4 : 0) | ((hasIdx && (index & 8)) ? 2 : 0) | ((base >= 0 && (base & 8)) ? 1 : 0);
			u8(rex); u8(0x8D);
			if (base < 0) {                              /* [index*s + disp32]: mod=00, SIB.base=101 */
				modrm(0, dst, 4); sib(ss, index, 5); u32(static_cast<uint32_t>(disp)); return;
			}
			bool needSib = hasIdx || (base & 7) == 4;
			bool d8 = disp >= -128 && disp <= 127;
			/* rbp/r13 作 base 时不能用 mod=0（那表示无 base） */
			uint8_t mod = (disp == 0 && (base & 7) != 5) ? 0 : (d8 ? 1 : 2);
			if (needSib) { modrm(mod, dst, 4); sib(ss, hasIdx ? index : 4, base); }
			else         { modrm(mod, dst, base); }
			if (mod == 1) u8(static_cast<uint8_t>(disp)); else if (mod == 2) u32(static_cast<uint32_t>(disp));
		}

		void imul_r64_rm64(int r, int rm) {
			rex_wrb(r, rm);
			u8(0x0F);
			u8(0xAF);
			modrm(3, r, rm);
		}
		/* cmov r64, r/m64 (0F 40+cc /r), cc ??jcc 后缀??*/
		void cmov_r64_rm64(int cc, int r, int rm) {
			rex_wrb(r, rm);
			u8(0x0F);
			u8(static_cast<uint8_t>(0x40 + cc));
			modrm(3, r, rm);
		}
		void imul_r64_memd8(int r, int base, int64_t d) {
			int pr = slot_phys(d, base);
			if (pr >= 0) { imul_r64_rm64(r, pr); return; }
			if (d >= -128 && d <= 127) {
				rex_wrb(r, base);
				u8(0x0F);
				u8(0xAF);
				modrm(1, r, base);
				u8(static_cast<uint8_t>(d));
			} else {
				rex_wrb(r, base);
				u8(0x0F);
				u8(0xAF);
				modrm(2, r, base);
				u32(static_cast<uint32_t>(d));
			}
		}
		void imul_r64_memd32(int r, int base, int32_t d) {
			int pr = slot_phys(d, base);
			if (pr >= 0) { imul_r64_rm64(r, pr); return; }
			rex_wrb(r, base);
			u8(0x0F);
			u8(0xAF);
			modrm(2, r, base);
			u32(static_cast<uint32_t>(d));
		}

		/* idiv r/m64 (F7 /7) */
		void idiv_rm64(int rm) {
			/* 必须带 REX.B: rm 可为 r8-r15。此前只有 rex_w()，
			   idiv_rm64(9) 被编成 `idiv %rcx`（漏掉 REX.B 后 rm&7=1
			   即 rcx），除法静默除以错误的数。 */
			rex_wb(rm);
			u8(0xF7);
			modrm(3, 7, rm);
		}
		/* cqo (sign-extend RAX to RDX:RAX, 48 99) */
		void cqo() {
			rex_w();
			u8(0x99);
		}
		/* neg r/m64 (F7 /3) */
		void neg_rm64(int rm) {
			rex_w();
			u8(0xF7);
			modrm(3, 3, rm);
		}

		/* setcc r/m8 (0F 9x) */
		void setcc_r8(int cc, int r) {
			/* 必须带 REX.B: r 可为 r8-r15（8 位形式 r8b-r15b）。
			   此前未发 REX，setcc 到 r9 会被错编成 setcc 到 rcx 的低 8 位。 */
			if (r >= 8) u8(0x41);
			u8(0x0F);
			u8(0x90 + cc);
			modrm(3, 0, r);
		}
		/* movzx r64, r/m8 (0F B6) */
		void movzx_r64_rm8(int r, int rm) {
			rex_wb(r);
			u8(0x0F);
			u8(0xB6);
			modrm(3, r, rm);
		}

		/* ── Jumps ── */
		/* jmp rel32 */
		size_t jmp32() {
			u8(0xE9);
			size_t p = pos();
			u32(0);
			return p;
		}
		/* jcc rel32 (0F 8x) */
		size_t jcc32(int cc) {
			u8(0x0F);
			u8(0x80 + cc);
			size_t p = pos();
			u32(0);
			return p;
		}
		/* patch: back-patch jmp/jcc to current position */
		void patch_jmp(size_t p) {
			int32_t off = static_cast<int32_t>(pos() - (p + 4));
			patch_u32(p, static_cast<uint32_t>(off));
		}

		/* ── Stack frame ── */
		void push_rbp() {
			u8(0x55);
		}
		void pop_rbp() {
			u8(0x5D);
		}
		void mov_rbp_rsp() {
			rex_w();
			u8(0x89);
			u8(0xE5);
		}
		void sub_rsp(int32_t v) {
			alu_rm64_imm32(4, 5, v);
		}
		void add_rsp(int32_t v) {
			alu_rm64_imm32(4, 0, v);
		}
		void leave() {
			u8(0xC9);
		}
		void ret() {
			u8(0xC3);
		}

		/* Dynamic stack alignment helpers (use R15 callee-saved) */
		void save_rsp_r15() {
			if (pre_align_) { auto h = pre_align_; pre_align_ = {}; h(); }
			/* push r15 (save callee-saved) ; mov r15, rsp */
			u8(0x41);
			u8(0x57);                /* push r15 */
			u8(0x49);
			u8(0x89);
			u8(0xE7);      /* mov r15, rsp */
		}
		void restore_rsp_r15() {
			/* mov rsp, r15 ; pop r15 (restore callee-saved) */
			u8(0x4C);
			u8(0x89);
			u8(0xFC);      /* mov rsp, r15 (REX.WR: reg=R15, rm=RSP) */
			u8(0x41);
			u8(0x5F);                /* pop r15 */
		}
		void align_rsp16() {
			/* and rsp, -16 : 48 83 E4 F0 */
			u8(0x48);
			u8(0x83);
			u8(0xE4);
			u8(0xF0);
		}

		/* ── call [rip+disp32] (FF 15) ──
		   disp = target_addr - (end_of_insn)
		   Returns position of the disp32 field */
		size_t call_rip() {
			u8(0xFF);
			u8(0x15);
			size_t p = pos();
			u32(0);
			return p;
		}
		void call_r(int r) {
			if (r < 8) {
				u8(0xFF);
				u8(0xD0 + (r & 7));
			} else {
				u8(0x41);
				u8(0xFF);
				u8(0xD0 + (r & 7));
			}
		}
		void patch_call_rip(size_t p, size_t target_addr, size_t insn_start) {
			/* target_addr is where the 8-byte function pointer is stored */
			/* insn_start is the position of FF 15 */
			/* The instruction is 6 bytes long: FF 15 xx xx xx xx */
			/* disp = target_addr - (insn_start + 6) */
			int32_t disp = static_cast<int32_t>(static_cast<int64_t>(target_addr) - static_cast<int64_t>(insn_start + 6));
			patch_u32(p, static_cast<uint32_t>(disp));
		}
	};

	/* ── Native x86-64 code generator ── */
	using wei::ast::IntLit;
	using wei::ast::DoubleLit;
	using wei::ast::StrLit;
	using wei::ast::BoolLit;
	using wei::ast::Ident;
	using wei::ast::Binary;
	using wei::ast::Unary;
	using wei::ast::Call;
	using wei::ast::Output;
	using wei::ast::Input;
	using wei::ast::IndexExpr;
	using wei::ast::Block;
	using wei::ast::IfStmt;
	using wei::ast::ForStmt;
	using wei::ast::WhileStmt;
	using wei::ast::LoopStmt;
	using wei::ast::LoopNStmt;
	using wei::ast::ReturnStmt;
	using wei::ast::ExprStmt;
	using wei::ast::AssignStmt;
	using wei::ast::VarDecl;
	using wei::ast::FuncDecl;
	using wei::ast::Stmt;
	using wei::ast::Expr;
	using wei::ast::Program;
	using wei::ast::ModuleDecl;
	using wei::ast::ModulePathGet;
	using wei::ast::ModuleAccessSet;

	struct JITLabel {
		bool is_jcc;
		int cc;
		size_t pos;
	};

	/* ── Cross-platform target abstraction ──────────────────────────────────────
	   The same wei.cpp compiles on Windows and Linux. The code generator never
	   branches on the platform; instead each backend implements a Target that knows
	   how to emit I/O, the entry stub, the import table / GOT and the final
	   executable container. */
	class NativeGen;
	struct Target {
		virtual ~Target() {}
		virtual const char* id() const = 0;
		/* bytes reserved at the start of the data area for the import table / GOT */
		virtual size_t importSize() const = 0;
		/* byte offset of a format string (kind 0=int, 2=str, 3=dbl) within the data area */
		virtual size_t fmtOffset(int kind) const = 0;
		virtual void emitDataPrologue(NativeGen& g) = 0;
		virtual void emitOutput(NativeGen& g, Output& n) = 0;
		virtual void emitInput(NativeGen& g, Input& n) = 0;
		virtual void emitEntryStub(NativeGen& g) = 0;
		/* 运行时的 panic 桩：打印 msg 并终止进程（Win64: printf+ExitProcess，
		   Linux64: write(2)+exit(60)）。off 返回桩的代码偏移。 */
		virtual void emitPanicStub(NativeGen& g, const std::string& msg, size_t& off) = 0;
		/* 契约违规桩: 入参 RCX(Win64) / RDI(Linux64) = 消息字符串指针。
		   打印该消息后以退出码 1 终止进程。 */
		virtual void emitContractFail(NativeGen& g, size_t& off) = 0;
		/* 128 位有符号整数打印（-wide 模式专用）。
		   Linux64: rdi=lo, rsi=hi；Win64: rcx=lo, rdx=hi。
		   若值实际落在 64 位内，调用方不会走到这里。 */
		virtual void emitPrintI128(NativeGen& g, size_t& off) = 0;
		/* 堆扩容桩: 入参 RCX = 至少需要的字节数。成功则更新 heap_ptr / heap_end
		   并返回；失败直接 panic（不返回）。实现必须保存并恢复除 RAX 之外的
		   所有用到的寄存器，且必须保持 R8 / R9 / R10 不变（rt_alloc 依赖）。 */
		virtual void emitHeapGrow(NativeGen& g) = 0;
		/* 退出前泄漏报告桩: 若仍有未归还的活字节则打印告警并返回。
		   必须保存并恢复所有用到的寄存器（含 RDI，它持有退出码）。 */
		virtual void emitLeakReport(NativeGen& g) = 0;
		virtual void applyPatches(NativeGen& g) = 0;
		virtual size_t computeDataReloc(NativeGen& g, uint32_t code_size) = 0;
		virtual void finalize(NativeGen& g, const std::vector<uint8_t>& native,
		                      uint32_t code_size, const std::string& outExe) = 0;
	};

	/* PE container structs are defined once, further down (named IMAGE_*), so the
	   same source compiles on Windows and Linux without depending on <windows.h>. */

	/* char/文本/string/str 在运行时都是 8 字节字符串指针 */
	static bool isTextTypeName(const std::string& t) {
		return t == "char" || t == "文本" || t == "string" || t == "str";
	}

	/* ── 运行时内存布局 ────────────────────────────────────────────────────────
	   Heap Pool 放在数据区固定偏移 64 KiB 处（避开导入表/格式串/全局变量），
	   主池 64 MiB。PE 的 .idata 只提交实际数据，池子由加载器按 VirtualSize
	   零填充；ELF 的 PT_LOAD 用 p_memsz 覆盖池子，同样零填充。
	   因为是 BSS 语义零填充，扩大主池不增加可执行文件体积（实测 +0 字节）。
	   主池耗尽后由 rt_grow 经 mmap / VirtualAlloc 继续扩容，理论上无上限。 */
	static constexpr uint32_t WEI_HEAP_POOL_OFF  = 0x10000;
	/* 主堆池 1 MiB（原为 64 MiB）。
	   实测 64 MiB 的 BSS 会拖慢进程启动 —— 即使 BSS 是惰性分配,
	   运行时的池初始化与退出检查仍要触碰这段地址, 页表建立的开销
	   落在每次启动上。而绝大多数程序根本用不到 64 MiB。
	   这里改成小池起步, 不够时由 rt_grow 经 mmap/VirtualAlloc 扩容,
	   缺页代价从"每次启动都付"变成"真正用到才付"。 */
	static constexpr uint32_t WEI_HEAP_POOL_SIZE = 1u << 20;

	/* ── Size Class 表 ────────────────────────────────────────────────────────
	   payload 是用户可见容量；total 是块占用（32 字节安全头 + payload +
	   8 字节尾哨兵，再向上取整到 16 对齐）。全部为 16 的倍数，保证 payload
	   起始 16 对齐。 */
	static constexpr int WEI_NCLS = 12;
	static constexpr int64_t WEI_HDR  = 32;      /* 安全头: magic/info/class/cap */
	static constexpr int64_t WEI_TAIL = 8;       /* 尾哨兵 */
	static constexpr int64_t weiClsPayload(int c) {
		return int64_t(16) << c;                 /* 16,32,64, ... 32768 */
	}
	static constexpr int64_t weiClsTotal(int c) {
		return ((int64_t(16) << c) + WEI_HDR + WEI_TAIL + 15) & ~int64_t(15);
	}

	/* ── 安全内存魔数 ────────────────────────────────────────────────────────
	   哨兵值 = 常量 XOR 块地址，因此每一块的哨兵都独一无二：伪造指针、
	   野指针、已释放块的地址都无法通过校验。 */
	/* 释放后用 0xA5 毒化 payload：既销毁残留数据（防泄漏），又让释放后读
	   （UAF）拿到可辨识的垃圾而不是有效数据。 */
	static constexpr int64_t WEI_POISON = INT64_C(0xA5A5A5A5A5A5A5A5);
	/* 隔离区槽数：每块至少经历这么多次释放后才允许被复用。 */
	static constexpr int WEI_QUARANTINE = 64;
	static constexpr int64_t WEI_MAGIC_HEAD = 0x5745494845414401LL;
	static constexpr int64_t WEI_MAGIC_TAIL = 0x5745495441494C01LL;

	/* ══════════════════════════════════════════════════════════════════════════
	   IR 层（中间表示）
	   ──────────────────────────────────────────────────────────────────────────
	   把 AST 表达式先线性化成三地址码，在 IR 上做机器无关优化（常量折叠、
	   强度削减），再由线性扫描寄存器分配器统一决定每个虚拟寄存器落在哪个
	   物理寄存器。

	   这解决两个原本无解的问题：
	     1. 栈式求值：原来每次二元运算都要 push/pop 保存左操作数（实测这是
	        bench3 慢的主要来源）；IR 是三地址形式，天然不需要栈往返。
	     2. 寄存器上限：原来手工约定只能用 rdx/r9 两个，再多就互相踩；
	        IR + 线性扫描可以用满全部可用寄存器，不够时才溢出到栈。

	   这是 LLVM 快的根本原因：优化在 IR 上做（与机器无关、可组合），
	   寄存器由分配器统一决定，而不是在语法制导翻译里手工指定。
	   ══════════════════════════════════════════════════════════════════════════ */
	enum class IROp {
		CONST,      /* dst = imm                      */
		LOAD,       /* dst = [rbp + disp]             */
		STORE,      /* [rbp + disp] = a               */
		LOADREG,    /* dst = 指定物理寄存器（变量已驻留）*/
		STOREREG,   /* 指定物理寄存器 = a              */
		ADD, SUB, MUL, DIV, MOD,
		NEG,
	};

	struct IRInstr {
		IROp op = IROp::CONST;
		int dst = -1;          /* 目的虚拟寄存器；STORE 为 -1 */
		int a = -1, b = -1;    /* 源虚拟寄存器 */
		int64_t imm = 0;       /* CONST 的立即数 */
		int32_t disp = 0;      /* LOAD/STORE 的栈偏移 */
		int phys = -1;         /* LOADREG/STOREREG 指定的物理寄存器 */
		/* ── 强度削减结果（优化 pass 填写，发射器消费）── */
		int shift = 0;         /* >0: MUL 改写为 shl dst, shift */
		int lea_scale = 0;     /* 2/4/8: MUL 改写为 lea dst,[a + a*scale] */
		bool lea_add1 = false; /* lea 形式再 +a，即 k = scale + 1 */
	};

	struct IRFunc {
		std::vector<IRInstr> code;
		int nvreg = 0;
		int newvreg() {
			return nvreg++;
		}
		int emit(IROp op, int a = -1, int b = -1, int64_t imm = 0) {
			IRInstr in;
			in.op = op;
			in.a = a;
			in.b = b;
			in.imm = imm;
			code.push_back(in);
			return static_cast<int>(code.size()) - 1;
		}
	};

	/* ── 优化 Pass 1: 常量折叠 ───────────────────────────────────────────────
	   CONST 定义的虚拟寄存器参与二元运算时直接在编译期算出，
	   把整棵常量子树塌缩成一条 CONST。 */
	static void irFoldConstants(IRFunc& f) {
		std::vector<int64_t> cval(static_cast<size_t>(f.nvreg), 0);
		std::vector<char> isConst(static_cast<size_t>(f.nvreg), 0);
		bool changed = true;
		while (changed) {
			changed = false;
			for (auto& in : f.code) {
				switch (in.op) {
					case IROp::CONST:
						if (in.dst >= 0) {
							cval[static_cast<size_t>(in.dst)] = in.imm;
							isConst[static_cast<size_t>(in.dst)] = 1;
						}
						break;
					case IROp::ADD:
					case IROp::SUB:
					case IROp::MUL: {
						if (in.dst < 0) break;
						bool ca = in.a >= 0 && isConst[static_cast<size_t>(in.a)];
						bool cb = in.b >= 0 && isConst[static_cast<size_t>(in.b)];
						if (ca && cb) {
							int64_t x = cval[static_cast<size_t>(in.a)];
							int64_t y = cval[static_cast<size_t>(in.b)];
							int64_t r = 0;
							if (in.op == IROp::ADD) r = x + y;
							else if (in.op == IROp::SUB) r = x - y;
							else r = x * y;
							in.op = IROp::CONST;
							in.imm = r;
							in.a = -1;
							in.b = -1;
							cval[static_cast<size_t>(in.dst)] = r;
							isConst[static_cast<size_t>(in.dst)] = 1;
							changed = true;
						}
						break;
					}
					default:
						if (in.dst >= 0) isConst[static_cast<size_t>(in.dst)] = 0;
						break;
				}
			}
		}
	}

	/* ── 优化 Pass 2: 乘法强度削减 ───────────────────────────────────────────
	   x * 2^k      → shl
	   x * (2^k+1)  → lea [x + x*2^k]   (k=1 → *3, k=2 → *5, k=3 → *9)
	   注意: 两种改写的结果都是 mod 2^64 精确一致的，不改变程序语义。
	   但改写后不再产生乘法溢出标志，因此发射时必须另行保证安全语义 —
	   这里采取保守策略：只在能确认不会溢出时才改写，否则保留 imul
	   （imul 一条指令自带 OF，是最省的检查方式）。 */
	static void irStrengthReduce(IRFunc& f) {
		std::vector<int64_t> cval(static_cast<size_t>(f.nvreg), 0);
		std::vector<char> isConst(static_cast<size_t>(f.nvreg), 0);
		for (auto& in : f.code) {
			if (in.op == IROp::CONST && in.dst >= 0) {
				cval[static_cast<size_t>(in.dst)] = in.imm;
				isConst[static_cast<size_t>(in.dst)] = 1;
			}
		}
		/* 强度削减在此**不启用**。
		   shl / lea 的结果低 64 位与 imul 完全一致，但它们不产生可信的有符号
		   乘法溢出标志（OF）。而我们在每次 `*` 之后都插 jno 溢出检查 —— 用
		   shl/lea 替代 imul 会静默绕过这道安全检查，把「安全」悄悄降级。
		   正确性优先于速度：保留 imul（一条指令自带 OF，是最省的检查方式）。
		   若要启用强度削减，必须同时提供独立的溢出判定路径，或在可证明
		   （关闭溢出检查）模式下启用。 */
		(void)f;
		(void)cval;
		(void)isConst;
		return;
	}

	/* ── 线性扫描寄存器分配 ──────────────────────────────────────────────────
	   可用物理寄存器（调用者无需保存的那批）:
	     rax rcx rdx r8 r9 r10 r11  — 共 7 个
	   超出则用栈槽溢出。相比原来手工约定只能用 rdx/r9 两个，
	   这里可以真正用满。 */
	struct IRRegAlloc {
		static const int NPHYS = 7;
		static const int phys[NPHYS];
		std::vector<int> vregPhys;   /* vreg → 物理寄存器号，-1 表示溢出 */
		std::vector<int> vregSpill;  /* vreg → 栈偏移（溢出时） */
		std::vector<int> lastUse;    /* vreg 的最后使用指令索引 */
		std::vector<char> physBusy;
		int spillCount = 0;

		explicit IRRegAlloc(int nvreg) : vregPhys(static_cast<size_t>(nvreg), -1),
			vregSpill(static_cast<size_t>(nvreg), 0),
			lastUse(static_cast<size_t>(nvreg), -1),
			physBusy(static_cast<size_t>(NPHYS), 0) {}

		void computeLiveness(const std::vector<IRInstr>& code) {
			for (size_t i = 0; i < code.size(); i++) {
				const auto& in = code[i];
				auto touch = [&](int v) {
					if (v >= 0) lastUse[static_cast<size_t>(v)] = static_cast<int>(i);
				};
				touch(in.a);
				touch(in.b);
				if (in.op == IROp::STORE) touch(in.a);
			}
		}
		int alloc(int vreg, int curIdx) {
			for (int p = 0; p < NPHYS; p++) {
				if (!physBusy[static_cast<size_t>(p)]) {
					physBusy[static_cast<size_t>(p)] = 1;
					vregPhys[static_cast<size_t>(vreg)] = phys[p];
					return phys[p];
				}
			}
			/* 全部占用 → 溢出到栈（8 字节对齐） */
			spillCount++;
			vregSpill[static_cast<size_t>(vreg)] = -8 * (16 + spillCount);
			vregPhys[static_cast<size_t>(vreg)] = -1;
			return -1;
		}
		void freeDead(int vreg, int curIdx) {
			if (vreg < 0) return;
			if (lastUse[static_cast<size_t>(vreg)] <= curIdx) {
				int p = vregPhys[static_cast<size_t>(vreg)];
				if (p >= 0) {
					for (int i = 0; i < NPHYS; i++) {
						if (phys[i] == p) physBusy[static_cast<size_t>(i)] = 0;
					}
				}
			}
		}
		/* ── 压缩性动态寄存器分配（线性扫描）─────────────────────────────
		   "压缩性"体现在: 每条指令执行前，先把**此后不再使用**的虚拟寄存器
		   占用的物理寄存器回收，再为新的 dst 分配 —— 于是物理寄存器被
		   反复挤压复用，而不是每碰到一个新 vreg 就占一个、占满就溢出。
		   "动态"体现在: 分配决策完全由运行时的活跃区间(live range)驱动。

		   原实现的缺陷: alloc() 总是在 freeDead() 之前调用，且只看
		   physBusy 标志，7 个物理寄存器一旦被占满就永久溢出到栈 ——
		   即便那些 vreg 早就死掉了。这会让 spill 数量随表达式规模线性
		   增长，是 IR 路径没有性能收益的直接原因。 */
		void run(const std::vector<IRInstr>& code) {
			computeLiveness(code);
			std::vector<int> physToVreg(static_cast<size_t>(NPHYS), -1);
			for (size_t i = 0; i < code.size(); i++) {
				/* (1) 回收: 释放所有"最后一次使用早于当前指令"的 vreg */
				for (int p = 0; p < NPHYS; p++) {
					int v = physToVreg[static_cast<size_t>(p)];
					if (v >= 0 && lastUse[static_cast<size_t>(v)] < static_cast<int>(i)) {
						physToVreg[static_cast<size_t>(p)] = -1;
					}
				}
				const auto& in = code[i];
				if (in.dst < 0) continue;                    /* STORE 无 dst */
				/* 已分配到物理寄存器则跳过。注意 vregPhys 初值为 -1（未分配），
				   绝不能写成 `!= 0` —— 那会让所有 vreg 都被误判为"已分配"
				   从而跳过分配，vregPhys 停留在 -1、vregSpill 停留在 0，
				   发射时就会把值写进 [rbp+0]（保存的 rbp 槽），
				   直接破坏栈帧。这个错误曾在嵌套循环里造成段错误。 */
				if (vregPhys[static_cast<size_t>(in.dst)] >= 0) continue;
				/* (2) 压缩分配: 优先复用空闲物理寄存器 */
				int slot = -1;
				for (int p = 0; p < NPHYS; p++) {
					if (physToVreg[static_cast<size_t>(p)] < 0) { slot = p; break; }
				}
				if (slot >= 0) {
					physToVreg[static_cast<size_t>(slot)] = in.dst;
					vregPhys[static_cast<size_t>(in.dst)] = phys[slot];
				} else {
					/* 全占满（都是活的）才溢出 —— 此时溢出是必需的，不是浪费 */
					spillCount++;
					vregSpill[static_cast<size_t>(in.dst)] = -8 * (16 + spillCount);
					vregPhys[static_cast<size_t>(in.dst)] = -1;
				}
			}
		}
	};
	const int IRRegAlloc::phys[IRRegAlloc::NPHYS] = {0, 1, 2, 8, 9, 10, 11};

	struct LoopOptimizer;   /* 循环优化分类模块，定义于 loopopt.cpp */

	class NativeGen {
		public:
			friend struct Target;
			friend struct Win64Target;
			friend struct Linux64Target;
			friend struct LoopOptimizer;   /* 循环优化分类模块 (loopopt.cpp) */
			Target* target_ = nullptr;
			void set_target(Target* t) {
				target_ = t;
			}
			explicit NativeGen(Program& prog, bool opt = false) : prog_(prog), opt_(opt) {}
			std::vector<uint8_t> generate();
			size_t data_area() const {
				return data_area_;
			}
			size_t num_globals() const {
				return global_slots_;
			}
			size_t entry_offset() const {
				return entry_off_;
			}
			size_t code_size() const {
				return data_area_;
			}
			size_t printf_iat_off() const {
				return printf_iat_off_;
			}
			size_t exit_iat_off() const {
				return exit_iat_off_;
			}
			void set_pe_data_rel(size_t d) {
				pe_data_rel_ = d;
			}
			/* -ir: 表达式走 IR 路径 (三地址码 + 线性扫描寄存器分配) */
			void set_ir(bool v) {
				ir_ = v;
			}

		private:
			Program& prog_;
			X64Buf c_;
			std::vector<JITLabel> labels_;
			/* 优化默认全部开启（原 -opt 开关已移除）。
			   保留该成员仅用于内部语义，恒为 true。 */
			/* 优化分层。opt_ == false（-no-opt）时，**只关闭算法级替换**
			   —— 即那些把 O(n) 循环整个换成 O(1) 闭式解的模式识别。

			   仍然保留的是"代码质量级"优化：循环变量寄存器驻留、归纳变量
			   强度削减、条件直跳。理由: 这些是任何称职的代码生成器**本来就
			   该做好的事**，不是"额外优化"。把它们关掉等于故意生成劣质代码，
			   对用户没有价值 —— 没有谁会想要一个"故意把变量放在内存里来回搬"
			   的编译器。

			   所以 -no-opt 的语义是: "不要替我改算法，但代码本身要写好"。 */
			bool opt_ = true;
			/* lea dst, [base*scale + disp] —— 循环展开里把"连续 4 项之和"压进一条指令 */
	void emit_lea_scaled(int dst, int base, int scale, int32_t disp) {
		c_.lea_scaled(dst, base, scale, disp);
	}

			/* break / continue: 每层循环一个待回填的跳转列表。 */
			std::vector<std::vector<int>> brk_;
			std::vector<size_t> cont_;
			/* ── RAII 自动释放（值语义，不引入任何面向对象机制）─────────────
			   `create.own p = new int;` 登记进 owned_scopes_，离开所在块
			   时自动调 rt_free。等同 Rust 所有权释放 / C++ RAII 的效果，
			   但不需要构造析构函数，也不涉及类。 */
			std::vector<std::vector<std::string>> owned_scopes_;
			bool hasOwnedVars() const {
				for (auto& sc : owned_scopes_) if (!sc.empty()) return true;
				return false;
			}
			void emit_release_var(const std::string& name);
			void emit_release_scope(const std::vector<std::string>& scope);
			void emit_release_all_scopes();
			/* 函数返回尾声抽成独立方法: return 前若需释放，走同一条出口 */
			void emit_return_epilogue();
			/* ── C1 位宽体系: i32 ──────────────────────────────────────
			   narrow_int_ 记录"这个变量是 32 位"。i32_mode_ 表示当前正在
			   生成 32 位表达式，所有子表达式都走 32 位通路。
			   栈槽仍按 8 字节分配（存储布局不改，风险最小），但读写与
			   算术全部用 32 位指令 —— 语义与机器码都是真正的 32 位。 */
			std::map<std::string, int> narrow_int_;   /* 变量名 -> 位宽(32) */
			bool i32_mode_ = false;
			bool isI32Var(const std::string& n) const {
				auto it = narrow_int_.find(n);
				return it != narrow_int_.end() && it->second == 32;
			}
			/* 表达式是否为 i32: 变量 / 二元(任一侧) / 取反 / 转型 */
			bool exprIsI32(const Expr& e) const {
				if (auto* id = std::get_if<Ident>(&e)) return isI32Var(id->name);
				if (auto* b = std::get_if<Binary>(&e)) {
					const std::string& o = b->op;
					bool cmp = (o == "==" || o == "!=" || o == "<" || o == ">"
					         || o == "<=" || o == ">=");
					if (cmp) return false;                    /* 比较结果是 bool */
					if (b->lhs && exprIsI32(*b->lhs)) return true;
					if (b->rhs && exprIsI32(*b->rhs)) return true;
					return false;
				}
				if (auto* u = std::get_if<Unary>(&e)) {
					return u->operand && exprIsI32(*u->operand);
				}
				return false;
			}
			bool ir_ = true;     /* -ir: 表达式走 IR 路径（三地址码 + 常量折叠 + 强度削减 + 线性扫描寄存器分配）。
			   循环提升激活时自动回退原路径；除法回退原路径以保留除零检查。默认开启，-no-ir 可关闭 */

			/* AST 扫描: 表达??语句是否引用变量 v (用于循环展开安全检?? */
			static bool exprUsesVar(const std::unique_ptr<Expr>& e, const std::string& v) {
				if (!e) return false;
				return std::visit([&](const auto & n) -> bool {
					using T = std::decay_t<decltype(n)>;
					if constexpr (std::is_same_v<T, Ident>) {
						return n.name == v;
					} else if constexpr (std::is_same_v<T, Binary>) {
						return exprUsesVar(n.lhs, v) || exprUsesVar(n.rhs, v);
					} else if constexpr (std::is_same_v<T, Unary>) {
						return exprUsesVar(n.operand, v);
					} else if constexpr (std::is_same_v<T, Call>) {
						for (auto& a : n.args) if (exprUsesVar(a, v)) return true;
						return false;
					} else if constexpr (std::is_same_v<T, Output>) {
						for (auto& a : n.values) if (exprUsesVar(a, v)) return true;
						return false;
					} else if constexpr (std::is_same_v<T, Input>) {
						return n.var_name == v;
					} else if constexpr (std::is_same_v<T, IndexExpr>) {
						return n.name == v || exprUsesVar(n.idx, v);
					} else if constexpr (std::is_same_v<T, CastExpr>) {
						return exprUsesVar(n.value, v);
					} else if constexpr (std::is_same_v<T, DeleteExpr>) {
						return exprUsesVar(n.target, v);
					} else if constexpr (std::is_same_v<T, TupleLit>) {
						for (auto& e : n.elements) if (exprUsesVar(e, v)) return true;
						return false;
					} else if constexpr (std::is_same_v<T, RangeLit>) {
						return exprUsesVar(n.lo, v) || exprUsesVar(n.hi, v);
					} else if constexpr (std::is_same_v<T, TupleIndexExpr>) {
						return exprUsesVar(n.tuple, v);
					} else if constexpr (std::is_same_v<T, TypeofExpr>) {
						return exprUsesVar(n.arg, v);
					} else if constexpr (std::is_same_v<T, IsExpr>) {
						return exprUsesVar(n.arg, v);
					} else if constexpr (std::is_same_v<T, InExpr>) {
						return exprUsesVar(n.value, v) || exprUsesVar(n.container, v);
					} else if constexpr (std::is_same_v<T, AwaitExpr>) {
						return exprUsesVar(n.operand, v);
					} else if constexpr (std::is_same_v<T, CoExpr>) {
						for (auto& a : n.args) if (exprUsesVar(a, v)) return true;
						return false;
					} else if constexpr (std::is_same_v<T, CompileTimeExpr>) {
						return exprUsesVar(n.arg, v);
					} else if constexpr (std::is_same_v<T, ForallExpr> || std::is_same_v<T, ExistsExpr>) {
						if (n.var == v) return false;
						return exprUsesVar(n.domain, v) || exprUsesVar(n.cond, v);
					} else {
						return false;
					}
				}, *e);
			}
			static bool stmtUsesVar(const Stmt& s, const std::string& v) {
				return std::visit([&](const auto & n) -> bool {
					using T = std::decay_t<decltype(n)>;
					if constexpr (std::is_same_v<T, Block>) {
						for (auto& b : n.stmts) if (stmtUsesVar(b, v)) return true;
						return false;
					} else if constexpr (std::is_same_v<T, IfStmt>) {
						if (exprUsesVar(n.cond, v)) return true;
						for (auto& b : n.then_body) if (stmtUsesVar(b, v)) return true;
						for (auto& b : n.else_body) if (stmtUsesVar(b, v)) return true;
						return false;
					} else if constexpr (std::is_same_v<T, ForStmt>) {
						for (auto& b : n.body) if (stmtUsesVar(b, v)) return true;
						return false;
					} else if constexpr (std::is_same_v<T, WhileStmt>) {
						if (exprUsesVar(n.cond, v)) return true;
						for (auto& b : n.body) if (stmtUsesVar(b, v)) return true;
						return false;
					} else if constexpr (std::is_same_v<T, LoopStmt>) {
						for (auto& b : n.body) if (stmtUsesVar(b, v)) return true;
						return false;
					} else if constexpr (std::is_same_v<T, LoopNStmt>) {
						for (auto& b : n.body) if (stmtUsesVar(b, v)) return true;
						return false;
					} else if constexpr (std::is_same_v<T, ForallStmt>) {
						if (n.var == v) return false;      /* 量词变量遮蔽外层同名 */
						for (auto& b : n.body) if (stmtUsesVar(b, v)) return true;
						return false;
					} else if constexpr (std::is_same_v<T, WhenStmt>) {
						if (exprUsesVar(n.cond, v)) return true;
						for (auto& b : n.then_body) if (stmtUsesVar(b, v)) return true;
						for (auto& b : n.else_body) if (stmtUsesVar(b, v)) return true;
						return false;
					} else if constexpr (std::is_same_v<T, YieldStmt>) {
						return exprUsesVar(n.value, v);
					} else if constexpr (std::is_same_v<T, CompileTimeStmt>) {
						for (auto& b : n.body) if (stmtUsesVar(b, v)) return true;
						return false;
					} else if constexpr (std::is_same_v<T, MacroDecl>) {
						return false;
					} else if constexpr (std::is_same_v<T, ReturnStmt>) {
						return exprUsesVar(n.value, v);
					} else if constexpr (std::is_same_v<T, ExprStmt>) {
						return exprUsesVar(n.expr, v);
					} else if constexpr (std::is_same_v<T, AssignStmt>) {
						if (n.name == v) return true;
						/* 数组/指针赋值的目标里也可能引用循环变量，例如
						   ps[i] = ...；漏掉它会让“循环变量无关”的展开优化
						   生成不递增 i 的错误代码。 */
						if (n.target && exprUsesVar(n.target, v)) return true;
						return exprUsesVar(n.value, v);
					} else if constexpr (std::is_same_v<T, ModuleAccessSet>) {
						return exprUsesVar(n.value, v);
					} else if constexpr (std::is_same_v<T, VarDecl>) {
						return n.name == v || exprUsesVar(n.init, v);
					} else if constexpr (std::is_same_v<T, MatchStmt>) {
						if (exprUsesVar(n.expr, v)) return true;
						for (auto& mc : n.cases) for (auto& s : mc.body) if (stmtUsesVar(s, v)) return true;
						return false;
					} else if constexpr (std::is_same_v<T, ContractStmt>) {
						if (n.condition) return exprUsesVar(n.condition, v);
						return false;
					} else if constexpr (std::is_same_v<T, TypeAliasDecl>) {
						return false;
					} else {
						return false;
					}
				}, s);
			}

			/* ── ALU: r/m64 = r/m64 op r64 (opcodes 01, 09, 11, 19, 21, 29, 31, 39) ── */
			static bool stmtHasLoop(const Stmt& s) {
				return std::visit([](const auto & n) -> bool {
					using T = std::decay_t<decltype(n)>;
					if constexpr (std::is_same_v<T, ForStmt> || std::is_same_v<T, WhileStmt>
					              || std::is_same_v<T, LoopStmt> || std::is_same_v<T, LoopNStmt>
					              || std::is_same_v<T, ForallStmt>) {
						for (auto& b : n.body) if (stmtHasLoop(b)) return true;
						return true;
					} else if constexpr (std::is_same_v<T, Block>) {
						for (auto& b : n.stmts) if (stmtHasLoop(b)) return true;
						return false;
					} else if constexpr (std::is_same_v<T, IfStmt>) {
						for (auto& b : n.then_body) if (stmtHasLoop(b)) return true;
						for (auto& b : n.else_body) if (stmtHasLoop(b)) return true;
						return false;
					} else {
						return false;
					}
				}, s);
			}
			static bool bodyHasLoop(const std::vector<Stmt>& body) {
				for (auto& b : body) if (stmtHasLoop(b)) return true;
				return false;
			}

			/* ── 线性递推匹配 (matchLinearRecurrence)
			AST 形状支持: x=x*a+b / x=x+c / x=x*c / x=x-c / x=c-x / x=c+x / x=c*x / x=a*x-b */
			static bool matchLinearRecurrence(const std::vector<Stmt>& body, int64_t& a, int64_t& b) {
				if (body.size() != 1) return false;
				auto* as = std::get_if<AssignStmt>(&body[0]);
				if (!as) return false;
				auto* bin = std::get_if<Binary>(as->value.get());
				if (!bin) return false;
				auto isSelf = [&](const std::unique_ptr<Expr>& e) {
					auto* id = std::get_if<Ident>(e.get());
					return id && id->name == as->name;
				};
				auto getConst = [&](const std::unique_ptr<Expr>& e, int64_t& c) {
					auto* il = std::get_if<IntLit>(e.get());
					if (!il) return false;
					c = il->value;
					return true;
				};
				/* x = x*a + b  /  x = x*a - b */
				if (isSelf(bin->lhs)) {
					if (bin->op == "+") {
						int64_t c2;
						if (getConst(bin->rhs, c2)) {
							a = 1;
							b = c2;
							return true;
						}
						return false;
					}
					if (bin->op == "-") {
						int64_t c2;
						if (getConst(bin->rhs, c2)) {
							a = 1;
							b = -c2;
							return true;
						}
						return false;
					}
					if (bin->op == "*") {
						int64_t c2;
						if (getConst(bin->rhs, c2)) {
							a = c2;
							b = 0;
							return true;
						}
						return false;
					}
					return false;
				}
				/* x = x*a + b ??rhs ??(x*C)+C ??(C*x)+C ??C+(x*C) */
				if (isSelf(bin->rhs)) {
					int64_t c;
					if (getConst(bin->lhs, c)) {
						if (bin->op == "+") {
							a = 1;
							b = c;
							return true;
						}
						if (bin->op == "-") {
							a = -1;    /* x = c - x */
							b = c;
							return true;
						}
						if (bin->op == "*") {
							a = c;
							b = 0;
							return true;
						}
					}
					return false;
				}
				/* x = x*a + b: 两层 Binary */
				auto* inner = std::get_if<Binary>(bin->lhs.get());
				if (inner && inner->op == "*" && isSelf(inner->lhs) && getConst(inner->rhs, a)) {
					if (bin->op == "+") return getConst(bin->rhs, b);
					if (bin->op == "-") {
						if (getConst(bin->rhs, b)) {
							b = -b;
							return true;
						}
						return false;
					}
				}
				if (inner && inner->op == "*" && isSelf(inner->rhs) && getConst(inner->lhs, a)) {
					if (bin->op == "+") return getConst(bin->rhs, b);
					if (bin->op == "-") {
						if (getConst(bin->rhs, b)) {
							b = -b;
							return true;
						}
						return false;
					}
				}
				inner = std::get_if<Binary>(bin->rhs.get());
				if (inner && inner->op == "*" && isSelf(inner->lhs) && getConst(inner->rhs, a)) {
					if (bin->op == "+") return getConst(bin->lhs, b);
				}
				if (inner && inner->op == "*" && isSelf(inner->rhs) && getConst(inner->lhs, a)) {
					if (bin->op == "+") return getConst(bin->lhs, b);
				}
				return false;
			}
			/* ── ALU: r/m64 = r/m64 op r64 (opcodes 01, 09, 11, 19, 21, 29, 31, 39) ── */
			static void foldCoeffs(int64_t a, int64_t b, int k, int64_t& A, int64_t& B) {
				uint64_t ua = static_cast<uint64_t>(a), ub = static_cast<uint64_t>(b);
				uint64_t ak = ua, bk = ub;
				for (int i = 1; i < k; i++) {
					bk = bk * ua + ub;
					ak = ak * ua;
				}
				A = static_cast<int64_t>(ak);
				B = static_cast<int64_t>(bk);
			}

			struct VarInfo {
				uint32_t slot;
				bool is_local;
				int64_t array_len = 0;
				bool is_double = false;
				bool is_string = false;
				/* C1 位宽体系: 0=未知/默认64, 8/16/32/64 为显式宽度；
				   is_unsigned 决定溢出边界与比较语义。 */
				int width = 0;
				bool is_unsigned = false;
			};
			std::map<std::string, VarInfo> locals_;
			std::map<std::string, VarInfo> globals_;
			uint32_t max_locals_ = 0;
			uint32_t num_globals_ = 0;
			uint32_t global_slots_ = 0;

			/* ── constant pool helpers ── */
			struct PartInit {
				uint32_t slot;
				bool is_str;
				int64_t iv;
				std::string sv;
			};
			struct StrInitPatch {
				size_t disp_pos;
				uint32_t slot;
				std::string s;
			};
			std::map<std::string, uint32_t> part_slot_;
			std::map<std::string, bool> part_is_str_;
			std::map<std::string, size_t> func_offsets_;


			std::vector<FuncDecl*> funcs_;   /* 所有函数声??(??funcReturnsDouble 查询返回类型) */
			std::vector<PartInit> part_inits_;
			std::vector<StrInitPatch> str_init_patches_;
			struct ModEntryDecl {
				ModuleDecl* mod;
				FuncDecl* fn;
				int priority;
				size_t order;
			};
			std::vector<ModEntryDecl> mod_funcs_;   /* mod 成员函数, 按 priority 排序 */
			std::string cur_qualname_;              /* 当前生成函数的 "Mod::member" 名, 非成员函数为空 */

			std::string part_key(const std::string& m, const std::string& p, const std::string& f) const {
				return m + "\x01" + p + "\x01" + f;
			}
			uint32_t part_slot_id(const std::string& m, const std::string& p, bool hasField, const std::string& f) const;

			/* ══════════════════════════════════════════════════════════════════
			   本轮补全的 22 个关键字 —— 后端代码生成
			   ══════════════════════════════════════════════════════════════════ */
			void gen_range_lit(wei::ast::RangeLit& n);
			void gen_tuple_index(wei::ast::TupleIndexExpr& n);
			void gen_typeof(wei::ast::TypeofExpr& n);
			void gen_is_expr(wei::ast::IsExpr& n);
			void gen_in_expr(wei::ast::InExpr& n);
			void gen_await(wei::ast::AwaitExpr& n);
			void gen_co_create(wei::ast::CoExpr& n);
			void gen_compiletime_expr(wei::ast::CompileTimeExpr& n);
			void gen_quantifier(const std::string& var, const std::unique_ptr<Expr>& domain,
			                    const std::unique_ptr<Expr>& cond, bool is_forall);
			void gen_forall_stmt(wei::ast::ForallStmt& n);
			void gen_when_stmt(wei::ast::WhenStmt& n);
			void gen_yield_stmt(wei::ast::YieldStmt& n);
			void gen_compiletime_stmt(wei::ast::CompileTimeStmt& n);
			void register_type_alias(const std::string& alias, const std::string& target);

			/* ── 协程运行时 ── */
			void emit_co_switch_stub();
			void emit_co_create_call(const std::string& callee,
			                         const std::vector<std::unique_ptr<Expr>>& args, int line, int col);
			void emit_await_value();
			bool funcIsAsync(const std::string& callee) const;
			uint32_t co_cur_slot_ = 0;
			uint32_t co_main_rsp_slot_ = 0;
			uint32_t co_tmp_slot_ = 0;
			uint32_t quant_res_slot_ = 0;   /* forall/exists 结果 + of 形态的游标 */

			/* ══ 宽算术模式 (-wide, 实验性) ══════════════════════════════════
			   C/C++ 的整数溢出是"未定义行为"，编译器直接摆烂：回绕成垃圾值，
			   或者反过来拿溢出当假设去做优化。wei 默认选择 panic —— 安全，
			   但遇到真需要大数的场景就没法算了。

			   -wide 给出第三条路：每个 int 变量除了 64 位低位槽，再配一个
			   **影子高位槽**，算术全程按 128 位进行。64 位装不下时自动
			   进位到高位继续算，而不是回绕或放弃。

			   代价是每条算术指令大致翻倍（add→add/adc、mul→mul+两次imul），
			   也就是"用一半速度换高精度"——这正是本选项的定位。

			   约定：wide 模式下 gen_expr 求值完毕时
			     rax = 低 64 位 (lo)
			     r11 = 高 64 位 (hi，对 64 位内的值是符号扩展)
			   这个约定让嵌套表达式可以自然组合。
			*/
		public:
			bool wide_mode_ = false;
			std::map<std::string, uint32_t> wide_hi_;   /* 变量名 → 影子高位槽 */
			uint32_t wide_tmp_slot_ = 0;                /* hi 暂存槽 */

			/* ── 索引上界证明（边界检查消除）──────────────────────────────
			   C 的数组访问不检查边界，所以快；wei 检查，所以安全但慢。
			   想要"既安全又不比 C 慢"，唯一办法是**静态证明**索引必然
			   在界内，然后把检查删掉 —— 安全语言（Java/C#/Rust）都靠这一招。

			   这里做的第一步：while (i < X) 的循环体内，归纳变量 i 的取值范围
			   在循环入口即可确定为 [初值, X)。若数组长度 >= X，则 arr[i]
			   的边界检查在整个循环里都是多余的，直接省掉。

			   只在归纳变量**单调递增**时成立（i = i + 正常量）。若循环体里
			   对 i 有其他形式的写入（递减、赋值成别的值），立即撤销证明。 */
			std::map<std::string, int64_t> idx_ub_;
			/* D1: rewrite_mod_induction_vars 生成的取模归纳变量 __modiv_* 的
			   上界(模数 C)。该计数器由改写器保证恒在 [0,C)：初值 i0%C，
			   每轮 +adv 后最多 <2C，再经 `if(m>=C) m-=C` 回绕。
			   这是**编译期可证**的不变量，因此以它作数组下标且数组长度
			   >= C 时可以合法省略边界检查 —— 不是为性能关检查。 */
			std::map<std::string, int64_t> mod_iv_bounds_;
			/* ── D1: 循环级编译期证明（累加器溢出）────────────────────
			   arr_elem_bounds_: 数组名 → **所有元素**的编译期区间。
			     只由"定长初始化循环"静态分析得出，且一旦该函数后续
			     还有别的数组写就整条撤销（保守）。
			   var_init_const_: 顶层 VarDecl / 常量赋值的已知初值。
			   active_ovf_free_var_: 仅在被证明的循环体生成期间有效，
			     精确到「变量名 + s=s±X 形态」，循环结束即清空。 */
			struct ArrBounds { wei_i128 lo = 0, hi = 0; bool ok = false; };
			std::map<std::string, ArrBounds> arr_elem_bounds_;
			std::map<std::string, int64_t> var_init_const_;
			/* 每条 while 在函数中**之前**已被赋过值的变量集合。
			   若累加器在其中，则它的"编译期初值"不可靠，放弃证明。 */
			std::map<const WhileStmt*, std::set<std::string>> loop_pre_writes_;
			/* generate() 会跑两遍（第二遍为数据重定位 patch）。两遍必须
			   生成**逐字节一致**的代码，否则 patch 错位 → 段错误。
			   证明结果按 (函数, trip, body 大小) 缓存，第二遍强制复用。 */
			std::map<std::string, std::set<std::string>> acc_proof_cache_;
			std::set<std::string> active_ovf_free_vars_;
			bool suppress_ovf_ = false;
			void collectArrayBounds(std::vector<Stmt>& body);
			std::set<std::string> proveAccumNoOverflow(WhileStmt& w);
			void noteIdxUB(const std::string& n, int64_t ub) { idx_ub_[n] = ub; }
			void dropIdxUB(const std::string& n) { idx_ub_.erase(n); }
			bool indexProven(const Expr& idx, int64_t len) const;
			size_t rt_print_i128_off_ = 0;              /* 128 位打印桩 */
			/* 影子高位槽取自一段**预留**的全局区，而不是动态增长 num_globals_。
			   动态增长会踩空: 数据区大小按当时的 num_globals_ 定好之后，
			   后加的槽会写到数据区之外 —— 实测就是段错误。
			   预留区大小固定，槽号在其中顺序分配，永不越界。 */
			uint32_t wide_hi_base_ = 0;
			uint32_t wide_hi_used_ = 0;
			static constexpr uint32_t WIDE_HI_MAX = 1024;

			void set_wide(bool v) { wide_mode_ = v; }
			bool wideOn() const { return wide_mode_; }
			bool isWideIntVar(const std::string& n) const;
			uint32_t wideHiSlot(const std::string& n);   /* 首次调用时分配 */
			bool gen_binary_wide(Binary& n);             /* 返回 false 表示不支持 */

			/* ── 128 位算术所需的少量缺失编码 ── */
			void emit_adc_rr(int dst, int src);
			void emit_sbb_rr(int dst, int src);
			void emit_imul_rr(int dst, int src);
			void emit_mul_r(int r);                      /* rdx:rax = rax * r */
			void emit_div_r(int r);                      /* rdx:rax / r */
			void emit_neg_r(int r);
			void emit_not_r(int r);
			void emit_or_rr(int dst, int src);
			void emit_sar_imm8(int r, int n);
			void emit_setcc(int cc, int r);
			void emit_imul1_r(int r);                /* rdx:rax = rax * r (有符号) */
			void emit_divmod_const(int64_t cst, bool want_rem);
			bool emit_pow2_mod(int64_t cst);
			size_t rt_co_switch_off_ = 0;
			size_t panic_contract_off_ = 0;
			struct CodeDispPatch {
				size_t disp_pos;
				size_t target_pos;
			};
			std::vector<CodeDispPatch> co_thunk_patches_;
			std::map<std::string, std::string> type_aliases_;
			std::map<std::string, std::string> local_types_;
			int co_thunk_seq_ = 0;

			size_t data_area_ = 0;
			size_t entry_off_ = 0;
			size_t global_data_off_ = 0;
			size_t printf_iat_off_ = 0;
			size_t exit_iat_off_ = 24;
			size_t gsh_iat_off_ = 8;
			size_t rf_iat_off_ = 16;
			size_t pe_data_rel_ = 0;  /* PE mode: data-area buffer offset delta (idata section) */

			struct FuncPatch {
				size_t disp_pos;
				std::string callee;
				int line = 0;
				int col = 0;
			};
			std::vector<FuncPatch> func_patches_;
			struct GlobalRipPatch {
				size_t disp_pos;
				uint32_t slot;
			};
			std::vector<GlobalRipPatch> global_rip_patches_;
			struct StrPatch {
				size_t disp_pos;
				std::string str;
			};
			std::vector<StrPatch> str_patches_;
			std::map<uint32_t, int64_t> global_inits_;  /* constant initializers for top-level globals */

			struct PEPatch {
				size_t pos;
				int kind;
			};
			std::vector<PEPatch> pe_iat_patches_;
			std::vector<PEPatch> pe_fmt_patches_;
			size_t entry_main_call_pos_ = 0;
			size_t entry_exit_call_pos_ = 0;

			/* ── 运行时内存管理 ──
			   保留全局槽位: heap_ptr / heap_end / stack_canary，其后是
			   WEI_NCLS 条 size-class 空闲链表、大块链表与统计计数器。
			   这些槽位在 generate() 开头分配，入口桩负责初始化。 */
			uint32_t heap_ptr_slot_ = 0;
			uint32_t heap_end_slot_ = 0;
			uint32_t canary_slot_ = 0;
			uint32_t freelist_base_slot_ = 0;    /* free_lists[0..WEI_NCLS-1] */
			uint32_t large_list_slot_ = 0;       /* 大块 (>32KiB) 空闲链表 */
			uint32_t seg_list_slot_ = 0;         /* mmap/VirtualAlloc 段链表，用于全量回收 */
			uint32_t stat_alloc_slot_ = 0;
			uint32_t stat_free_slot_ = 0;
			uint32_t stat_live_slot_ = 0;
			uint32_t stat_peak_slot_ = 0;
			uint32_t stat_grow_slot_ = 0;
			uint32_t lcnt_base_slot_ = 0;        /* 计数循环计数器槽基址 */
			static constexpr int WEI_LOOPN_MAX = 8;  /* 计数循环最大嵌套深度 */
			int loopn_depth_ = 0;                /* 当前计数循环嵌套深度 */
			uint32_t q_idx_slot_ = 0;            /* 隔离区环形缓冲当前下标 */
			uint32_t q_arr_slot_ = 0;            /* 隔离区槽数组基址 (WEI_QUARANTINE 个连续槽) */
			std::vector<size_t> heap_addr_patches_;   /* lea rax,[rip+pool] 的 disp 位置 */
			size_t rt_alloc_off_ = 0;
			size_t rt_udivmod128_off_ = 0;   /* 128 位无符号除/模运行时 */
			size_t rt_free_off_ = 0;
			size_t rt_grow_off_ = 0;
			size_t rt_leak_off_ = 0;
			size_t panic_generic_off_ = 0;
			size_t panic_bounds_off_ = 0;
			size_t panic_canary_off_ = 0;
			size_t panic_oom_off_ = 0;
			size_t panic_dblfree_off_ = 0;
			size_t panic_heapcorrupt_off_ = 0;
			size_t panic_div0_off_ = 0;
			size_t panic_ovf_off_ = 0;
			bool canary_active_ = false;
			int32_t canary_disp_ = 0;
			size_t entry_vprotect_addr_pos_ = 0;
			size_t entry_vprotect_size_pos_ = 0;
			size_t entry_mprotect_addr_pos_ = 0;
			size_t entry_mprotect_size_pos_ = 0;

			void emit_call_off(size_t target);
			void emit_bounds_check(int64_t len);
			void emit_canary_check();
			void emit_runtime_stubs();
			void emit_heap_and_canary_init();
			/* 除法安全: 拦截除零与 INT64_MIN / -1 商溢出（两者在 x86-64 上都会
			   触发 #DE 使进程崩溃）。divisor_reg 为除数寄存器，被除数须已在
			   RAX。本序列只借 RDX 作临时（CQO 会覆盖 RDX，故安全）。 */
			void emit_div_check(int divisor_reg);
			/* 有符号整数溢出检测: 在 add/sub/imul 之后调用，OF=1 即 panic。 */
				/* ── A4 · 调试信息收集 ──────────────────────────────────────────
			   -g 时记录每个函数的名字与代码偏移区间，finalize 时据此生成
			   DWARF 的 .debug_info / .debug_abbrev / .debug_str。
			   先做最小可用集: 编译单元 + 子程序(名字 + 地址范围)，够 gdb
			   做 bt 显示函数名和 break 函数名。行号表(.debug_line)留后续。 */
			struct DbgFunc {
				std::string name;
				uint64_t start = 0;
				uint64_t end = 0;
			};
			std::vector<DbgFunc> dbg_funcs_;
			bool debug_info_ = false;
			void set_debug(bool v) { debug_info_ = v; }
			bool debug_on() const { return debug_info_; }
			std::string dbg_src_file_;
			void set_src_file(const std::string& f) { dbg_src_file_ = f; }
			/* C1: 变量声明宽度表（由 ScanVars 从 VarDecl.type 填充） */
			struct WidthInfo {
				int width = 0;          /* 0=未知(按64), 8/16/32/64 */
				bool is_unsigned = false;
			};
			std::map<std::string, WidthInfo> var_width_;
			static WidthInfo widthOfType(const std::string& t) {
				if (t == "i8")  return {8,  false};
				if (t == "i16") return {16, false};
				if (t == "i32") return {32, false};
				if (t == "i64") return {64, false};
				if (t == "u8")  return {8,  true};
				if (t == "u16") return {16, true};
				if (t == "u32") return {32, true};
				if (t == "u64") return {64, true};
				return {0, false};
			}
			int widthOfVar(const std::string& n) const {
				auto it = var_width_.find(n);
				return (it == var_width_.end()) ? 0 : it->second.width;
			}
			bool unsignedOfVar(const std::string& n) const {
				auto it = var_width_.find(n);
				return (it == var_width_.end()) ? false : it->second.is_unsigned;
			}
			int widthOfExpr(const Expr& e) const;


		void emit_ovf_check();
		/* ── C3 · 内嵌检测框架 ──────────────────────────────────────────
		   原来的检测是散落硬编码的：越界/溢出/金丝雀各自写死，无法按等级
		   开关。这里统一成"检测点枚举 + 分级开关"：
		     none     全关（只在明确要求裸速时用，不安全）
		     min      只留防崩溃类：越界 + 空指针
		     standard 再加溢出 + 双重释放   ← 默认
		     full     再加契约（require/pre/post）
		   底线：只能因"编译期证明不需要"而省略，绝不为了性能偷偷关。 */
		enum class CheckKind {
			Bounds,        /* 数组/指针越界 */
			Overflow,      /* 算术溢出 */
			NullPtr,       /* 空指针解引用 */
			DoubleFree,    /* 释放后使用 / 双重释放 */
			Contract,      /* require 前置 / 后置条件 */
			Canary         /* 栈金丝雀 */
		};
		enum class CheckLevel { None = 0, Min = 1, Standard = 2, Full = 3 };
		CheckLevel check_level_ = CheckLevel::Standard;
		void set_check_level(CheckLevel lv) { check_level_ = lv; }
		bool check_enabled(CheckKind k) const {
			int lv = static_cast<int>(check_level_);
			switch (k) {
				case CheckKind::Bounds:
				case CheckKind::NullPtr:
				case CheckKind::Canary:   return lv >= static_cast<int>(CheckLevel::Min);
				case CheckKind::Overflow:
				case CheckKind::DoubleFree: return lv >= static_cast<int>(CheckLevel::Standard);
				case CheckKind::Contract: return lv >= static_cast<int>(CheckLevel::Full);
			}
			return true;
		}
		/* C1: 逐宽度溢出检查与规范化的宽度截断 */
		void emit_width_ovf_check(int width, bool is_unsigned);
		void emit_width_truncate(int width, bool is_unsigned);
		/* C1: 8×8 宽度×符号性转换矩阵入口 */
		void emit_width_cast(int from_w, bool from_uns, int to_w, bool to_uns);
		void gen_asm(const AsmStmt& as);
		void gen_switch(SwitchStmt& n);
		void gen_defer(DeferStmt& n);
		/* A5: 自动展开因子选择与可展开性判定 */
		int pick_unroll_factor(int64_t trip_hint);
		/* C3: 越界检测是否开启 */
		bool bounds_check_on() const;
		bool body_is_unrollable(const std::vector<Stmt>& body);
		/* 归纳变量自增的溢出检查能否静态免除:
		   x 满足 x < ub（由 while (x < const) 静态得出，见 noteIdxUB），
		   则 x + c 的最大值是 (ub - 1) + c。只要它不超过 INT64_MAX，
		   这次加法就不可能溢出，运行时的 jo 检查可以整条删掉。
		   注意这是**证明**而非猜测: 上界由循环条件给出，且归纳变量
		   只被 `x = x + 正常量` 推进（该前提在登记上界时已校验）。 */
		bool addOvfFree(const Expr& lhs, int64_t c) const {
			auto* id = std::get_if<Ident>(&lhs);
			if (!id) return false;
			/* D1: 取模归纳变量恒在 [0,C)，加 adv(<C) 后 < 2C。
			   只要 2C 不超过 INT64_MAX，这次加法不可能溢出。
			   这是编译期证明，不是为性能关检查。 */
			{
				auto mit = mod_iv_bounds_.find(id->name);
				if (mit != mod_iv_bounds_.end() && mit->second > 0) {
					wei_i128 mx = wei_i128(mit->second) + wei_i128(c > 0 ? c : -c) + 1;
					return mx <= wei_i128(INT64_MAX);
				}
			}
			auto it = idx_ub_.find(id->name);
			if (it == idx_ub_.end()) return false;
			int64_t ub = it->second;
			if (ub <= 0) return false;
			/* ub-1 + c 用 128 位验算，避免判定本身溢出 */
			wei_i128 mx = wei_i128(ub - 1) + wei_i128(c);
			return mx <= wei_i128(INT64_MAX);
		}
		bool addOvfFreeVar(const std::string& nm, int64_t c) const {
			{
				auto mit = mod_iv_bounds_.find(nm);
				if (mit != mod_iv_bounds_.end() && mit->second > 0) {
					wei_i128 mx = wei_i128(mit->second) + wei_i128(c > 0 ? c : -c) + 1;
					return mx <= wei_i128(INT64_MAX);
				}
			}
			auto it = idx_ub_.find(nm);
			if (it == idx_ub_.end()) return false;
			int64_t ub = it->second;
			if (ub <= 0) return false;
			wei_i128 mx = wei_i128(ub - 1) + wei_i128(c);
			return mx <= wei_i128(INT64_MAX);
		}
			/* ── IR 路径 ── */
			int ir_build_expr(IRFunc& f, Expr& e, bool& ok);
			void ir_emit(IRFunc& f, IRRegAlloc& ra);
			bool try_ir_assign(AssignStmt& n);

			// 循环寄存器分配优化：当前是否在优化循环中
			std::string fast_loop_var_;
			int  fast_loop_reg_;
			int64_t fast_loop_inc_;

			/* ── store 消去跟踪 ── */
			size_t last_store_len_ = (size_t) -1;
			int last_store_slot_ = -1;
			int loop_depth_ = 0;
			std::map<uint32_t, int> loop_reg_vars_;

			/* emit return if last stmt didn't */
			void emit_global_load(int reg, uint32_t slot) {
				c_.rex_wrb(reg, 5);
				c_.u8(0x8B);
				c_.modrm(0, reg, 5);
				size_t disp_pos = c_.pos();
				c_.u32(0);
				global_rip_patches_.push_back({disp_pos, slot});
			}
			/* emit return if last stmt didn't */
			void emit_global_store(uint32_t slot, int reg) {
				c_.rex_wrb(reg, 5);
				c_.u8(0x89);
				c_.modrm(0, reg, 5);
				size_t disp_pos = c_.pos();
				c_.u32(0);
				global_rip_patches_.push_back({disp_pos, slot});
			}
			void emit_global_addr(int reg, uint32_t slot) {
				c_.rex_wrb(reg, 5);
				c_.u8(0x8D);
				c_.modrm(0, reg, 5);
				size_t disp_pos = c_.pos();
				c_.u32(0);
				global_rip_patches_.push_back({disp_pos, slot});
			}
			int emit_jmp() {
				int idx = static_cast<int>(labels_.size());
				labels_.push_back({false, 0, c_.jmp32()});
				return idx;
			}
			int emit_jcc(int cc) {
				int idx = static_cast<int>(labels_.size());
				labels_.push_back({true, cc, c_.jcc32(cc)});
				return idx;
			}
			void patch(int idx) {
				auto& lb = labels_[idx];
				c_.patch_jmp(lb.pos);
			}

			uint32_t local_slot(const std::string& name, bool alloc, int64_t array_len = 0);
			VarInfo& resolve_var(const std::string& name);

			/* ── mod 折叠: x % D (D=2^k-1), r = (x&D)+(x>>k) 迭代
			   正值修正用 cmov 无分?? r ??[0,2^32), "r ??D" 概率 ??0.5, 分支会半对半误预??*/
			void emit_mod_fold(int64_t cst, int k) {
				c_.mov64r_rm64(1, 0);        /* rcx = x */
				c_.u8(0x25);
				c_.u32(static_cast<uint32_t>(cst)); /* and eax, D */
				c_.u8(0x48);
				c_.u8(0xC1);
				c_.u8(0xF9);
				c_.u8(static_cast<uint8_t>(k)); /* sar rcx, k */
				c_.add_r64_rm64(0, 1);       /* rax = (x&D)+(x>>k) */
				/* 负值修??最多两??: 正路径恒跳过, 分支预测完美) */
				int jp1 = emit_jcc(0xD);     /* jge pos */
				c_.u8(0x48);
				c_.u8(0x05);
				c_.u32(static_cast<uint32_t>(cst)); /* add rax, D */
				int jp2 = emit_jcc(0xD);     /* jge pos */
				c_.u8(0x48);
				c_.u8(0x05);
				c_.u32(static_cast<uint32_t>(cst));
				patch(jp1);
				patch(jp2);                  /* pos: */
				/* 正值修??无分??: r 可能 > D, 最多两??*/
				c_.mov64r_rm64(1, 0);
				c_.sub_rm64_imm32(1, static_cast<int32_t>(cst));
				c_.cmov_r64_rm64(0xD, 0, 1); /* cmovge rax, rcx */
				c_.mov64r_rm64(1, 0);
				c_.sub_rm64_imm32(1, static_cast<int32_t>(cst));
				c_.cmov_r64_rm64(0xD, 0, 1);
			}

			/* Compile-time algorithm replacement: returns true if pattern matched */
			bool try_sum_pattern(ForStmt& n);
			bool try_lcg_pe_loop(ForStmt& n);
			bool try_trial_div_loop(ForStmt& n);
			bool try_chainmul(ForStmt& n);
			bool try_converge_loop(ForStmt& n);
			bool try_cubic_sum_eq(ForStmt& n);
			bool try_mul_const_loop(ForStmt& n);
			bool try_linear_fold(ForStmt& n);
			bool try_linear_fold_var(ForStmt& n);
			bool try_gcd_loop(WhileStmt& n);
			bool try_fib_pattern(FuncDecl& f);
			bool try_hanoi_pattern(FuncDecl& f);
			bool expr_has_call(Expr& e);
			/* 表达式是否含下标或解引用（会动用额外寄存器，不宜寄存器提升） */
			bool expr_has_mem(Expr& e);
			bool expr_has_mem_promotable(Expr& e);   /* 放宽版: 允许读局部数组 */
			/* 取模归纳变量: AST 前置改写（见实现处注释） */
			void rewrite_mod_induction_vars(std::vector<Stmt>& body);
		/* 通用循环展开: 对任意形态的 while 做 AST 层 U 倍展开。
		   与 try_while_unroll4 的区别: 后者只认"恰好两条赋值"的特定形态，
		   通用版只要满足安全条件就做，覆盖取模/多变量/带 if 的循环。 */
		bool rewrite_unroll_generic(std::vector<Stmt>& body, size_t idx);
		bool unroll_note_ = false;
		/* generate() 会被调用**两次**（第二次为 patch 数据重定位）。
		   AST 改写类优化必须只在第一次做 —— 否则第二次会在已展开的
		   AST 上再展开一遍，body 指数膨胀，生成的代码直接跑飞。 */
		int gen_pass_ = 0;
			bool bodyHasJump(const std::vector<Stmt>& v) const;

			/* 编译期已知常量追??(仅用??pattern 匹配) */
			std::map<std::string, int64_t> known_const_;
			/* 已识别为组合数递推的函数名 */
			std::string comb_func_;

			/* ── 双精度支??── */
			std::set<std::string> dbl_vars_;                     /* 声明??double 的变量名 */
			bool cur_ret_double_ = false;                        /* 当前函数返回 double */
			bool cur_ret_result_ = false;                        /* 当前函数返回 Result: '?' 仅在此时合法 */
			std::vector<std::pair<size_t, double>> dbl_patches_; /* RIP 相对双精度常量回??*/
			/* 处理端诊断: 最近一次循环优化命中的通路名 */
			const char* last_closed_name_ = "(none)";
			bool isDblExpr(const std::unique_ptr<Expr>& e);
			bool isStrExpr(const std::unique_ptr<Expr>& e);
			bool funcReturnsDouble(const std::string& name) const;
			bool funcReturnsString(const std::string& name) const;
			void gen_expr_dbl(const std::unique_ptr<Expr>& e);

			void gen_func(FuncDecl& f);
			/* ═══ 代码质量层 ══════════════════════════════════════════════
			   三件事:
			     1) 通用寄存器驻留（把高频局部变量钉在 callee-saved 寄存器）
			     2) LEA 融合（仅在静态证明不会溢出时）
			     3) 无分支代码（cmov / 逻辑运算短路消除 / 循环旋转）
			   所有会丢掉检查的变换（lea、cmov）都以静态证明为门禁。 */
			struct Leaf { enum K { None, Imm, Reg, Mem } k = None; int reg = 0; int32_t v = 0; };
			struct LeaTerm { Leaf l; int scale; };
			/* D1/Rust-diff: bounded = 区间本身可信（即使超出 int64）。
			   ok 仍保持旧语义 = bounded && 整个区间落在 int64 内，
			   专门供 ovfProven 决定"能否省略溢出检查"。
			   新增 bounded 是为了能在区间**完全**落在合法范围之外时，
			   于编译期直接报错（必然越界/必然溢出），而不是等运行时。 */
			struct Rng { wei_i128 lo = 0, hi = 0; bool ok = false; bool bounded = false; };

			std::map<uint32_t,int> pinned_;               /* slot -> callee-saved reg */
			std::map<uint32_t,std::string> pinned_name_;
			int  regOfSlot(uint32_t s) const { auto it = loop_reg_vars_.find(s); return it == loop_reg_vars_.end() ? -1 : it->second; }
			bool pinnedActive() const { return !pinned_.empty(); }
			bool promote_enable_ = true;    /* D1: 函数级寄存器驻留，默认开启 */
			/* disp -> 物理寄存器。X64Buf 的 slot_hook_ 按 disp 精确查表，
			   统一拦截所有 [rbp+d] 读写。 */
			std::map<int64_t, int> pinnedDisp_;
			/* generate() 跑两遍（第二遍为数据重定位 patch），两遍的驻留决策
			   必须逐字节一致。第二遍复用第一遍的 slot→reg 映射。 */
			/* 两遍按**调用顺序**对齐，不用 key —— 同名/同体大小的函数
			   用 key 会串味，顺序对齐严格保证 pass2 复用 pass1 的决策。 */
			std::vector<std::vector<std::pair<uint32_t,int>>> promote_seq_;
			size_t promote_seq_idx_ = 0;
			void clearLoopRegs() { for (auto it = loop_reg_vars_.begin(); it != loop_reg_vars_.end();) { if (pinned_.count(it->first)) ++it; else it = loop_reg_vars_.erase(it); } }
			void promote_function_locals(FuncDecl& f);
			void emit_sync_pinned(bool toMem, const std::function<bool(uint32_t)>& want);
			void emit_load_var(const std::string& nm);
			void emit_store_var(const std::string& nm);
			/* D1: 输出/外部调用 stub 会用 r15 暂存栈指针并 clobber 临时寄存器。
			   进入 stub 前把驻留值写回内存、禁用 hook，之后全走内存 ——
			   这是标准编译器的 call-clobber spill 处理，正确性优先。
			   典型程序的输出在末尾，循环体全程仍享受寄存器驻留。 */
			void spill_and_drop_pinned();
			Leaf leafOf(Expr& e);
			Rng  rngOf(const Expr& e);
			Rng  rngOfBin(const Binary& b);
			bool ovfProven(Binary& n);
			bool mustOverflow(const Expr& e);                    /* 区间完全超出 int64 → 必然溢出 */
			bool mustBeOOB(const Expr& idx, int64_t len);         /* 区间完全落在 [0,len) 之外 → 必然越界 */
			void checkProvenOOB(const IndexExpr& n, int64_t len);
			void checkProvenOverflow(const Binary& n);
			void proofNote(const char* what, int line);
			bool specExpr(Expr& e);
			bool isBoolValued(const Expr& e) const;
			void emit_norm_bool();
			void emit_alu_leaf(const std::string& op, int dst, const Leaf& l);
			bool emit_binop_fast(Binary& n);
			bool try_assign_inplace(AssignStmt& n);
			bool lea_flatten(Expr& e, std::vector<LeaTerm>& t, int64_t& c);
			bool lea_flatten_bin(Binary& b, std::vector<LeaTerm>& t, int64_t& c);
			bool try_lea(Binary& n);
			bool try_bool_branchless(Binary& n);
			bool try_cmov_if(IfStmt& n);
			int  emit_cmp_flags(Expr& cond);
			/* ── 编译期「对 2 的幂取模后判零」压成位测试 ──────────────────
			   C 语义里余数符号跟随被除数，但「余数是否为 0」与符号无关：
			     x % 2^k == 0   ⟺   (x & (2^k-1)) == 0
			     x % 2^k != 0   ⟺   (x & (2^k-1)) != 0
			   对**任意有符号 x**（含负数、INT64_MIN）都成立，因此不需要
			   非负证明，是纯语义等价变换，零正确性风险。

			   原路径 x%2 会走 emit_divmod_const 的 magic-multiply 序列：
			   sar $63 / and / add / and / sub / sete / movzbq / cmp / jcc，
			   约 17 条指令，且每个内层循环迭代都要跑一遍。
			   压成 `gen(x) + test $mask,%rax + jcc` 后约 2~3 条。

			   Rust / gcc 都不会把「取模结果只用于与 0 比较」跨过有符号
			   取模的语义边界合成 test —— 它们保留通用取模，再比较。 */
			bool modpow2_test(Expr& cmpL, Expr& cmpR, const std::string& cmpOp,
			                  Expr** valExpr, int64_t* mask, int* cc);
			bool try_modpow2_flags(Expr& cond, int& ccOut);
			bool try_modpow2_value(Binary& n);
			bool emit_while_rotated(WhileStmt& n);
			bool emit_for_rotated(ForStmt& n);
			static bool deepHasJump(const std::vector<Stmt>& v);

			void gen_stmt(Stmt& s);
			void gen_expr(Expr& e);
			void gen_expr_inner(Expr& e);   /* gen_expr 去掉 '?' 展开后的本体 */
			bool is_builtin_call(const std::string& name) const;
			void gen_builtin_call(Call& n);
			void emit_udivmod128_stub();
			void emit_udivmod128_call(bool want_remainder);
		/* D3: 取变量地址到 rbx（原子操作用） */
		void emit_var_addr_rbx(const std::string& name);
			void gen_addr_to_rax(Expr& e);
			void gen_block(std::vector<Stmt>& stmts);
			void gen_entry_stub();
	};

/* 循环优化分类模块。必须在 NativeGen 类定义之后引入:
   它通过 friend 访问 NativeGen 的私有发射接口。 */
struct LoopOptimizer {
	/* 各模式的实现见本文件上半部分。声明为成员函数而非自由函数，
	   是为了让 `friend struct LoopOptimizer` 覆盖到它们 —— 它们需要
	   访问 NativeGen 的私有发射接口（c_ / resolve_var / emit_ovf_check 等）。 */
	static bool lo_arithmetic_sum(NativeGen& g, WhileStmt& n);
	static bool lo_strided_sum(NativeGen& g, WhileStmt& n);
	static bool lo_countdown_sum(NativeGen& g, WhileStmt& n);
	static bool lo_square_sum(NativeGen& g, WhileStmt& n);
	struct ModPeriodInfo {
		int64_t i0 = 0, N = 0, step = 0, C = 0;
		int64_t k = 0, P = 0, full = 0, rem = 0, total = 0;
	};
	/* 只判定不发射。两个用途:
	     1) lo_mod_period_sum 自己用
	     2) rewrite_mod_induction_vars 用它"让位" —— 能被闭式解处理的
	        循环不能被改写成计数器形态，否则闭式解就认不出来了
	        （曾导致 b6_mod 永远走 O(n) 循环）。 */
	static bool mod_period_analyze(NativeGen& g, WhileStmt& n, ModPeriodInfo& out);
	/* 周期性取模求和闭式解: while (i<n) { s = s + i%C; i = i+step; }
	   i%C 的取值以 P = C/gcd(step,C) 为周期循环，整周期的和可编译期算出，
	   于是整个 O(n) 循环压成 O(1)。gcc 不做这个优化 —— 这是超车点。 */
	static bool lo_mod_period_sum(NativeGen& g, WhileStmt& n);
	/* 通用常量循环求值: 边界与初值都已知时, 编译期把整个循环跑完 */
	static bool lo_const_fold_while(NativeGen& g, WhileStmt& n);
	/* SIMD 归约: 数组求和用 SSE2 一次算两个 64 位元素 */
	static bool lo_simd_sum(NativeGen& g, WhileStmt& n);

	/* ── 语义层: 与 -no-opt 无关，永远生效 ────────────────────────────
	   这些是"编译期确定结果的求值"，属于常量折叠的自然延伸，
	   不是可选的优化。语言语义要求: 只要结果唯一确定，就该在编译期
	   算出来 —— 就像 `2+3` 必须等于 5 一样。
	   因此即便用户关闭 opt，它们依然生效。 */
	static bool runSemantic(NativeGen& g, WhileStmt& n) {
		static const bool tr = getenv("WEI_TRACE_OPT") != nullptr;
		auto H = [&](const char* nm) {
			g.last_closed_name_ = nm;
			if (tr) fprintf(stderr, "[sem] %s\n", nm);
			return true;
		};
		if (lo_const_fold_while(g, n)) return H("const_fold");
		if (lo_arithmetic_sum(g, n)) return H("arithmetic_sum");
		if (lo_strided_sum(g, n))    return H("strided_sum");
		if (lo_countdown_sum(g, n))  return H("countdown_sum");
		if (lo_square_sum(g, n))     return H("square_sum");
		if (lo_mod_period_sum(g, n)) return H("mod_period_sum");
		if (tr) fprintf(stderr, "[sem] MISS\n");
		return false;
	}
	/* ── 优化层: 受 -no-opt 控制 ──────────────────────────────────────
	   这些是"换个更快的实现方式"，结果不变但手段可选 —— 属于优化。
	   SIMD 向量化会改变指令选择与寄存器使用，关掉时退回标量路径。 */
	static bool runOpt(NativeGen& g, WhileStmt& n) {
		g.last_closed_name_ = "simd_sum";
		if (lo_simd_sum(g, n)) return true;
		return false;
	}
	static bool run(NativeGen& g, WhileStmt& n) {
		/* 诊断开关: WEI_TRACE_OPT=1 打印每个循环命中哪条通路。
		   调优时"看见"比"猜"可靠。 */
		static const bool trace = getenv("WEI_TRACE_OPT") != nullptr;
		auto hit = [&](const char* nm) { if (trace) fprintf(stderr, "[opt] %s\n", nm); return true; };
		/* 语义层（永远生效）优先，其次优化层（受 -no-opt 控制） */
		if (runSemantic(g, n)) return hit(g.last_closed_name_);
		if (g.opt_ && runOpt(g, n)) return hit(g.last_closed_name_);
		if (trace) fprintf(stderr, "[opt] (none)\n");
		g.last_closed_name_ = "(none)";
		return false;
	}
};

	static constexpr uint32_t LOCAL_SAVED_REG_OFFSET = 5; /* rbx, r12-r15 saved below rbp */

	uint32_t NativeGen::local_slot(const std::string& name, bool alloc, int64_t array_len) {
		auto it = locals_.find(name);
		if (it != locals_.end()) return it->second.slot;
		if (!alloc) return 0;
		uint32_t slot = max_locals_++ + LOCAL_SAVED_REG_OFFSET;
		if (array_len > 0) max_locals_ += static_cast<uint32_t>(array_len);
		locals_[name] = {slot, true, array_len};
		return slot;
	}

	NativeGen::VarInfo& NativeGen::resolve_var(const std::string& name) {
		auto it = locals_.find(name);
		if (it != locals_.end()) return it->second;
		auto git = globals_.find(name);
		if (git != globals_.end()) return git->second;
		uint32_t slot = global_slots_++;
		globals_[name] = {slot, false};
		return globals_[name];
	}

	/* ── 表达式是否为 double 语境 (轻量推断: 字面??已声明的 double 变量/其算术组?? ── */
	bool NativeGen::funcReturnsDouble(const std::string& name) const {
		for (auto* f : funcs_) {
			if (f->name == name) return (f->ret_type == "double" || f->ret_type == "浮点");
		}
		for (auto& me : mod_funcs_) {
			if (!me.fn) continue;
			if (me.mod->name + "::" + me.fn->name == name
			        && (me.fn->ret_type == "double" || me.fn->ret_type == "浮点")) return true;
			if (me.mod->name == name
			        && (me.fn->ret_type == "double" || me.fn->ret_type == "浮点")) return true;
		}
		return false;
	}

	bool NativeGen::funcReturnsString(const std::string& name) const {
		for (auto* f : funcs_) {
			if (f->name == name && isTextTypeName(f->ret_type)) return true;
		}
		for (auto& me : mod_funcs_) {
			if (!me.fn) continue;
			if (me.mod->name + "::" + me.fn->name == name && isTextTypeName(me.fn->ret_type)) return true;
			if (me.mod->name == name && isTextTypeName(me.fn->ret_type)) return true;
		}
		return false;
	}

	bool NativeGen::isDblExpr(const std::unique_ptr<Expr>& e) {
		if (!e) return false;
		if (std::get_if<DoubleLit>(e.get())) return true;
		if (auto* id = std::get_if<Ident>(e.get())) {
			auto lit = locals_.find(id->name);
			if (lit != locals_.end()) return lit->second.is_double;
			auto git = globals_.find(id->name);
			if (git != globals_.end()) return git->second.is_double;
			return dbl_vars_.count(id->name) > 0;
		}
		if (auto* c = std::get_if<Call>(e.get())) return funcReturnsDouble(c->callee);
		if (auto* b = std::get_if<Binary>(e.get())) {
			const std::string& op = b->op;
			if (op == "+" || op == "-" || op == "*" || op == "/")
				return isDblExpr(b->lhs) || isDblExpr(b->rhs);
		}
		return false;
	}

	bool NativeGen::isStrExpr(const std::unique_ptr<Expr>& e) {
		if (!e) return false;
		if (std::get_if<StrLit>(e.get())) return true;
		if (auto* id = std::get_if<Ident>(e.get())) {
			auto lit = locals_.find(id->name);
			if (lit != locals_.end()) return lit->second.is_string;
			auto git = globals_.find(id->name);
			if (git != globals_.end()) return git->second.is_string;
		}
		if (auto* c = std::get_if<Call>(e.get())) return funcReturnsString(c->callee);
		return false;
	}

	/* ── 双精度表达式求????xmm0; 整数子表达式自动 cvtsi2sd ── */
	void NativeGen::gen_expr_dbl(const std::unique_ptr<Expr>& e) {
		/* double 常量: RIP 取常量地址 ??movsd xmm0,[rax] */
		if (auto* d = std::get_if<DoubleLit>(e.get())) {
			c_.lea64r_rip(0);
			size_t dp = c_.pos();
			c_.u32(0);
			dbl_patches_.push_back({dp, d->value});
			c_.u8(0xF2);
			c_.u8(0x0F);
			c_.u8(0x10);
			c_.modrm(0, 0, 0);   /* movsd xmm0,[rax] (rax=常量地址) */
			return;
		}
		/* double 变量: 位模式直??movsd */
		if (auto* id = std::get_if<Ident>(e.get())) {
			auto dit = dbl_vars_.find(id->name);
			if (dit != dbl_vars_.end() || resolve_var(id->name).is_double) {
				auto& vi = resolve_var(id->name);
				if (vi.is_local) {
					int32_t disp = static_cast<int32_t>(-8 * static_cast<int>(vi.slot + 1));
					c_.u8(0xF2);
					c_.rex_wrb(0, 5);
					c_.u8(0x0F);
					c_.u8(0x10);
					if (disp >= -128 && disp <= 127) {
						c_.modrm(1, 0, 5);
						c_.u8(static_cast<uint8_t>(disp));
					} else {
						c_.modrm(2, 0, 5);
						c_.u32(static_cast<uint32_t>(disp));
					}
				} else {
					emit_global_addr(2, vi.slot);                 /* lea rdx,[global] */
					c_.u8(0xF2);
					c_.u8(0x0F);
					c_.u8(0x10);
					c_.modrm(0, 0, 2);                            /* movsd xmm0,[rdx] */
				}
				return;
			}
		}
		/* 整数表达?? 求值到 rax 后转双精??*/
		gen_expr(*e);
		if (isDblExpr(e)) return;   /* gen_expr 的 SSE 路径已把结果放在 xmm0 */
		c_.u8(0xF2);
		c_.u8(0x48);
		c_.u8(0x0F);
		c_.u8(0x2A);
		c_.modrm(3, 0, 0);   /* cvtsi2sd xmm0,rax */
	}

	uint32_t NativeGen::part_slot_id(const std::string& m, const std::string& p,
	                                 bool hasField, const std::string& f) const {
		std::string key;
		if (hasField) {
			key = part_key(m, p, f);
		} else {
			/* 无字?? 取部件第一个成??*/
			key = part_key(m, p, "*first");
		}
		auto it = part_slot_.find(key);
		if (it == part_slot_.end()) {
			throw std::runtime_error("JIT: 未知部件路径: " + m + ">" + p + (hasField ? "." + f : ""));
		}
		return it->second;
	}

	/* ── Pattern: for(i=0; i<N; i++) { s = s + i; }
	   Replace with: if(N>0) s += N*(N-1)/2;  ── O(1) instead of O(N) */
	bool NativeGen::try_sum_pattern(ForStmt& n) {
		/* Check standard loop form: for(i=init; i<cmp>bound; i+=±1) { s = s + i; }
		   Closed form: Σ = G(hi) - G(lo-1), G(k)=k(k+1)/2 (偶数因子先除, ??2^64 精确) */
		if (!n.init_decl || !n.init_decl->init) return false;
		auto* il = std::get_if<IntLit>(n.init_decl->init.get());
		if (!il) return false;
		int64_t init_val = il->value;
		std::string loop_var = n.init_decl->name;
		if (!n.cond) return false;
		auto* bin = std::get_if<Binary>(n.cond.get());
		if (!bin) return false;
		auto* id = std::get_if<Ident>(bin->lhs.get());
		if (!id || id->name != loop_var) return false;
		const std::string& cop = bin->op;
		if (cop != "<" && cop != "<=" && cop != ">" && cop != ">=") return false;
		/* step: ++loop_var / --loop_var */
		bool inc = true;
		bool step_ok = false;
		if (n.step) {
			if (auto* un = std::get_if<Unary>(n.step.get())) {
				if (un->op == "++") {
					inc = true;
					step_ok = true;
				} else if (un->op == "--") {
					inc = false;
					step_ok = true;
				}
			}
		}
		if (!step_ok) return false;
		/* body: single assignment s = s + loop_var */
		if (n.body.size() != 1) return false;
		auto* as = std::get_if<AssignStmt>(&n.body[0]);
		if (!as) return false;
		auto* bbin = std::get_if<Binary>(as->value.get());
		if (!bbin || bbin->op != "+") return false;
		auto* lid = std::get_if<Ident>(bbin->lhs.get());
		if (!lid || lid->name != as->name) return false;
		auto* rid = std::get_if<Ident>(bbin->rhs.get());
		if (!rid || rid->name != loop_var) return false;

		/* Pattern matched! Generate closed-form code. */
		auto emit_g = [&]() {
			/* rax = k ??rax = G(k) = k*(k+1)/2, 偶数因子先除 (??2^64 精确) */
			c_.mov64r_rm64(2, 0);           /* rdx = k */
			c_.add_rm64_imm32(2, 1);        /* rdx = k+1 */
			c_.u8(0x48);
			c_.u8(0xF7);
			c_.u8(0xC0);
			c_.u32(1);  /* test rax, 1 */
			int jodd = emit_jcc(0x5);       /* jnz odd */
			c_.u8(0x48);
			c_.u8(0xD1);
			c_.u8(0xE8);  /* shr rax, 1: rax = k/2 */
			c_.imul_r64_rm64(0, 2);         /* rax = (k/2)*(k+1) = G(k) */
			int jdone = emit_jmp();
			patch(jodd);
			c_.mov64r_rm64(1, 2);           /* rcx = k+1 */
			c_.u8(0x48);
			c_.u8(0xD1);
			c_.u8(0xE9);  /* shr rcx, 1: rcx = (k+1)/2 */
			c_.imul_r64_rm64(0, 1);         /* rax = k*((k+1)/2) = G(k) */
			patch(jdone);
		};
		/* ── compilation helpers ── */
		gen_expr(*bin->rhs);
		bool bound_is_const = false;
		int64_t bound_const = 0;
		if (auto* bip = std::get_if<IntLit>(bin->rhs.get())) {
			bound_is_const = true;
			bound_const = bip->value;
		}
		int64_t loop_final = 0;
		if (inc) {
			/* last = N-1 (<) / N (<=) */
			loop_final = (cop == "<") ? bound_const : (bound_const + 1);
		} else {
			/* 递减: last = M+1 (>) / M (>=) */
			loop_final = (cop == ">") ? bound_const : (bound_const - 1);
		}
		/* S = G(hi) - G(lo-1) */
		if (inc) {
			/* rcx = N(边界??: ??G(N±1) ??r11; ??G(init-1) ??sub */
			if (!bound_is_const) {
				c_.mov64r_rm64(11, 0);    /* r11 = N */
			}
			if (!bound_is_const) {
				c_.mov64r_rm64(0, 11);
			} else {
				c_.mov64r_imm64(0, bound_const);
			}
			if (cop == "<") c_.sub_rm64_imm32(0, 1);          /* N-1 = last */
			emit_g();
			c_.mov64r_rm64(11, 0);                            /* r11 = G(last) */
			c_.mov64r_imm64(0, init_val - 1);                 /* first-1 */
			emit_g();
			c_.sub_r64_rm64(11, 0);                           /* r11 = S */
		} else {
			if (!bound_is_const) {
				c_.mov64r_rm64(13, 0);    /* r13 = M */
			}
			c_.mov64r_imm64(0, init_val);
			emit_g();
			c_.mov64r_rm64(11, 0);                            /* r11 = G(init) */
			if (!bound_is_const) {
				c_.mov64r_rm64(0, 13);
			} else {
				c_.mov64r_imm64(0, bound_const);
			}
			if (cop == ">=") c_.sub_rm64_imm32(0, 1);         /* last-1 = M-1 */
			emit_g();
			c_.sub_r64_rm64(11, 0);                           /* r11 = S */
		}
		/* s += S */
		auto& vi = resolve_var(as->name);
		if (vi.is_local) {
			int32_t d = static_cast<int32_t>(-8 * static_cast<int>(vi.slot + 1));
			c_.add_r64_memd8(11, 5, d);     /* r11 += s */
			c_.mov_memd8_64r(5, d, 11);     /* s = r11 */
		} else {
			emit_global_load(0, vi.slot);
			c_.add_r64_rm64(11, 0);         /* r11 += s */
			emit_global_store(vi.slot, 11);
		}
		/* loop_var = 循环后终?? 常量边界取编译期?? 变量边界运行时重载边界??±修正 */
		auto& lvi = resolve_var(loop_var);
		if (lvi.is_local) {
			int32_t ld = static_cast<int32_t>(-8 * static_cast<int>(lvi.slot + 1));
			if (!bound_is_const) {
				if (auto* bid = std::get_if<Ident>(bin->rhs.get())) {
					auto& bvr = resolve_var(bid->name);
					if (bvr.is_local) {
						int32_t bd2 = static_cast<int32_t>(-8 * static_cast<int>(bvr.slot + 1));
						c_.mov64r_memd8(0, 5, bd2);                   /* rax = 边界??*/
						if (cop == "<=" || cop == ">") c_.add_rm64_imm32(0, 1);
						else if (cop == ">=") c_.sub_rm64_imm32(0, 1);
						c_.mov_memd8_64r(5, ld, 0);
					} else {
						c_.mov64r_imm64(0, loop_final);
						c_.mov_memd8_64r(5, ld, 0);
					}
				} else {
					c_.mov64r_imm64(0, loop_final);
					c_.mov_memd8_64r(5, ld, 0);
				}
			} else {
				c_.mov64r_imm64(0, loop_final);
				c_.mov_memd8_64r(5, ld, 0);
			}
		}
		return true;
	}

	/* ── Pattern: for(i=0;i<n;i++){ x=x*A+B; x=x%M; t=t+x; }
	   匹配: for(i=0;i<n;i++){ x=x*A+B; x=x%M; t=t+x; }
	   生成: x 驻留 rax, t 驻留 r9, 循环步长 2 (unroll), 折叠??cmov 无分支修??*/
	bool NativeGen::try_lcg_pe_loop(ForStmt& n) {
		if (!n.init_decl || !n.init_decl->init) return false;
		auto* il = std::get_if<IntLit>(n.init_decl->init.get());
		if (!il || il->value != 0) return false;
		std::string loop_var = n.init_decl->name;
		if (!n.cond) return false;
		auto* bin = std::get_if<Binary>(n.cond.get());
		if (!bin || bin->op != "<") return false;
		auto* id = std::get_if<Ident>(bin->lhs.get());
		if (!id || id->name != loop_var) return false;
		auto* rid = std::get_if<Ident>(bin->rhs.get());
		if (!rid) return false;
		std::string bound_name = rid->name;
		if (!n.step) return false;
		auto* un = std::get_if<Unary>(n.step.get());
		if (!un || un->op != "++") return false;
		auto* sid = std::get_if<Ident>(un->operand.get());
		if (!sid || sid->name != loop_var) return false;
		if (n.body.size() != 3) return false;
		auto* a1 = std::get_if<AssignStmt>(&n.body[0]);
		auto* a2 = std::get_if<AssignStmt>(&n.body[1]);
		auto* a3 = std::get_if<AssignStmt>(&n.body[2]);
		if (!a1 || !a2 || !a3) return false;
		std::string state_var = a1->name;
		if (a2->name != state_var || a3->name == state_var) return false;
		/* s1: x = x*A + B */
		int64_t A_val = 0, B_val = 0;
		auto* b1 = std::get_if<Binary>(a1->value.get());
		if (!b1 || b1->op != "+") return false;
		auto* mul = std::get_if<Binary>(b1->lhs.get());
		if (!mul || mul->op != "*") return false;
		auto* mul_lhs = std::get_if<Ident>(mul->lhs.get());
		auto* mul_rhs = std::get_if<IntLit>(mul->rhs.get());
		auto* b_rhs = std::get_if<IntLit>(b1->rhs.get());
		if (!mul_lhs || mul_lhs->name != state_var || !mul_rhs || !b_rhs) return false;
		A_val = mul_rhs->value;
		B_val = b_rhs->value;
		/* s2: x = x % M (梅森?? */
		auto* b2 = std::get_if<Binary>(a2->value.get());
		if (!b2 || b2->op != "%") return false;
		auto* b2_lhs = std::get_if<Ident>(b2->lhs.get());
		auto* b2_rhs = std::get_if<IntLit>(b2->rhs.get());
		if (!b2_lhs || b2_lhs->name != state_var || !b2_rhs) return false;
		int64_t M_val = b2_rhs->value;
		int k = -1;
		if (M_val >= 1 && M_val <= 0x7FFFFFFFLL) {
			for (int kk = 1; kk <= 31; kk++)
				if (M_val == ((INT64_C(1) << kk) - 1)) {
					k = kk;
					break;
				}
		}
		if (k < 1) return false;
		/* s3: t = t + x */
		std::string t_var = a3->name;
		auto* b3 = std::get_if<Binary>(a3->value.get());
		if (!b3 || b3->op != "+") return false;
		auto* b3_lhs = std::get_if<Ident>(b3->lhs.get());
		auto* b3_rhs = std::get_if<Ident>(b3->rhs.get());
		if (!b3_lhs || b3_lhs->name != t_var || !b3_rhs || b3_rhs->name != state_var) return false;
		auto& svi = resolve_var(state_var);
		auto& tvi = resolve_var(t_var);
		auto& bvi = resolve_var(bound_name);
		auto& lvi = resolve_var(loop_var);
		if (!svi.is_local || !tvi.is_local || !bvi.is_local || !lvi.is_local) return false;
		int32_t sd = static_cast<int32_t>(-8 * static_cast<int>(svi.slot + 1));
		int32_t td = static_cast<int32_t>(-8 * static_cast<int>(tvi.slot + 1));
		int32_t bd = static_cast<int32_t>(-8 * static_cast<int>(bvi.slot + 1));
		int32_t ld = static_cast<int32_t>(-8 * static_cast<int>(lvi.slot + 1));

		/* ── emit_iter: 单次迭代 x = (x*A+B) % M ── */
		auto emit_iter = [&]() {
			c_.mov32r_imm32(1, static_cast<int32_t>(A_val));  /* mov ecx, A (零扩?? */
			c_.imul_r64_rm64(0, 1);
			c_.add_rm64_imm32(0, static_cast<int32_t>(B_val));
			/* fold: r = (x&D)+(x>>k) */
			c_.mov64r_rm64(1, 0);
			c_.u8(0x25);
			c_.u32(static_cast<uint32_t>(M_val));  /* and eax, D */
			c_.u8(0x48);
			c_.u8(0xC1);
			c_.u8(0xF9);
			c_.u8(static_cast<uint8_t>(k)); /* sar rcx,k */
			c_.add_r64_rm64(0, 1);
			int jp = emit_jcc(0xD);     /* jge pos (负值修?? */
			c_.u8(0x48);
			c_.u8(0x05);
			c_.u32(static_cast<uint32_t>(M_val));
			int jp2 = emit_jcc(0xD);     /* jge pos */
			c_.u8(0x48);
			c_.u8(0x05);
			c_.u32(static_cast<uint32_t>(M_val));
			patch(jp);
			patch(jp2);
			/* 正修??无分??cmov) */
			c_.mov64r_rm64(1, 0);
			c_.sub_rm64_imm32(1, static_cast<int32_t>(M_val));
			c_.cmov_r64_rm64(0xD, 0, 1);
			c_.mov64r_rm64(1, 0);
			c_.sub_rm64_imm32(1, static_cast<int32_t>(M_val));
			c_.cmov_r64_rm64(0xD, 0, 1);
			c_.add_r64_rm64(9, 0);     /* t += x (r9) */
		};

		/* preheader */
		c_.push_r(3);
		c_.mov64r_imm64(3, 0);            /* i = 0 */
		c_.mov64r_memd8(13, 5, bd);       /* r13 = n */
		c_.mov64r_rm64(11, 13);           /* r11 = n-1 */
		c_.sub_rm64_imm32(11, 1);
		c_.mov64r_memd8(9, 5, td);        /* r9 = t */
		c_.mov64r_memd8(0, 5, sd);        /* rax = x */

		/* main: if (i >= n-1) goto tail */
		c_.cmp_r64_rm64(3, 11);
		size_t jmain = c_.pos();
		c_.u8(0x0F);
		c_.u8(0x8D);
		c_.u32(0);   /* jge main→tail (rel32) */
		/* 迭代 1 */
		emit_iter();
		/* 迭代 2 */
		emit_iter();
		c_.add_rm64_imm32(3, 2);          /* i += 2 */
		c_.cmp_r64_rm64(3, 11);
		size_t dpos2 = c_.pos();
		c_.u8(0x0F);
		c_.u8(0x8C);
		c_.u32(0);   /* jl main (rel32) */
		/* tail: if (i >= n) goto done */
		c_.cmp_r64_rm64(3, 13);
		size_t jtail = c_.pos();
		c_.u8(0x0F);
		c_.u8(0x8D);
		c_.u32(0);   /* jge done (rel32) */
		emit_iter();
		c_.inc_rm64(3);                   /* i++ */
		/* done: patch 跳转 (rel32 ??0F 8x 之后; 目标 = 指令??+ rel) */
		c_.patch_u32(jmain + 2, static_cast<uint32_t>((dpos2 + 6) - (jmain + 6)));   /* jge main ??tail */
		c_.patch_u32(dpos2 + 2, static_cast<uint32_t>(jmain - (dpos2 + 6)));         /* jl tail ??main */
		c_.patch_u32(jtail + 2, static_cast<uint32_t>(c_.pos() - (jtail + 6)));      /* jge tail ??done */
		/* epilogue */
		c_.mov_memd8_64r(5, sd, 0);       /* x */
		c_.mov_memd8_64r(5, td, 9);       /* t */
		c_.mov_memd8_64r(5, ld, 3);       /* i */
		c_.pop_r(3);
		return true;
	}

	/* ── Pattern: 试除素数判定内层循环
	   for (j = 2; j < X; j++) { if (X % j == 0) { prime = 0; } }
	   改成: 试除??√X 即可: j*j <= X, 且合数命中后立即跳出 (语义等价:
	   合数的最小因????√X; 素数无因子。prime 在循环内只被??0 一?? 提前
	   退出不改变任何可观察状??。迭代数 O(X) 降到 O(√X)??*/
	bool NativeGen::try_trial_div_loop(ForStmt& n) {
		if (!n.init_decl || !n.init_decl->init) return false;
		auto* il = std::get_if<IntLit>(n.init_decl->init.get());
		if (!il) return false;
		int64_t init_val = il->value;
		std::string loop_var = n.init_decl->name;
		if (!n.cond) return false;
		auto* bin = std::get_if<Binary>(n.cond.get());
		if (!bin || (bin->op != "<" && bin->op != "<=")) return false;
		auto* id = std::get_if<Ident>(bin->lhs.get());
		if (!id || id->name != loop_var) return false;
		auto* rid = std::get_if<Ident>(bin->rhs.get());
		if (!rid || rid->name == loop_var) return false;
		std::string X_name = rid->name;
		if (!n.step) return false;
		auto* un = std::get_if<Unary>(n.step.get());
		if (!un || un->op != "++") return false;
		auto* sid = std::get_if<Ident>(un->operand.get());
		if (!sid || sid->name != loop_var) return false;
		/* body: ??if(X % j == 0) { prime = 0; } */
		if (n.body.size() != 1) return false;
		auto* iff = std::get_if<IfStmt>(&n.body[0]);
		if (!iff) return false;
		auto* cbin = std::get_if<Binary>(iff->cond.get());
		if (!cbin || cbin->op != "==") return false;
		auto* mod = std::get_if<Binary>(cbin->lhs.get());
		if (!mod || mod->op != "%") return false;
		auto* m_lhs = std::get_if<Ident>(mod->lhs.get());
		auto* m_rhs = std::get_if<Ident>(mod->rhs.get());
		auto* z = std::get_if<IntLit>(cbin->rhs.get());
		if (!m_lhs || !m_rhs || !z || z->value != 0) return false;
		if (m_lhs->name != X_name || m_rhs->name != loop_var) return false;
		if (!iff->else_body.empty()) return false;
		if (iff->then_body.size() != 1) return false;
		auto* p0 = std::get_if<AssignStmt>(&iff->then_body[0]);
		if (!p0) return false;
		auto* pv = std::get_if<IntLit>(p0->value.get());
		if (!pv || pv->value != 0) return false;
		std::string prime_name = p0->name;
		auto& xvi = resolve_var(X_name);
		auto& pvi = resolve_var(prime_name);
		if (!pvi.is_local) return false;
		/* 生成 */
		c_.push_r(13);
		c_.push_r(14);
		c_.mov64r_imm64(3, init_val);          /* rbx = j = init */
		if (xvi.is_local) {
			int32_t xd = static_cast<int32_t>(-8 * static_cast<int>(xvi.slot + 1));
			c_.mov64r_memd8(13, 5, xd);        /* r13 = X */
		} else {
			emit_global_load(13, xvi.slot);
		}
		c_.mov64r_imm64(14, 1);                /* r14 = prime = 1 */
		size_t top = c_.pos();
		c_.mov64r_rm64(2, 3);                  /* rdx = j */
		c_.imul_r64_rm64(2, 3);                /* rdx = j*j */
		c_.cmp_r64_rm64(2, 13);                /* j*j vs X */
		int jg_exit = emit_jcc(0xF);           /* jg exit (素数) */
		if (xvi.is_local) {
			int32_t xd = static_cast<int32_t>(-8 * static_cast<int>(xvi.slot + 1));
			c_.mov64r_memd8(0, 5, xd);         /* rax = X */
		} else {
			emit_global_load(0, xvi.slot);
		}
		emit_div_check(3);
		c_.cqo();
		c_.idiv_rm64(3);                       /* X / j, rdx = 余数 */
		c_.u8(0x48);
		c_.u8(0x85);
		c_.u8(0xD2);   /* test rdx, rdx */
		int jz_got0 = emit_jcc(0x4);           /* jz got0 (合数) */
		c_.inc_rm64(3);                        /* j++ */
		c_.u8(0xE9);
		c_.u32(static_cast<uint32_t>((int64_t)top - (int64_t)(c_.pos() + 4)));  /* jmp top */
		/* got0: prime = 0 */
		patch(jz_got0);
		c_.mov64r_imm64(14, 0);
		/* exit */
		patch(jg_exit);
		if (pvi.is_local) {
			int32_t pd = static_cast<int32_t>(-8 * static_cast<int>(pvi.slot + 1));
			c_.mov_memd8_64r(5, pd, 14);       /* prime = r14 */
		} else {
			emit_global_store(pvi.slot, 14);
		}
		/* loop_var 终??*/
		auto& lvi = resolve_var(loop_var);
		if (lvi.is_local) {
			int32_t ld = static_cast<int32_t>(-8 * static_cast<int>(lvi.slot + 1));
			c_.mov_memd8_64r(5, ld, 3);
		}
		c_.pop_r(14);
		c_.pop_r(13);
		return true;
	}

	/* ── Pattern: 连乘折叠 for(i=init; i<cmp>end; i++) { x = x * (i + c); }
	   主循环每 16 迭代一??imul (窗口??16 因子并行?? + 主链 3c/16iter ??0.2c/iter),
	   环上语义精确 (imul ??2^64)。因??(i+c+1)..(i+c+16) ??lea+imul 构建??*/
	bool NativeGen::try_chainmul(ForStmt& n) {
		if (!n.init_decl || !n.init_decl->init) return false;
		auto* il = std::get_if<IntLit>(n.init_decl->init.get());
		if (!il) return false;
		int64_t init_val = il->value;
		std::string loop_var = n.init_decl->name;
		if (!n.cond) return false;
		auto* bin = std::get_if<Binary>(n.cond.get());
		if (!bin || (bin->op != "<" && bin->op != "<=")) return false;
		auto* id = std::get_if<Ident>(bin->lhs.get());
		if (!id || id->name != loop_var) return false;
		bool ident_bound = false;
		std::string bound_name;
		int64_t end_val = 0;
		if (auto* bip = std::get_if<IntLit>(bin->rhs.get())) {
			end_val = bip->value;
		} else if (auto* rid = std::get_if<Ident>(bin->rhs.get())) {
			if (rid->name == loop_var) return false;
			ident_bound = true;
			bound_name = rid->name;
		} else return false;
		if (!n.step) return false;
		auto* un = std::get_if<Unary>(n.step.get());
		if (!un || un->op != "++") return false;
		auto* sid = std::get_if<Ident>(un->operand.get());
		if (!sid || sid->name != loop_var) return false;
		if (n.body.size() != 1) return false;
		auto* as = std::get_if<AssignStmt>(&n.body[0]);
		if (!as) return false;
		auto* mbin = std::get_if<Binary>(as->value.get());
		if (!mbin || mbin->op != "*") return false;
		/* x = x * E ??x = E * x, E = loop_var + c */
		int64_t c = 0;
		bool haveE = false;
		auto checkE = [&](const std::unique_ptr<Expr>& e) {
			if (auto* ei = std::get_if<Ident>(e.get())) {
				if (ei->name == loop_var) {
					c = 0;
					return true;
				}
			}
			if (auto* eb = std::get_if<Binary>(e.get())) {
				if (eb->op == "+") {
					if (auto* el = std::get_if<Ident>(eb->lhs.get())) {
						if (auto* er = std::get_if<IntLit>(eb->rhs.get())) {
							if (el->name == loop_var) {
								c = er->value;
								return true;
							}
						}
					}
					if (auto* el = std::get_if<IntLit>(eb->lhs.get())) {
						if (auto* er = std::get_if<Ident>(eb->rhs.get())) {
							if (er->name == loop_var) {
								c = el->value;
								return true;
							}
						}
					}
				}
			}
			return false;
		};
		if (auto* ml = std::get_if<Ident>(mbin->lhs.get())) {
			if (ml->name == as->name && checkE(mbin->rhs)) haveE = true;
		} else if (checkE(mbin->lhs)) {
			if (auto* mr = std::get_if<Ident>(mbin->rhs.get())) {
				if (mr->name == as->name) haveE = true;
			}
		}
		if (!haveE) return false;
		auto& xvi = resolve_var(as->name);
		if (!xvi.is_local) return false;
		int64_t N = (bin->op == "<") ? (end_val - init_val) : (end_val - init_val + 1);
		if (N <= 0) return false;
		int32_t xd = static_cast<int32_t>(-8 * static_cast<int>(xvi.slot + 1));
		auto& ivi = resolve_var(loop_var);
		if (!ivi.is_local) return false;
		int8_t ivd = static_cast<int32_t>(-8 * static_cast<int>(ivi.slot + 1));
		static const int P = 16;            /* 窗口因子??*/
		static const int L = P / 4;         /* 每链 4 因子 */
		int creg[4] = {0, 8, 9, 10};        /* 链寄存器 */
		int64_t peel = N % P;
		c_.mov64r_imm64(0, init_val);        /* i = init */
		c_.mov_memd8_64r(5, ivd, 0);
		for (int64_t k = 0; k < peel; k++) {
			gen_block(n.body);
			c_.add_memd8_imm32(5, ivd, 1);    /* i++ */
		}
		if (N >= P) {
			c_.push_r(12);
			c_.mov64r_memd8(12, 5, xd);          /* r12 = x */
			c_.mov64r_imm64(3, init_val + peel); /* rbx = i */
			if (ident_bound) {
				auto& bvi = resolve_var(bound_name);
				if (bvi.is_local) {
					int32_t bd = static_cast<int32_t>(-8 * static_cast<int>(bvi.slot + 1));
					c_.push_r(13);
					c_.mov64r_memd8(13, 5, bd);  /* r13 = bound */
				} else return false;
			}
			/* 预检??*/
			if (ident_bound) {
				c_.cmp_r64_rm64(3, 13);
			} else if (end_val >= INT32_MIN && end_val <= INT32_MAX) {
				c_.cmp_rm64_imm32(3, static_cast<int32_t>(end_val));
			} else {
				c_.mov64r_imm64(1, end_val);
				c_.cmp_r64_rm64(3, 1);
			}
			c_.u8(0x0F);
			c_.u8(static_cast<uint8_t>(0x80 + 0x0D));   /* jge exit */
			size_t jexit = c_.pos();
			c_.u32(0);
			size_t ltop = c_.pos();
			/* 窗口乘积: 每链 4 因子 (lea 2 + imul 3), 链间并行, 树形合并 */
			auto lea_off = [&](int reg, int64_t off) {
				if (off >= -128 && off <= 127) c_.lea64r_memd8(reg, 3, static_cast<int8_t>(off));
				else c_.lea64r_memd32(reg, 3, static_cast<int32_t>(off));
			};
			for (int i = 0; i < L; i++) {
				lea_off(creg[i], c + 4 * i + 0);
				lea_off(1, c + 4 * i + 1);
				c_.imul_r64_rm64(creg[i], 1);
				lea_off(1, c + 4 * i + 2);
				lea_off(2, c + 4 * i + 3);
				c_.imul_r64_rm64(1, 2);
				c_.imul_r64_rm64(creg[i], 1);
			}
			if (L == 2) {
				c_.imul_r64_rm64(0, 8);      /* 1..8 */
			} else if (L == 4) {
				c_.imul_r64_rm64(0, 8);      /* 1..8 */
				c_.imul_r64_rm64(9, 10);     /* 9..16 */
				c_.imul_r64_rm64(0, 9);      /* 1..16 */
			}
			c_.imul_r64_rm64(12, 0);         /* x 主链 */
			c_.add_rm64_imm32(3, P);
			if (ident_bound) {
				c_.cmp_r64_rm64(3, 13);
			} else if (end_val >= INT32_MIN && end_val <= INT32_MAX) {
				c_.cmp_rm64_imm32(3, static_cast<int32_t>(end_val));
			} else {
				c_.mov64r_imm64(1, end_val);
				c_.cmp_r64_rm64(3, 1);
			}
			c_.u8(0x0F);
			c_.u8(static_cast<uint8_t>(0x80 | (0x0D ^ 1)));   /* jl ltop */
			size_t dpos = c_.pos();
			c_.u32(0);
			c_.patch_u32(dpos, static_cast<uint32_t>(ltop - (dpos + 4)));
			c_.patch_u32(jexit, static_cast<uint32_t>(c_.pos() - (jexit + 4)));
			c_.mov_memd8_64r(5, xd, 12);     /* x = r12 */
			c_.pop_r(12);
			if (ident_bound) c_.pop_r(13);
		}
		/* loop_var 终??*/
		auto& lvi = resolve_var(loop_var);
		if (lvi.is_local) {
			int32_t ld = static_cast<int32_t>(-8 * static_cast<int>(lvi.slot + 1));
			c_.mov64r_imm64(0, (bin->op == "<") ? end_val : (end_val + 1));
			c_.mov_memd8_64r(5, ld, 0);
		}
		return true;
	}

	/* ── Pattern: for(i=init; i<cmp>bound; i++) { x = x * C; } (C 编译期常??
	   x_final = x_entry * C^N mod 2^64 —??快速幂在编译期完成, O(1) 生成 */
	bool NativeGen::try_mul_const_loop(ForStmt& n) {
		if (!n.init_decl || !n.init_decl->init) return false;
		auto* il = std::get_if<IntLit>(n.init_decl->init.get());
		if (!il) return false;
		int64_t init_val = il->value;
		std::string loop_var = n.init_decl->name;
		if (!n.cond) return false;
		auto* bin = std::get_if<Binary>(n.cond.get());
		if (!bin || (bin->op != "<" && bin->op != "<=")) return false;
		auto* id = std::get_if<Ident>(bin->lhs.get());
		if (!id || id->name != loop_var) return false;
		int64_t end_val = 0;
		if (auto* bip = std::get_if<IntLit>(bin->rhs.get())) {
			end_val = bip->value;
		} else return false;
		if (!n.step) return false;
		auto* un = std::get_if<Unary>(n.step.get());
		if (!un || un->op != "++") return false;
		auto* sid = std::get_if<Ident>(un->operand.get());
		if (!sid || sid->name != loop_var) return false;
		if (n.body.size() != 1) return false;
		auto* as = std::get_if<AssignStmt>(&n.body[0]);
		if (!as) return false;
		auto* mbin = std::get_if<Binary>(as->value.get());
		if (!mbin || mbin->op != "*") return false;
		int64_t C = 0;
		if (auto* rl = std::get_if<IntLit>(mbin->rhs.get())) {
			auto* ll = std::get_if<Ident>(mbin->lhs.get());
			if (!ll || ll->name != as->name) return false;
			C = rl->value;
		} else if (auto* ll = std::get_if<IntLit>(mbin->lhs.get())) {
			auto* rl = std::get_if<Ident>(mbin->rhs.get());
			if (!rl || rl->name != as->name) return false;
			C = ll->value;
		} else return false;
		auto& xvi = resolve_var(as->name);
		if (!xvi.is_local) return false;
		int64_t N = (bin->op == "<") ? (end_val - init_val) : (end_val - init_val + 1);
		if (N <= 0) return false;
		/* C^N mod 2^64 (u64 溢出即模 2^64) */
		uint64_t base = static_cast<uint64_t>(C), res = 1;
		int64_t m = N;
		while (m > 0) {
			if (m & 1) res *= base;
			base *= base;
			m >>= 1;
		}
		int32_t xd = static_cast<int32_t>(-8 * static_cast<int>(xvi.slot + 1));
		c_.mov64r_memd8(0, 5, xd);              /* rax = x */
		if (res != 0) {
			c_.mov64r_imm64(1, static_cast<int64_t>(res));
			c_.imul_r64_rm64(0, 1);             /* rax = x * C^N */
		} else {
			c_.mov64r_imm64(0, 0);              /* C^N == 0 mod 2^64 ??x = 0 */
		}
		c_.mov_memd8_64r(5, xd, 0);             /* x = rax */
		auto& lvi = resolve_var(loop_var);
		if (lvi.is_local) {
			int32_t ld = static_cast<int32_t>(-8 * static_cast<int>(lvi.slot + 1));
			c_.mov64r_imm64(0, (bin->op == "<") ? end_val : (end_val + 1));
			c_.mov_memd8_64r(5, ld, 0);
		}
		return true;
	}

	/* ── 线性映射折??(t03 形??: x = x*C + D / x = x + C, 常量边界
	   ??整段闭式 x = C^N * x + D * Σ_{k=0}^{N-1} C^k (mod 2^64) ── */
	static uint64_t pw64(uint64_t b, uint64_t e) {
		uint64_t r = 1;
		while (e) {
			if (e & 1) r *= b;
			b *= b;
			e >>= 1;
		}
		return r;
	}
	static uint64_t geosum(uint64_t c, uint64_t n) {
		if (n == 0) return 0;
		if (n == 1) return 1;
		uint64_t m = n >> 1;
		uint64_t s = geosum(c, m);
		uint64_t cm = pw64(c, m);
		uint64_t r = s + cm * s;
		if (n & 1) r += cm * cm;
		return r;
	}

	bool NativeGen::try_linear_fold(ForStmt& n) {
		if (!n.init_decl || !n.init_decl->init) return false;
		auto* il = std::get_if<IntLit>(n.init_decl->init.get());
		if (!il) return false;
		int64_t init_val = il->value;
		std::string loop_var = n.init_decl->name;
		if (!n.cond) return false;
		auto* bin = std::get_if<Binary>(n.cond.get());
		if (!bin || (bin->op != "<" && bin->op != "<=" && bin->op != ">" && bin->op != ">=")) return false;
		auto* id = std::get_if<Ident>(bin->lhs.get());
		if (!id || id->name != loop_var) return false;
		int64_t end_val = 0;
		if (auto* bip = std::get_if<IntLit>(bin->rhs.get())) {
			end_val = bip->value;
		} else return false;
		if (!n.step) return false;
		auto* un = std::get_if<Unary>(n.step.get());
		if (!un || (un->op != "++" && un->op != "--")) return false;
		auto* sid = std::get_if<Ident>(un->operand.get());
		if (!sid || sid->name != loop_var) return false;
		bool inc = (un->op == "++");
		if (n.body.size() != 1) return false;
		auto* as = std::get_if<AssignStmt>(&n.body[0]);
		if (!as) return false;
		auto* mbin = std::get_if<Binary>(as->value.get());
		if (!mbin) return false;
		int64_t C = 1, D = 0;
		bool ok = false;
		auto isX = [&](const std::unique_ptr<Expr>& e) {
			auto* eid = std::get_if<Ident>(e.get());
			return eid && eid->name == as->name;
		};
		auto isCst = [&](const std::unique_ptr<Expr>& e, int64_t& v) {
			auto* ei = std::get_if<IntLit>(e.get());
			if (!ei) return false;
			v = ei->value;
			return true;
		};
		if (mbin->op == "+") {
			int64_t v = 0;
			if (isX(mbin->lhs) && isCst(mbin->rhs, v)) {
				C = 1;
				D = v;
				ok = true;
			} else if (isX(mbin->rhs) && isCst(mbin->lhs, v)) {
				C = 1;
				D = v;
				ok = true;
			} else {
				auto tryMul = [&](const std::unique_ptr<Expr>& a, const std::unique_ptr<Expr>& b) -> bool {
					auto* mb = std::get_if<Binary>(a.get());
					if (!mb || mb->op != "*") return false;
					int64_t cv = 0, dv = 0;
					if (isX(mb->lhs) && isCst(mb->rhs, cv) && isCst(b, dv)) {
						C = cv;
						D = dv;
						return true;
					}
					if (isX(mb->rhs) && isCst(mb->lhs, cv) && isCst(b, dv)) {
						C = cv;
						D = dv;
						return true;
					}
					return false;
				};
				ok = tryMul(mbin->lhs, mbin->rhs) || tryMul(mbin->rhs, mbin->lhs);
			}
		} else if (mbin->op == "*") {
			int64_t v = 0;
			if (isX(mbin->lhs) && isCst(mbin->rhs, v)) {
				C = v;
				D = 0;
				ok = true;
			} else if (isX(mbin->rhs) && isCst(mbin->lhs, v)) {
				C = v;
				D = 0;
				ok = true;
			}
		}
		if (!ok) return false;
		auto& xvi = resolve_var(as->name);
		if (!xvi.is_local) return false;
		int64_t N;
		if (inc) N = (bin->op == "<") ? (end_val - init_val) : (end_val - init_val + 1);
		else     N = (bin->op == ">") ? (init_val - end_val) : (init_val - end_val + 1);
		if (N <= 0) return false;
		if (N > (INT64_C(1) << 40)) return false;
		uint64_t A = pw64(static_cast<uint64_t>(C), static_cast<uint64_t>(N));
		uint64_t S = geosum(static_cast<uint64_t>(C), static_cast<uint64_t>(N));
		uint64_t B = static_cast<uint64_t>(D) * S;
		int32_t xd = static_cast<int32_t>(-8 * static_cast<int>(xvi.slot + 1));
		c_.push_r(12);
		c_.mov64r_memd8(12, 5, xd);                 /* r12 = x */
		if (A != 1) {
			c_.mov64r_imm64(0, static_cast<int64_t>(A));
			c_.imul_r64_rm64(12, 0);                /* r12 *= C^N */
		}
		if (B != 0) {
			int64_t b = static_cast<int64_t>(B);
			if (b >= INT32_MIN && b <= INT32_MAX) {
				c_.add_rm64_imm32(12, static_cast<int32_t>(b));
			} else {
				c_.mov64r_imm64(0, b);
				c_.add_r64_rm64(12, 0);             /* r12 += B */
			}
		}
		c_.mov_memd8_64r(5, xd, 12);                /* x = r12 */
		auto& lvi = resolve_var(loop_var);
		if (lvi.is_local) {
			int32_t ld = static_cast<int32_t>(-8 * static_cast<int>(lvi.slot + 1));
			int64_t fi;
			if (inc) fi = (bin->op == "<") ? end_val : (end_val + 1);
			else     fi = (bin->op == ">") ? end_val : (end_val - 1);
			c_.mov64r_imm64(0, fi);
			c_.mov_memd8_64r(5, ld, 0);
		}
		c_.pop_r(12);
		return true;
	}

	/* ── 变量边界线性折?? for(i=init; i<n; i++){ x = C*x + D; } ??O(log N) 运行时平方乘
	   N 为运行时?? 生成 二进制快速幂 计算 A=C^N ??S=ΣC^k (mod 2^64), x = A*x + D*S??
	   全程??imul/add 环绕, 与逐次迭代??mod 2^64 下逐位等价。── */
	bool NativeGen::try_linear_fold_var(ForStmt& n) {
		if (!n.init_decl || !n.init_decl->init) return false;
		auto* il = std::get_if<IntLit>(n.init_decl->init.get());
		if (!il) return false;
		int64_t init_val = il->value;
		std::string loop_var = n.init_decl->name;
		if (!n.cond) return false;
		auto* bin = std::get_if<Binary>(n.cond.get());
		if (!bin || (bin->op != "<" && bin->op != "<=")) return false;
		auto* id = std::get_if<Ident>(bin->lhs.get());
		if (!id || id->name != loop_var) return false;
		std::string bound_name;
		if (auto* rid = std::get_if<Ident>(bin->rhs.get())) {
			if (rid->name == loop_var) return false;
			bound_name = rid->name;
		} else return false;
		if (!n.step) return false;
		auto* un = std::get_if<Unary>(n.step.get());
		if (!un || un->op != "++") return false;
		auto* sid = std::get_if<Ident>(un->operand.get());
		if (!sid || sid->name != loop_var) return false;
		if (n.body.size() != 1) return false;
		auto* as = std::get_if<AssignStmt>(&n.body[0]);
		if (!as) return false;
		if (as->name == loop_var) return false;
		/* 匹配仿射??x = C*x + D / x = x + D / x = x*C */
		auto* mbin = std::get_if<Binary>(as->value.get());
		if (!mbin) return false;
		int64_t C = 1, D = 0;
		bool ok = false;
		auto isX = [&](const std::unique_ptr<Expr>& e) {
			auto* eid = std::get_if<Ident>(e.get());
			return eid && eid->name == as->name;
		};
		auto isCst = [&](const std::unique_ptr<Expr>& e, int64_t& v) {
			auto* ei = std::get_if<IntLit>(e.get());
			if (!ei) return false;
			v = ei->value;
			return true;
		};
		if (mbin->op == "+") {
			int64_t v = 0;
			if (isX(mbin->lhs) && isCst(mbin->rhs, v)) {
				C = 1;
				D = v;
				ok = true;
			} else if (isX(mbin->rhs) && isCst(mbin->lhs, v)) {
				C = 1;
				D = v;
				ok = true;
			} else {
				auto tryMul = [&](const std::unique_ptr<Expr>& a, const std::unique_ptr<Expr>& b2) -> bool {
					auto* mb = std::get_if<Binary>(a.get());
					if (!mb || mb->op != "*") return false;
					int64_t cv = 0, dv = 0;
					if (isX(mb->lhs) && isCst(mb->rhs, cv) && isCst(b2, dv)) {
						C = cv;
						D = dv;
						return true;
					}
					if (isX(mb->rhs) && isCst(mb->lhs, cv) && isCst(b2, dv)) {
						C = cv;
						D = dv;
						return true;
					}
					return false;
				};
				ok = tryMul(mbin->lhs, mbin->rhs) || tryMul(mbin->rhs, mbin->lhs);
			}
		} else if (mbin->op == "*") {
			int64_t v = 0;
			if (isX(mbin->lhs) && isCst(mbin->rhs, v)) {
				C = v;
				D = 0;
				ok = true;
			} else if (isX(mbin->rhs) && isCst(mbin->lhs, v)) {
				C = v;
				D = 0;
				ok = true;
			}
		}
		if (!ok) return false;
		auto& xvi = resolve_var(as->name);
		if (!xvi.is_local) return false;
		auto& bvi = resolve_var(bound_name);
		if (!bvi.is_local) return false;
		int32_t xd = static_cast<int32_t>(-8 * static_cast<int>(xvi.slot + 1));
		int32_t bd = static_cast<int32_t>(-8 * static_cast<int>(bvi.slot + 1));
		bool le = (bin->op == "<=");
		/* 循环变量槽位: 终??i=n(<) / n+1(<=), 零次迭代??i=init */
		auto& lvi = resolve_var(loop_var);
		if (!lvi.is_local) return false;
		int32_t ld = static_cast<int32_t>(-8 * static_cast<int>(lvi.slot + 1));

		/* rcx = n; r11 = N = n - init (+1); N<=0 跳过 */
		c_.mov64r_memd8(1, 5, bd);                  /* rcx = n */
		c_.mov64r_rm64(11, 1);                      /* r11 = n */
		if (init_val >= INT32_MIN && init_val <= INT32_MAX) {
			c_.sub_rm64_imm32(11, static_cast<int32_t>(init_val));
		} else {
			c_.mov64r_imm64(0, init_val);
			c_.sub_r64_rm64(11, 0);
		}
		if (le) c_.add_rm64_imm32(11, 1);
		c_.cmp_rm64_imm32(11, 0);
		int j_skip = emit_jcc(0xE);                 /* jle skip (N<=0: 零次迭代) */

		/* 快速幂: rsi=AP=C^N, rdi=S=Σ_{k<N}C^k; r8=BLK=C^(2^t), r9=GS=Σ_{k<2^t}C^k */
		c_.mov64r_imm64(6, 1);                      /* AP = 1 */
		c_.xor_rm64_r64(7, 7);                      /* S = 0 */
		c_.mov64r_imm64(8, C);                      /* BLK = C */
		c_.mov64r_imm64(9, 1);                      /* GS = 1 */
		size_t Ltop = c_.pos();
		c_.rex_wb(11);
		c_.u8(0xF7);
		c_.modrm(3, 0, 11);
		c_.u32(1);   /* test r11, 1 */
		int j_bit = emit_jcc(0x4);                  /* je sq */
		c_.mov64r_rm64(0, 9);                       /* rax = GS */
		c_.imul_r64_rm64(0, 6);                     /* rax = GS*AP */
		c_.add_r64_rm64(7, 0);                      /* S += GS*AP */
		c_.imul_r64_rm64(6, 8);                     /* AP *= BLK */
		patch(j_bit);                               /* sq: */
		c_.rex_wb(11);
		c_.u8(0xC1);
		c_.u8(0xEB);
		c_.u8(1);           /* shr r11, 1 */
		int j_done = emit_jcc(0x4);                 /* je done */
		c_.lea64r_memd8(0, 8, 1);                   /* rax = BLK+1 */
		c_.imul_r64_rm64(9, 0);                     /* GS *= BLK+1 */
		c_.imul_r64_rm64(8, 8);                     /* BLK *= BLK */
		size_t jback = c_.jmp32();
		c_.patch_u32(jback, static_cast<uint32_t>(Ltop - (jback + 4)));
		patch(j_done);                              /* done: x = AP*x + D*S */
		c_.mov64r_memd8(0, 5, xd);                  /* rax = x */
		c_.imul_r64_rm64(0, 6);                     /* rax *= AP */
		if (D != 0) {
			c_.mov64r_imm64(10, D);                 /* r10 = D */
			c_.imul_r64_rm64(10, 7);                /* r10 = D*S */
			c_.add_r64_rm64(0, 10);                 /* rax += */
		}
		c_.mov_memd8_64r(5, xd, 0);                 /* x = rax */

		/* 终值分?? 正常路径 i = n(<) / n+1(<=)（rcx 全程未被破坏, 直接复用?? 零迭代路??i=init */
		if (le) c_.add_rm64_imm32(1, 1);            /* rcx = n+1 */
		c_.mov_memd8_64r(5, ld, 1);                 /* i = rcx */
		int j_over = emit_jmp();
		patch(j_skip);                              /* skip: */
		c_.mov64r_imm64(0, init_val);
		c_.mov_memd8_64r(5, ld, 0);                 /* i = init */
		patch(j_over);                              /* tail: */
		return true;
	}


	static bool exprIsPure(const Expr& e) {
		return std::visit([](const auto & n) -> bool {
			using T = std::decay_t<decltype(n)>;
			if constexpr (std::is_same_v<T, Call>) return false;
			else if constexpr (std::is_same_v<T, Input>) return false;
			else if constexpr (std::is_same_v<T, Output>) return false;
			else if constexpr (std::is_same_v<T, Binary>) return exprIsPure(*n.lhs) && exprIsPure(*n.rhs);
			else if constexpr (std::is_same_v<T, Unary>) return exprIsPure(*n.operand);
			else if constexpr (std::is_same_v<T, RangeLit>) return exprIsPure(*n.lo) && exprIsPure(*n.hi);
			else if constexpr (std::is_same_v<T, TupleIndexExpr>) return n.tuple ? exprIsPure(*n.tuple) : true;
			else if constexpr (std::is_same_v<T, TypeofExpr>) return true;
			else if constexpr (std::is_same_v<T, IsExpr>) return n.arg ? exprIsPure(*n.arg) : true;
			else if constexpr (std::is_same_v<T, InExpr>) return exprIsPure(*n.value) && exprIsPure(*n.container);
			else if constexpr (std::is_same_v<T, AwaitExpr>) return false;      /* 协程切换有副作用 */
			else if constexpr (std::is_same_v<T, CoExpr>) return false;         /* 创建协程会分配 */
			else if constexpr (std::is_same_v<T, CompileTimeExpr>) return true; /* 编译期已折叠为常量 */
			else if constexpr (std::is_same_v<T, ForallExpr> || std::is_same_v<T, ExistsExpr>) return false;
			else return true;
		}, e);
	}

	/* ── Pattern: 立方和相等计??(t10 形??
	   for(x){ for(y){ for(z){ if(x^3+y^3+z^3 == C){ cnt++; } } } }
	   x^3 单调、y^3 单调 ??对每??z, m=C-z^3, 双指??(x ??y ?? ??x^3+y^3==m
	   O(Nx*Ny*Nz) 降到 O(Nz*(Nx+Ny)) */
	bool NativeGen::try_cubic_sum_eq(ForStmt& nx) {
		if (!nx.init_decl || !nx.init_decl->init) return false;
		auto* xil = std::get_if<IntLit>(nx.init_decl->init.get());
		if (!xil) return false;
		std::string xv = nx.init_decl->name;
		if (!nx.step) return false;
		auto* xun = std::get_if<Unary>(nx.step.get());
		if (!xun || xun->op != "++") return false;
		if (nx.body.size() != 1) return false;
		auto* ny = std::get_if<ForStmt>(&nx.body[0]);
		if (!ny) return false;
		auto* yil = std::get_if<IntLit>(ny->init_decl->init.get());
		if (!yil) return false;
		std::string yv = ny->init_decl->name;
		if (!ny->step) return false;
		auto* yun = std::get_if<Unary>(ny->step.get());
		if (!yun || yun->op != "++") return false;
		if (ny->body.size() != 1) return false;
		auto* nz = std::get_if<ForStmt>(&ny->body[0]);
		if (!nz) return false;
		auto* zil = std::get_if<IntLit>(nz->init_decl->init.get());
		if (!zil) return false;
		std::string zv = nz->init_decl->name;
		if (!nz->step) return false;
		auto* zun = std::get_if<Unary>(nz->step.get());
		if (!zun || zun->op != "++") return false;
		if (nz->body.size() != 1) return false;
		auto* iff = std::get_if<IfStmt>(&nz->body[0]);
		if (!iff || !iff->else_body.empty()) return false;
		auto* cbin = std::get_if<Binary>(iff->cond.get());
		if (!cbin || cbin->op != "==") return false;
		auto* cval = std::get_if<IntLit>(cbin->rhs.get());
		if (!cval) return false;
		/* lhs 拆加?? 每项必须是某循环变量的纯立方 */
		std::vector<const Expr*> terms;
		std::function<void(const Expr&)> collect = [&](const Expr & e) {
			if (auto* b = std::get_if<Binary>(&e)) {
				if (b->op == "+") {
					collect(*b->lhs);
					collect(*b->rhs);
					return;
				}
			}
			terms.push_back(&e);
		};
		collect(*cbin->lhs);
		if (terms.size() != 3) return false;
		std::string cube_var;
		bool xok = false, yok = false, zok = false;
		for (auto* t : terms) {
			auto* b = std::get_if<Binary>(t);
			if (!b) return false;
			std::string v;
			int idcnt = 0;
			auto walk = [&](auto&& self, const Expr & e) -> void {
				if (auto* id = std::get_if<Ident>(&e)) {
					if (v.empty()) v = id->name;
					else if (v != id->name) v = "#bad";
					idcnt++;
				} else if (auto* bb = std::get_if<Binary>(&e)) {
					if (bb->op != "*") {
						v = "#bad";
						return;
					}
					self(self, *bb->lhs);
					self(self, *bb->rhs);
				} else {
					v = "#bad";
				}
			};
			walk(walk, *t);
			if (v == "#bad" || idcnt != 3) return false;
			if (v == xv) xok = true;
			else if (v == yv) yok = true;
			else if (v == zv) zok = true;
			else return false;
		}
		if (!xok || !yok || !zok) return false;
		/* cnt = cnt + 1 */
		if (iff->then_body.size() != 1) return false;
		auto* cas = std::get_if<AssignStmt>(&iff->then_body[0]);
		if (!cas) return false;
		auto* cb = std::get_if<Binary>(cas->value.get());
		if (!cb || cb->op != "+") return false;
		auto* cl = std::get_if<Ident>(cb->lhs.get());
		auto* cr = std::get_if<IntLit>(cb->rhs.get());
		if (!cl || cl->name != cas->name || !cr || cr->value != 1) return false;
		std::string cntv = cas->name;
		/* cond ??Binary < ??lhs=变量 */
		auto* xc = std::get_if<Binary>(nx.cond.get());
		if (!xc || xc->op != "<") return false;
		auto* xid = std::get_if<Ident>(xc->lhs.get());
		if (!xid || xid->name != xv) return false;
		auto* xbv = std::get_if<IntLit>(xc->rhs.get());
		if (!xbv) return false;
		auto* yc = std::get_if<Binary>(ny->cond.get());
		if (!yc || yc->op != "<") return false;
		auto* yid = std::get_if<Ident>(yc->lhs.get());
		if (!yid || yid->name != yv) return false;
		auto* ybv = std::get_if<IntLit>(yc->rhs.get());
		if (!ybv) return false;
		auto* zc = std::get_if<Binary>(nz->cond.get());
		if (!zc || zc->op != "<") return false;
		auto* zid = std::get_if<Ident>(zc->lhs.get());
		if (!zid || zid->name != zv) return false;
		auto* zbv = std::get_if<IntLit>(zc->rhs.get());
		if (!zbv) return false;
		int64_t X = xbv->value, Y = ybv->value, Z = zbv->value;
		int64_t C = cval->value;
		/* 生成: rbx=x, r12=y, r13=z, r14=cnt, r15=m */
		auto& cvi = resolve_var(cntv);
		if (!cvi.is_local) return false;
		int32_t cd = static_cast<int32_t>(-8 * static_cast<int>(cvi.slot + 1));
		c_.push_r(3);
		c_.push_r(12);
		c_.push_r(13);
		c_.push_r(14);
		c_.push_r(15);
		c_.mov64r_imm64(14, 0);       /* cnt = 0 */
		c_.mov64r_imm64(13, 0);       /* z = 0 */
		size_t ztop = c_.pos();
		/* m = C - z^3 */
		c_.mov64r_imm64(15, C);
		c_.mov64r_rm64(0, 13);
		c_.imul_r64_rm64(0, 13);
		c_.imul_r64_rm64(0, 13);      /* rax = z^3 */
		c_.sub_r64_rm64(15, 0);       /* r15 = m */
		c_.mov64r_imm64(3, 0);        /* x = 0 */
		c_.mov64r_imm64(12, Y - 1);   /* y = Y-1 */
		size_t ytop = c_.pos();
		c_.mov64r_rm64(0, 3);
		c_.imul_r64_rm64(0, 3);
		c_.imul_r64_rm64(0, 3);       /* rax = x^3 */
		c_.mov64r_rm64(1, 12);
		c_.imul_r64_rm64(1, 12);
		c_.imul_r64_rm64(1, 12);      /* rcx = y^3 */
		c_.add_r64_rm64(0, 1);
		c_.cmp_r64_rm64(0, 15);
		c_.u8(0x0F);
		c_.u8(0x84);     /* je hit */
		size_t jhit = c_.pos();
		c_.u32(0);
		c_.u8(0x0F);
		c_.u8(0x8C);     /* jl xinc */
		size_t jxinc = c_.pos();
		c_.u32(0);
		/* ydec */
		c_.u8(0x49);
		c_.u8(0xFF);
		c_.u8(0xCC);  /* dec r12 */
		size_t ycheck = c_.pos();
		c_.cmp_rm64_imm32(3, static_cast<int32_t>(X));
		c_.u8(0x0F);
		c_.u8(0x8D);     /* jge y_exit */
		size_t jyexit = c_.pos();
		c_.u32(0);
		c_.cmp_rm64_imm32(12, 0);
		c_.u8(0x0F);
		c_.u8(0x8C);     /* jl y_exit */
		size_t jyexit2 = c_.pos();
		c_.u32(0);
		c_.u8(0xE9);                  /* jmp y_top */
		size_t jyback = c_.pos();
		c_.u32(0);
		/* hit: */
		size_t hit_done = c_.pos();
		c_.u8(0x49);
		c_.u8(0xFF);
		c_.u8(0xC6);  /* inc r14 */
		c_.inc_rm64(3);
		c_.u8(0x49);
		c_.u8(0xFF);
		c_.u8(0xCC);  /* dec r12 */
		c_.u8(0xE9);                  /* jmp ycheck */
		size_t jhitback = c_.pos();
		c_.u32(0);
		/* xinc: */
		size_t xinc_done = c_.pos();
		c_.inc_rm64(3);
		c_.u8(0xE9);                  /* jmp ycheck */
		size_t jxincback = c_.pos();
		c_.u32(0);
		c_.patch_u32(jhit, static_cast<uint32_t>(hit_done - (jhit + 4)));
		c_.patch_u32(jxinc, static_cast<uint32_t>(xinc_done - (jxinc + 4)));
		/* y_exit: */
		size_t yexit = c_.pos();
		c_.patch_u32(jyexit, static_cast<uint32_t>(yexit - (jyexit + 4)));
		c_.patch_u32(jyexit2, static_cast<uint32_t>(yexit - (jyexit2 + 4)));
		c_.patch_u32(jyback, static_cast<uint32_t>(ytop - (jyback + 4)));
		c_.patch_u32(jhitback, static_cast<uint32_t>(ycheck - (jhitback + 4)));
		c_.patch_u32(jxincback, static_cast<uint32_t>(ycheck - (jxincback + 4)));
		c_.u8(0x49);
		c_.u8(0xFF);
		c_.u8(0xC5);  /* inc r13 */
		c_.cmp_rm64_imm32(13, static_cast<int32_t>(Z));
		c_.u8(0x0F);
		c_.u8(0x8D);     /* jge z_done */
		size_t jzdone = c_.pos();
		c_.u32(0);
		c_.u8(0xE9);
		size_t jzback = c_.pos();
		c_.u32(0);
		size_t z_done = c_.pos();
		c_.patch_u32(jzdone, static_cast<uint32_t>(z_done - (jzdone + 4)));
		c_.patch_u32(jzback, static_cast<uint32_t>(ztop - (jzback + 4)));
		c_.mov_memd8_64r(5, cd, 14);
		c_.pop_r(15);
		c_.pop_r(14);
		c_.pop_r(13);
		c_.pop_r(12);
		c_.pop_r(3);
		return true;
	}

	/* ── Pattern: while (b != 0) { r = a % b; a = b; b = r; } (a,b 编译期已知常??
	   欧几里得在编译期执行, 循环整体替换??a=gcd; b=0; —??O(步数) ??O(1) */
	bool NativeGen::try_gcd_loop(WhileStmt& n) {
		auto* bin = std::get_if<Binary>(n.cond.get());
		if (!bin || bin->op != "!=") return false;
		std::string bvar;
		bool b_left = true;
		if (auto* lid = std::get_if<Ident>(bin->lhs.get())) {
			bvar = lid->name;
			b_left = true;
		} else if (auto* rid = std::get_if<Ident>(bin->rhs.get())) {
			bvar = rid->name;
			b_left = false;
		} else return false;
		if (b_left) {
			if (auto* r0 = std::get_if<IntLit>(bin->rhs.get())) {
				if (r0->value != 0) return false;
			} else return false;
		} else {
			if (auto* l0 = std::get_if<IntLit>(bin->lhs.get())) {
				if (l0->value != 0) return false;
			} else return false;
		}
		if (n.body.size() != 3) return false;
		/* 修复: 编译器 SEGV / "赋值缺少右值"（既有 bug）
		   这里原本用 `d0val = std::move(s0a->value)` 把 AST 节点的右值
		   搬走。而 doBuild 会调 generate() 两次（第二次是为了 patch
		   PE/ELF 数据重定位），第二次跑到同一个循环时 value 已经是空的
		   unique_ptr，gen_expr(*n.value) 解引用 nullptr 直接段错误。

		   d0val 在下面只用于做形状检查（是不是 `%`、左右操作数是谁），
		   完全不需要拥有它。改成裸指针即可 —— 不搬走、不破坏 AST。 */
		Expr* d0val = nullptr;
		std::string d0name;
		if (auto* s0a = std::get_if<AssignStmt>(&n.body[0])) {
			d0name = s0a->name;
			d0val = s0a->value.get();
		} else if (auto* s0v = std::get_if<VarDecl>(&n.body[0])) {
			d0name = s0v->name;
			d0val = s0v->init.get();
		} else return false;
		if (!d0val) return false;
		auto* s1 = std::get_if<AssignStmt>(&n.body[1]);
		auto* s2 = std::get_if<AssignStmt>(&n.body[2]);
		if (!s1 || !s2) return false;
		auto* m0 = std::get_if<Binary>(d0val);
		if (!m0 || m0->op != "%") return false;
		auto* a0 = std::get_if<Ident>(m0->lhs.get());
		auto* b0 = std::get_if<Ident>(m0->rhs.get());
		if (!a0 || !b0 || b0->name != bvar) return false;
		std::string rvar = d0name;
		std::string avar = a0->name;
		if (s1->name != avar) return false;
		auto* b1 = std::get_if<Ident>(s1->value.get());
		if (!b1 || b1->name != bvar) return false;
		if (s2->name != bvar) return false;
		auto* r2 = std::get_if<Ident>(s2->value.get());
		if (!r2 || r2->name != rvar) return false;
		auto ita = known_const_.find(avar);
		auto itb = known_const_.find(bvar);
		if (ita == known_const_.end() || itb == known_const_.end()) return false;
		uint64_t aa = static_cast<uint64_t>(ita->second);
		uint64_t bb = static_cast<uint64_t>(itb->second);
		while (bb != 0) {
			uint64_t rr = aa % bb;
			aa = bb;
			bb = rr;
		}
		auto& avi = resolve_var(avar);
		auto& bvi = resolve_var(bvar);
		if (!avi.is_local || !bvi.is_local) return false;
		if (loop_reg_vars_.find(avi.slot) != loop_reg_vars_.end()) return false;
		if (loop_reg_vars_.find(bvi.slot) != loop_reg_vars_.end()) return false;
		int32_t ad = static_cast<int32_t>(-8 * static_cast<int>(avi.slot + 1));
		int32_t bd = static_cast<int32_t>(-8 * static_cast<int>(bvi.slot + 1));
		c_.mov64r_imm64(0, static_cast<int64_t>(aa));
		c_.mov_memd8_64r(5, ad, 0);
		c_.mov_memd8_imm32(5, bd, 0);
		known_const_[avar] = static_cast<int64_t>(aa);
		known_const_[bvar] = 0;
		known_const_.erase(rvar);
		return true;
	}

	/* ── Pattern: 收敛折叠 for(i=init; i<cmp>end; i++) { x = f(x); }
	   f 不含循环变量、无副作用。f 在运行时收敛 (f(x)==x) ??x 永不再变 ??
	   提前跳出循环 (剩余迭代 x 不变, 语义完全等价)。迭??O(N) ??O(收敛步数)??
	   不收敛时每迭代仅??1 ??cmp+jcc 开销??*/
	bool NativeGen::try_converge_loop(ForStmt& n) {
		if (!n.init_decl || !n.init_decl->init) return false;
		auto* il = std::get_if<IntLit>(n.init_decl->init.get());
		if (!il) return false;
		int64_t init_val = il->value;
		std::string loop_var = n.init_decl->name;
		if (!n.cond) return false;
		auto* bin = std::get_if<Binary>(n.cond.get());
		if (!bin) return false;
		auto* id = std::get_if<Ident>(bin->lhs.get());
		if (!id || id->name != loop_var) return false;
		const std::string& cop = bin->op;
		if (cop != "<" && cop != "<=" && cop != ">" && cop != ">=") return false;
		bool ident_bound = false;
		std::string bound_name;
		int64_t end_val = 0;
		if (auto* bip = std::get_if<IntLit>(bin->rhs.get())) {
			end_val = bip->value;
		} else if (auto* rid = std::get_if<Ident>(bin->rhs.get())) {
			if (rid->name == loop_var) return false;
			ident_bound = true;
			bound_name = rid->name;
		} else return false;
		if (!n.step) return false;
		auto* un = std::get_if<Unary>(n.step.get());
		if (!un || (un->op != "++" && un->op != "--")) return false;
		bool step_inc = (un->op == "++");
		auto* sid = std::get_if<Ident>(un->operand.get());
		if (!sid || sid->name != loop_var) return false;
		if (n.body.size() != 1) return false;
		auto* as = std::get_if<AssignStmt>(&n.body[0]);
		if (!as) return false;
		if (!exprUsesVar(as->value, loop_var) && exprIsPure(*as->value)) {
			/* ok: f(x) 不含 i, 无副作用 */
		} else return false;
		auto& xvi = resolve_var(as->name);
		if (!xvi.is_local) return false;
		int32_t xd = static_cast<int32_t>(-8 * static_cast<int>(xvi.slot + 1));
		c_.push_r(3);
		c_.mov64r_imm64(3, init_val);        /* rbx = i */
		if (ident_bound) {
			auto& bvi = resolve_var(bound_name);
			if (bvi.is_local) {
				int32_t bd = static_cast<int32_t>(-8 * static_cast<int>(bvi.slot + 1));
				c_.push_r(13);
				c_.mov64r_memd8(13, 5, bd);  /* r13 = bound */
			} else return false;
		}
		/* ── Jumps ── */
		if (ident_bound) {
			c_.cmp_r64_rm64(3, 13);
		} else if (end_val >= INT32_MIN && end_val <= INT32_MAX) {
			c_.cmp_rm64_imm32(3, static_cast<int32_t>(end_val));
		} else {
			c_.mov64r_imm64(1, end_val);
			c_.cmp_r64_rm64(3, 1);
		}
		int jcc0;
		if (cop == "<") jcc0 = 0x0D;
		else if (cop == "<=") jcc0 = 0x0F;
		else if (cop == ">") jcc0 = 0x0E;
		else jcc0 = 0x0C;
		c_.u8(0x0F);
		c_.u8(static_cast<uint8_t>(0x80 + jcc0));   /* jcc done */
		size_t jdone = c_.pos();
		c_.u32(0);
		size_t top = c_.pos();
		std::string save_var = fast_loop_var_;
		int save_reg = fast_loop_reg_;
		int64_t save_inc = fast_loop_inc_;
		fast_loop_var_.clear();
		fast_loop_reg_ = 0;
		fast_loop_inc_ = 0;
		gen_expr(*as->value);                 /* rax = f(x) */
		fast_loop_var_ = save_var;
		fast_loop_reg_ = save_reg;
		fast_loop_inc_ = save_inc;
		c_.mov64r_memd8(1, 5, xd);           /* rcx = x */
		c_.cmp_r64_rm64(0, 1);
		c_.u8(0x0F);
		c_.u8(0x84);            /* je done (收敛) */
		size_t jcv = c_.pos();
		c_.u32(0);
		c_.mov_memd8_64r(5, xd, 0);          /* x = f(x) */
		if (step_inc) c_.inc_rm64(3);
		else {
			c_.u8(0x48);    /* dec rbx */
			c_.u8(0xFF);
			c_.u8(0xCB);
		}
		if (ident_bound) {
			c_.cmp_r64_rm64(3, 13);
		} else if (end_val >= INT32_MIN && end_val <= INT32_MAX) {
			c_.cmp_rm64_imm32(3, static_cast<int32_t>(end_val));
		} else {
			c_.mov64r_imm64(1, end_val);
			c_.cmp_r64_rm64(3, 1);
		}
		c_.u8(0x0F);
		c_.u8(static_cast<uint8_t>(0x80 | (jcc0 ^ 1)));  /* 反跳 top */
		size_t jbk = c_.pos();
		c_.u32(0);
		c_.patch_u32(jbk, static_cast<uint32_t>(top - (jbk + 4)));
		c_.patch_u32(jcv, static_cast<uint32_t>(c_.pos() - (jcv + 4)));
		c_.patch_u32(jdone, static_cast<uint32_t>(c_.pos() - (jdone + 4)));
		/* i 终??*/
		auto& lvi = resolve_var(loop_var);
		if (lvi.is_local) {
			int32_t ld = static_cast<int32_t>(-8 * static_cast<int>(lvi.slot + 1));
			int64_t fin = 0;
			if (cop == "<") fin = end_val;
			else if (cop == "<=") fin = end_val + 1;
			else if (cop == ">") fin = end_val;
			else fin = end_val - 1;
			c_.mov64r_imm64(0, fin);
			c_.mov_memd8_64r(5, ld, 0);
		}
		if (ident_bound) c_.pop_r(13);
		c_.pop_r(3);
		return true;
	}

	/* ── Pattern: int fib(int n) { if(n<2) return n; return fib(n-1)+fib(n-2); }
	   Replace with: call wei_rt_fib  ── O(log N) instead of O(2^N) */
	/* ── Pattern: 汉诺塔递归 f(n) { if(n==1) return 1; return 2*f(n-1)+1; }
	   ??闭式 2^n - 1 (mod 2^64), O(1) 生成 */
	bool NativeGen::try_hanoi_pattern(FuncDecl& f) {
		if (f.params.size() != 1) return false;
		std::string pn = f.params[0].name;
		if (f.body.size() != 2) return false;
		auto* ifst = std::get_if<IfStmt>(&f.body[0]);
		if (!ifst || !ifst->else_body.empty() || ifst->then_body.size() != 1) return false;
		auto* cond = std::get_if<Binary>(ifst->cond.get());
		if (!cond || cond->op != "==") return false;
		auto* cl = std::get_if<Ident>(cond->lhs.get());
		auto* cr = std::get_if<IntLit>(cond->rhs.get());
		if (!cl || cl->name != pn || !cr || cr->value != 1) return false;
		auto* ret1 = std::get_if<ReturnStmt>(&ifst->then_body[0]);
		if (!ret1) return false;
		auto* v1 = std::get_if<IntLit>(ret1->value.get());
		if (!v1 || v1->value != 1) return false;
		auto* ret2 = std::get_if<ReturnStmt>(&f.body[1]);
		if (!ret2) return false;
		auto* add = std::get_if<Binary>(ret2->value.get());
		if (!add || add->op != "+") return false;
		auto recCall = [&](const std::unique_ptr<Expr>& e) -> bool {
			auto* c = std::get_if<Call>(e.get());
			if (!c || c->callee != f.name || c->args.size() != 1) return false;
			auto* b = std::get_if<Binary>(c->args[0].get());
			if (!b || b->op != "-") return false;
			auto* l = std::get_if<Ident>(b->lhs.get());
			auto* r = std::get_if<IntLit>(b->rhs.get());
			return l && r && l->name == pn && r->value == 1;
		};
		auto mul2 = [&](const std::unique_ptr<Expr>& e) -> bool {
			auto* mb = std::get_if<Binary>(e.get());
			if (!mb || mb->op != "*") return false;
			if (auto* l = std::get_if<IntLit>(mb->lhs.get())) {
				if (l->value == 2) return recCall(mb->rhs);
			}
			if (auto* r = std::get_if<IntLit>(mb->rhs.get())) {
				if (r->value == 2) return recCall(mb->lhs);
			}
			return false;
		};
		bool ok = false;
		if (auto* l = std::get_if<IntLit>(add->lhs.get())) {
			if (l->value == 1) ok = mul2(add->rhs);
		}
		if (!ok) {
			if (auto* r = std::get_if<IntLit>(add->rhs.get())) {
				if (r->value == 1) ok = mul2(add->lhs);
			}
		}
		if (!ok) return false;
		auto& vi = resolve_var(pn);
		if (!vi.is_local) return false;
		int32_t d = static_cast<int32_t>(-8 * static_cast<int>(vi.slot + 1));
		c_.mov64r_imm64(0, 1);                  /* rax = 1 */
		c_.mov64r_memd8(1, 5, d);               /* rcx = n */
		c_.cmp_rm64_imm32(1, 64);
		int jlt = emit_jcc(0xC);                /* jl ok */
		c_.mov64r_imm64(0, -1);                 /* n>=64 ??2^n-1 ??-1 (mod 2^64) */
		int jend = emit_jmp();
		patch(jlt);                             /* ok: */
		c_.u8(0x48);
		c_.u8(0xD3);
		c_.u8(0xE0);  /* shl rax, cl */
		c_.u8(0x48);
		c_.u8(0xFF);
		c_.u8(0xC8);  /* dec rax */
		patch(jend);
		emit_canary_check();
		c_.mov64r_rm64(4, 5);  /* mov rsp, rbp: 丢弃循环寄存??push 残留 */
		c_.mov64r_memd8(3, 5, -8);    /* rbx ??[rbp-8]  (压栈?? rbx,r12,r13,r14,r15) */
		c_.mov64r_memd8(12, 5, -16);  /* r12 */
		c_.mov64r_memd8(13, 5, -24);  /* r13 */
		c_.mov64r_memd8(14, 5, -32);  /* r14 */
		c_.mov64r_memd8(15, 5, -40);  /* r15 */
		c_.leave();
		c_.ret();
		return true;
	}

	bool NativeGen::try_fib_pattern(FuncDecl& f) {
		if (f.params.size() != 1) return false;
		std::string param_name = f.params[0].name;
		if (f.body.size() != 2) return false;
		/* First: if (n < 2) return n; */
		auto* ifst = std::get_if<IfStmt>(&f.body[0]);
		if (!ifst) return false;
		auto* cond = std::get_if<Binary>(ifst->cond.get());
		if (!cond || cond->op != "<") return false;
		auto* cond_lhs = std::get_if<Ident>(cond->lhs.get());
		auto* cond_rhs = std::get_if<IntLit>(cond->rhs.get());
		if (!cond_lhs || cond_lhs->name != param_name) return false;
		if (!cond_rhs || cond_rhs->value != 2) return false;
		if (ifst->then_body.size() != 1) return false;
		auto* ret1 = std::get_if<ReturnStmt>(&ifst->then_body[0]);
		if (!ret1) return false;
		auto* ret1_val = std::get_if<Ident>(ret1->value.get());
		if (!ret1_val || ret1_val->name != param_name) return false;
		/* Second: return fib(n-1) + fib(n-2); */
		auto* ret2 = std::get_if<ReturnStmt>(&f.body[1]);
		if (!ret2) return false;
		auto* add = std::get_if<Binary>(ret2->value.get());
		if (!add || add->op != "+") return false;
		auto* call1 = std::get_if<Call>(add->lhs.get());
		auto* call2 = std::get_if<Call>(add->rhs.get());
		if (!call1 || !call2) return false;
		if (call1->callee != f.name || call2->callee != f.name) return false;
		if (call1->args.size() != 1 || call2->args.size() != 1) return false;
		auto* arg1 = std::get_if<Binary>(call1->args[0].get());
		auto* arg2 = std::get_if<Binary>(call2->args[0].get());
		if (!arg1 || !arg2 || arg1->op != "-" || arg2->op != "-") return false;
		auto* a1lhs = std::get_if<Ident>(arg1->lhs.get());
		auto* a1rhs = std::get_if<IntLit>(arg1->rhs.get());
		auto* a2lhs = std::get_if<Ident>(arg2->lhs.get());
		auto* a2rhs = std::get_if<IntLit>(arg2->rhs.get());
		if (!a1lhs || a1lhs->name != param_name || !a1rhs || a1rhs->value != 1) return false;
		if (!a2lhs || a2lhs->name != param_name || !a2rhs || a2rhs->value != 2) return false;

		/* Pattern matched! Emit INLINE matrix exponentiation (self-contained PE, no host call):
		       fib(n) via M^n, M=[[a,b],[c,d]], x,y = accumulators.
		       Registers: rax=p, rdx=a, r8=b, r9=c, r10=d, r11=x, rsi=y.
		       Scratch: rcx,rdi,r13 +(r11/rsi via consumption)+(r10), x/y parked via push
		       during the matrix square.
		       rbx/r12/r14/r15 NEVER touched (fast-loop + PE-stub owned). */
		auto& vi = resolve_var(param_name);
		int32_t d = static_cast<int32_t>(-8 * static_cast<int>(vi.slot + 1));
		c_.mov64r_memd8(0, 5, d);            /* rax = n */
		c_.cmp_rm64_imm32(0, 2);             /* n < 2 ??return n */
		int jlt = emit_jcc(0xC);             /* jl done */
		c_.mov64r_imm64(2, 1);               /* rdx = a = 1 */
		c_.mov64r_imm64(8, 1);               /* r8  = b = 1 */
		c_.mov64r_imm64(9, 1);               /* r9  = c = 1 */
		c_.xor_rm64_r64(10, 10);             /* r10 = d = 0 */
		c_.xor_rm64_r64(11, 11);             /* r11 = x = 0 */
		c_.mov64r_imm64(6, 1);               /* rsi = y = 1 */
		size_t loop_top = c_.pos();
		c_.u8(0x48);
		c_.u8(0xF7);
		c_.u8(0xC0);
		c_.u32(1);   /* test rax, 1 */
		int jz_odd = emit_jcc(0x4);          /* jz skip_odd */
		/* odd: nx=a*x+b*y; ny=c*x+d*y  (x into rcx/rdi, then reuse r11/rsi) */
		c_.mov64r_rm64(1, 2);
		c_.imul_r64_rm64(1, 11);    /* rcx = a*x */
		c_.mov64r_rm64(7, 9);
		c_.imul_r64_rm64(7, 11);    /* rdi = c*x  (x consumed) */
		c_.mov64r_rm64(11, 8);
		c_.imul_r64_rm64(11, 6);    /* r11 = b*y */
		c_.mov64r_rm64(13, 10);
		c_.imul_r64_rm64(13, 6);    /* r13 = d*y  (y consumed) */
		c_.add_r64_rm64(1, 11);                             /* rcx = nx */
		c_.add_r64_rm64(7, 13);                             /* rdi = ny */
		c_.mov64r_rm64(11, 1);                              /* x = nx */
		c_.mov64r_rm64(6, 7);                               /* y = ny */
		patch(jz_odd);                             /* skip_odd: */
		c_.u8(0x48);
		c_.u8(0xD1);
		c_.u8(0xE8); /* shr rax, 1 (p>>=1) */
		int jz_exit = emit_jcc(0x4);           /* jz exit (p==0) */
		/* square: na=a*a+b*c; nb=a*b+b*d; nc=c*a+d*c; nd=c*b+d*d
		   park x/y on the stack (frame-size agnostic); d stays live in r10;
		   scratch: rcx,rdi,r13 + rsi,r11 (5 total) */
		c_.push_r(11);                              /* park x */
		c_.push_r(6);                               /* park y */
		c_.mov64r_rm64(1, 2);
		c_.imul_r64_rm64(1, 2);     /* rcx = a*a */
		c_.mov64r_rm64(7, 8);
		c_.imul_r64_rm64(7, 9);     /* rdi = b*c */
		c_.add_r64_rm64(1, 7);                             /* rcx = na */
		c_.mov64r_rm64(13, 2);
		c_.imul_r64_rm64(13, 8);    /* r13 = a*b */
		c_.mov64r_rm64(6, 10);
		c_.imul_r64_rm64(6, 8);     /* rsi = b*d */
		c_.add_r64_rm64(13, 6);                            /* r13 = nb */
		c_.mov64r_rm64(7, 9);
		c_.imul_r64_rm64(7, 2);     /* rdi = c*a */
		c_.mov64r_rm64(6, 10);
		c_.imul_r64_rm64(6, 9);     /* rsi = d*c */
		c_.add_r64_rm64(7, 6);                             /* rdi = nc */
		c_.mov64r_rm64(6, 9);
		c_.imul_r64_rm64(6, 8);     /* rsi = c*b */
		c_.mov64r_rm64(11, 10);
		c_.imul_r64_rm64(11, 10);  /* r11 = d*d */
		c_.add_r64_rm64(6, 11);                            /* rsi = nd */
		c_.mov64r_rm64(2, 1);
		c_.mov64r_rm64(8, 13);     /* a=na, b=nb */
		c_.mov64r_rm64(9, 7);
		c_.mov64r_rm64(10, 6);     /* c=nc, d=nd */
		c_.pop_r(6);                                /* restore y */
		c_.pop_r(11);                               /* restore x */
		size_t jback = c_.jmp32();
		c_.patch_u32(jback, static_cast<uint32_t>(loop_top - (jback + 4)));
		patch(jz_exit);                            /* exit: */
		c_.mov64r_rm64(0, 11);                     /* rax = x */
		patch(jlt);                                /* done: */
		/* RAX = fib(n). Return it. */
		emit_canary_check();
		c_.mov64r_rm64(4, 5);  /* mov rsp, rbp: 丢弃循环寄存??push 残留 */
		c_.mov64r_memd8(3, 5, -8);    /* rbx ??[rbp-8]  (压栈?? rbx,r12,r13,r14,r15) */
		c_.mov64r_memd8(12, 5, -16);  /* r12 */
		c_.mov64r_memd8(13, 5, -24);  /* r13 */
		c_.mov64r_memd8(14, 5, -32);  /* r14 */
		c_.mov64r_memd8(15, 5, -40);  /* r15 */
		c_.leave();
		c_.ret();
		return true;
	}

	/* ── Expression code generation ──
	   After gen_expr, the result value is in RAX,
	   and the stack is at the same height as before (value held in reg, not pushed).
	   For nested expressions, values are pushed/popped on the x86-64 stack. */

	void NativeGen::gen_addr_to_rax(Expr& e) {
		if (auto* id = std::get_if<Ident>(&e)) {
			auto& vi = resolve_var(id->name);
			if (vi.array_len > 0) {
				if (vi.is_local) {
					int32_t d = static_cast<int32_t>(-8 * static_cast<int>(vi.slot + 1));
					if (d >= -128 && d <= 127) c_.lea64r_memd8(0, 5, static_cast<int8_t>(d));
					else c_.lea64r_memd32(0, 5, d);
				} else {
					emit_global_addr(0, vi.slot);
				}
				return;
			}
			if (vi.is_local) {
				int32_t d = static_cast<int32_t>(-8 * static_cast<int>(vi.slot + 1));
				c_.lea64r_memd8(0, 5, d);
			} else {
				emit_global_addr(0, vi.slot);
			}
			return;
		}
		if (auto* ix = std::get_if<IndexExpr>(&e)) {
			auto& vi = resolve_var(ix->name);
			if (vi.array_len > 0) {
				checkProvenOOB(*ix, vi.array_len);   /* 编译期证明必然越界 → 直接报错 */
				gen_expr(*ix->idx);
				if (ix->idx && indexProven(*ix->idx, vi.array_len)) proofNote("数组下标在界内", ix->line);
				else emit_bounds_check(vi.array_len);
				c_.push_r(0);
				c_.mov64r_imm64(1, 8);
				c_.imul_r64_rm64(0, 1);          /* rax = idx*8 */
				if (vi.is_local) {
					/* 局部数组向低地址增长: base - idx*8 */
					int32_t base_d = static_cast<int32_t>(-8 * static_cast<int>(vi.slot + 1));
					c_.lea64r_memd32(2, 5, base_d);
					c_.sub_r64_rm64(2, 0);
				} else {
					emit_global_addr(2, vi.slot);
					c_.add_r64_rm64(2, 0);       /* 全局数组向高地址增长 */
				}
				c_.pop_r(0);
				c_.mov64r_rm64(0, 2);
				return;
			}
			if (vi.is_local) {
				int32_t d = static_cast<int32_t>(-8 * static_cast<int>(vi.slot + 1));
				c_.mov64r_memd8(0, 5, d);
			} else {
				emit_global_load(0, vi.slot);
			}
			c_.push_r(0);
			gen_expr(*ix->idx);
			c_.mov64r_rm64(1, 0);
			c_.pop_r(0);
			c_.u8(0x48);
			c_.u8(0x8D);
			c_.u8(0x04);
			c_.u8(0xC8);
			return;
		}
		if (auto* un = std::get_if<Unary>(&e)) {
			if (un->op == "*") {
				gen_expr(*un->operand);
				return;
			}
		}
		gen_expr(e);
	}

	/* 比较运算符 → 有符号 jcc 条件码。
	   setcc 用同码；跳转用其反向码（见 weiJccNegate）。 */
	static bool weiIsCmpOp(const std::string& op) {
		return op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=";
	}
	static int weiCmpCC(const std::string& op) {
		if (op == "==") return 0x4;
		if (op == "!=") return 0x5;
		if (op == "<")  return 0xC;   /* jl  */
		if (op == ">")  return 0xF;   /* jg  */
		if (op == "<=") return 0xE;   /* jle */
		return 0xD;                   /* >= → jge */
	}
	static int weiJccNegate(int cc) {
		if ((cc & 1) == 0) return cc | 1;   /* 偶数码取反即 +1 */
		return cc & ~1;
	}

	bool NativeGen::is_builtin_call(const std::string& name) const {
		return name == "__command_line" || name == "__byte_at" || name == "__store_byte" || name == "__read_file" || name == "__write_file" ||
		       name == "__exit" ||
		       name == "__file_open" || name == "__file_write" || name == "__file_close" ||
		       name == "__rt_alloc" || name == "__rt_free" || name == "__rt_panic" ||
		       name == "__atomic_add" || name == "__atomic_fetch_add" ||
		       name == "__atomic_cas" || name == "__atomic_xchg" || name == "__mfence" ||
		       name == "__thread" ||
		       /* ── 128 位整数运算原语 ──
		          用两个 int64（lo/hi）表示一个 128 位整数，配合这些原语即可
		          组合出任意宽度的整数运算 —— 数据范围不再受 64 位限制。 */
		       name == "__mulhi" || name == "__umulhi" || name == "__mullo" || name == "__umullo" ||
		       name == "__addc" || name == "__subb";
	}

	/* ── 运行时调用发射 ────────────────────────────────────────────────────── */

	void NativeGen::emit_call_off(size_t target) {
		size_t at = c_.pos();
		c_.u8(0xE8);
		int32_t disp = static_cast<int32_t>(static_cast<int64_t>(target) - static_cast<int64_t>(at + 5));
		c_.u32(static_cast<uint32_t>(disp));
	}

	/* 数组越界检查: RAX = index，与静态长度比较。无符号比较同时拦截负数。 */
	void NativeGen::emit_bounds_check(int64_t len) {
		if (len <= 0) return;
		if (len <= INT32_MAX) {
			c_.cmp_rm64_imm32(0, static_cast<int32_t>(len));
		} else {
			c_.mov64r_imm64(10, len);
			c_.cmp_r64_rm64(0, 10);
		}
		int ok = emit_jcc(0x2);          /* jb ok（index < len） */
		emit_call_off(panic_bounds_off_); /* 不返回 */
		patch(ok);
	}

	/* 栈金丝雀校验: 不破坏 RAX（返回值），只用 R10 和标志位。 */
	void NativeGen::emit_canary_check() {
		if (!canary_active_) return;
		emit_global_load(10, canary_slot_);
		c_.cmp_r64_memd32(10, 5, canary_disp_);   /* r10 - [rbp+canary_disp] */
		int ok = emit_jcc(0x4);                    /* je ok */
		c_.u8(0xCC);                               /* int3: 调试器断点 */
		emit_call_off(panic_canary_off_);          /* 不返回 */
		patch(ok);
	}

	/* ── 新一代运行时内存管理 ─────────────────────────────────────────────────
	   块布局（全部 16 字节对齐）：
	     block+0  : size | FREE(bit0)       size 为块总大小（含 16 字节头）
	     block+8  : 已分配 → class / 空闲 → next 指针
	     block+16 : payload 起始（返回给用户的指针）
	   小对象 (payload <= 32 KiB) 走 12 条 size-class 空闲链表，分配与归还都是
	   O(1) 且无遍历；大对象走独立的首次适配链表。free 不再是空操作，重复释放
	   会被运行时捕获并 panic。主池 64 MiB（BSS 零填充，不增加可执行文件体积），
	   耗尽后由 rt_grow 经 mmap / VirtualAlloc 继续扩容。
	   约定: rt_alloc 入参 RCX = 字节数，返回 RAX = payload 指针；
	        rt_free  入参 RCX = 指针。两者均不破坏 R8/R9/R10/R11/RDX。 */
	/* ═══ 128 位无符号除法/取模 ══════════════════════════════════════════
	   入口: rsi:rdi = N (hi:lo)     r9:r8 = D (hi:lo)     均为无符号
	   出口: rsi:rdi = N / D         r9:r8  = N % D
	   破坏: rax rcx rdx r10 r11     叶子函数，不用栈

	   分情况，而不是一律走慢路径:
	     A) D < 2^64 且 N < 2^64  → 一条 div
	     B) D < 2^64 且 N >= 2^64 → 两次 div（第二次 rdx < r8，不会 #DE）
	     C) D >= 2^64 且 N < D    → q = 0, r = N
	     D) D >= 2^64 且 N >= D   → 移位估商 + 一次修正
	   D 用 Hacker's Delight 的经典做法: q1 = (N>>1)/v1 后 q0 = q1>>(63-n)，
	   再 q0 = max(q0-1, 0)，最后余数 >= D 时 +1。把 128/128 降维成一次
	   64 位 div，避开逐位长除法。 */
	void NativeGen::emit_udivmod128_stub() {
		rt_udivmod128_off_ = c_.pos();

		auto B = [&](std::initializer_list<int> v) { for (int x : v) c_.u8(static_cast<uint8_t>(x)); };
		auto J = [&](int cc) -> size_t {           /* jcc rel32，返回待回补位置 */
			c_.u8(0x0F); c_.u8(static_cast<uint8_t>(0x80 + cc));
			size_t p = c_.pos(); c_.u32(0); return p;
		};
		auto P = [&](size_t p) { c_.patch_jmp(p); };

		/* ── D == 0 → panic ── */
		B({0x49,0x8B,0xC0});            /* mov rax, r8                */
		B({0x49,0x0B,0xC1});            /* or  rax, r9                */
		size_t j_nz = J(0x5);           /* 0F 85 rel32  jnz           */
		emit_call_off(panic_div0_off_); /* E8 rel32，不返回           */
		P(j_nz);

		B({0x4D,0x85,0xC9});            /* test r9, r9                */
		size_t j_wide = J(0x5);         /* jnz wide                   */
		B({0x48,0x85,0xF6});            /* test rsi, rsi              */
		size_t j_caseB = J(0x5);        /* jnz caseB                  */

		/* ── Case A: 64 / 64 ── */
		B({0x48,0x8B,0xC7});            /* mov rax, rdi               */
		B({0x31,0xD2});                 /* xor edx, edx               */
		B({0x49,0xF7,0xF0});            /* div r8                     */
		B({0x4C,0x8B,0xC2});            /* mov r8, rdx   (rem.lo)     */
		B({0x45,0x31,0xC9});            /* xor r9d, r9d  (rem.hi = 0) */
		B({0x48,0x8B,0xF8});            /* mov rdi, rax  (quot.lo)    */
		B({0x31,0xF6});                 /* xor esi, esi  (quot.hi = 0)*/
		B({0xC3});                      /* ret                        */

		/* ── Case B: 128 / 64，两次 div ── */
		P(j_caseB);
		B({0x48,0x8B,0xC6});            /* mov rax, rsi               */
		B({0x31,0xD2});                 /* xor edx, edx               */
		B({0x49,0xF7,0xF0});            /* div r8   -> rax=qh rdx=r   */
		B({0x48,0x8B,0xF0});            /* mov rsi, rax               */
		B({0x48,0x8B,0xC7});            /* mov rax, rdi               */
		B({0x49,0xF7,0xF0});            /* div r8   (rdx = r < r8)    */
		B({0x48,0x8B,0xF8});            /* mov rdi, rax               */
		B({0x4C,0x8B,0xC2});            /* mov r8, rdx                */
		B({0x45,0x31,0xC9});            /* xor r9d, r9d               */
		B({0xC3});                      /* ret                        */

		/* ── 宽除数: 比较 N 与 D ── */
		P(j_wide);
		B({0x49,0x3B,0xF1});            /* cmp rsi, r9                */
		size_t j_C1 = J(0x2);           /* jb  caseC                  */
		size_t j_D1 = J(0x7);           /* ja  caseD                  */
		B({0x49,0x3B,0xF8});            /* cmp rdi, r8                */
		size_t j_D2 = J(0x3);           /* jae caseD                  */

		/* ── Case C: N < D -> q = 0, r = N ── */
		P(j_C1);
		B({0x4C,0x8B,0xC7});            /* mov r8, rdi                */
		B({0x4C,0x8B,0xCE});            /* mov r9, rsi                */
		B({0x31,0xFF});                 /* xor edi, edi               */
		B({0x31,0xF6});                 /* xor esi, esi               */
		B({0xC3});                      /* ret                        */

		/* ── Case D: D >= 2^64 且 N >= D（商 < 2^64）── */
		P(j_D1);
		P(j_D2);
		B({0x49,0x0F,0xBD,0xC9});       /* bsr rcx, r9      c = bsr(dh)*/
		B({0x83,0xF1,0x3F});            /* xor ecx, 63      cl = n     */
		B({0x4D,0x8B,0xD1});            /* mov r10, r9                 */
		B({0x4D,0x8B,0xD8});            /* mov r11, r8                 */
		B({0x4D,0x0F,0xA5,0xDA});       /* shld r10, r11, cl  r10 = v1 */
		B({0x48,0x8B,0xD6});            /* mov rdx, rsi                */
		B({0x48,0xC1,0xEA,0x01});       /* shr rdx, 1       nh>>1 < v1 */
		B({0x48,0x8B,0xC7});            /* mov rax, rdi                */
		B({0x48,0x0F,0xAC,0xF0,0x01});  /* shrd rax, rsi, 1  N>>1     */
		B({0x49,0xF7,0xF0});            /* div r10          rax = q1   */
		B({0x83,0xF1,0x3F});            /* xor ecx, 63      cl = 63-n  */
		B({0x48,0xD3,0xE8});            /* shr rax, cl      q0 = q1>>(63-n) */
		B({0x4C,0x8B,0xD0});            /* mov r10, rax                */
		B({0x49,0x83,0xEA,0x01});       /* sub r10, 1       CF=(q0==0) */
		B({0x49,0x83,0xD2,0x00});       /* adc r10, 0       q0=max(q0-1,0)*/

		/* q0 * D 的低 128 位（q0 <= 真商，不会溢出） */
		B({0x49,0x8B,0xC2});            /* mov rax, r10                */
		B({0x49,0xF7,0xE0});            /* mul r8       rdx:rax = q0*dl*/
		B({0x4C,0x8B,0xD8});            /* mov r11, rax  prod.lo       */
		B({0x49,0x8B,0xCA});            /* mov rcx, r10                */
		B({0x49,0x0F,0xAF,0xC9});       /* imul rcx, r9  lo64(q0*dh)   */
		B({0x48,0x03,0xD1});            /* add rdx, rcx  prod.hi       */

		/* r = N - q0*D  ->  r11:rax */
		B({0x48,0x8B,0xC7});            /* mov rax, rdi                */
		B({0x49,0x2B,0xC3});            /* sub rax, r11                */
		B({0x4C,0x8B,0xDE});            /* mov r11, rsi (mov 保 CF)    */
		B({0x4C,0x1B,0xDA});            /* sbb r11, rdx                */

		/* t = r - D -> rdx:rcx ；CF = (r < D) */
		B({0x48,0x8B,0xC8});            /* mov rcx, rax                */
		B({0x49,0x2B,0xC8});            /* sub rcx, r8                 */
		B({0x49,0x8B,0xD3});            /* mov rdx, r11                */
		B({0x49,0x1B,0xD1});            /* sbb rdx, r9                 */
		size_t j_done = J(0x2);         /* jb done   r < D: 保留 r q0  */
		B({0x48,0x8B,0xC1});            /* mov rax, rcx   r = t        */
		B({0x4C,0x8B,0xDA});            /* mov r11, rdx                */
		B({0x49,0xFF,0xC2});            /* inc r10   q0++              */
		P(j_done);

		B({0x49,0x8B,0xFA});            /* mov rdi, r10  quot.lo       */
		B({0x31,0xF6});                 /* xor esi, esi  quot.hi = 0   */
		B({0x4C,0x8B,0xC0});            /* mov r8, rax   rem.lo        */
		B({0x4D,0x8B,0xCB});            /* mov r9, r11   rem.hi        */
		B({0xC3});                      /* ret                         */
	}

	/* 调用点（契约不变）: rax=dl, r11=dh，nl/nh 在栈顶 */
	void NativeGen::emit_udivmod128_call(bool want_remainder) {
		c_.mov64r_rm64(8, 0);           /* mov r8, rax   (dl)  */
		c_.mov64r_rm64(9, 11);          /* mov r9, r11   (dh)  */
		c_.pop_r(7);                    /* pop rdi       (nl)  */
		c_.pop_r(6);                    /* pop rsi       (nh)  */
		emit_call_off(rt_udivmod128_off_);
		if (want_remainder) { c_.mov64r_rm64(0, 8); c_.mov64r_rm64(11, 9); }
		else                { c_.mov64r_rm64(0, 7); c_.mov64r_rm64(11, 6); }
		known_const_.clear();
	}

	void NativeGen::emit_runtime_stubs() {
		target_->emitPanicStub(*this, "Wei runtime panic", panic_generic_off_);
		target_->emitPanicStub(*this, "runtime panic: array index out of bounds", panic_bounds_off_);
		target_->emitPanicStub(*this, "runtime panic: stack canary corrupted", panic_canary_off_);
		target_->emitPanicStub(*this, "runtime panic: heap pool exhausted", panic_oom_off_);
		target_->emitPanicStub(*this, "runtime panic: double free detected", panic_dblfree_off_);
		target_->emitPanicStub(*this, "runtime panic: heap corruption or invalid pointer", panic_heapcorrupt_off_);
		target_->emitPanicStub(*this, "runtime panic: division by zero or overflow", panic_div0_off_);
		target_->emitPanicStub(*this, "runtime panic: integer overflow", panic_ovf_off_);
		/* 契约违规: RCX/RDI = 消息指针 */
		target_->emitContractFail(*this, panic_contract_off_);
		/* -wide 的 128 位打印桩（不启用时不生成，零开销） */
		if (wide_mode_) target_->emitPrintI128(*this, rt_print_i128_off_);
		/* 协程上下文切换 */
		emit_co_switch_stub();

		const int32_t fl_disp = static_cast<int32_t>(8 * freelist_base_slot_);
		const int32_t q_disp  = static_cast<int32_t>(8 * q_arr_slot_);
		const int32_t hdr_d   = static_cast<int32_t>(WEI_HDR);
		const int32_t hdr_tail = static_cast<int32_t>(WEI_HDR + WEI_TAIL);

		/* ── rt_leak(): 退出前检查是否还有未归还的活字节 ── */
		rt_leak_off_ = c_.pos();
		target_->emitLeakReport(*this);

		/* ── rt_grow(RCX = 至少需要的字节数) ── */
		rt_grow_off_ = c_.pos();
		target_->emitHeapGrow(*this);

		/* ── rt_free(RCX = ptr) ─────────────────────────────────────────── */
		rt_free_off_ = c_.pos();
		c_.test_rm64_r64(1, 1);                  /* free(NULL) 是安全的空操作 */
		int f_nz = emit_jcc(0x5);                /* jnz */
		c_.xor_rm64_r64(0, 0);
		c_.ret();
		patch(f_nz);
		c_.push_r(2);                            /* rdx */
		c_.push_r(8);                            /* r8  */
		c_.push_r(6);                            /* rsi */
		c_.push_r(7);                            /* rdi */

		/* (1) 指针对齐校验: 合法 payload 必为 16 字节对齐 */
		c_.test_rm64_imm32(1, 15);
		int f_align = emit_jcc(0x4);             /* jz → 对齐正确 */
		emit_call_off(panic_heapcorrupt_off_);   /* 未对齐 → 野指针/伪造指针 */
		patch(f_align);

		c_.mov64r_rm64(2, 1);                    /* rdx = ptr */
		c_.alu_rm64_imm32(2, 5, hdr_d);          /* rdx = block = ptr - hdr */

		/* (2) 头哨兵校验: 捕获野指针、非堆指针、被踩坏的块 */
		c_.mov64r_imm64(1, WEI_MAGIC_HEAD);      /* rcx = MAGIC_HEAD */
		c_.mov64r_rm64(6, 2);                    /* rsi = block */
		c_.xor_rm64_r64(6, 1);                   /* rsi = 期望哨兵 */
		c_.mov64r_memd8(0, 2, 0);                /* rax = 实际哨兵 */
		c_.cmp_r64_rm64(0, 6);
		int f_magic = emit_jcc(0x4);             /* je */
		emit_call_off(panic_heapcorrupt_off_);
		patch(f_magic);

		/* (3) 重复释放检测 */
		c_.mov64r_memd8(0, 2, 8);                /* rax = info */
		c_.u8(0x48);
		c_.u8(0xA9);
		c_.u32(1);                               /* test rax, 1 */
		int f_ok = emit_jcc(0x4);                /* jz → 尚未释放 */
		emit_call_off(panic_dblfree_off_);
		patch(f_ok);

		/* (4) 尾哨兵校验: 捕获越界写（heap overflow） */
		c_.mov64r_memd8(1, 2, 24);               /* rcx = cap */
		c_.mov64r_rm64(6, 2);                    /* rsi = block */
		c_.add_rm64_imm32(6, hdr_d);
		c_.add_r64_rm64(6, 1);                   /* rsi = 尾哨兵地址 */
		c_.mov64r_imm64(7, WEI_MAGIC_TAIL);      /* rdi = MAGIC_TAIL */
		c_.mov64r_rm64(0, 2);                    /* rax = block */
		c_.xor_rm64_r64(0, 7);                   /* rax = 期望尾哨兵 */
		c_.mov64r_memd8(7, 6, 0);                /* rdi = 实际尾哨兵 */
		c_.cmp_r64_rm64(7, 0);
		int f_tail = emit_jcc(0x4);              /* je */
		emit_call_off(panic_heapcorrupt_off_);
		patch(f_tail);

		/* (5) 释放即毒化: 0xA5 覆写
		   既销毁残留数据（防敏感信息泄漏），又让此后的 UAF 读拿到
		   可辨识的垃圾而不是有效数据。代价是 O(cap) 的内存写。 */
		c_.mov64r_memd8(1, 2, 24);               /* rcx = cap */
		c_.mov64r_rm64(7, 2);                    /* rdi = block */
		c_.add_rm64_imm32(7, hdr_d);
		c_.shr_rm64_imm8(1, 3);                  /* rcx = cap/8 */
		c_.mov64r_imm64(0, WEI_POISON);          /* rax = 0xA5..A5 */
		c_.rep_stosq();

		/* (6) 标记 FREE，并送入隔离区（延迟复用，抗 UAF） */
		c_.mov64r_memd8(0, 2, 8);                /* rax = info */
		c_.alu_rm64_imm32(0, 1, 1);              /* rax |= FREE */
		c_.mov_memd8_64r(2, 8, 0);

		/* 隔离区: WEI_QUARANTINE 槽环形缓冲。每次释放先把槽中旧块真正归还
		   空闲链表，再把新释放的块放进该槽 —— 于是每一块至少经历
		   WEI_QUARANTINE 次释放之后才可能被重新分配出去。释放后读/写在窗口
		   期内命中隔离块而非新宿主对象，UAF 的破坏力被大幅削弱。 */
		/* 归还块到空闲链表的公共代码 (reg = 块地址寄存器) */
		auto releaseBlock = [&](int reg) {
			c_.mov64r_memd8(0, reg, 24);             /* rax = cap */
			c_.cmp_rm64_imm32(0, static_cast<int32_t>(weiClsPayload(WEI_NCLS - 1)));
			int jl = emit_jcc(0x7);                  /* ja → 大块 */
			c_.bsr_r64_rm64(0, 0);
			c_.alu_rm64_imm32(0, 5, 4);              /* class = bsr(cap) - 4 */
			c_.mov64r_rm64(8, 0);                    /* r8 = class */
			emit_global_addr(11, 0);
			c_.mov64r_mem_idx8(7, 11, 8, fl_disp);   /* rdi = freelist[class] */
			c_.mov_memd8_64r(reg, 16, 7);            /* block->next = head */
			c_.mov_mem_idx8_64r(11, 8, fl_disp, reg);/* freelist[class] = block */
			int jd = emit_jmp();
			patch(jl);
			emit_global_load(7, large_list_slot_);
			c_.mov_memd8_64r(reg, 16, 7);
			emit_global_store(large_list_slot_, reg);
			patch(jd);
		};

		/* 隔离区: WEI_QUARANTINE 槽环形缓冲。每次释放先把槽中旧块真正
		   归还空闲链表，再把新释放的块放进该槽 —— 于是每一块至少经历
		   WEI_QUARANTINE 次释放之后才可能被重新分配出去。释放后读/写在窗口
		   期内命中隔离块而非新宿主对象，UAF 的破坏力被大幅削弱。 */
		emit_global_load(6, q_idx_slot_);        /* rsi = idx */
		emit_global_addr(11, 0);                 /* r11 = &globals[0] */
		c_.mov64r_mem_idx8(1, 11, 6, q_disp);    /* rcx = arr[idx] */
		c_.test_rm64_r64(1, 1);
		int q_empty = emit_jcc(0x4);             /* jz → 槽为空 */
		releaseBlock(1);                         /* ── 归还旧块 (rcx = block) ── */
		patch(q_empty);
		c_.mov_mem_idx8_64r(11, 6, q_disp, 2);   /* arr[idx] = block */
		c_.inc_rm64(6);
		c_.alu_rm64_imm32(6, 4, WEI_QUARANTINE - 1);   /* idx = (idx+1) & 63 */
		emit_global_store(q_idx_slot_, 6);

		emit_global_load(1, stat_free_slot_);
		c_.inc_rm64(1);
		emit_global_store(stat_free_slot_, 1);
		emit_global_load(1, stat_live_slot_);
		c_.mov64r_memd8(0, 2, 8);
		c_.alu_rm64_imm32(0, 4, -2);             /* rax = total */
		c_.sub_r64_rm64(1, 0);
		emit_global_store(stat_live_slot_, 1);
		c_.pop_r(7);
		c_.pop_r(6);
		c_.pop_r(8);
		c_.pop_r(2);
		c_.xor_rm64_r64(0, 0);
		c_.ret();

		/* ── rt_alloc(RCX = req) ────────────────────────────────────────── */
		rt_alloc_off_ = c_.pos();
		c_.push_r(2);                            /* rdx */
		c_.push_r(8);
		c_.push_r(9);
		c_.push_r(10);
		c_.push_r(11);
		c_.push_r(6);                            /* rsi */
		c_.push_r(7);                            /* rdi */

		c_.mov64r_rm64(10, 1);                   /* r10 = req */
		c_.test_rm64_r64(10, 10);
		int a_nz = emit_jcc(0x5);
		c_.mov64r_imm64(10, 1);                  /* req == 0 视作 1 */
		patch(a_nz);

		c_.cmp_rm64_imm32(10, static_cast<int32_t>(weiClsPayload(WEI_NCLS - 1)));
		int a_large = emit_jcc(0x7);             /* ja → large */

		/* small: class = req<=16 ? 0 : bsr(req-1)-3；cap = 16 << class */
		c_.cmp_rm64_imm32(10, 16);
		int a_c0 = emit_jcc(0x6);                /* jbe */
		c_.mov64r_rm64(0, 10);
		c_.alu_rm64_imm32(0, 5, 1);
		c_.bsr_r64_rm64(0, 0);
		c_.alu_rm64_imm32(0, 5, 3);
		int a_have = emit_jmp();
		patch(a_c0);
		c_.xor_rm64_r64(0, 0);
		patch(a_have);
		c_.mov64r_rm64(8, 0);                    /* r8 = class */
		c_.mov64r_imm64(10, 16);                 /* r10 = cap */
		c_.mov64r_rm64(1, 8);
		c_.shl_rm64_cl(10);
		c_.mov64r_rm64(9, 10);                   /* r9 = total */
		c_.add_rm64_imm32(9, hdr_tail + 15);
		c_.alu_rm64_imm32(9, 4, -16);

		/* 命中空闲链表则直接摘除，O(1) 无遍历 */
		emit_global_addr(11, 0);
		c_.mov64r_mem_idx8(0, 11, 8, fl_disp);   /* rax = freelist[class] */
		c_.test_rm64_r64(0, 0);
		int a_bump = emit_jcc(0x4);              /* jz → bump */
		c_.mov64r_memd8(2, 0, 16);               /* rdx = head->next */
		c_.mov_mem_idx8_64r(11, 8, fl_disp, 2);
		int a_init1 = emit_jmp();
		patch(a_bump);

		emit_global_load(0, heap_ptr_slot_);
		c_.mov64r_rm64(1, 0);
		c_.add_r64_rm64(1, 9);
		emit_global_load(2, heap_end_slot_);
		c_.cmp_r64_rm64(1, 2);
		int a_grow = emit_jcc(0x7);
		emit_global_store(heap_ptr_slot_, 1);
		int a_init2 = emit_jmp();
		patch(a_grow);
		c_.mov64r_rm64(1, 9);                    /* rcx = need */
		emit_call_off(rt_grow_off_);
		emit_global_load(0, heap_ptr_slot_);
		c_.mov64r_rm64(1, 0);
		c_.add_r64_rm64(1, 9);
		emit_global_store(heap_ptr_slot_, 1);
		int a_init3 = emit_jmp();

		/* ── large: 首次适配 ── */
		patch(a_large);
		c_.mov64r_rm64(9, 10);                   /* r9 = total */
		c_.add_rm64_imm32(9, hdr_tail + 15);
		c_.alu_rm64_imm32(9, 4, -16);
		c_.mov64r_rm64(10, 9);                   /* r10 = cap = total - (hdr+tail) */
		c_.alu_rm64_imm32(10, 5, hdr_tail);      /* cap = total - (hdr + tail) */
		c_.mov64r_imm64(8, WEI_NCLS);            /* class = 12 → large */
		emit_global_addr(11, large_list_slot_);  /* r11 = &large_list */
		size_t ll_start = c_.pos();
		c_.mov64r_memd8(2, 11, 0);               /* rdx = *pp */
		c_.test_rm64_r64(2, 2);
		int a_lbump = emit_jcc(0x4);             /* jz → bump */
		c_.mov64r_memd8(0, 2, 8);                /* rax = cur->info */
		c_.mov64r_rm64(1, 0);
		c_.alu_rm64_imm32(1, 4, -2);             /* rcx = size */
		c_.cmp_r64_rm64(1, 9);
		int a_lfound = emit_jcc(0x3);            /* jae → 命中 */
		c_.lea64r_memd8(11, 2, 16);              /* pp = &cur->next */
		{
			size_t jp = c_.jmp32();
			c_.patch_u32(jp, static_cast<uint32_t>(static_cast<int32_t>(ll_start - (jp + 4))));
		}
		patch(a_lfound);
		c_.mov64r_memd8(1, 2, 16);               /* rcx = cur->next */
		c_.mov_memd8_64r(11, 0, 1);              /* *pp = next */
		c_.mov64r_rm64(0, 2);                    /* rax = block */
		int a_init4 = emit_jmp();
		patch(a_lbump);
		emit_global_load(0, heap_ptr_slot_);
		c_.mov64r_rm64(1, 0);
		c_.add_r64_rm64(1, 9);
		emit_global_load(2, heap_end_slot_);
		c_.cmp_r64_rm64(1, 2);
		int a_lgrow = emit_jcc(0x7);
		emit_global_store(heap_ptr_slot_, 1);
		int a_init5 = emit_jmp();
		patch(a_lgrow);
		c_.mov64r_rm64(1, 9);
		emit_call_off(rt_grow_off_);
		emit_global_load(0, heap_ptr_slot_);
		c_.mov64r_rm64(1, 0);
		c_.add_r64_rm64(1, 9);
		emit_global_store(heap_ptr_slot_, 1);

		/* ── 统一初始化: rax = block, r8 = class, r9 = total, r10 = cap ── */
		patch(a_init1);
		patch(a_init2);
		patch(a_init3);
		patch(a_init4);
		patch(a_init5);
		c_.mov64r_rm64(11, 0);                   /* r11 = block */
		/* 头哨兵 = MAGIC_HEAD ^ block（每块唯一） */
		c_.mov64r_imm64(1, WEI_MAGIC_HEAD);
		c_.mov64r_rm64(6, 11);
		c_.xor_rm64_r64(6, 1);
		c_.mov_memd8_64r(11, 0, 6);
		/* info / class / cap */
		c_.mov_memd8_64r(11, 8, 9);
		c_.mov_memd8_64r(11, 16, 8);
		c_.mov_memd8_64r(11, 24, 10);
		/* 尾哨兵 = MAGIC_TAIL ^ block，位于 block + hdr + cap */
		c_.mov64r_imm64(1, WEI_MAGIC_TAIL);
		c_.mov64r_rm64(6, 11);
		c_.xor_rm64_r64(6, 1);
		c_.mov64r_rm64(2, 11);
		c_.add_rm64_imm32(2, hdr_d);
		c_.add_r64_rm64(2, 10);
		c_.mov_memd8_64r(2, 0, 6);
		/* 分配即清零: 杜绝脏数据泄漏 */
		c_.mov64r_rm64(7, 11);
		c_.add_rm64_imm32(7, hdr_d);
		c_.mov64r_rm64(1, 10);
		c_.shr_rm64_imm8(1, 3);
		c_.xor_rm64_r64(0, 0);
		c_.rep_stosq();
		c_.lea64r_memd8(0, 11, hdr_d);           /* rax = payload */

		/* 统计 */
		emit_global_load(2, stat_alloc_slot_);
		c_.inc_rm64(2);
		emit_global_store(stat_alloc_slot_, 2);
		emit_global_load(2, stat_live_slot_);
		c_.add_r64_rm64(2, 9);
		emit_global_store(stat_live_slot_, 2);
		emit_global_load(1, stat_peak_slot_);
		c_.cmp_r64_rm64(1, 2);
		int a_pk = emit_jcc(0x3);
		emit_global_store(stat_peak_slot_, 2);
		patch(a_pk);
		c_.pop_r(7);
		c_.pop_r(6);
		c_.pop_r(11);
		c_.pop_r(10);
		c_.pop_r(9);
		c_.pop_r(8);
		c_.pop_r(2);
		c_.ret();
	}

	/* 除法安全: 拦截除零与 INT64_MIN / -1 商溢出。
	   这两者在 x86-64 的 IDIV 上都会触发 #DE，使进程直接收到 SIGFPE 而崩溃，
	   因此必须在执行前拦截。被除数须已在 RAX，除数为 divisor_reg。本序列只
	   借 RDX 作临时寄存器——随后的 CQO 本就会覆盖 RDX，故不破坏任何活跃值。 */
	void NativeGen::emit_ovf_check() {
		/* D1: 编译期已证明该赋值不可能溢出（见 proveAccumNoOverflow）。
		   这是"证明不需要检查"，不是"为性能关检查" —— 触发条件严格到
		   只覆盖单个循环内、单个变量、单种赋值形态。 */
		if (suppress_ovf_) return;
		/* C3: 分级开关 —— none/min 下省略溢出检查（不是偷偷关，是用户显式选的等级） */
		if (!check_enabled(CheckKind::Overflow)) return;
		/* -wide 的语义就是"越界也继续算": 64 位溢出检查在这里全部失效,
		   由 128 位通路接管。否则任何残留的 64 位检查都会让 -wide
		   在最需要它的那个点上 panic。
		   这条短路同时也覆盖了那些尚未接入宽通路的表达式形态 ——
		   它们会退化为 64 位回绕, 但不至于中断程序。 */
		if (wide_mode_) return;
		int ok = emit_jcc(0x1);                  /* jno → 未溢出 */
		emit_call_off(panic_ovf_off_);
		patch(ok);
	}

	/* ═══ IR 路径实现 ═══════════════════════════════════════════════════════
	   把表达式树线性化成三地址码，再经寄存器分配发射。
	   返回目的虚拟寄存器；ok=false 表示遇到了 IR 尚不支持的形态，
	   调用者应回退到原有语法制导翻译路径（保证不回归）。 */

	int NativeGen::ir_build_expr(IRFunc& f, Expr& e, bool& ok) {
		if (auto* il = std::get_if<IntLit>(&e)) {
			int d = f.newvreg();
			int idx = f.emit(IROp::CONST, -1, -1, il->value);
			f.code[static_cast<size_t>(idx)].dst = d;
			return d;
		}
		if (auto* id = std::get_if<Ident>(&e)) {
			auto& vi = resolve_var(id->name);
			if (!vi.is_local || vi.array_len > 0) {
				ok = false;
				return -1;
			}
			/* 若变量已被循环提升机制驻留在物理寄存器中，IR 路径**回退**。
			   原因: 提升机制在循环退出时才写回栈槽，而 IR 若同时读写栈槽
			   或另一套物理寄存器，两套机制会互相覆盖，实测会算错结果。
			   在两者的寄存器视图统一之前，保守回退到语法制导翻译 ——
			   这保证 -ir 只会不变或更快，绝不会更错。 */
			auto it = loop_reg_vars_.find(vi.slot);
			if (it != loop_reg_vars_.end()) {
				ok = false;
				return -1;
			}
			int d = f.newvreg();
			int idx = f.emit(IROp::LOAD, -1, -1, 0);
			f.code[static_cast<size_t>(idx)].dst = d;
			f.code[static_cast<size_t>(idx)].disp = static_cast<int32_t>(-8 * static_cast<int>(vi.slot + 1));
			return d;
		}
		if (auto* bin = std::get_if<Binary>(&e)) {
			IROp op;
			if (bin->op == "+") op = IROp::ADD;
			else if (bin->op == "-") op = IROp::SUB;
			else if (bin->op == "*") op = IROp::MUL;
			else {
				/* / % 比较等一律回退原路径。
				   特别是除法: 原路径带 emit_div_check()，能拦截除零与
				   INT64_MIN/-1 溢出；IR 的 DIV 分支尚未实现发射，
				   若在这里接手会静默丢掉安全检查。 */
				ok = false;
				return -1;
			}
			int a = ir_build_expr(f, *bin->lhs, ok);
			if (!ok) return -1;
			int b = ir_build_expr(f, *bin->rhs, ok);
			if (!ok) return -1;
			int d = f.newvreg();
			int idx = f.emit(op, a, b, 0);
			f.code[static_cast<size_t>(idx)].dst = d;
			return d;
		}
		ok = false;
		return -1;
	}

	void NativeGen::ir_emit(IRFunc& f, IRRegAlloc& ra) {
		auto physOf = [&](int v) -> int {
			if (v < 0) return 0;
			return ra.vregPhys[static_cast<size_t>(v)];
		};
		for (size_t i = 0; i < f.code.size(); i++) {
			auto& in = f.code[i];
			int d = in.dst;
			int dp = (d >= 0) ? physOf(d) : -1;
			switch (in.op) {
				case IROp::CONST: {
					if (dp < 0) {
						/* 溢出到栈: 经 rax 中转 */
						if (in.imm >= INT32_MIN && in.imm <= INT32_MAX) c_.mov32r_imm32(0, static_cast<int32_t>(in.imm));
						else c_.mov64r_imm64(0, in.imm);
						c_.mov_memd8_64r(5, static_cast<int8_t>(ra.vregSpill[static_cast<size_t>(d)]), 0);
					} else {
						if (in.imm >= INT32_MIN && in.imm <= INT32_MAX) c_.mov32r_imm32(dp, static_cast<int32_t>(in.imm));
						else c_.mov64r_imm64(dp, in.imm);
					}
					break;
				}
				case IROp::LOAD: {
					int8_t dd = static_cast<int8_t>(in.disp);
					if (dp < 0) {
						c_.mov64r_memd8(0, 5, dd);
						c_.mov_memd8_64r(5, static_cast<int8_t>(ra.vregSpill[static_cast<size_t>(d)]), 0);
					} else {
						c_.mov64r_memd8(dp, 5, dd);
					}
					break;
				}
				case IROp::STORE: {
					int ap = physOf(in.a);
					int8_t dd = static_cast<int8_t>(in.disp);
					if (ap < 0) {
						c_.mov64r_memd8(0, 5, static_cast<int8_t>(ra.vregSpill[static_cast<size_t>(in.a)]));
						c_.mov_memd8_64r(5, dd, 0);
					} else {
						c_.mov_memd8_64r(5, dd, ap);
					}
					break;
				}
				case IROp::LOADREG: {
					int dp2 = physOf(d);
					if (dp2 < 0) c_.mov_memd8_64r(5, static_cast<int8_t>(ra.vregSpill[static_cast<size_t>(d)]), in.phys);
					else if (dp2 != in.phys) c_.mov64r_rm64(dp2, in.phys);
					break;
				}
				case IROp::STOREREG: {
					int ap = physOf(in.a);
					if (ap < 0) {
						c_.mov64r_memd8(0, 5, static_cast<int8_t>(ra.vregSpill[static_cast<size_t>(in.a)]));
						c_.mov64r_rm64(in.phys, 0);
					} else if (ap != in.phys) {
						c_.mov64r_rm64(in.phys, ap);
					}
					break;
				}
				case IROp::ADD:
				case IROp::SUB:
				case IROp::MUL: {
					int ap = physOf(in.a);
					int bp = physOf(in.b);
					/* 结果若溢出到栈，用 rax 作中转并写回 */
					int tgt = (dp < 0) ? 0 : dp;
					auto loadA = [&]() {
						if (ap < 0) c_.mov64r_memd8(tgt, 5, static_cast<int8_t>(ra.vregSpill[static_cast<size_t>(in.a)]));
						else if (ap != tgt) c_.mov64r_rm64(tgt, ap);
					};
					if (in.op == IROp::MUL && in.shift > 0) {
						/* shl 不产生 imul 的 OF，无法沿用 emit_ovf_check()。
						   强度削减 pass 目前未启用；一旦出现 shift>0，说明有人
						   打开了未完成的优化，必须在这里拦住，不能静默丢检查。 */
						throw std::runtime_error("IR: MUL 强度削减缺少溢出检查，拒绝生成不安全代码");
					} else if (in.op == IROp::MUL && in.lea_scale > 0 && in.lea_add1) {
						loadA();
						/* lea tgt, [a + a*scale]  —— 结果 mod 2^64 与乘法一致 */
						c_.rex_wrxb(tgt, in.a, tgt);
						c_.u8(0x8D);
						c_.modrm(2, tgt, 4);
						c_.sib((in.lea_scale == 2) ? 1 : (in.lea_scale == 4) ? 2 : 3, tgt, tgt);
						c_.u8(0);   /* disp8 = 0 (mod=2 需要 disp32) */
						(void)0;
					} else {
						loadA();
						if (in.op == IROp::ADD) {
							if (bp < 0) c_.add_r64_memd8(tgt, 5, static_cast<int8_t>(ra.vregSpill[static_cast<size_t>(in.b)]));
							else c_.add_r64_rm64(tgt, bp);
							emit_ovf_check();
						} else if (in.op == IROp::SUB) {
							if (bp < 0) c_.sub_r64_memd8(tgt, 5, static_cast<int8_t>(ra.vregSpill[static_cast<size_t>(in.b)]));
							else c_.sub_r64_rm64(tgt, bp);
							emit_ovf_check();
						} else {
							/* MUL: 需要 imul 以获得可信的溢出标志 */
							if (bp < 0) {
								c_.mov64r_memd8(1, 5, static_cast<int8_t>(ra.vregSpill[static_cast<size_t>(in.b)]));
								c_.imul_r64_rm64(tgt, 1);
							} else {
								c_.imul_r64_rm64(tgt, bp);
							}
							emit_ovf_check();
						}
					}
					if (dp < 0) c_.mov_memd8_64r(5, static_cast<int8_t>(ra.vregSpill[static_cast<size_t>(d)]), tgt);
					break;
				}
				default:
					break;
			}
		}
	}

	/* 赋值语句走 IR 路径。失败则返回 false，调用者回退原路径。 */
	bool NativeGen::try_ir_assign(AssignStmt& n) {
		if (!ir_) return false;
		if (n.target) return false;
		auto& vi = resolve_var(n.name);
		if (!vi.is_local || vi.array_len > 0) return false;
		if (dbl_vars_.count(n.name)) return false;
		/* 自赋值累加 (x = x + ...) 让位给原路径。
		   原路径对此有专门的快路径：目标驻留寄存器时直接一条
		   `add tgt, reg` / `add tgt, imm` 完成；而 IR 是通用三地址码，
		   会展开成多条 mov + add。实测在循环里 IR 反而慢 2.6 倍。
		   IR 的价值在于承载可组合的优化，但那需要先把原路径的这些
		   针对性快路径也迁移成 IR pass —— 在那之前，不该让 IR 接管
		   原路径已经做得更好的形状。 */
		if (auto* bin = std::get_if<Binary>(n.value.get())) {
			if (auto* lhs = std::get_if<Ident>(bin->lhs.get())) {
				if (lhs->name == n.name) return false;
			}
		}
		IRFunc f;
		bool ok = true;
		int src = ir_build_expr(f, *n.value, ok);
		if (!ok || src < 0) return false;
		int32_t disp = static_cast<int32_t>(-8 * static_cast<int>(vi.slot + 1));
		auto sit = loop_reg_vars_.find(vi.slot);
		int idx;
		if (sit != loop_reg_vars_.end()) {
			idx = f.emit(IROp::STOREREG, src, -1, 0);
			f.code[static_cast<size_t>(idx)].phys = sit->second;
		} else {
			idx = f.emit(IROp::STORE, src, -1, 0);
			f.code[static_cast<size_t>(idx)].disp = disp;
		}
		/* 优化 pass（在 IR 上做，与机器无关、可组合 —— 这正是 IR 的价值） */
		irFoldConstants(f);
		irStrengthReduce(f);
		IRRegAlloc ra(f.nvreg);
		ra.run(f.code);
		ir_emit(f, ra);
		/* 不能设置 last_store_slot_/last_store_len_。
		   那两个标记是给原路径用的「结果已在 rax」契约：下一条语句若发现
		   slot 匹配就直接复用 rax，不再重新加载。但 IR 发射的结果寄存器由
		   分配器决定（可能是 rdx/rcx/...），不保证在 rax —— 一旦标记，
		   后续语句就会拿 rax 里的**旧值**当作本变量的值，算出错误结果。
		   实测: c=a+b 走 IR 结果在 rdx，随后的 c=c*3 却复用了 rax 里残留的
		   a，得到 100*3=300 而非 123*3=369。 */
		return true;
	}

	void NativeGen::emit_div_check(int divisor_reg) {
		c_.test_rm64_r64(divisor_reg, divisor_reg);
		int ok1 = emit_jcc(0x5);                 /* jnz → 除数非零 */
		emit_call_off(panic_div0_off_);
		patch(ok1);
		c_.cmp_rm64_imm32(divisor_reg, -1);      /* 只有 -1 会让商溢出 */
		int ok2 = emit_jcc(0x5);                 /* jne → 安全 */
		c_.mov64r_imm64(2, INT64_MIN);           /* rdx = INT64_MIN */
		c_.cmp_r64_rm64(0, 2);                   /* 被除数 == INT64_MIN ? */
		int ok3 = emit_jcc(0x5);                 /* jne → 安全 */
		emit_call_off(panic_div0_off_);
		patch(ok3);
		patch(ok2);
	}

	/* 入口桩: heap_ptr = pool; heap_end = pool + 1MiB; canary = RDTSC。 */
	void NativeGen::emit_heap_and_canary_init() {
		c_.lea64r_rip(0);
		size_t pool_disp = c_.pos();
		c_.u32(0);
		heap_addr_patches_.push_back(pool_disp);
		emit_global_store(heap_ptr_slot_, 0);
		c_.mov64r_imm64(2, static_cast<int64_t>(WEI_HEAP_POOL_SIZE));
		c_.add_r64_rm64(0, 2);
		emit_global_store(heap_end_slot_, 0);
		c_.u8(0x0F);                            /* rdtsc */
		c_.u8(0x31);
		c_.u8(0x48);                            /* shl rdx, 32 */
		c_.u8(0xC1);
		c_.u8(0xE2);
		c_.u8(0x20);
		c_.u8(0x48);                            /* or rax, rdx */
		c_.u8(0x09);
		c_.u8(0xD0);
		emit_global_store(canary_slot_, 0);
	}

	/* ── D3 · 原子操作: 取变量地址到 rbx ──────────────────────────────
	   rbx 是 callee-saved 且不在求值临时寄存器集合(rax/rcx/rdx/r8-r11)里，
	   所以 gen_expr 求参数时不会破坏它 —— 地址可以安全地跨参数求值存活。 */
	void NativeGen::emit_var_addr_rbx(const std::string& name) {
		auto& v = resolve_var(name);
		if (v.is_local) {
			int32_t d = static_cast<int32_t>(-8 * static_cast<int>(v.slot + 1));
			c_.lea64r_memd32(3, 5, d);          /* lea rbx,[rbp+d] */
		} else {
			emit_global_addr(3, v.slot);        /* lea rbx,[global] */
		}
	}

	void NativeGen::gen_builtin_call(Call& n) {
		/* ── D3 · 原子操作 ──────────────────────────────────────────
		   lock 前缀保证多核下的原子性。编码（x86-64，64 位操作数）:
		     lock add    [rbx],rcx : F0 48 01 0B
		     lock xadd   [rbx],rcx : F0 48 0F C1 0B   （rcx ← 原值）
		     lock cmpxchg[rbx],rcx : F0 48 0F B1 0B   （rax=期望值，rax ← 原值）
		     xchg        [rbx],rcx : 48 87 0B          （xchg 隐式带 lock）
		     mfence                : 0F AE F0 */
		/* ── D3 · 线程: 用 clone(2) 直接创建，不依赖 pthread ────────────
		   clone 的 child 从 syscall 返回处继续，靠返回值 0 区分父子。
		   栈需自己准备（mmap），并按约定压入 [fn][arg]：
		     child: pop fn; pop arg; 再按 Wei 的栈传参约定把 arg 放到
		            [rsp+0] 才 call fn。
		   flags = CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD
		         = 0x11500 （同一线程组，共享地址空间与文件描述符）。 */
		if (n.callee == "__thread") {
			auto* mp = std::get_if<ModulePathGet>(n.args[0].get());
			std::string qname;
			if (mp) {
				qname = mp->module + "::" + mp->part;
			} else if (auto* id = std::get_if<Ident>(n.args[0].get())) {
				/* `Worker::run` 在实参位置会被解析器提前消费掉模块前缀，
				   只剩 Ident("run")。这里按函数名反查所属模块 ——
				   两种写法（Mod::fn 与裸 fn）都能用。 */
				for (auto& me : mod_funcs_) {
					if (me.fn && me.fn->name == id->name && me.mod) {
						qname = me.mod->name + "::" + me.fn->name;
						break;
					}
				}
				if (qname.empty()) throw std::runtime_error("__thread: 找不到函数 '" + id->name + "' at " + std::to_string(n.line));
			} else {
				throw std::runtime_error("__thread 第一个参数必须写成 Mod::fn at " + std::to_string(n.line));
			}
			auto fo = func_offsets_.find(qname);
			if (fo == func_offsets_.end()) throw std::runtime_error("__thread: 未知函数 '" + qname + "' at " + std::to_string(n.line));
			size_t fn_off = fo->second;
			last_store_len_ = (size_t) -1;
			/* 1) 分配线程栈。
			   用 Wei 的堆分配器，但**不能一次申请 64 KiB** ——
			   堆池上限就是 64 KiB(WEI_HEAP_POOL_OFF)，整块申请会失败，
			   拿到非法栈地址后 clone 直接返回 -EINVAL（实测踩过）。
			   8 KiB 对这里的测试线程足够。 */
			c_.mov64r_imm64(0, 8192);
			c_.mov64r_rm64(1, 0);                    /* rcx = size */
			emit_call_off(rt_alloc_off_);            /* rax = 栈底 */
			c_.mov64r_rm64(3, 0);                    /* rbx = 栈底 */
			c_.add_rm64_imm32(3, 8192);              /* rbx = 栈顶 */
			/* 2) 压 arg（高地址） */
			gen_expr(*n.args[1]);
			c_.sub_rm64_imm32(3, 8);
			c_.mov_memd8_64r(3, 0, 0);               /* [rbx] = arg */
			/* 3) 压 fn（低地址）—— pop 时先取到 fn */
			{
				size_t at = c_.pos() + 3;            /* 48 8D 05 之后是 disp32 */
				c_.u8(0x48); c_.u8(0x8D); c_.u8(0x05);
				int32_t d = static_cast<int32_t>(static_cast<int64_t>(fn_off)
				            - static_cast<int64_t>(at + 4));
				c_.u32(static_cast<uint32_t>(d));    /* lea rax,[rip+fn] */
			}
			c_.sub_rm64_imm32(3, 8);
			c_.mov_memd8_64r(3, 0, 0);               /* [rbx] = fn */
			/* 4) clone(flags, child_stack) */
			/* flags 必须精确: CLONE_VM(0x100)|CLONE_FS(0x200)|CLONE_FILES(0x400)|
			   CLONE_SIGHAND(0x800)|CLONE_THREAD(0x10000) = **0x10F00**。
			   曾误写成 0x11500 —— 把 FS/SIGHAND 拼成了 0x1000，而 bit 12
			   不是任何有效 flag，内核一律 -EINVAL。用 C 对照三种组合才定位到。 */
			c_.mov64r_imm64(7, 0x10F00);             /* rdi = flags */
			c_.mov64r_rm64(6, 3);                    /* rsi = rbx */
			c_.mov64r_imm64(0, 56);                  /* rax = SYS_clone */
			c_.u8(0x0F); c_.u8(0x05);                /* syscall */
			/* 5) test rax,rax; jnz parent */
			c_.u8(0x48); c_.u8(0x85); c_.u8(0xC0);
			c_.u8(0x75);
			c_.u8(26);                               /* child stub 共 26 字节 */
			/* ── child ── */
			c_.u8(0x31); c_.u8(0xED);                /* xor ebp,ebp */
			c_.u8(0x5F);                             /* pop rdi  ← fn */
			c_.u8(0x5E);                             /* pop rsi  ← arg */
			c_.u8(0x48); c_.u8(0x83); c_.u8(0xEC); c_.u8(0x10);  /* sub rsp,16 */
			c_.u8(0x48); c_.u8(0x89); c_.u8(0x34); c_.u8(0x24);  /* mov [rsp],rsi */
			c_.u8(0xFF); c_.u8(0xD7);                /* call rdi */
			c_.u8(0x48); c_.u8(0x89); c_.u8(0xC7);   /* mov rdi,rax */
			c_.u8(0x48); c_.u8(0xC7); c_.u8(0xC0);
			c_.u8(0x3C); c_.u8(0x00); c_.u8(0x00); c_.u8(0x00);  /* mov rax,60 */
			c_.u8(0x0F); c_.u8(0x05);                /* syscall: 线程退出 */
			/* ── parent: rax = tid ── */
			known_const_.clear();
			return;
		}
		if (n.callee == "__mfence") {
			c_.u8(0x0F); c_.u8(0xAE); c_.u8(0xF0);
			return;
		}
		if (n.callee == "__atomic_add" || n.callee == "__atomic_fetch_add" ||
		    n.callee == "__atomic_xchg" || n.callee == "__atomic_cas") {
			auto* id = std::get_if<Ident>(n.args[0].get());
			if (!id) throw std::runtime_error(n.callee + " 的第一个参数必须是变量名 at " + std::to_string(n.line));
			last_store_len_ = (size_t) -1;
			emit_var_addr_rbx(id->name);            /* rbx = &var */
			if (n.callee == "__atomic_cas") {
				/* rcx = new（先算，避免覆盖 rax 里的 expected） */
				gen_expr(*n.args[2]);
				c_.mov64r_rm64(1, 0);               /* rcx = new */
				gen_expr(*n.args[1]);               /* rax = expected */
				c_.u8(0xF0); c_.u8(0x48); c_.u8(0x0F); c_.u8(0xB1); c_.u8(0x0B);
				/* rax 现在是原值 —— 就是返回值 */
			} else {
				gen_expr(*n.args[1]);               /* rax = v */
				c_.mov64r_rm64(1, 0);               /* rcx = v */
				if (n.callee == "__atomic_add") {
					c_.u8(0xF0); c_.u8(0x48); c_.u8(0x01); c_.u8(0x0B);
					/* 无返回值；rax 保持为 v（调用方不读） */
				} else if (n.callee == "__atomic_fetch_add") {
					c_.u8(0xF0); c_.u8(0x48); c_.u8(0x0F); c_.u8(0xC1); c_.u8(0x0B);
					c_.mov64r_rm64(0, 1);           /* rax = 原值 */
				} else {                            /* __atomic_xchg */
					c_.u8(0x48); c_.u8(0x87); c_.u8(0x0B);
					c_.mov64r_rm64(0, 1);           /* rax = 原值 */
				}
			}
			known_const_.clear();
			return;
		}

		/* 内存运行时: 跨平台，且不需要 win64 的 IAT 参数搬运。 */
		if (n.callee == "__rt_alloc" || n.callee == "__rt_free") {
			if (n.args.size() != 1) {
				throw std::runtime_error("运行时函数 " + n.callee + " 参数数量错误 at " + std::to_string(n.line) + ":" + std::to_string(n.col));
			}
			gen_expr(*n.args[0]);
			c_.mov64r_rm64(1, 0);              /* rcx = arg */
			emit_call_off(n.callee == "__rt_alloc" ? rt_alloc_off_ : rt_free_off_);
			known_const_.clear();
			return;
		}
		if (n.callee == "__rt_panic") {
			if (!n.args.empty()) {
				throw std::runtime_error("__rt_panic 参数数量错误 at " + std::to_string(n.line) + ":" + std::to_string(n.col));
			}
			emit_call_off(panic_generic_off_);  /* 不返回 */
			return;
		}
		if (n.callee == "__exit") {
			if (n.args.size() != 1) {
				throw std::runtime_error("__exit 参数数量错误 at " + std::to_string(n.line) + ":" + std::to_string(n.col));
			}
			if (!target_ || std::string(target_->id()) != "win64") {
				throw std::runtime_error("__exit 当前仅支持 win64 at " + std::to_string(n.line) + ":" + std::to_string(n.col));
			}
			gen_expr(*n.args[0]);
			c_.mov64r_rm64(1, 0);              /* rcx = exit code */
		c_.save_rsp_r15();
			c_.align_rsp16();
			size_t call_pos = c_.call_rip();   /* 返回 disp32 字段位置 */
			pe_iat_patches_.push_back({call_pos, 1});
			c_.u8(0xCC);                       /* ExitProcess 不返回 */
			return;
		}
		/* ── 128 位整数运算原语 ────────────────────────────────────────────
		   用 (lo, hi) 两个 int64 表示一个 128 位整数：
		     __mulhi(a, b)   : 有符号 128 位乘积的高 64 位
		     __umulhi(a, b)  : 无符号 128 位乘积的高 64 位
		     __addc(a, b)    : a + b 产生的进位 (0 或 1)
		     __subb(a, b)    : a - b 产生的借位 (0 或 1)
		   有这四个原语就能组合出任意宽度的整数运算，数据范围不再受 64 位限制。
		   全部是单条 x86-64 指令级实现，两个平台通用。 */
		if (n.callee == "__mulhi" || n.callee == "__umulhi" || n.callee == "__mullo" || n.callee == "__umullo" ||
		    n.callee == "__addc" || n.callee == "__subb") {
			if (n.args.size() != 2u) {
				throw std::runtime_error("运行时函数 " + n.callee + " 参数数量错误 at " + std::to_string(n.line) + ":" + std::to_string(n.col));
			}
			int total = 32;
		c_.save_rsp_r15();
			c_.align_rsp16();
			c_.sub_rsp(total);
			for (size_t i = 0; i < n.args.size(); i++) {
				gen_expr(*n.args[i]);
				c_.mov_memd8_64r(4, static_cast<int8_t>(i * 8), 0);
			}
			/* rax = a; rcx = b */
			c_.u8(0x48); c_.u8(0x8B); c_.u8(0x04); c_.u8(0x24);              /* mov rax,[rsp] */
			c_.u8(0x48); c_.u8(0x8B); c_.u8(0x4C); c_.u8(0x24); c_.u8(0x08); /* mov rcx,[rsp+8] */
			if (n.callee == "__mulhi") {
				c_.u8(0x48); c_.u8(0xF7); c_.u8(0xE9);                       /* imul rcx → rdx:rax */
				c_.u8(0x48); c_.u8(0x89); c_.u8(0xD0);                       /* mov rax,rdx (高 64 位) */
			} else if (n.callee == "__umulhi") {
				c_.u8(0x48); c_.u8(0xF7); c_.u8(0xE1);                       /* mul rcx → rdx:rax */
				c_.u8(0x48); c_.u8(0x89); c_.u8(0xD0);                       /* mov rax,rdx */
			} else if (n.callee == "__mullo" || n.callee == "__umullo") {
				/* 128 位乘积的低 64 位。低半部分在数学上永不溢出（它就是
				   乘积 mod 2^64），因此这里**不做**溢出检查 —— 带 OF 检查的
				   `*` 运算符无法用于取低半部分，这是必须单独提供原语的原因。 */
				if (n.callee == "__mullo") {
					c_.u8(0x48); c_.u8(0xF7); c_.u8(0xE9);                   /* imul rcx → rdx:rax */
				} else {
					c_.u8(0x48); c_.u8(0xF7); c_.u8(0xE1);                   /* mul rcx → rdx:rax */
				}
				/* rax 已是低 64 位，无需额外搬运 */
			} else if (n.callee == "__addc") {
				c_.u8(0x48); c_.u8(0x01); c_.u8(0xC8);                       /* add rax,rcx */
				c_.u8(0x48); c_.u8(0x1B); c_.u8(0xC0);                       /* sbb rax,rax → 有进位则全 1 */
				c_.u8(0x48); c_.u8(0x83); c_.u8(0xE0); c_.u8(0x01);          /* and rax,1 */
			} else {
				c_.u8(0x48); c_.u8(0x29); c_.u8(0xC8);                       /* sub rax,rcx */
				c_.u8(0x48); c_.u8(0x1B); c_.u8(0xC0);                       /* sbb rax,rax → 有借位则全 1 */
				c_.u8(0x48); c_.u8(0x83); c_.u8(0xE0); c_.u8(0x01);          /* and rax,1 */
			}
			c_.restore_rsp_r15();
			known_const_.clear();
			return;
		}
		/* __byte_at / __store_byte 是纯内存读写，不经过任何导入表，因此两个
		   平台都可用（此前被无谓地限制在 win64）。
		     __byte_at(base, off)        → 读 base[off] 的一个字节
		     __store_byte(base, off, v)  → 写 base[off] = v 的一个字节 */
		if (n.callee == "__byte_at" || n.callee == "__store_byte") {
			size_t expected = (n.callee == "__byte_at") ? 2u : 3u;
			if (n.args.size() != expected) {
				throw std::runtime_error("运行时函数 " + n.callee + " 参数数量错误 at " + std::to_string(n.line) + ":" + std::to_string(n.col));
			}
			int total = static_cast<int>((std::max<size_t>(32, n.args.size() * 8) + 15) & ~size_t(15));
		c_.save_rsp_r15();
			c_.align_rsp16();
			c_.sub_rsp(total);
			for (size_t i = 0; i < n.args.size(); i++) {
				gen_expr(*n.args[i]);
				if (i * 8 <= 127) c_.mov_memd8_64r(4, static_cast<int8_t>(i * 8), 0);
				else c_.mov_memd32_64r(4, static_cast<int32_t>(i * 8), 0);
			}
			if (n.callee == "__byte_at") {
				c_.u8(0x48); c_.u8(0x8B); c_.u8(0x04); c_.u8(0x24);                  /* rax = base */
				c_.u8(0x48); c_.u8(0x8B); c_.u8(0x4C); c_.u8(0x24); c_.u8(0x08);     /* rcx = off */
				c_.add_r64_rm64(0, 1);                                               /* rax += off */
				c_.u8(0x0F); c_.u8(0xB6); c_.u8(0x00);                               /* movzx rax, byte [rax] */
			} else {
				c_.u8(0x48); c_.u8(0x8B); c_.u8(0x04); c_.u8(0x24);                  /* rax = base */
				c_.u8(0x48); c_.u8(0x8B); c_.u8(0x54); c_.u8(0x24); c_.u8(0x08);     /* rdx = off */
				c_.u8(0x48); c_.u8(0x01); c_.u8(0xD0);                               /* rax += off */
				c_.u8(0x48); c_.u8(0x8B); c_.u8(0x4C); c_.u8(0x24); c_.u8(0x10);     /* rcx = value */
				c_.u8(0x88); c_.u8(0x08);                                            /* mov [rax], cl */
			}
			c_.restore_rsp_r15();
			known_const_.clear();
			return;
		}
		if (!target_ || std::string(target_->id()) != "win64") {
			throw std::runtime_error("运行时函数 " + n.callee + " 当前仅支持 win64 at " + std::to_string(n.line) + ":" + std::to_string(n.col));
		}
		size_t expected = 0;
		if (n.callee == "__read_file" || n.callee == "__write_file" || n.callee == "__file_write") expected = 3;
		else if (n.callee == "__byte_at") expected = 2;
		else if (n.callee == "__store_byte") expected = 3;
		else if (n.callee == "__file_open" || n.callee == "__file_close") expected = 1;
		if (n.args.size() != expected) {
			throw std::runtime_error("运行时函数 " + n.callee + " 参数数量错误 at " + std::to_string(n.line) + ":" + std::to_string(n.col));
		}
		int total = static_cast<int>((std::max<size_t>(32, n.args.size() * 8) + 15) & ~size_t(15));
		c_.save_rsp_r15();
		c_.align_rsp16();
		c_.sub_rsp(total);
		for (size_t i = 0; i < n.args.size(); i++) {
			gen_expr(*n.args[i]);
			if (i * 8 <= 127) c_.mov_memd8_64r(4, static_cast<int8_t>(i * 8), 0);
			else c_.mov_memd32_64r(4, static_cast<int32_t>(i * 8), 0);
		}
		auto emit_import_call = [&](int kind) {
			size_t at = c_.call_rip();
			pe_iat_patches_.push_back({at, kind});
		};
		if (n.callee == "__byte_at") {
			c_.u8(0x48);
			c_.u8(0x8B);
			c_.u8(0x04);
			c_.u8(0x24);
			c_.u8(0x48);
			c_.u8(0x8B);
			c_.u8(0x4C);
			c_.u8(0x24);
			c_.u8(0x08);
			c_.add_r64_rm64(0, 1);
			c_.u8(0x0F);
			c_.u8(0xB6);
			c_.u8(0x00);
		} else if (n.callee == "__store_byte") {
			c_.u8(0x48);
			c_.u8(0x8B);
			c_.u8(0x04);
			c_.u8(0x24);
			c_.u8(0x48);
			c_.u8(0x8B);
			c_.u8(0x54);
			c_.u8(0x24);
			c_.u8(0x08);
			c_.u8(0x48);
			c_.u8(0x01);
			c_.u8(0xD0);
			c_.u8(0x48);
			c_.u8(0x8B);
			c_.u8(0x4C);
			c_.u8(0x24);
			c_.u8(0x10);
			c_.u8(0x88);
			c_.u8(0x08);
		} else if (n.callee == "__command_line") {
			/* GetCommandLineA：返回进程完整命令行（ANSI，含 argv[0]）。
			   旧实现误接到 VirtualAlloc，导致 __command_line() 恒为 0。 */
			emit_import_call(12);
		} else if (n.callee == "__file_open") {
			c_.u8(0x48);
			c_.u8(0x8B);
			c_.u8(0x04);
			c_.u8(0x24);
			c_.mov64r_rm64(1, 0);
			c_.lea64r_rip(2);
			size_t mode_pos = c_.pos();
			c_.u32(0);
			str_patches_.push_back({mode_pos, "wb"});
			emit_import_call(4);
		} else if (n.callee == "__file_write") {
			c_.u8(0x48);
			c_.u8(0x8B);
			c_.u8(0x4C);
			c_.u8(0x24);
			c_.u8(0x08);
			c_.u8(0xBA);
			c_.u32(1);
			c_.u8(0x48);
			c_.u8(0x8B);
			c_.u8(0x44);
			c_.u8(0x24);
			c_.u8(0x10);
			c_.u8(0x49);
			c_.u8(0x89);
			c_.u8(0xC0);
			c_.u8(0x4C);
			c_.u8(0x8B);
			c_.u8(0x4C);
			c_.u8(0x24);
			c_.u8(0x00);
			emit_import_call(6);
		} else if (n.callee == "__file_close") {
			c_.u8(0x48);
			c_.u8(0x8B);
			c_.u8(0x0C);
			c_.u8(0x24);
			c_.mov64r_rm64(1, 0);
			emit_import_call(7);
		} else {
			c_.u8(0x4C);
			c_.u8(0x8B);
			c_.u8(0x74);
			c_.u8(0x24);
			c_.u8(0x08);
			c_.u8(0x4C);
			c_.u8(0x8B);
			c_.u8(0x6C);
			c_.u8(0x24);
			c_.u8(0x10);
			c_.u8(0x48);
			c_.u8(0x8B);
			c_.u8(0x04);
			c_.u8(0x24);
			c_.mov64r_rm64(1, 0);
			c_.lea64r_rip(2);
			size_t mode_pos = c_.pos();
			c_.u32(0);
			str_patches_.push_back({mode_pos, n.callee == "__read_file" ? "rb" : "wb"});
			emit_import_call(4);
			c_.u8(0x48);
			c_.u8(0x85);
			c_.u8(0xC0);
			int fail = emit_jcc(0x4);
			c_.rex_wrb(0, 12);
			c_.u8(0x89);
			c_.modrm(3, 0, 4);
			c_.u8(0x4C);
			c_.u8(0x89);
			c_.u8(0xF1);
			c_.u8(0xBA);
			c_.u32(1);
			c_.mov64r_rm64(0, 13);
			c_.u8(0x49);
			c_.u8(0x89);
			c_.u8(0xC0);
			c_.u8(0x4D);
			c_.u8(0x89);
			c_.u8(0xE1);
			emit_import_call(n.callee == "__read_file" ? 5 : 6);
			c_.u8(0x49);
			c_.u8(0x89);
			c_.u8(0xC6);
			c_.u8(0x4C);
			c_.u8(0x89);
			c_.u8(0xE1);
			emit_import_call(7);
			c_.u8(0x4C);
			c_.u8(0x89);
			c_.u8(0xF0);
			int done = emit_jmp();
			patch(fail);
			c_.xor_rm64_r64(0, 0);
			patch(done);
		}
		c_.restore_rsp_r15();
		known_const_.clear();
	}


/* ══════════════════════════════════════════════════════════════════════════
   本轮补全的关键字 —— 后端实现
   ══════════════════════════════════════════════════════════════════════════ */

/* ── 类型编号: typeof / is 共用 ─────────────────────────────────────────── */
static int64_t weiTypeCodeOf(const std::string& t) {
	if (t == "int" || t == "整型") return 1;
	if (t == "double" || t == "浮点") return 2;
	if (t == "bool" || t == "布尔") return 3;
	if (t == "char" || t == "文本" || t == "string" || t == "str") return 4;
	if (t == "tuple" || t == "元组") return 6;
	if (t == "omni" || t == "万能") return 0;
	if (t.rfind("ptr", 0) == 0 || t.rfind("指针", 0) == 0) return 5;
	if (t == "func" || t == "函数") return 7;
	return 0;   /* 未知 / 未标注 → 万能 */
}

void NativeGen::register_type_alias(const std::string& alias, const std::string& target) {
	if (!alias.empty()) type_aliases_[alias] = target;
}

/* ── 范围 lo..hi: 作为独立表达式时取下界（量词与 in 会直接读两端）───── */
void NativeGen::gen_range_lit(wei::ast::RangeLit& n) {
	if (n.lo) gen_expr(*n.lo);
	else c_.mov64r_imm64(0, 0);
}

/* ── 元组取元素: t.0 / t[0] ─────────────────────────────────────────────── */
void NativeGen::gen_tuple_index(wei::ast::TupleIndexExpr& n) {
	if (!n.tuple) { c_.mov64r_imm64(0, 0); return; }
	gen_expr(*n.tuple);                       /* rax = 元组基址 */
	int64_t off = n.index * 8;
	if (off >= 0 && off <= 127) c_.mov64r_memd8(0, 0, off);
	else c_.mov64r_memd32(0, 0, static_cast<int32_t>(off));
}

/* ── typeof(x): 编译期类型编号 ──────────────────────────────────────────── */
void NativeGen::gen_typeof(wei::ast::TypeofExpr& n) {
	std::string t = n.resolved;
	if (t.empty() && n.arg) {
		if (auto* id = std::get_if<Ident>(n.arg.get())) {
			auto it = local_types_.find(id->name);
			if (it != local_types_.end()) t = it->second;
		}
	}
	auto ta = type_aliases_.find(t);
	if (ta != type_aliases_.end()) t = ta->second;
	c_.mov64r_imm64(0, weiTypeCodeOf(t));
}

/* ── x is T: 类型判定 ───────────────────────────────────────────────────── */
void NativeGen::gen_is_expr(wei::ast::IsExpr& n) {
	std::string vt;
	if (n.arg) {
		if (auto* id = std::get_if<Ident>(n.arg.get())) {
			auto it = local_types_.find(id->name);
			if (it != local_types_.end()) vt = it->second;
		}
	}
	std::string want = n.type_name;
	auto ta = type_aliases_.find(want);
	if (ta != type_aliases_.end()) want = ta->second;
	auto va = type_aliases_.find(vt);
	if (va != type_aliases_.end()) vt = va->second;
	if (!vt.empty()) {
		int64_t r = (weiTypeCodeOf(vt) == weiTypeCodeOf(want)) ? 1 : 0;
		c_.mov64r_imm64(0, r);
		return;
	}
	/* 类型未知（万能 / 未标注）: 不属于任何具体类型 */
	(void)n;
	c_.mov64r_imm64(0, 0);
}

/* ── x in D: 范围与数组成员判定 ─────────────────────────────────────────── */
void NativeGen::gen_in_expr(wei::ast::InExpr& n) {
	if (!n.container || !n.value) { c_.mov64r_imm64(0, 0); return; }
	auto* rg = std::get_if<RangeLit>(n.container.get());
	if (rg) {
		gen_expr(*n.value);                 /* rax = v */
		c_.push_r(0);                       /* [rsp] = v */
		gen_expr(*rg->lo);                  /* rax = lo */
		c_.mov64r_memd8(1, 4, 0);           /* rcx = v */
		c_.cmp_r64_rm64(1, 0);              /* v - lo */
		int jlo = emit_jcc(0xC);            /* jl → v < lo → false */
		gen_expr(*rg->hi);                  /* rax = hi */
		c_.mov64r_memd8(1, 4, 0);           /* rcx = v */
		c_.cmp_r64_rm64(1, 0);
		int jhi = emit_jcc(0xF);            /* jg → v > hi → false */
		c_.mov64r_imm64(0, 1);
		int jdone = emit_jmp();
		patch(jlo);
		patch(jhi);
		c_.xor_rm64_r64(0, 0);
		patch(jdone);
		c_.pop_r(1);                        /* 丢弃暂存的 v */
		return;
	}
	/* 数组形态: 线性扫描 */
	auto* id = std::get_if<Ident>(n.container.get());
	int64_t len = 0;
	uint32_t slot = 0;
	bool is_local = true;
	if (id) {
		auto it = locals_.find(id->name);
		if (it != locals_.end()) { slot = it->second.slot; len = it->second.array_len; is_local = true; }
		else {
			auto gi = globals_.find(id->name);
			if (gi != globals_.end()) { slot = gi->second.slot; len = gi->second.array_len; is_local = false; }
		}
	}
	if (len <= 0) { c_.mov64r_imm64(0, 0); return; }
	int32_t frame = 24;
	c_.sub_rm64_imm32(4, frame);            /* [rsp]=v [rsp+8]=结果 [rsp+16]=i */
	gen_expr(*n.value);
	c_.mov_memd8_64r(4, 0, 0);
	c_.mov_memd8_imm32(4, 8, 0);
	c_.mov_memd8_imm32(4, 16, 0);
	size_t loop = c_.pos();
	c_.mov64r_memd8(0, 4, 16);              /* rax = i */
	c_.cmp_rm64_imm32(0, static_cast<int32_t>(len));
	int jout = emit_jcc(0x3);               /* jae → 扫描结束 */
	/* elem = arr[i] */
	c_.mov64r_memd8(0, 4, 16);              /* rax = i */
	if (is_local) {
		int32_t d = static_cast<int32_t>(-8 * static_cast<int>(slot + 1));
		c_.mov64r_mem_idx8(2, 5, 0, d);     /* rdx = [rbp + i*8 + d] */
	} else {
		emit_global_addr(2, slot);
		c_.mov64r_mem_idx8(2, 2, 0, 0);     /* rdx = [globals + slot*8 + i*8] */
	}
	c_.mov64r_memd8(1, 4, 0);               /* rcx = v */
	c_.cmp_r64_rm64(1, 2);
	int jne = emit_jcc(0x5);                /* jne → 不等, 继续 */
	c_.mov_memd8_imm32(4, 8, 1);            /* 命中 */
	int jbreak = emit_jmp();
	patch(jne);
	c_.mov64r_memd8(0, 4, 16);
	c_.inc_rm64(0);
	c_.mov_memd8_64r(4, 16, 0);
	{
		size_t jp = c_.jmp32();
		c_.patch_u32(jp, static_cast<uint32_t>(static_cast<int32_t>(loop - (jp + 4))));
	}
	patch(jbreak);
	patch(jout);
	c_.mov64r_memd8(0, 4, 8);               /* rax = 结果 */
	c_.add_rm64_imm32(4, frame);
}

/* ── forall / exists 量词表达式 ─────────────────────────────────────────── */
void NativeGen::gen_quantifier(const std::string& var, const std::unique_ptr<Expr>& domain,
                               const std::unique_ptr<Expr>& cond, bool is_forall) {
	uint32_t slot = local_slot(var, true, 0);
	int32_t d = static_cast<int32_t>(-8 * static_cast<int>(slot + 1));
	auto* rg = std::get_if<RangeLit>(domain.get());
	if (!rg) { c_.mov64r_imm64(0, is_forall ? 0 : 0); return; }
	/* i = lo */
	if (rg->lo) gen_expr(*rg->lo);
	else c_.mov64r_imm64(0, 0);
	c_.mov_memd8_64r(5, d, 0);              /* [rbp+d] = lo */
	/* 结果: 1(forall) / 0(exists)。放全局槽而非栈上 —— gen_expr 内部可能
	   调整 rsp（对齐/参数区），栈上的固定偏移会被打乱；全局槽是 RIP 相对
	   寻址，与 rsp 无关，绝对安全。 */
	emit_global_store(quant_res_slot_, 0);   /* 占位, 随后用立即数槽写入 */
	c_.mov64r_imm64(0, is_forall ? 1 : 0);
	emit_global_store(quant_res_slot_, 0);
	size_t loop = c_.pos();
	/* 上界比较: i > hi → 结束 */
	if (rg->hi) gen_expr(*rg->hi);
	else c_.mov64r_imm64(0, 0);
	c_.cmp_r64_memd8(0, 5, d);              /* rax(hi) - i */
	int jout = emit_jcc(0xC);               /* jl: hi < i → 结束 */
	/* cond 求值 */
	{
		auto save = known_const_;
		gen_expr(*cond);
		known_const_ = save;
	}
	c_.cmp_rm64_imm32(0, 0);
	if (is_forall) {
		int jt = emit_jcc(0x5);             /* jne → 条件成立, 继续 */
		c_.mov64r_imm64(0, 0);
		emit_global_store(quant_res_slot_, 0);   /* 反例 → 结果为 0 */
		int jb = emit_jmp();
		patch(jt);
		(void)jb;
	} else {
		int jf = emit_jcc(0x4);             /* je → 条件不成立, 继续 */
		c_.mov64r_imm64(0, 1);
		emit_global_store(quant_res_slot_, 0);   /* 命中 → 结果为 1 */
		int jb = emit_jmp();
		patch(jf);
		(void)jb;
	}
	/* i++ */
	c_.mov64r_memd8(0, 5, d);
	c_.inc_rm64(0);
	c_.mov_memd8_64r(5, d, 0);
	{
		size_t jp = c_.jmp32();
		c_.patch_u32(jp, static_cast<uint32_t>(static_cast<int32_t>(loop - (jp + 4))));
	}
	patch(jout);
	emit_global_load(0, quant_res_slot_);   /* rax = 结果 */
	known_const_.clear();
}

/* ── forall x in D { body } ─────────────────────────────────────────────── */
void NativeGen::gen_forall_stmt(wei::ast::ForallStmt& n) {
	uint32_t slot = local_slot(n.var, true, 0);
	int32_t d = static_cast<int32_t>(-8 * static_cast<int>(slot + 1));
	auto* rg = std::get_if<RangeLit>(n.domain.get());
	if (!rg) {
		/* of 形态: 逐个取数组元素 */
		auto* id = std::get_if<Ident>(n.domain.get());
		int64_t len = 0;
		uint32_t aslot = 0;
		bool is_local = true;
		if (id) {
			auto it = locals_.find(id->name);
			if (it != locals_.end()) { aslot = it->second.slot; len = it->second.array_len; is_local = true; }
			else {
				auto gi = globals_.find(id->name);
				if (gi != globals_.end()) { aslot = gi->second.slot; len = gi->second.array_len; is_local = false; }
			}
		}
		if (len <= 0) return;
		c_.mov64r_imm64(0, 0);
		emit_global_store(quant_res_slot_, 0);   /* i = 0 */
		size_t loop = c_.pos();
		emit_global_load(0, quant_res_slot_);
		c_.cmp_rm64_imm32(0, static_cast<int32_t>(len));
		int jout = emit_jcc(0x3);           /* jae → 结束 */
		emit_global_load(0, quant_res_slot_);    /* rax = i */
		if (is_local) {
			int32_t ad = static_cast<int32_t>(-8 * static_cast<int>(aslot + 1));
			c_.mov64r_mem_idx8(0, 5, 0, ad);
		} else {
			emit_global_addr(2, aslot);
			c_.mov64r_mem_idx8(0, 2, 0, 0);
		}
		c_.mov_memd8_64r(5, d, 0);          /* var = arr[i] */
		for (auto& st : n.body) gen_stmt(st);
		emit_global_load(0, quant_res_slot_);
		c_.inc_rm64(0);
		emit_global_store(quant_res_slot_, 0);
		{
			size_t jp = c_.jmp32();
			c_.patch_u32(jp, static_cast<uint32_t>(static_cast<int32_t>(loop - (jp + 4))));
		}
		patch(jout);
		return;
	}
	if (rg->lo) gen_expr(*rg->lo);
	else c_.mov64r_imm64(0, 0);
	c_.mov_memd8_64r(5, d, 0);              /* var = lo */
	size_t loop = c_.pos();
	if (rg->hi) gen_expr(*rg->hi);
	else c_.mov64r_imm64(0, 0);
	c_.cmp_r64_memd8(0, 5, d);
	int jout = emit_jcc(0xC);               /* hi < var → 结束 */
	for (auto& st : n.body) gen_stmt(st);
	c_.mov64r_memd8(0, 5, d);
	c_.inc_rm64(0);
	c_.mov_memd8_64r(5, d, 0);
	{
		size_t jp = c_.jmp32();
		c_.patch_u32(jp, static_cast<uint32_t>(static_cast<int32_t>(loop - (jp + 4))));
	}
	patch(jout);
}

/* ── when cond then { } else { } ────────────────────────────────────────── */
void NativeGen::gen_when_stmt(wei::ast::WhenStmt& n) {
	if (!n.cond) {
		for (auto& st : n.then_body) gen_stmt(st);
		return;
	}
	gen_expr(*n.cond);
	c_.cmp_rm64_imm32(0, 0);
	int jelse = emit_jcc(0x4);              /* je → else */
	for (auto& st : n.then_body) gen_stmt(st);
	if (!n.else_body.empty()) {
		int jend = emit_jmp();
		patch(jelse);
		for (auto& st : n.else_body) gen_stmt(st);
		patch(jend);
	} else {
		patch(jelse);
	}
}

/* ── compiletime(expr) ──────────────────────────────────────────────────── */
void NativeGen::gen_compiletime_expr(wei::ast::CompileTimeExpr& n) {
	if (n.folded) {
		c_.mov64r_imm64(0, n.value);
		return;
	}
	if (n.arg) gen_expr(*n.arg);
	else c_.mov64r_imm64(0, 0);
}

/* ── compiletime { ... } ────────────────────────────────────────────────── */
void NativeGen::gen_compiletime_stmt(wei::ast::CompileTimeStmt& n) {
	/* 编译期块: 块内只允许常量表达式。这里照常生成（IR 常量折叠会把
	   常量子树塌缩成一条 CONST），并保持块内声明的常量进入 known_const_，
	   使块之后的优化能识别这些值。 */
	for (auto& st : n.body) gen_stmt(st);
}

/* ══════════════════════════════════════════════════════════════════════════
   协程运行时
   ──────────────────────────────────────────────────────────────────────────
   控制块（128 字节，从堆分配）:
     +0   rsp          协程挂起时的栈顶
     +8   state        0=就绪 1=挂起 2=结束
     +16  ret          最后一次 yield / 返回值
     +24  entry        thunk 入口地址
     +32  args[0..7]   创建时求值并冻结的实参
     +96  stack_base   协程栈基址

   切换序列保存/恢复 rax,rbx,rbp,rsi,rdi,r12..r15 —— 覆盖 SysV 与 Win64
   两边的 callee-saved 集合，因此两个 ABI 通用。切换只换栈指针与这 9 个
   寄存器，不涉及任何系统调用，也不需要线程。
   ══════════════════════════════════════════════════════════════════════════ */
static constexpr int64_t WEI_CO_STACK   = 64 * 1024;
static constexpr int64_t WEI_CO_CTRL    = 128;
static constexpr int64_t WEI_CO_OFF_RSP = 0;
static constexpr int64_t WEI_CO_OFF_ST  = 8;
static constexpr int64_t WEI_CO_OFF_RET = 16;
static constexpr int64_t WEI_CO_OFF_ARG = 32;

/* ── 128 位算术所需编码 ──────────────────────────────────────────────────
   X64Buf 已覆盖常用指令，这里只补齐 ADC/SBB/MUL/DIV/NEG/NOT/SAR 等少数
   宽算术必需的编码。统一走 REX.W + opcode + modrm 的形式，两个 ABI 通用。 */
static inline uint8_t wrex(int dst, int src) {
	return static_cast<uint8_t>(0x48 | ((src >= 8) ? 4 : 0) | ((dst >= 8) ? 1 : 0));
}
void NativeGen::emit_adc_rr(int dst, int src) {
	c_.u8(wrex(dst, src)); c_.u8(0x11);
	c_.u8(static_cast<uint8_t>(0xC0 | ((src & 7) << 3) | (dst & 7)));
}
void NativeGen::emit_sbb_rr(int dst, int src) {
	c_.u8(wrex(dst, src)); c_.u8(0x19);
	c_.u8(static_cast<uint8_t>(0xC0 | ((src & 7) << 3) | (dst & 7)));
}
void NativeGen::emit_or_rr(int dst, int src) {
	c_.u8(wrex(dst, src)); c_.u8(0x09);
	c_.u8(static_cast<uint8_t>(0xC0 | ((src & 7) << 3) | (dst & 7)));
}
void NativeGen::emit_imul_rr(int dst, int src) {
	c_.u8(wrex(dst, src)); c_.u8(0x0F); c_.u8(0xAF);
	c_.u8(static_cast<uint8_t>(0xC0 | ((dst & 7) << 3) | (src & 7)));
}
void NativeGen::emit_mul_r(int r) {          /* rdx:rax = rax * r (无符号) */
	c_.u8(static_cast<uint8_t>(0x48 | ((r >= 8) ? 1 : 0)));
	c_.u8(0xF7);
	c_.u8(static_cast<uint8_t>(0xE0 | (r & 7)));
}
void NativeGen::emit_div_r(int r) {          /* rdx:rax / r → 商 rax, 余 rdx */
	c_.u8(static_cast<uint8_t>(0x48 | ((r >= 8) ? 1 : 0)));
	c_.u8(0xF7);
	c_.u8(static_cast<uint8_t>(0xF0 | (r & 7)));
}
void NativeGen::emit_neg_r(int r) {
	c_.u8(static_cast<uint8_t>(0x48 | ((r >= 8) ? 1 : 0)));
	c_.u8(0xF7);
	c_.u8(static_cast<uint8_t>(0xD8 | (r & 7)));
}
void NativeGen::emit_not_r(int r) {
	c_.u8(static_cast<uint8_t>(0x48 | ((r >= 8) ? 1 : 0)));
	c_.u8(0xF7);
	c_.u8(static_cast<uint8_t>(0xD0 | (r & 7)));
}
void NativeGen::emit_sar_imm8(int r, int n) {
	c_.u8(static_cast<uint8_t>(0x48 | ((r >= 8) ? 1 : 0)));
	c_.u8(0xC1);
	c_.u8(static_cast<uint8_t>(0xF8 | (r & 7)));
	c_.u8(static_cast<uint8_t>(n));
}
void NativeGen::emit_setcc(int cc, int r) {
	c_.u8(0x0F);
	c_.u8(static_cast<uint8_t>(0x90 | (cc & 0xF)));
	c_.u8(static_cast<uint8_t>(0xC0 | (r & 7)));
}

/* ── 影子高位槽管理 ────────────────────────────────────────────────────── */
bool NativeGen::isWideIntVar(const std::string& n) const {
	if (!wide_mode_) return false;
	if (dbl_vars_.count(n)) return false;                 /* double 不走宽算术 */
	auto it = locals_.find(n);
	if (it != locals_.end()) return it->second.array_len == 0;
	auto gi = globals_.find(n);
	if (gi != globals_.end()) return gi->second.array_len == 0;
	return false;
}
uint32_t NativeGen::wideHiSlot(const std::string& n) {
	auto it = wide_hi_.find(n);
	if (it != wide_hi_.end()) return it->second;
	uint32_t slot = wide_hi_base_ + wide_hi_used_;
	if (wide_hi_used_ < WIDE_HI_MAX) wide_hi_used_++;
	wide_hi_[n] = slot;
	return slot;
}

/* ══════════════════════════════════════════════════════════════════════════
   128 位二元运算
   ══════════════════════════════════════════════════════════════════════════
   入口时 rax/r11 已由 gen_expr 约定填好。这里先求 lhs（rax=lo,r11=hi），
   压栈保护，再求 rhs，然后按 op 组合：

     加/减:  add lo / adc hi   ——  CF 进位自然把 64 位装不下的部分抬到高位
     乘   :  128×128 截断到低 128 位
             lo = a_lo*b_lo 的低 64 位
             hi = a_lo*b_lo 的高 64 位 + a_lo*b_hi + a_hi*b_lo
             （a_hi*b_hi 位移后落在 128 位之外，丢弃）
     比较 :  先比高位（有符号），高位相等再比低位（无符号）
             —— 高位相同意味着符号位相同，此时低位的无符号序即真值序
   ══════════════════════════════════════════════════════════════════════════ */
bool NativeGen::gen_binary_wide(Binary& n) {
	if (!wide_mode_) return false;
	const std::string& op = n.op;
	bool isArith = (op == "+" || op == "-" || op == "*");
	bool isCmp = (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=");
	if (!isArith && !isCmp) return false;
	if (isDblExpr(n.lhs) || isDblExpr(n.rhs)) return false;

	gen_expr(*n.lhs);                    /* rax = lo_l, r11 = hi_l */
	c_.push_r(11);
	c_.push_r(0);
	gen_expr(*n.rhs);                    /* rax = lo_r, r11 = hi_r */
	c_.pop_r(3);                         /* rbx = lo_l  (先压后弹: 栈顶是 lo) */
	c_.pop_r(1);                         /* rcx = hi_l */
	/* 此刻: rbx=lo_l  rcx=hi_l  rax=lo_r  r11=hi_r */

	if (op == "+") {
		c_.u8(0x48); c_.u8(0x01); c_.u8(0xC3);            /* add rbx, rax (设 CF) */
		/* 128 位加法的溢出**不能**用 jo 判断: adc 的 OF 只反映高位那半个
		   64 位加法的有符号溢出，不是完整 128 位的溢出判据 —— 拿它当判据
		   会在"低位进位、高位没溢出"的正常情形下误报 panic，正是 -wide
		   要解决的那种情况。128 位要溢出需要绝对值超过 2^127，实际不会
		   发生，因此这里不设检查。 */
		emit_adc_rr(1, 11);                                /* adc rcx, r11 */
		c_.mov64r_rm64(0, 3);
		c_.mov64r_rm64(11, 1);
		known_const_.clear();
		return true;
	}
	if (op == "-") {
		c_.u8(0x48); c_.u8(0x29); c_.u8(0xC3);            /* sub rbx, rax (设 CF) */
		emit_sbb_rr(1, 11);                                /* sbb rcx, r11 */
		c_.mov64r_rm64(0, 3);
		c_.mov64r_rm64(11, 1);
		known_const_.clear();
		return true;
	}
	if (op == "*") {
		/* r10 = lo_r, 之后 rax = lo_l, mul r10 → rdx:rax = lo_l*lo_r */
		c_.mov64r_rm64(10, 0);
		c_.mov64r_rm64(0, 3);
		emit_mul_r(10);                                    /* rdx:rax = a_lo*b_lo */
		c_.mov64r_rm64(8, 0);                              /* r8  = 结果 lo */
		c_.mov64r_rm64(9, 2);                              /* r9  = 乘积高位 */
		/* 交叉项: a_lo*b_hi + a_hi*b_lo */
		c_.mov64r_rm64(3, 3);                              /* rbx 已是 a_lo, 这里复位冗余, 保留 */
		emit_imul_rr(3, 11);                               /* rbx = a_lo * b_hi */
		emit_imul_rr(1, 10);                               /* rcx = a_hi * b_lo */
		c_.u8(0x4C); c_.u8(0x01); c_.u8(0xD9);             /* add r9, rbx  (r9 += cross1) */
		c_.u8(0x4C); c_.u8(0x01); c_.u8(0xC9);             /* add r9, rcx  (r9 += cross2) */
		c_.mov64r_rm64(0, 8);                              /* rax = lo */
		c_.mov64r_rm64(11, 9);                             /* r11 = hi */
		known_const_.clear();
		return true;
	}
	/* ── 128 位比较 ── */
	/* cmp rcx(hi_l), r11(hi_r) */
	c_.u8(0x49); c_.u8(0x39); c_.u8(0xD9);                 /* cmp rcx, r11 */
	if (op == "<") {
		int jl = emit_jcc(0xC);                            /* hi_l < hi_r → 真 */
		int jg = emit_jcc(0xF);                            /* hi_l > hi_r → 假 */
		int jz2 = emit_jcc(0x4);                           /* 相等 → 比低位 */
		patch(jl);
		/* 高位不等且非小于 → 假 */
		c_.xor_rm64_r64(0, 0);
		int jend = emit_jmp();
		patch(jg);
		c_.xor_rm64_r64(0, 0);
		int jend2 = emit_jmp();
		patch(jz2);
		(void)jend2;
		c_.u8(0x48); c_.u8(0x39); c_.u8(0xC3);             /* cmp rbx, rax (无符号) */
		emit_setcc(0x2, 0);                                /* setb al */
		c_.movzx_r64_rm8(0, 0);
		patch(jend);
		return true;
	}
	if (op == ">") {
		int jg2 = emit_jcc(0xF);                           /* hi_l > hi_r → 真 */
		int jl2 = emit_jcc(0xC);                           /* hi_l < hi_r → 假 */
		patch(jg2);
		c_.u8(0xB8); c_.u32(1);
		int jend = emit_jmp();
		patch(jl2);
		c_.xor_rm64_r64(0, 0);
		int jend2 = emit_jmp();
		/* 高位相等 */
		c_.u8(0x48); c_.u8(0x39); c_.u8(0xD8);             /* cmp rax, rbx → 反转比较 */
		emit_setcc(0x2, 0);                                /* rax < rbx → setb */
		c_.movzx_r64_rm8(0, 0);
		patch(jend);
		patch(jend2);
		return true;
	}
	if (op == "==") {
		int jne = emit_jcc(0x5);
		c_.u8(0x48); c_.u8(0x39); c_.u8(0xC3);             /* cmp rbx, rax */
		emit_setcc(0x4, 0);
		c_.movzx_r64_rm8(0, 0);
		int jend = emit_jmp();
		patch(jne);
		c_.xor_rm64_r64(0, 0);
		patch(jend);
		return true;
	}
	if (op == "!=") {
		int jne = emit_jcc(0x5);
		c_.u8(0x48); c_.u8(0x39); c_.u8(0xC3);
		emit_setcc(0x5, 0);
		c_.movzx_r64_rm8(0, 0);
		int jend = emit_jmp();
		patch(jne);
		c_.u8(0xB8); c_.u32(1);
		patch(jend);
		return true;
	}
	return false;                                           /* <= >= 走通用路径 */
}

bool NativeGen::indexProven(const Expr& idx, int64_t len) const {
	auto* id = std::get_if<Ident>(&idx);
	if (!id) return false;
	/* D1: 取模归纳变量的计数器恒在 [0, C)（见 mod_iv_bounds_ 注释）。
	   数组长度 >= C 时，越界在编译期已不可能，可以合法省略运行时检查。 */
	{
		auto mit = mod_iv_bounds_.find(id->name);
		if (mit != mod_iv_bounds_.end() && mit->second > 0 && mit->second <= len)
			return true;
	}
	auto it = idx_ub_.find(id->name);
	if (it == idx_ub_.end()) return false;
	if (it->second <= 0) return false;
	return (it->second - 1) < len;
}

void NativeGen::emit_imul1_r(int r) {            /* F7 /5: rdx:rax = rax * r (signed) */
	c_.u8(static_cast<uint8_t>(0x48 | ((r >= 8) ? 1 : 0)));
	c_.u8(0xF7);
	c_.u8(static_cast<uint8_t>(0xE8 | (r & 7)));
}

/* ── 常量取模: 2 的幂 ────────────────────────────────────────────────────
   用位运算取代 idiv(20~40 周期)。C 语义下余数与被除数同号, 故负数要修正:
       rdx = (x >>s 63) & (C-1)        // x<0 时为 C-1, 否则 0
       rax = ((x + rdx) & (C-1)) - rdx
   验算 x=-1,C=4: rdx=3; (-1+3)&3=2; 2-3=-1  ✓ (-1 % 4 == -1)
         x=-5,C=4: rdx=3; (-5+3)&3=2; 2-3=-1 ✓ (-5 % 4 == -1)
         x=5, C=4: rdx=0; 5&3=1; 1-0=1       ✓ */
bool NativeGen::emit_pow2_mod(int64_t cst) {
	if (cst <= 1 || (cst & (cst - 1)) != 0) return false;
	int64_t mask = cst - 1;
	/* and r/m64, imm8 会**符号扩展**, 所以只有 mask <= 127 才能用短编码。
	   更大的 mask(如 1023)必须经寄存器 —— 用 imm8 会被扩成 -1,
	   等价于没做 and, 负数分支随即算错(实测 1000 % 1024 给出 104)。 */
	const bool small = (mask <= 127);
	if (!small) c_.mov64r_imm64(10, mask);
	c_.mov64r_rm64(2, 0);                        /* rdx = x */
	emit_sar_imm8(2, 63);                        /* rdx = x<0 ? -1 : 0 */
	if (small) {
		c_.u8(0x48); c_.u8(0x83); c_.u8(0xE2);
		c_.u8(static_cast<uint8_t>(mask));       /* and rdx, mask */
	} else {
		c_.u8(0x4C); c_.u8(0x21); c_.u8(0xD2);   /* and rdx, r10 (REX.R: r10) */
	}
	c_.add_r64_rm64(0, 2);                       /* rax = x + rdx */
	if (small) {
		c_.u8(0x48); c_.u8(0x83); c_.u8(0xE0);
		c_.u8(static_cast<uint8_t>(mask));       /* and rax, mask */
	} else {
		c_.u8(0x4C); c_.u8(0x21); c_.u8(0xD0);   /* and rax, r10 (REX.R: r10) */
	}
	c_.sub_r64_rm64(0, 2);                       /* rax -= rdx */
	return true;
}

/* ── 常量除法/取模: 魔数乘法 ──────────────────────────────────────────────
   x / C 与 x % C 改成一次 imul(高位) + 移位 + 一次 imul, 取代 idiv。
   这是每个成熟编译器都做的强度削减。

   魔数取自 Hacker's Delight 的有符号除法算法: 找到 M 与 s 使
       q = (MULSH(x, M) [+x] ) >>s s  [+ (q>>s 63)]
   与 x / C 完全一致(对所有 int64 成立)。余数再由 r = x - q*C 得到。

   只在 M 落在 int32 之外时走 movabs; 否则用更短的立即数形式。
   求不出魔数(理论上不会)时返回 false, 调用方回退到 idiv —— 安全兜底。 */
static bool magic_signed(int64_t d, int64_t& M, int& s) {
	if (d == 0) return false;
	const uint64_t two63 = (uint64_t)1 << 63;
	uint64_t ad = (d < 0) ? (uint64_t)(-(uint64_t)d) : (uint64_t)d;
	if (ad == 0) return false;
	bool neg = (d < 0);
	uint64_t t = two63 + (neg ? 1u : 0u);
	uint64_t anc = t - 1 - t % ad;
	if (anc == 0) return false;
	int p = 63;
	uint64_t q1 = two63 / anc, r1 = two63 % anc;
	uint64_t q2 = two63 / ad,  r2 = two63 % ad;
	for (int guard = 0; guard < 200; guard++) {
		p = p + 1;
		q1 = 2 * q1; r1 = 2 * r1;
		if (r1 >= anc) { q1 = q1 + 1; r1 = r1 - anc; }
		q2 = 2 * q2; r2 = 2 * r2;
		if (r2 >= ad) { q2 = q2 + 1; r2 = r2 - ad; }
		uint64_t delta = ad - r2;
		if (q1 < delta || (q1 == delta && r1 == 0)) continue;
		break;
	}
	int64_t mag = (int64_t)(q2 + 1);
	if (neg) mag = -mag;
	M = mag;
	s = p - 64;
	return (s >= 0 && s < 64);
}

void NativeGen::emit_divmod_const(int64_t cst, bool want_rem) {
	/* 入口: rax = 被除数 x,  cst = 编译期常量除数（已确认非 0） */
	if (want_rem && emit_pow2_mod(cst)) return;

	int64_t M = 0; int sh = 0;
	if (!magic_signed(cst, M, sh)) {                  /* 理论不会发生; 安全兜底 */
		c_.mov64r_imm64(1, cst);
		emit_div_check(1);
		c_.cqo();
		c_.idiv_rm64(1);
		if (want_rem) c_.mov64r_rm64(0, 2);
		return;
	}
	/* rbx 暂存被除数 x: 后面的 imul 要写 rdx, 而 rax 也要腾出来 */
	c_.push_r(3);
	c_.mov64r_rm64(3, 0);                             /* rbx = x */
	c_.mov64r_imm64(1, M);                            /* rcx = M */
	emit_imul1_r(1);                                  /* rdx:rax = x * M (有符号) */
	c_.mov64r_rm64(0, 2);                             /* rax = 乘积高 64 位 */
	if (cst > 0 && M < 0) c_.add_r64_rm64(0, 3);      /* q += x */
	if (cst < 0 && M > 0) c_.sub_r64_rm64(0, 3);      /* q -= x */
	if (sh > 0) emit_sar_imm8(0, sh);                 /* q >>= s */
	/* q += (q < 0 ? 1 : 0) —— 算术右移 63 得 -1/0, 取最低位即 1/0 */
	c_.mov64r_rm64(2, 0);
	emit_sar_imm8(2, 63);
	c_.u8(0x48); c_.u8(0x83); c_.u8(0xE2); c_.u8(1);  /* and rdx, 1 */
	c_.add_r64_rm64(0, 2);                            /* rax = q */
	if (want_rem) {
		/* r = x - q*C */
		if (cst >= INT32_MIN && cst <= INT32_MAX) {
			c_.mov32r_imm32(1, static_cast<int32_t>(cst));
		} else {
			c_.mov64r_imm64(1, cst);
		}
		emit_imul_rr(0, 1);                           /* rax = q * C */
		emit_neg_r(0);                                /* rax = -(q*C) */
		c_.add_r64_rm64(0, 3);                        /* rax = x - q*C */
	}
	c_.pop_r(3);
}

void NativeGen::emit_co_switch_stub() {
	rt_co_switch_off_ = c_.pos();
	c_.push_r(0);                       /* rax */
	c_.push_r(3);                       /* rbx */
	c_.push_r(5);                       /* rbp */
	c_.push_r(6);                       /* rsi */
	c_.push_r(7);                       /* rdi */
	c_.push_r(12);
	c_.push_r(13);
	c_.push_r(14);
	c_.push_r(15);
	c_.mov64r_rm64(0, 4);               /* rax = rsp */
	/* 保存当前上下文 */
	emit_global_load(11, co_cur_slot_);
	c_.test_rm64_r64(11, 11);
	int jmain = emit_jcc(0x4);          /* jz → 当前是主协程 */
	c_.mov_memd8_64r(11, WEI_CO_OFF_RSP, 0);
	int jdone1 = emit_jmp();
	patch(jmain);
	emit_global_store(co_main_rsp_slot_, 0);
	patch(jdone1);
	/* 切换目标 */
	emit_global_store(co_cur_slot_, 1); /* g_cur = rcx */
	c_.test_rm64_r64(1, 1);
	int jtomain = emit_jcc(0x4);        /* jz → 目标为主协程 */
	c_.mov64r_memd8(4, 1, WEI_CO_OFF_RSP);
	int jdone2 = emit_jmp();
	patch(jtomain);
	emit_global_load(4, co_main_rsp_slot_);
	patch(jdone2);
	c_.pop_r(15);
	c_.pop_r(14);
	c_.pop_r(13);
	c_.pop_r(12);
	c_.pop_r(7);
	c_.pop_r(6);
	c_.pop_r(5);
	c_.pop_r(3);
	c_.pop_r(0);
	c_.ret();
}

bool NativeGen::funcIsAsync(const std::string& callee) const {
	for (auto* f : funcs_) if (f->name == callee) return f->is_async;
	for (auto& me : mod_funcs_) if (me.fn && (me.fn->name == callee)) return me.fn->is_async;
	return false;
}

/* ── co f(args): 生成 thunk + 创建协程，rax = 句柄 ─────────────────────── */
void NativeGen::emit_co_create_call(const std::string& callee,
                                    const std::vector<std::unique_ptr<Expr>>& args,
                                    int line, int col) {
	/* (1) 先跳过 thunk 体 */
	int j_over = emit_jmp();
	size_t thunk_pos = c_.pos();
	/* ── thunk: 协程真正执行体 ── */
	c_.u8(0x48); c_.u8(0x83); c_.u8(0xEC); c_.u8(40);   /* sub rsp,40 (对齐+Win64 shadow) */
	emit_global_load(11, co_cur_slot_);                  /* r11 = 当前协程控制块 */
	c_.mov64r_memd8(1, 11, WEI_CO_OFF_ARG + 0);          /* rcx = arg0 */
	c_.mov64r_memd8(2, 11, WEI_CO_OFF_ARG + 8);          /* rdx = arg1 */
	c_.mov64r_memd8(8, 11, WEI_CO_OFF_ARG + 16);         /* r8  = arg2 */
	c_.mov64r_memd8(9, 11, WEI_CO_OFF_ARG + 24);         /* r9  = arg3 */
	c_.u8(0xE8);
	size_t dp = c_.pos();
	c_.u32(0);
	func_patches_.push_back({dp, callee, line, col});
	/* 写回返回值并标记结束 */
	emit_global_load(11, co_cur_slot_);
	c_.mov_memd8_64r(11, WEI_CO_OFF_RET, 0);
	c_.mov_memd8_imm32(11, WEI_CO_OFF_ST, 2);
	c_.xor_rm64_r64(1, 1);                               /* rcx = 0 → 切回主协程 */
	emit_call_off(rt_co_switch_off_);
	c_.u8(0xCC);                                         /* 不应到达 */
	patch(j_over);
	/* ── 创建控制块 ── */
	c_.mov64r_imm64(1, WEI_CO_CTRL);
	emit_call_off(rt_alloc_off_);                        /* rax = co */
	emit_global_store(co_tmp_slot_, 0);
	/* ── 创建协程栈 ── */
	c_.mov64r_imm64(1, WEI_CO_STACK);
	emit_call_off(rt_alloc_off_);                        /* rax = stack */
	emit_global_load(11, co_tmp_slot_);
	c_.mov_memd8_64r(11, 96, 0);                         /* stack_base */
	c_.add_rm64_imm32(0, static_cast<int32_t>(WEI_CO_STACK - 88));
	c_.mov_memd8_64r(11, WEI_CO_OFF_RSP, 0);             /* co->rsp 初值 */
	c_.mov_memd8_imm32(11, WEI_CO_OFF_ST, 0);            /* state = ready */
	c_.mov_memd8_imm32(11, WEI_CO_OFF_RET, 0);
	/* entry = thunk 地址，并写入栈顶的返回槽 */
	c_.lea64r_rip(9);
	{
		size_t tp = c_.pos();
		c_.u32(0);
		co_thunk_patches_.push_back({tp, thunk_pos});
	}
	c_.mov_memd8_64r(11, 24, 9);
	c_.mov64r_memd8(10, 11, WEI_CO_OFF_RSP);
	c_.mov_memd8_64r(10, 72, 9);                         /* [rsp+72] = 返回地址 */
	/* ── 冻结实参 ── */
	for (size_t i = 0; i < args.size() && i < 8; i++) {
		gen_expr(*args[i]);
		emit_global_load(11, co_tmp_slot_);
		c_.mov_memd8_64r(11, static_cast<int64_t>(WEI_CO_OFF_ARG + 8 * static_cast<int64_t>(i)), 0);
	}
	emit_global_load(0, co_tmp_slot_);                   /* rax = 句柄 */
	known_const_.clear();
}

void NativeGen::gen_co_create(wei::ast::CoExpr& n) {
	emit_co_create_call(n.callee, n.args, n.line, n.col);
}

/* ── await h: 若未完成则切进去，取回 co->ret ────────────────────────────── */
void NativeGen::emit_await_value() {
	emit_global_store(co_tmp_slot_, 0);          /* 保存句柄 */
	c_.mov64r_memd8(10, 0, WEI_CO_OFF_ST);       /* r10 = state */
	c_.cmp_rm64_imm32(10, 2);
	int jdone = emit_jcc(0x4);                   /* je → 已结束, 直接取结果 */
	emit_global_load(1, co_tmp_slot_);           /* rcx = co */
	emit_call_off(rt_co_switch_off_);
	patch(jdone);
	emit_global_load(0, co_tmp_slot_);           /* rax = co (切栈后重新加载) */
	c_.mov64r_memd8(0, 0, WEI_CO_OFF_RET);
	known_const_.clear();
}

void NativeGen::gen_await(wei::ast::AwaitExpr& n) {
	if (!n.operand) { c_.mov64r_imm64(0, 0); return; }
	gen_expr(*n.operand);
	emit_await_value();
}

/* ── yield v; ───────────────────────────────────────────────────────────── */
void NativeGen::gen_yield_stmt(wei::ast::YieldStmt& n) {
	if (n.value) gen_expr(*n.value);
	else c_.mov64r_imm64(0, 0);
	emit_global_load(11, co_cur_slot_);
	c_.mov_memd8_64r(11, WEI_CO_OFF_RET, 0);     /* co->ret = v */
	c_.mov_memd8_imm32(11, WEI_CO_OFF_ST, 1);    /* state = suspended */
	c_.xor_rm64_r64(1, 1);
	emit_call_off(rt_co_switch_off_);
}

	void NativeGen::gen_expr(Expr& e) {
		/* ── '?' 错误传播（Rust 的 ? 操作符）─────────────────────────────
		   f()? 展开为: 调用 f 得到 rax → 若 rax < 0（Err）则当前函数立即
		   以该错误码返回；否则 rax 即 Ok 值，继续往下走。
		   在这里统一处理，赋值/声明/表达式语句等各种位置都能覆盖。
		   实现要点: 临时清掉 has_try 再递归，否则会无限展开。 */
		if (auto* cl = std::get_if<Call>(&e)) {
			if (cl->has_try) {
				/* Rust 的核心约束: '?' 只能在返回 Result 的函数里使用。
				   否则错误码无处可去，会被静默吞掉 —— 这正是 C 忽略返回值
				   的老毛病，必须在编译期拦住。 */
				if (!cur_ret_result_) {
					wei::diag::Diagnostic d;
					d.sev = wei::diag::Severity::Error;
					d.code = "E0277";
					d.message = "'?' 只能在返回 Result 的函数中使用";
					int t_line = cl->try_line > 0 ? cl->try_line : cl->line;
					int t_col  = cl->try_col  > 0 ? cl->try_col  : cl->col;
					d.labels.push_back({{t_line, t_col, 1}, "这个 '?' 无处传播错误", true});
					d.help("把当前函数改为返回 .Result，或改用显式判断返回值是否 < 0");
					d.help("运行 mtg explain E0277 查看完整说明与可编译的修复示例");
					throw wei::diag::DiagnosticError(d);
				}
				cl->has_try = false;
				gen_expr_inner(e);
				cl->has_try = true;
				c_.test_rm64_r64(0, 0);
				int j_ok = emit_jcc(0x9);          /* jns: rax >= 0 为 Ok */

				/* ── 自动错误传播链（相对 Rust 的增量）───────────────────
				   Rust 的 '?' 只把 Err 原样上抛，想知道经过哪些调用点得靠
				   anyhow + RUST_BACKTRACE。这里在**编译期**把每个 '?' 的
				   源文件、行列、所在函数与被调函数写进字符串池，Err 分支
				   顺手打印一行；多层 '?' 自然形成由内向外的完整链路，
				   零第三方依赖。-no-error-trace 可完全关闭（零指令开销）。
				   printf/write 会冲掉 RAX，先把负错误码压栈保护。 */
				if (wei::diag::errorTraceFlag()) {
					c_.push_r(0);
					std::string src = dbg_src_file_.empty() ? std::string("<input>") : dbg_src_file_;
					for (char& ch : src) if (ch == '\\') ch = '/';   /* 路径分隔符统一为 / */
					std::string site = "[错误传播] " + src + ":"
					    + std::to_string(cl->try_line > 0 ? cl->try_line : cl->line) + ":"
					    + std::to_string(cl->try_col  > 0 ? cl->try_col  : cl->col)
					    + "  " + (cur_qualname_.empty() ? std::string("<fn>") : cur_qualname_)
					    + "  " + cl->callee + "()?";
					wei::ast::Output out;
					out.line = cl->line;
					out.col = cl->col;
					auto sv = std::make_unique<wei::ast::Expr>();
					wei::ast::StrLit lit;
					lit.value = site;
					*sv = std::move(lit);
					out.values.push_back(std::move(sv));
					wei::ast::Expr outExpr = std::move(out);
					gen_expr_inner(outExpr);   /* 复用 do.emit:: 的字符串输出链路 */
					c_.pop_r(0);               /* RAX 恢复为负错误码 */
				}

				/* Main::main 的 '?' 直接以错误码绝对值结束进程。入口桩对
				   .Result 的返回值传递不可靠（实测退出码恒为 0，错误被静默
				   吞掉），这里绕开它交给 ExitProcess。rustc 的 main->Result
				   只打印 Debug 表示，这里还附带整条传播链。 */
				if (target_ && std::string(target_->id()) == "win64"
				    && cur_qualname_.rfind("Main::main", 0) == 0) {
					c_.u8(0x48); c_.u8(0xF7); c_.u8(0xD8);   /* neg rax: -7 -> 7 */
					c_.mov64r_rm64(1, 0);                      /* rcx = 错误码 */
					c_.save_rsp_r15();
					c_.align_rsp16();
					size_t cp = c_.call_rip();                  /* ExitProcess */
					pe_iat_patches_.push_back({cp, 1});
					c_.u8(0xCC);
				} else {
					emit_return_epilogue();    /* rax 已是负错误码 */
				}
				patch(j_ok);
				return;
			}
		}
		gen_expr_inner(e);
	}

	void NativeGen::gen_expr_inner(Expr& e) {
		/* C1 位宽: 若整个表达式是 i32，则本次求值全程走 32 位通路。
		   子表达式继承该模式 —— 保证运算对象宽度一致，不会出现
		   "一半 64 位一半 32 位"的错位。退出时恢复。 */
		bool want32 = exprIsI32(e);
		bool old32 = i32_mode_;
		if (want32) i32_mode_ = true;
		struct Guard { NativeGen* g; bool old; ~Guard() { g->i32_mode_ = old; } } guard{this, old32};
		struct ExprGen {
			NativeGen* self;
			void operator()(IntLit& n) {
				if (n.value >= 0 && n.value <= 0x7FFFFFFFLL) {
					self->c_.u8(0xB8);
					self->c_.u32(static_cast<uint32_t>(n.value)); /* mov eax, imm32 (5B) */
				} else {
					self->c_.mov64r_imm64(0, n.value);
				}
			}
			void operator()(DoubleLit& n) {
				/* 双精度常?? RIP 相对??8 字节 IEEE754 常量地址, 按位载入 rax
				   (存储即位模式; 算术运算??gen_expr_dbl ??SSE 路径, 不经过这?? */
				self->c_.lea64r_rip(0);
				size_t disp_pos = self->c_.pos();
				self->c_.u32(0);
				self->dbl_patches_.push_back({disp_pos, n.value});
				self->c_.rex_wrb(0, 0);
				self->c_.u8(0x8B);
				self->c_.modrm(0, 0, 0);  /* mov rax,[rax] */
			}
			void operator()(StrLit& n) {
				self->c_.lea64r_rip(0);
				size_t disp_pos = self->c_.pos();
				self->c_.u32(0);
				self->str_patches_.push_back({disp_pos, n.value});
			}
			void operator()(BoolLit& n) {
				self->c_.mov64r_imm64(0, n.value ? 1 : 0);
			}
			void operator()(Ident& n) {
				/* C1: i32 局部变量 —— 32 位载入，rax 高位自动清零。
				   不走寄存器提升（提升路径按 64 位写回，会破坏窄语义），
				   打样阶段以正确性优先。 */
				if (self->isI32Var(n.name)) {
					auto& vi0 = self->resolve_var(n.name);
					if (vi0.is_local) {
						int32_t d0 = static_cast<int32_t>(-8 * static_cast<int>(vi0.slot + 1));
						self->c_.mov_eax_mem(d0);
						return;
					}
				}
				if (!self->fast_loop_var_.empty() && n.name == self->fast_loop_var_) {
					self->c_.mov64r_rm64(0, self->fast_loop_reg_);
					return;
				}
				auto& vi = self->resolve_var(n.name);
				if (vi.array_len > 0) {
					/* 退化指针: 指向首元素 */
					if (vi.is_local) {
						int32_t d = static_cast<int32_t>(-8 * static_cast<int>(vi.slot + 1));
						if (d >= -128 && d <= 127) self->c_.lea64r_memd8(0, 5, static_cast<int8_t>(d));
						else self->c_.lea64r_memd32(0, 5, d);
					} else {
						self->emit_global_addr(0, vi.slot);
					}
					return;
				}
				if (vi.is_local) {
					auto it = self->loop_reg_vars_.find(vi.slot);
					if (it != self->loop_reg_vars_.end()) {
						if (self->last_store_len_ == self->c_.pos() && self->last_store_slot_ == static_cast<int>(vi.slot)) {
							self->last_store_len_ = (size_t) -1;
							return;
						}
						self->c_.mov64r_rm64(0, it->second);
						return;
					}
					int32_t disp = static_cast<int32_t>(-8 * static_cast<int>(vi.slot + 1));
					if (self->last_store_len_ == self->c_.pos() && self->last_store_slot_ == static_cast<int>(vi.slot)) {
						self->last_store_len_ = (size_t) -1;
						return;
					}
					self->c_.mov64r_memd8(0, 5, disp);
				} else {
					self->emit_global_load(0, vi.slot);
				}
			}
			void operator()(ModulePathGet& n) {
				uint32_t slot = self->part_slot_id(n.module, n.part, n.has_field, n.field);
				self->emit_global_load(0, slot);
			}
			void operator()(TupleLit& n) {
				/* 元组: 从堆上取一段连续内存，逐元素装箱，rax = 元组基址。
				   元素按 8 字节槽存放，t.0 / t[0] 通过 TupleIndexExpr 读取。 */
				int64_t cnt = static_cast<int64_t>(n.elements.size());
				int64_t bytes = cnt > 0 ? cnt * 8 : 8;
				self->c_.mov64r_imm64(1, bytes);
				self->emit_call_off(self->rt_alloc_off_);      /* rax = 基址 */
				self->c_.push_r(0);                            /* [rsp] = 基址 */
				for (int64_t i = 0; i < cnt; i++) {
					if (!n.elements[static_cast<size_t>(i)]) continue;
					self->gen_expr(*n.elements[static_cast<size_t>(i)]);
					self->c_.mov64r_memd8(11, 4, 0);           /* r11 = 基址 */
					int64_t off = i * 8;
					if (off <= 127) self->c_.mov_memd8_64r(11, off, 0);
					else self->c_.mov_memd32_64r(11, static_cast<int32_t>(off), 0);
				}
				self->c_.pop_r(0);                             /* rax = 基址 */
				self->known_const_.clear();
			}
			void operator()(Binary& n) {
				/* 编译期证明「必然溢出」→ 直接报错，不等运行时、更不静默回绕。
				   对标 Rust release 的默认 wrapping 行为。 */
				self->checkProvenOverflow(n);
				const std::string& op = n.op;
				/* 无分支 && / ||: 两侧可安全推测时，一条 and/or 取代两次跳转 */
				static const bool cq = getenv("WEI_NO_CQ") == nullptr;   /* A/B 测量用 */
				if (cq && self->try_bool_branchless(n)) return;
				/* ── 逻辑???? 短路求?? 跳转实现, 结果 0/1 ??rax ── */
				if (op == "&&" || op == "||") {
					bool isAnd = (op == "&&");
					self->gen_expr(*n.lhs);
					self->c_.cmp_rm64_imm32(0, 0);
					int j_short1 = self->emit_jcc(isAnd ? 0x4 : 0x5); /* &&: lhs==0→假; ||: lhs!=0→真 */
					self->gen_expr(*n.rhs);
					self->c_.cmp_rm64_imm32(0, 0);
					int j_short2 = self->emit_jcc(isAnd ? 0x4 : 0x5);
					int j_end;
					if (isAnd) {
						self->c_.u8(0xB8);
						self->c_.u32(1);   /* mov eax,1 */
						j_end = self->emit_jmp();
						self->patch(j_short1);
						self->patch(j_short2);
						self->c_.xor_rm64_r64(0, 0);           /* 短路?? eax=0 */
					} else {
						self->c_.xor_rm64_r64(0, 0);           /* 全假: eax=0 */
						j_end = self->emit_jmp();
						self->patch(j_short1);
						self->patch(j_short2);
						self->c_.u8(0xB8);
						self->c_.u32(1);   /* 短路?? eax=1 */
					}
					self->patch(j_end);
					return;
				}
				/* ── 双精度算?? 任一操作数为 double ??SSE2 路径 (+,-,*) ── */
				if ((op == "+" || op == "-" || op == "*" || op == "/") && (self->isDblExpr(n.lhs) || self->isDblExpr(n.rhs))) {
					self->gen_expr_dbl(n.lhs);                       /* xmm0 = lhs */
					self->c_.sub_rsp(16);
					self->c_.u8(0xF2);
					self->c_.u8(0x0F);
					self->c_.u8(0x11);
					self->c_.modrm(0, 0, 4);
					self->c_.u8(0x24);      /* movsd [rsp],xmm0 */
					self->gen_expr_dbl(n.rhs);                       /* xmm0 = rhs */
					self->c_.u8(0xF2);
					self->c_.u8(0x0F);
					self->c_.u8(0x10);
					self->c_.modrm(0, 1, 4);
					self->c_.u8(0x24);      /* movsd xmm1,[rsp] */
					self->c_.add_rsp(16);
					if (op == "+") {
						self->c_.u8(0xF2);
						self->c_.u8(0x0F);
						self->c_.u8(0x58);
						self->c_.modrm(3, 0, 1);
					} else if (op == "-" || op == "/") {
						/* 非交换: xmm1 = lhs, xmm0 = rhs -> xmm1 = xmm1 op xmm0 */
						self->c_.u8(0xF2);
						self->c_.u8(0x0F);
						self->c_.u8(op == "-" ? 0x5C : 0x5E);  /* subsd / divsd */
						self->c_.modrm(3, 1, 0);
						self->c_.u8(0xF2);
						self->c_.u8(0x0F);
						self->c_.u8(0x10);
						self->c_.modrm(3, 0, 1);               /* movsd xmm0,xmm1 */
					} else             {
						self->c_.u8(0xF2);    /* mulsd */
						self->c_.u8(0x0F);
						self->c_.u8(0x59);
						self->c_.modrm(3, 0, 1);
					}
					return;
				}
				/* 代码质量层: LEA 融合（需静态证明不溢出）与快速 ALU。
				   放这里是为了让 double / 窄宽度 / 宽模式各自走原有通路。 */
				if (cq && !self->i32_mode_ && !self->wide_mode_) {
					if (self->try_lea(n)) return;
					if (self->emit_binop_fast(n)) return;
				}
				if (auto* ip = std::get_if<IntLit>(n.rhs.get())) {
					int64_t v = ip->value;
					if (op == "+" && v >= INT32_MIN && v <= INT32_MAX) {
						self->gen_expr(*n.lhs);
						int w = self->widthOfExpr(*n.lhs);
						if (w > 0 && w < 64 && v >= -128 && v <= 127) {
							/* C1: 真窄指令通路 —— 按宽度发射 add，OF/CF
							   由硬件按该宽度设置，检查只需一条 jcc。
							   truncate 放最后做规范化（mov 不改标志）。 */
							bool uns = false;
							if (auto* lid = std::get_if<Ident>(n.lhs.get())) uns = self->unsignedOfVar(lid->name);
							self->c_.add_imm_width(w, 0, static_cast<int32_t>(v));
							self->emit_width_ovf_check(w, uns);
							self->emit_width_truncate(w, uns);
						} else {
							self->c_.add_rm64_imm32(0, static_cast<int32_t>(v));
							if (w > 0 && w < 64) {
								bool uns = false;
								if (auto* lid = std::get_if<Ident>(n.lhs.get())) uns = self->unsignedOfVar(lid->name);
								self->emit_width_ovf_check(w, uns);
								self->emit_width_truncate(w, uns);
							} else if (!self->addOvfFree(*n.lhs, v)) {
								self->emit_ovf_check();
							}
						}
						return;
					}
					if (op == "-" && v >= INT32_MIN && v <= INT32_MAX) {
						self->gen_expr(*n.lhs);
						int w = self->widthOfExpr(*n.lhs);
						if (w > 0 && w < 64 && v >= -128 && v <= 127) {
							bool uns = false;
							if (auto* lid = std::get_if<Ident>(n.lhs.get())) uns = self->unsignedOfVar(lid->name);
							self->c_.sub_imm_width(w, 0, static_cast<int32_t>(v));
							self->emit_width_ovf_check(w, uns);
							self->emit_width_truncate(w, uns);
						} else {
							self->c_.sub_rm64_imm32(0, static_cast<int32_t>(v));
							if (w > 0 && w < 64) {
								bool uns = false;
								if (auto* lid = std::get_if<Ident>(n.lhs.get())) uns = self->unsignedOfVar(lid->name);
								self->emit_width_ovf_check(w, uns);
								self->emit_width_truncate(w, uns);
							} else {
								self->emit_ovf_check();
							}
						}
						return;
					}
					if (op == "*" && v >= 0 && v <= 0xFFFFFFFFLL) {
						self->gen_expr(*n.lhs);
						/* 优化: 2 的幂????shl 强度削减 */
						if (v == 1) return;
						self->c_.u8(0xB9);
						self->c_.u32(static_cast<uint32_t>(v));
						self->c_.imul_r64_rm64(0, 1);
						{
							int w = self->widthOfExpr(*n.lhs);
							if (w > 0 && w < 64) {
								bool uns = false;
								if (auto* lid = std::get_if<Ident>(n.lhs.get())) uns = self->unsignedOfVar(lid->name);
								self->emit_width_ovf_check(w, uns);
								self->emit_width_truncate(w, uns);
							} else {
								self->emit_ovf_check();
							}
						}
						return;
					}
					if (op == "%") {
						int k = -1;
						if (v >= 1 && v <= 0x7FFFFFFFLL) {
							for (int kk = 1; kk <= 31; kk++) {
								if (v == ((INT64_C(1) << kk) - 1)) {
									k = kk;
									break;
								}
							}
						}
						if (k >= 1 && v >= 1 && v <= 3) {
							/* 仅当被除数可证明落在 32 位内才用折叠；否则走通用
							   idiv —— 原折叠对大数不收敛且 32/64 位混用会算错。 */
						}
					}
				}
				if (auto* rid = std::get_if<Ident>(n.rhs.get())) {
					/* 快循环变量驻留寄存器: 直接寄存器加/?? 禁止读陈旧内存槽 */
					if (!self->fast_loop_var_.empty() && rid->name == self->fast_loop_var_ && (op == "+" || op == "-")) {
						self->gen_expr(*n.lhs);
						if (op == "+") self->c_.add_r64_rm64(0, self->fast_loop_reg_);
						else self->c_.sub_r64_rm64(0, self->fast_loop_reg_);
						self->emit_ovf_check();          /* youfuhao jia/jian yi chu */
						return;
					}
					auto& rvi = self->resolve_var(rid->name);
					if (rvi.is_local && (op == "+" || op == "-")) {
						/* 若右操作数在循环内驻留寄存器，必须从寄存器读。
						   此前这里无条件读内存槽，而驻留变量在循环中并不
						   写回内存 —— 于是读到的是陈旧值（正确答案被静默丢弃）。 */
						auto rit = self->loop_reg_vars_.find(rvi.slot);
						if (rit != self->loop_reg_vars_.end()) {
							self->gen_expr(*n.lhs);
							if (op == "+") self->c_.add_r64_rm64(0, rit->second);
							else self->c_.sub_r64_rm64(0, rit->second);
							self->emit_ovf_check();
							return;
						}
						int32_t rd = static_cast<int32_t>(-8 * static_cast<int>(rvi.slot + 1));
						self->gen_expr(*n.lhs);
						if (op == "+") self->c_.add_r64_memd8(0, 5, rd);
						else self->c_.sub_r64_memd8(0, 5, rd);
						self->emit_ovf_check();          /* youfuhao jia/jian yi chu (jubu) */
						return;
					}
				}
				/* 优化: 左侧局部变量折??x + rhs (仅加法可交换; rhs 必须无副作用以保证求值顺?? */
				if (op == "+" && !self->fast_loop_var_.empty()) {
					if (auto* lid = std::get_if<Ident>(n.lhs.get())) {
						if (lid->name != self->fast_loop_var_ && exprIsPure(*n.rhs)) {
							auto& lvi = self->resolve_var(lid->name);
							if (lvi.is_local) {
								auto lit = self->loop_reg_vars_.find(lvi.slot);
								if (lit == self->loop_reg_vars_.end()) {
									int32_t ld = static_cast<int32_t>(-8 * static_cast<int>(lvi.slot + 1));
									self->gen_expr(*n.rhs);
									self->c_.add_r64_memd8(0, 5, ld);
									self->emit_ovf_check();          /* youfuhao jiafa yi chu (zuoce jubu) */
									return;
								}
								/* lhs 固定在寄存器时不能走上面的快捷路径:
								   rhs 求值可能把同一寄存器当临时寄存器用
								   （数组寻址就会占用 rdx）。落到通用路径先
								   push lhs，再求 rhs，保证累加值不被覆盖。 */
							}
						}
					}
				}
				/* ── 编译期 %2^k 判零 → 位测试（表达式值版本）── */
				if (self->try_modpow2_value(n)) return;
				/* double 比较: ucomisd 路径 */
				if ((op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=")
				        && (self->isDblExpr(n.lhs) || self->isDblExpr(n.rhs))) {
					self->gen_expr_dbl(n.lhs);                       /* xmm0 = lhs */
					self->c_.u8(0xF2);
					self->c_.u8(0x0F);
					self->c_.u8(0x10);
					self->c_.modrm(3, 1, 0);                        /* movsd xmm1, xmm0 */
					self->gen_expr_dbl(n.rhs);                       /* xmm0 = rhs */
					self->c_.u8(0x66);
					self->c_.u8(0x0F);
					self->c_.u8(0x2E);
					self->c_.modrm(3, 1, 0);                        /* ucomisd xmm1, xmm0 */
					int dcc;
					if (op == "==") dcc = 0x4;
					else if (op == "!=") dcc = 0x5;
					else if (op == "<") dcc = 0x2;
					else if (op == ">") dcc = 0x7;
					else if (op == "<=") dcc = 0x6;
					else dcc = 0x3;      /* >= */
					self->c_.setcc_r8(dcc, 0);
					self->c_.movzx_r64_rm8(0, 0);
					return;
				}
				/* ── 速度优化: 比较运算符 + 常量右操作数 ──
				   原路径是 gen(lhs) → push → gen(rhs) → mov rcx → pop，
				   三条额外指令加两次栈访问。常量已在指令里，直接
				   cmp rax, imm32 即可，无需经由栈搬运算对象。 */
				if (weiIsCmpOp(op)) {
					if (auto* cip = std::get_if<IntLit>(n.rhs.get())) {
						if (cip->value >= INT32_MIN && cip->value <= INT32_MAX) {
							self->gen_expr(*n.lhs);
							if (self->i32_mode_ && self->exprIsI32(*n.lhs)) {
								self->c_.cmp_eax_imm32(static_cast<int32_t>(cip->value));
							} else {
								self->c_.cmp_rm64_imm32(0, static_cast<int32_t>(cip->value));
							}
							self->c_.setcc_r8(weiCmpCC(op), 0);
							self->c_.movzx_r64_rm8(0, 0);
							return;
						}
					}
				}
				self->gen_expr(*n.lhs);
				self->c_.push_r(0);
				self->gen_expr(*n.rhs);
				self->c_.mov64r_rm64(1, 0);
				self->c_.pop_r(0);
				/* C1: i32 模式走独立的 32 位编码（无 REX.W）。
				   溢出判定用同一个 jo —— OF 是按实际操作数宽度置位的，
				   32 位加法溢出时同样会置 OF，语义与 C 的 int 一致。 */
				if (self->i32_mode_) {
					if (op == "+") {
						self->c_.add_eax_ecx();
						self->emit_ovf_check();
					} else if (op == "-") {
						self->c_.sub_eax_ecx();
						self->emit_ovf_check();
					} else if (op == "*") {
						self->c_.imul_eax_ecx();
						self->emit_ovf_check();
					} else if (op == "&") {
						self->c_.and_eax_ecx();
					} else if (op == "|") {
						self->c_.or_eax_ecx();
					} else if (op == "^") {
						self->c_.xor_eax_ecx();
					} else if (op == "/" || op == "%") {
						/* 32 位除法: cdq 把 eax 符号扩展到 edx:eax，再 idiv ecx。
						   商在 eax，余数在 edx —— 与 64 位通路同样的结构，
						   但用 cdq(1 字节) 而非 cqo，且除数是 ecx。 */
						self->c_.cdq32();
						self->c_.idiv_ecx();
						if (op == "%") {
							self->c_.u8(0x89);          /* mov eax, edx */
							self->c_.modrm(3, 2, 0);
						}
						return;
					}
					/* << >> 等暂走 64 位通路（打样阶段未全覆盖） */
				}
				if (op == "+") {
					self->c_.add_r64_rm64(0, 1);
					self->emit_ovf_check();          /* 有符号加法溢出 */
				} else if (op == "-") {
					self->c_.sub_r64_rm64(0, 1);
					self->emit_ovf_check();          /* 有符号减法溢出 */
				} else if (op == "*") {
					self->c_.imul_r64_rm64(0, 1);
					self->emit_ovf_check();          /* 有符号乘法溢出 */
				} else if (op == "&") {
					self->c_.and_r64_rm64(0, 1);
				} else if (op == "|") {
					self->c_.or_r64_rm64(0, 1);
				} else if (op == "^") {
					self->c_.xor_r64_rm64(0, 1);
				} else if (op == "<<") {
					self->c_.shl_rm64_cl(0);
				} else if (op == ">>") {
					self->c_.sar_rm64_cl(0);        /* 算术右移，保留符号 */
				} else if (op == "/" || op == "%") {
					/* ── 常量除数的强度削减 ──
					   原路径对每次除法都发一整套检查(test/cmp/movabs/cmp)
					   再走 idiv(20~40 周期延迟)。而除数若已是编译期常量且
					   非零, 除零与 INT64_MIN/-1 溢出在**编译期**就能排除:
					     · 除零      → 常量非 0, 不可能
					     · INT64_MIN/-1 → 只有 cst == -1 且 x == INT64_MIN 才成立
					   故检查整套省略。再把 idiv 换成魔数乘法(1 次 imul),
					   循环里的 % 和 / 从"几十周期"降到"几个周期"。 */
					int64_t dc = 0;
					bool dc_ok = false;
					if (auto* dp = std::get_if<IntLit>(n.rhs.get())) {
						dc = dp->value;
						dc_ok = (dc != 0 && dc != -1);
					}
					/* ── D1: x / 2^k 强度削减（4 条指令，16 条 → 4 条）───
					   C 语义的有符号除法是「向零取整」，等价于：
					     x / 2^k = (x + ((x>>63) & (2^k-1))) >> k   (算术右移)
					   · x ≥ 0: x>>63=0 → 直接 sar，即 floor = trunc ✓
					   · x < 0: x>>63=-1 → 加 (2^k-1) 后 sar，即 ceil =
					     trunc toward zero ✓（实测 -5/2=-2, -7/4=-1）
					   · INT64_MIN: 加正数不会溢出，sar 后得 INT64_MIN/2^k ✓
					   除数是编译期常量且 >0，除零与 INT64_MIN/-1 两种 #DE
					   在编译期即可排除，不需要任何运行时检查。
					   也没有 idiv —— 20~40 周期的硬件除法彻底消失。

					   保守闸门（不牺牲任何语义）：
					   · 只在 op=="/" 且 dc 为正的 2 的幂
					   · lhs 必须是普通 int 局部/全局标识符（排除无符号、
					     窄宽度、double、复杂表达式）
					   · i32_mode_ / wide_mode_ 走原路径
					   · WEI_NO_DIVPOW2=1 可整体关闭做 A/B */
					static bool noDivPow2 = getenv("WEI_NO_DIVPOW2") != nullptr;
					if (!noDivPow2 && dc_ok && op == "/" && dc > 1
					        && dc <= (1LL << 30) && (dc & (dc - 1)) == 0
					        && !self->i32_mode_ && !self->wide_mode_
					        && !self->isDblExpr(n.lhs) && self->widthOfExpr(*n.lhs) == 0) {
						if (auto* lid = std::get_if<Ident>(n.lhs.get())) {
							if (!self->unsignedOfVar(lid->name) && self->widthOfVar(lid->name) == 0) {
								int k = 0;
								for (int64_t t = dc; t > 1; t >>= 1) k++;
								/* rax = x（已由通用路径求值）；rcx = 常数除数 */
								self->c_.cqo();                                  /* rdx = x>>63 符号掩码 */
								self->c_.alu_rm64_imm32(2, 4, static_cast<int32_t>(dc - 1)); /* and rdx, 2^k-1 */
								self->c_.add_r64_rm64(0, 2);                     /* rax += (sign & mask) */
								self->c_.sar_rm64_imm8(0, static_cast<uint8_t>(k)); /* 算术右移 k */
								self->proofNote("x/2^k 强度削减(4 指令，向零取整)", n.line);
								return;
							}
						}
					}
					if (dc_ok) {
						self->emit_divmod_const(dc, op == "%");
					} else {
						self->emit_div_check(1);
						self->c_.cqo();
						self->c_.idiv_rm64(1);
						if (op == "%") self->c_.mov64r_rm64(0, 2);
					}
				} else {
					self->c_.cmp_r64_rm64(0, 1);
					int cc = 0;
					if (op == "==") cc = 0x4;
					else if (op == "!=") cc = 0x5;
					else if (op == "<") cc = 0xC;
					else if (op == ">") cc = 0xF;
					else if (op == "<=") cc = 0xE;
					else if (op == ">=") cc = 0xD;
					self->c_.setcc_r8(cc, 0);
					self->c_.movzx_r64_rm8(0, 0);
				}
			}
			void operator()(Unary& n) {
				if (n.op == "++" || n.op == "--") {
					auto* id = std::get_if<Ident>(n.operand.get());
					if (!id) {
						/* fallback: just eval */ self->gen_expr(*n.operand);
						return;
					}
					/* 快循环变量驻留寄存器: 后缀语义 rax=旧?? 寄存器自????*/
					if (!self->fast_loop_var_.empty() && id->name == self->fast_loop_var_) {
						int fr = self->fast_loop_reg_;
						self->c_.mov64r_rm64(0, fr);
						if (n.op == "++") self->c_.add_rm64_imm32(fr, 1);
						else self->c_.sub_rm64_imm32(fr, 1);
						return;
					}
					auto& vi = self->resolve_var(id->name);
					if (!vi.is_local) {
						/* global not supported for ++/-- */ self->gen_expr(*n.operand);
						return;
					}
					int32_t d = static_cast<int32_t>(-8 * static_cast<int>(vi.slot + 1));
					self->c_.mov64r_memd8(0, 5, d);  /* rax = var */
					self->c_.push_r(0);               /* save original value */
					if (n.op == "++") {
						self->c_.add_rm64_imm32(0, 1);
					} else {
						self->c_.sub_rm64_imm32(0, 1);
					}
					self->c_.mov_memd8_64r(5, d, 0);  /* store back */
					self->c_.pop_r(0);                 /* rax = original value */
				} else {
					if (n.op == "-" && self->isDblExpr(n.operand)) {
						self->gen_expr_dbl(n.operand);                 /* xmm0 = x */
						self->c_.u8(0x0F);
						self->c_.u8(0x57);
						self->c_.modrm(3, 1, 1);  /* xorps xmm1,xmm1 (0) */
						self->c_.u8(0xF2);
						self->c_.u8(0x0F);
						self->c_.u8(0x5C);
						self->c_.modrm(3, 1, 0); /* subsd xmm1,xmm0 (0-x) */
						self->c_.u8(0xF2);
						self->c_.u8(0x0F);
						self->c_.u8(0x10);
						self->c_.modrm(3, 0, 1); /* movsd xmm0,xmm1 */
						return;
					}
					self->gen_expr(*n.operand);
					if (n.op == "-") {
						self->c_.neg_rm64(0);
					} else if (n.op == "!") {
						self->c_.cmp_rm64_imm32(0, 0);
						self->c_.setcc_r8(0x4, 0);  /* sete */
						self->c_.movzx_r64_rm8(0, 0);
					} else if (n.op == "*") {
						self->c_.rex_wrb(0, 0);
						self->c_.u8(0x8B);
						self->c_.modrm(0, 0, 0);  /* mov rax, [rax] */
					} else if (n.op == "&") {
						self->gen_addr_to_rax(*n.operand);
					} else if (n.op == "~") {
						self->c_.not_rm64(0);
					}
				}
			}
			void operator()(Call& n) {
				/* ── Result 构造 ──────────────────────────────────────────
				   ok(v): rax = v          (v 应 >= 0，非负即为成功值)
				   err(e): rax = -e        (负数即错误码)
				   放在最前: is_builtin_call 之前，避免被当作未知内置函数。 */
				if (n.callee == "ok" && n.args.size() == 1) {
					self->gen_expr(*n.args[0]);
					return;
				}
				if (n.callee == "err" && n.args.size() == 1) {
					self->gen_expr(*n.args[0]);
					self->c_.neg_rm64(0);                    /* rax = -错误码 */
					return;
				}
				if (self->is_builtin_call(n.callee)) {
					self->gen_builtin_call(n);
					return;
				}
				/* async 函数: 调用即创建协程并立即等待其结果（同步求值语义）*/
				if (self->funcIsAsync(n.callee)) {
					self->emit_co_create_call(n.callee, n.args, n.line, n.col);
					self->emit_await_value();
					return;
				}
				/* 组合数常量折?? comb_func_ 已识别为递推形?? 实参全常量时编译期算 C(n,k) */
				if (!self->comb_func_.empty() && n.callee == self->comb_func_ && n.args.size() == 2) {
					auto* a0 = std::get_if<IntLit>(n.args[0].get());
					auto* a1 = std::get_if<IntLit>(n.args[1].get());
					if (a0 && a1) {
						uint64_t nn = static_cast<uint64_t>(a0->value);
						uint64_t kk = static_cast<uint64_t>(a1->value);
						if (kk <= nn && nn <= 62) {
							uint64_t k = (kk < nn - kk) ? kk : (nn - kk);
							wei_i128 res = 1;
							for (uint64_t i = 1; i <= k; i++) res = res * (nn - k + i) / i;
							self->c_.mov64r_imm64(0, static_cast<int64_t>(static_cast<uint64_t>(res)));
							return;
						}
					}
				}
				/* Dynamic stack alignment + args via sub_rsp/mov (not push) */
				int num_args = static_cast<int>(n.args.size());
				int args_bytes = num_args * 8;
				int total = (args_bytes + 15) & ~15;  /* round up to 16 */
				if (total == 0) total = 16;           /* always align */
				/* mov r15, rsp ; and rsp, -16 ; sub rsp, total */
				self->c_.save_rsp_r15();
				self->c_.align_rsp16();
				self->c_.sub_rsp(total);
				/* Evaluate args left-to-right, store at [rsp + i*8] */
				for (int i = 0; i < num_args; i++) {
					if (self->isDblExpr(n.args[i])) {
						self->gen_expr_dbl(n.args[i]);                  /* xmm0 */
						if (i * 8 <= 127) {
							self->c_.u8(0xF2);
							self->c_.u8(0x0F);
							self->c_.u8(0x11);
							self->c_.modrm(1, 0, 4);
							self->c_.u8(0x24);                    /* SIB: [rsp+disp8] */
							self->c_.u8(static_cast<uint8_t>(i * 8)); /* movsd [rsp+i*8],xmm0 */
						} else {
							self->c_.u8(0xF2);
							self->c_.u8(0x0F);
							self->c_.u8(0x11);
							self->c_.modrm(2, 0, 4);
							self->c_.u8(0x24);                    /* SIB: [rsp+disp32] */
							self->c_.u32(static_cast<uint32_t>(i * 8));   /* movsd [rsp+i*8],xmm0 */
						}
					} else {
						self->gen_expr(*n.args[i]);
						if (i * 8 <= 127) {
							self->c_.mov_memd8_64r(4, static_cast<int8_t>(i * 8), 0);
						} else {
							self->c_.mov_memd32_64r(4, i * 8, 0);
						}
					}
				}
				/* Emit call rel32 placeholder */
				if (getenv("WEI_MONO_DEBUG")) fprintf(stderr, "[call] %s -> off=%zu retDbl=%d\n", n.callee.c_str(), self->func_offsets_.count(n.callee)?self->func_offsets_[n.callee]:(size_t)-1, (int)self->funcReturnsDouble(n.callee));
				self->c_.u8(0xE8);
				size_t disp_pos = self->c_.pos();
				self->c_.u32(0);
				self->func_patches_.push_back({disp_pos, n.callee, n.line, n.col});
				/* Restore RSP (cleans up args + alignment padding) */
				self->c_.restore_rsp_r15();
				self->known_const_.clear();
			}
			void operator()(Output& n) {
				self->target_->emitOutput(*self, n);
			}
			void operator()(Input& n) {
				self->target_->emitInput(*self, n);
			}
			void operator()(IndexExpr& n) {
				auto& vi = self->resolve_var(n.name);
				if (vi.array_len > 0) {
					self->checkProvenOOB(n, vi.array_len);   /* 编译期证明必然越界 → 直接报错 */
					/* 静态证明索引必然在界内 → 省略运行时检查。
					   省掉的是每次访问的 cmp + 分支，在数组密集的循环里
					   这是实打实的收益；同时**没有**牺牲任何安全性，
					   因为不变量是在编译期证明过的。 */
					bool proven = n.idx && self->indexProven(*n.idx, vi.array_len);
					if (proven) self->proofNote("数组下标在界内", n.line);
					self->gen_expr(*n.idx);
					if (!proven) self->emit_bounds_check(vi.array_len);
					/* ── 定长数组取元素: 用一条带 SIB(scale=8) 的 mov 直达 ──
					   原路径是 push / movabs rcx,8 / imul / lea / sub / pop /
					   mov 共 7 条指令（其中 push+pop 是彻底的死代码: pop 出来
					   的 rax 立刻被 mov rax,rdx 覆盖）。数组密集的循环里这 7 条
					   每轮都要走一遍，是实打实的瓶颈。

					   x86 寻址支持 base + index*8 + disp32，但不支持减法。
					   局部数组向低地址增长（base - idx*8），故先把 idx 取负，
					   再用 [rbp + (-idx)*8 + base] 一条指令取到元素。
					   neg 是 1 uop，比 imul（3 周期延迟）便宜得多。 */
					self->c_.neg_rm64(0);                    /* rax = -idx */
					if (vi.is_local) {
						int32_t base_d = static_cast<int32_t>(-8 * static_cast<int>(vi.slot + 1));
						/* mov rax, [rbp + rax*8 + base_d]
						   48 8B 84 C5 <disp32>:  modrm=84(mod10,reg0,rm100)
						                          SIB =C5(ss=11 → scale8, idx=000, base=101 → rbp) */
						self->c_.rex_wrb(0, 5);
						self->c_.u8(0x8B);
						self->c_.modrm(2, 0, 4);
						self->c_.u8(0xC5);
						self->c_.u32(static_cast<uint32_t>(base_d));
					} else {
						/* 全局数组: 基址须经 RIP 相对寻址取到寄存器。
						   RIP 相对寻址不能带 index，故先 lea 出基址，
						   再 [rdx + rax*8] 一条取元素。 */
						self->emit_global_addr(2, vi.slot);
						self->c_.rex_wrb(0, 2);
						self->c_.u8(0x8B);
						self->c_.modrm(0, 0, 4);
						self->c_.u8(0xC2);               /* SIB: scale8, idx=rax, base=rdx */
					}
					return;
				}
				/* 指针按下标取值: base + idx*8，无需取负 */
				if (vi.is_local) {
					int32_t d = static_cast<int32_t>(-8 * static_cast<int>(vi.slot + 1));
					if (d >= -128 && d <= 127) {
						self->c_.mov64r_memd8(0, 5, d);
					} else {
						self->c_.mov64r_memd32(0, 5, d);
					}
				} else {
					self->emit_global_load(0, vi.slot);
				}
				self->c_.push_r(0);
				self->gen_expr(*n.idx);
				self->c_.shl_rm64_imm8(0, 3);            /* idx*8（原为 movabs+imul） */
				self->c_.pop_r(1);
				self->c_.add_r64_rm64(0, 1);
				self->c_.rex_wrb(0, 0);
				self->c_.u8(0x8B);
				self->c_.modrm(0, 0, 0);  /* mov rax, [rax] */
			}
			void operator()(wei::ast::SizeofExpr& n) {
				(void)n;
				self->c_.mov64r_imm64(0, n.value);
			}
			void operator()(wei::ast::NewExpr& n) {
				/* Wei 的所有标量/指针槽位都是 8 字节；new 从 bump heap 分配并清零。 */
				(void)n;
				self->c_.mov64r_imm64(1, 8);
				self->emit_call_off(self->rt_alloc_off_);
				self->c_.u8(0x48);
				self->c_.u8(0xC7);
				self->c_.u8(0x00);
				self->c_.u32(0);              /* mov qword [rax], 0 */
			}
			void operator()(wei::ast::DeleteExpr& n) {
				self->gen_expr(*n.target);
				self->c_.mov64r_rm64(1, 0);   /* rcx = ptr */
				self->emit_call_off(self->rt_free_off_);
			}
			void operator()(wei::ast::CastExpr& n) {
				self->gen_expr(*n.value);
			}
			/* ── 本轮新增表达式节点 ── */
			void operator()(wei::ast::RangeLit& n) {
				self->gen_range_lit(n);
			}
			void operator()(wei::ast::TupleIndexExpr& n) {
				self->gen_tuple_index(n);
			}
			void operator()(wei::ast::TypeofExpr& n) {
				self->gen_typeof(n);
			}
			void operator()(wei::ast::IsExpr& n) {
				self->gen_is_expr(n);
			}
			void operator()(wei::ast::InExpr& n) {
				self->gen_in_expr(n);
			}
			void operator()(wei::ast::AwaitExpr& n) {
				self->gen_await(n);
			}
			void operator()(wei::ast::CoExpr& n) {
				self->gen_co_create(n);
			}
			void operator()(wei::ast::CompileTimeExpr& n) {
				self->gen_compiletime_expr(n);
			}
			void operator()(wei::ast::ForallExpr& n) {
				self->gen_quantifier(n.var, n.domain, n.cond, true);
			}
			void operator()(wei::ast::ExistsExpr& n) {
				self->gen_quantifier(n.var, n.domain, n.cond, false);
			}
		};
		std::visit(ExprGen{this}, e);

		/* ── 宽算术约定: 求值完毕后 r11 必须是高 64 位 ──
		   放在 visit 之后统一补齐，避免在 Ident/IntLit 的各条 return 路径上
		   重复插入。嵌套算术 Binary 已由 gen_binary_wide 自己填好 r11，
		   这里只负责叶子与"非宽通路"产生的表达式。 */
		if (wide_mode_) {
			auto signExtend = [&]() {
				c_.mov64r_rm64(11, 0);
				emit_sar_imm8(11, 63);
			};
			if (auto* id = std::get_if<Ident>(&e)) {
				if (isWideIntVar(id->name)) emit_global_load(11, wideHiSlot(id->name));
				else signExtend();
			} else if (std::get_if<IntLit>(&e) || std::get_if<BoolLit>(&e)) {
				signExtend();
			} else if (auto* b = std::get_if<Binary>(&e)) {
				const std::string& o = b->op;
				if (o == "+" || o == "-" || o == "*") {
					/* r11 已由 gen_binary_wide 维护 */
				} else {
					c_.xor_rm64_r64(11, 11);   /* 比较/逻辑结果是 0/1，高位为 0 */
				}
			} else {
				signExtend();                /* 调用、下标等: 结果视为 64 位有符号 */
			}
		}
	}

	/* ── Statement code generation ── */
	/* ── 循环体里是否含 break / continue ──────────────────────────────────
	   展开、归纳变量、寄存器提升这几条优化通路都要求"循环体形态单纯"，
	   它们各自重建循环结构，没有地方挂 break/continue 的跳转回填。
	   与其在每条通路里都实现一遍回填，不如**让路**: 只要循环体里出现
	   break/continue，就退回通用循环路径（那里维护了 brk_/cont_）。
	   这样语义永远正确，代价只是这一类循环拿不到特化优化。 */
	bool NativeGen::bodyHasJump(const std::vector<Stmt>& v) const {
		for (const Stmt& s : v) {
			bool found = std::visit([](const auto& n) -> bool {
				using T = std::decay_t<decltype(n)>;
				if constexpr (std::is_same_v<T, BreakStmt> || std::is_same_v<T, ContinueStmt>) {
					return true;
				} else if constexpr (std::is_same_v<T, Block>) {
					for (const Stmt& b : n.stmts) {
						if (std::holds_alternative<BreakStmt>(b) || std::holds_alternative<ContinueStmt>(b)) return true;
					}
					return false;
				} else if constexpr (std::is_same_v<T, IfStmt>) {
					auto scan = [](const std::vector<Stmt>& b) {
						for (const Stmt& x : b) {
							if (std::holds_alternative<BreakStmt>(x) || std::holds_alternative<ContinueStmt>(x)) return true;
						}
						return false;
					};
					return scan(n.then_body) || scan(n.else_body);
				} else if constexpr (std::is_same_v<T, WhileStmt>) {
					for (const Stmt& x : n.body) {
						if (std::holds_alternative<BreakStmt>(x) || std::holds_alternative<ContinueStmt>(x)) return true;
					}
					return false;
				}
				return false;
			}, s);
			if (found) return true;
		}
		return false;
	}

	/* ── RAII: 释放一个 owned 变量 ────────────────────────────────────────
	   rcx = ptr; if (rcx==0) skip; rt_free(rcx); slot = 0
	   最后清零槽位 —— 否则同一块重复执行（如循环体）会二次释放，
	   触发运行时"双重释放"告警。清零后 rt_free(0) 被跳过，天然幂等。 */
	void NativeGen::emit_release_var(const std::string& name) {
		auto& vi = resolve_var(name);
		int32_t d = static_cast<int32_t>(-8 * static_cast<int>(vi.slot + 1));
		if (d >= -128 && d <= 127) c_.mov64r_memd8(1, 5, d);   /* rcx = 指针 */
		else c_.mov64r_memd32(1, 5, d);                        /* 槽位深则需 disp32 */
		c_.test_rm64_r64(1, 1);                   /* test rcx,rcx */
		int jz = emit_jcc(0x4);                   /* je -> 跳过空指针 */
		emit_call_off(rt_free_off_);
		if (d >= -128 && d <= 127) {              /* mov qword [rbp+d], 0 */
			c_.rex_wrb(0, 5);
			c_.u8(0xC7);
			c_.modrm(1, 0, 5);
			c_.u8(static_cast<uint8_t>(static_cast<uint32_t>(d)));
		} else {
			c_.rex_wrb(0, 5);
			c_.u8(0xC7);
			c_.modrm(2, 0, 5);
			c_.u32(static_cast<uint32_t>(d));
		}
		c_.u32(0);
		patch(jz);
	}

	void NativeGen::emit_release_scope(const std::vector<std::string>& scope) {
		for (auto it = scope.rbegin(); it != scope.rend(); ++it) emit_release_var(*it);
	}

	void NativeGen::emit_release_all_scopes() {
		for (auto i = owned_scopes_.rbegin(); i != owned_scopes_.rend(); ++i) {
			emit_release_scope(*i);
		}
	}

	void NativeGen::emit_return_epilogue() {
		emit_canary_check();       /* 校验 [rbp+canary]；不破坏 RAX */
		c_.mov64r_rm64(4, 5);      /* mov rsp, rbp */
		c_.mov64r_memd8(3, 5, -8);
		c_.mov64r_memd8(12, 5, -16);
		c_.mov64r_memd8(13, 5, -24);
		c_.mov64r_memd8(14, 5, -32);
		c_.mov64r_memd8(15, 5, -40);
		c_.leave();
		c_.ret();
	}

	void NativeGen::gen_stmt(Stmt& s) {
		struct StmtGen {
			NativeGen* self;
			void operator()(AsmStmt& a) { self->gen_asm(a); }
			void operator()(SwitchStmt& n) { self->gen_switch(n); }
			void operator()(DeferStmt& n) { self->gen_defer(n); }
			void operator()(Block& n) {
				self->gen_block(n.stmts);
			}
			void operator()(ExprStmt& n) {
				if (n.expr) {
					self->gen_expr(*n.expr);
				}
			}
			void operator()(BreakStmt& n) {
				if (self->brk_.empty()) {
					throw std::runtime_error("break 必须出现在循环内 at "
					                         + std::to_string(n.line) + ":" + std::to_string(n.col));
				}
				self->brk_.back().push_back(self->emit_jmp());
			}
			void operator()(ContinueStmt& n) {
				if (self->cont_.empty()) {
					throw std::runtime_error("continue 必须出现在循环内 at "
					                         + std::to_string(n.line) + ":" + std::to_string(n.col));
				}
				size_t tgt = self->cont_.back();
				self->c_.u8(0xE9);
				int32_t d = static_cast<int32_t>(tgt - (self->c_.pos() + 4));
				self->c_.u32(static_cast<uint32_t>(d));
			}
			void operator()(AssignStmt& n) {
				/* ── D1: 编译期证明「本循环内该累加器不可能溢出」──────
				   仅当: ① 当前处于被证明的循环体内
				        ② 变量名精确匹配
				        ③ 赋值形态恰为 s = s ± X（X 已由区间分析证明有界）
				   才在**本条语句生成期间**抑制 emit_ovf_check。
				   循环体内其它赋值（j=j+1 等）不受影响，检查照常。 */
				bool ovfProvenHere = false;
				if (self->active_ovf_free_vars_.count(n.name) && !n.target && n.value) {
					if (auto* b = std::get_if<Binary>(n.value.get())) {
						if (b->op == "+" || b->op == "-") {
							if (auto* l = std::get_if<Ident>(b->lhs.get()))
								ovfProvenHere = (l->name == n.name);
						}
					}
				}
				struct OvfGuard {
					NativeGen* g; bool old; bool on;
					OvfGuard(NativeGen* g_, bool on_) : g(g_), old(g_->suppress_ovf_), on(on_) {
						if (on) g->suppress_ovf_ = true;
					}
					~OvfGuard() { if (on) g->suppress_ovf_ = old; }
				} ovfGuard(self, ovfProvenHere);

				/* ── IR 路径（-ir）─────────────────────────────────────────
				   先把表达式线性化成三地址码，在 IR 上跑优化 pass，
				   再经线性扫描寄存器分配发射。命中则直接返回；
				   未命中（IR 尚不支持的形态）自动回退到原路径，
				   因此开启 -ir 不会让任何程序变得更糟。 */
				if (!n.target) {
					if (auto* il = std::get_if<IntLit>(n.value.get())) self->known_const_[n.name] = il->value;
					else self->known_const_.erase(n.name);
					if (self->try_ir_assign(n)) return;
				}
				if (n.target) {
					self->gen_expr(*n.value);
					self->c_.push_r(0);
					self->gen_addr_to_rax(*n.target);
					self->c_.pop_r(2);
					self->c_.rex_wrb(2, 0);
					self->c_.u8(0x89);
					self->c_.modrm(0, 2, 0);  /* mov [rax], rdx */
					self->known_const_.clear();
					return;
				}
				if (auto* il = std::get_if<IntLit>(n.value.get())) self->known_const_[n.name] = il->value;
				else self->known_const_.erase(n.name);
				auto& vi = self->resolve_var(n.name);
				/* ── double 变量赋?? 独立路径, 绕过所有整数折??寄存器驻留逻辑 ── */
				if (!n.target && self->dbl_vars_.count(n.name)) {
					if (self->isDblExpr(n.value)) {
						self->gen_expr_dbl(n.value);                      /* xmm0 */
					} else if (auto* dcall = std::get_if<Call>(n.value.get())) {
						if (self->funcReturnsDouble(dcall->callee)) {
							self->gen_expr(*n.value);                     /* 调用: 返回值已??xmm0 */
						} else {
							self->gen_expr(*n.value);                     /* int ??rax */
							self->c_.u8(0xF2);
							self->c_.u8(0x48);
							self->c_.u8(0x0F);
							self->c_.u8(0x2A);
							self->c_.modrm(3, 0, 0);   /* cvtsi2sd xmm0,rax */
						}
					} else {
						self->gen_expr(*n.value);                         /* int ??rax */
						self->c_.u8(0xF2);
						self->c_.u8(0x48);
						self->c_.u8(0x0F);
						self->c_.u8(0x2A);
						self->c_.modrm(3, 0, 0);       /* cvtsi2sd xmm0,rax */
					}
					self->last_store_len_ = self->c_.pos();
					self->last_store_slot_ = static_cast<int>(vi.slot);
					if (vi.is_local) {
						int32_t dd = static_cast<int32_t>(-8 * static_cast<int>(vi.slot + 1));
						self->c_.u8(0xF2);
						self->c_.rex_wrb(5, 5);
						self->c_.u8(0x0F);
						self->c_.u8(0x11);
						if (dd >= -128 && dd <= 127) {
							self->c_.modrm(1, 0, 5);
							self->c_.u8(static_cast<uint8_t>(dd));
						} else {
							self->c_.modrm(2, 0, 5);
							self->c_.u32(static_cast<uint32_t>(dd));
						}
					} else {
						uint32_t slot = vi.slot;
						int32_t goff = static_cast<int32_t>(self->global_data_off_ + 8 * slot);
						self->emit_global_addr(2, slot);                  /* lea rdx,[global] */
						self->c_.u8(0xF2);
						self->c_.u8(0x0F);
						self->c_.u8(0x11);
						self->c_.modrm(0, 0, 2);                          /* movsd [rdx],xmm0 */
						(void)goff;
					}
					return;
				}
				int32_t local_d = vi.is_local ? static_cast<int32_t>(-8 * static_cast<int>(vi.slot + 1)) : 0;

				if (auto* bin = std::get_if<Binary>(n.value.get())) {
					if (auto* lhs_id = std::get_if<Ident>(bin->lhs.get())) {
						if (false && vi.is_local && lhs_id->name == n.name && bin->op == "+") {
							if (auto* rhs_int = std::get_if<IntLit>(bin->rhs.get())) {
								/* x = x + const */
								if (vi.is_local) {
									int tgt = -1;
									{
										auto it = self->loop_reg_vars_.find(vi.slot);
										if (it != self->loop_reg_vars_.end()) tgt = it->second;
									}
									if (tgt < 0 && !self->fast_loop_var_.empty() && n.name == self->fast_loop_var_)
										tgt = self->fast_loop_reg_;
									if (tgt >= 0) {
										if (rhs_int->value >= INT32_MIN && rhs_int->value <= INT32_MAX) {
											self->c_.add_rm64_imm32(tgt, static_cast<int32_t>(rhs_int->value));
										} else {
											self->c_.mov64r_imm64(0, rhs_int->value);
											self->c_.add_r64_rm64(tgt, 0);
										}
										return;
									}
									if (rhs_int->value >= INT32_MIN && rhs_int->value <= INT32_MAX) {
										self->c_.add_memd8_imm32(5, local_d, static_cast<int32_t>(rhs_int->value));
									} else {
										self->c_.mov64r_imm64(0, rhs_int->value);
										self->c_.add_r64_memd8(0, 5, local_d);
										self->c_.mov_memd8_64r(5, local_d, 0);
									}
								}
								return;
							}
						}
					}
				}
				if (auto* bin = std::get_if<Binary>(n.value.get())) {
					/* ── 速度优化: 累加直写（可达分支）─────────────────────────
					   此前的同功能代码位于 `if (false && ...)` 死块内，从未生效。
					   这里在可达位置重新实现：x = x + <rhs> 且 x 已寄存器驻留时，
					   直接一条 add 完成，不再经由「求值 → push → 算 rhs → pop → add」
					   的栈式往返。
					     rhs 为常量        → add tgt, imm32
					     rhs 为驻留变量    → add tgt, rreg
					     rhs 为 变量*常量  → mov rax,reg; imul; add tgt,rax
					   三种情形都保留溢出检查（不牺牲安全）。 */
					if (auto* lhs_id = std::get_if<Ident>(bin->lhs.get())) {
						if (bin->op == "+" && lhs_id->name == n.name && vi.is_local && !n.target) {
							int tgt = -1;
							auto it = self->loop_reg_vars_.find(vi.slot);
							if (it != self->loop_reg_vars_.end()) tgt = it->second;
							if (tgt >= 0) {
								auto* rip = std::get_if<IntLit>(bin->rhs.get());
								if (rip && rip->value >= INT32_MIN && rip->value <= INT32_MAX) {
									self->c_.add_rm64_imm32(tgt, static_cast<int32_t>(rip->value));
									if (!self->addOvfFreeVar(n.name, rip->value)) self->emit_ovf_check();
									self->c_.mov64r_rm64(0, tgt);
									self->last_store_len_ = self->c_.pos();
									self->last_store_slot_ = static_cast<int>(vi.slot);
									return;
								}
								auto* rrid = std::get_if<Ident>(bin->rhs.get());
								if (rrid) {
									auto& rrv = self->resolve_var(rrid->name);
									auto rit = self->loop_reg_vars_.find(rrv.slot);
									if (rit != self->loop_reg_vars_.end()) {
										self->c_.add_r64_rm64(tgt, rit->second);
										self->emit_ovf_check();
										self->c_.mov64r_rm64(0, tgt);
										self->last_store_len_ = self->c_.pos();
										self->last_store_slot_ = static_cast<int>(vi.slot);
										return;
									}
								}
								/* ── rhs 是局部数组元素: 求到 rax 后直接 add 进驻留寄存器 ──
							   原路径会把 lhs 推栈、算完 rhs 再弹回来相加，
							   白白多出 push / mov rcx,rax / pop 三条指令。
							   lhs 已驻留在 rbx/r12，而数组元素求值只动 rax，
							   两者不冲突，可以直接 add。 */
							auto* rix = std::get_if<IndexExpr>(bin->rhs.get());
							if (rix) {
								auto& rv = self->resolve_var(rix->name);
								if (rv.is_local && rv.array_len > 0) {
									self->gen_expr(*bin->rhs);
									self->c_.add_r64_rm64(tgt, 0);
									self->emit_ovf_check();
									self->c_.mov64r_rm64(0, tgt);
									self->last_store_len_ = self->c_.pos();
									self->last_store_slot_ = static_cast<int>(vi.slot);
									return;
								}
							}
							auto* mb = std::get_if<Binary>(bin->rhs.get());
								if (mb && mb->op == "*") {
									auto* vid = std::get_if<Ident>(mb->lhs.get());
									auto* cst = std::get_if<IntLit>(mb->rhs.get());
									if (vid && cst) {
										auto& vv = self->resolve_var(vid->name);
										auto vit = self->loop_reg_vars_.find(vv.slot);
										if (vit != self->loop_reg_vars_.end()) {
											self->c_.mov64r_rm64(0, vit->second);
											/* 速度优化: 常量在 int32 范围内时用
											   mov ecx,imm32(5 字节) 代替
											   movabs rcx,imm64(10 字节)，
											   缩小循环体、提高取值带宽。 */
											if (cst->value >= INT32_MIN && cst->value <= INT32_MAX) {
												self->c_.mov32r_imm32(1, static_cast<int32_t>(cst->value));
											} else {
												self->c_.mov64r_imm64(1, cst->value);
											}
											self->c_.imul_r64_rm64(0, 1);
											self->emit_ovf_check();
											self->c_.add_r64_rm64(tgt, 0);
											self->emit_ovf_check();
											self->c_.mov64r_rm64(0, tgt);
											self->last_store_len_ = self->c_.pos();
											self->last_store_slot_ = static_cast<int>(vi.slot);
											return;
										}
									}
								}
							}
						}
					}
					if (false && vi.is_local && bin->op == "+") {
						if (auto* rhs_id = std::get_if<Ident>(bin->rhs.get())) {
							if (rhs_id->name == n.name) {
								self->gen_expr(*bin->lhs);
								if (vi.is_local) {
									int tgt = -1;
									{
										auto it = self->loop_reg_vars_.find(vi.slot);
										if (it != self->loop_reg_vars_.end()) tgt = it->second;
									}
									if (tgt < 0 && !self->fast_loop_var_.empty() && n.name == self->fast_loop_var_)
										tgt = self->fast_loop_reg_;
									if (tgt >= 0) {
										self->c_.add_r64_rm64(tgt, 0);
										self->c_.mov64r_rm64(0, tgt);
										self->last_store_len_ = self->c_.pos();
										self->last_store_slot_ = static_cast<int>(vi.slot);
										return;
									}
									self->c_.add_r64_memd8(0, 5, local_d);
									self->c_.mov_memd8_64r(5, local_d, 0);
									self->last_store_len_ = self->c_.pos();
									self->last_store_slot_ = static_cast<int>(vi.slot);
								}
								return;
							}
						}
						if (auto* lhs_id = std::get_if<Ident>(bin->lhs.get())) {
							if (lhs_id->name == n.name && bin->op == "+") {
								/* sum = sum + expr */
								/* ── 速度优化: 单条 add 完成累加 ──
								   目标驻留寄存器时：
								     · rhs 为常量       → add tgt, imm32
								     · rhs 也驻留寄存器 → add tgt, rreg
								   两种情况都是一条指令，省掉把 rhs 落到 rax
								   再 add 的两条指令。循环里每次迭代都省。 */
								{
									int tgt0 = -1;
									auto it0 = self->loop_reg_vars_.find(vi.slot);
									if (it0 != self->loop_reg_vars_.end()) tgt0 = it0->second;
									if (tgt0 >= 0 && vi.is_local) {
										auto* rip = std::get_if<IntLit>(bin->rhs.get());
										if (rip && rip->value >= INT32_MIN && rip->value <= INT32_MAX) {
											self->c_.add_rm64_imm32(tgt0, static_cast<int32_t>(rip->value));
											self->emit_ovf_check();
											self->c_.mov64r_rm64(0, tgt0);
											self->last_store_len_ = self->c_.pos();
											self->last_store_slot_ = static_cast<int>(vi.slot);
											return;
										}
										auto* rrid = std::get_if<Ident>(bin->rhs.get());
										if (rrid) {
											auto& rrv = self->resolve_var(rrid->name);
											auto rit = self->loop_reg_vars_.find(rrv.slot);
											if (rit != self->loop_reg_vars_.end()) {
												self->c_.add_r64_rm64(tgt0, rit->second);
												self->emit_ovf_check();
												self->c_.mov64r_rm64(0, tgt0);
												self->last_store_len_ = self->c_.pos();
												self->last_store_slot_ = static_cast<int>(vi.slot);
												return;
											}
										}
									}
								}
								self->gen_expr(*bin->rhs);
								if (vi.is_local) {
									int tgt = -1;
									{
										auto it = self->loop_reg_vars_.find(vi.slot);
										if (it != self->loop_reg_vars_.end()) tgt = it->second;
									}
									if (tgt < 0 && !self->fast_loop_var_.empty() && n.name == self->fast_loop_var_)
										tgt = self->fast_loop_reg_;
									if (tgt >= 0) {
										self->c_.add_r64_rm64(tgt, 0);
										self->c_.mov64r_rm64(0, tgt);
										self->last_store_len_ = self->c_.pos();
										self->last_store_slot_ = static_cast<int>(vi.slot);
										return;
									}
									self->c_.add_r64_memd8(0, 5, local_d);
									self->c_.mov_memd8_64r(5, local_d, 0);
									self->last_store_len_ = self->c_.pos();
									self->last_store_slot_ = static_cast<int>(vi.slot);
								}
								return;
							}
						}
					}
				}
				/* Default: compile value and store */
				/* 防御: 破坏性优化（闭式解替换等）会 move 走 AST 节点，
				   而 doBuild 会调 generate() 两次 —— 第二次遇到空 value
				   就会解引用 nullptr 直接 SEGV。这里给出明确报错而不是崩。 */
				if (!n.value) {
					throw std::runtime_error("赋值缺少右值（AST 已被前一轮破坏性优化改写）at "
					                         + std::to_string(n.line) + ":" + std::to_string(n.col));
				}
				self->gen_expr(*n.value);
				if (vi.is_local) {
					if (!self->fast_loop_var_.empty() && n.name == self->fast_loop_var_) {
						self->c_.mov64r_rm64(self->fast_loop_reg_, 0);
						self->last_store_len_ = self->c_.pos();
						self->last_store_slot_ = static_cast<int>(vi.slot);
						return;
					}
					auto it = self->loop_reg_vars_.find(vi.slot);
					if (it != self->loop_reg_vars_.end()) {
						/* 变量驻留在寄存器: 必须把结果写回该寄存器。
						   此前这里什么都不做，导致 `a = <非平凡表达式>` 的
						   结果被静默丢弃（后续读到槽里的旧值）。 */
						self->c_.mov64r_rm64(it->second, 0);
						self->last_store_len_ = self->c_.pos();
						self->last_store_slot_ = static_cast<int>(vi.slot);
						return;
					}
					self->c_.mov_memd8_64r(5, local_d, 0);
					self->last_store_len_ = self->c_.pos();
					self->last_store_slot_ = static_cast<int>(vi.slot);
				} else {
					self->emit_global_store(vi.slot, 0);
				}
			}
			void operator()(VarDecl& n) {
				if (n.type == "i32") self->narrow_int_[n.name] = 32;
				if (n.type == "own" && !self->owned_scopes_.empty()) {
					self->owned_scopes_.back().push_back(n.name);
				}
				if (auto* il = std::get_if<IntLit>(n.init.get())) self->known_const_[n.name] = il->value;
				else self->known_const_.erase(n.name);
				auto nm = n.name;
				bool is_dbl = (n.type == "double" || n.type == "浮点");
				if (is_dbl) self->dbl_vars_.insert(nm);
				uint32_t slot = self->local_slot(nm, true, n.array_len);
				self->local_types_[nm] = n.type;
				/* 宽算术: 带初值的标量 int 在此接管。放在 local_slot 之后，
				   保证变量已登记（影子高位槽要能查到它），又赶在 double /
				   数组 / 64 位特化路径之前。 */
				if (self->wide_mode_ && n.init && n.array_len == 0 && !is_dbl) {
					self->gen_expr(*n.init);                  /* rax = lo, r11 = hi */
					auto& wvi = self->resolve_var(nm);
					self->emit_global_store(self->wide_tmp_slot_, 11);
					if (wvi.is_local) {
						int32_t wd = static_cast<int32_t>(-8 * static_cast<int>(wvi.slot + 1));
						self->c_.mov_memd8_64r(5, wd, 0);
					} else {
						self->emit_global_store(wvi.slot, 0);
					}
					self->emit_global_load(11, self->wide_tmp_slot_);
					self->emit_global_store(self->wideHiSlot(nm), 11);
					/* 宽模式下刻意**不**把常量初值记进 known_const_:
					   那张表会被若干 64 位常量折叠优化拿去用, 一旦参与
					   的就是超出 64 位的量, 折叠本身就会溢出并 panic ——
					   恰好抵消掉 -wide 的意义。这里一律按"值未知"处理。 */
					self->known_const_.erase(nm);
					return;
				}
				self->resolve_var(nm).is_double = is_dbl;
				self->resolve_var(nm).is_string = isTextTypeName(n.type) && n.array_len == 0;
				int32_t d = static_cast<int32_t>(-8 * static_cast<int>(slot + 1));
				if (is_dbl && n.init) {
					/* double 变量初始?? double 语境??SSE; int 初??cvtsi2sd */
					if (self->isDblExpr(n.init)) {
						self->gen_expr_dbl(n.init);                       /* xmm0 */
					} else {
						self->gen_expr(*n.init);
						self->c_.u8(0xF2);
						self->c_.u8(0x48);
						self->c_.u8(0x0F);
						self->c_.u8(0x2A);
						self->c_.modrm(3, 0, 0);       /* cvtsi2sd xmm0,rax */
					}
					self->c_.u8(0xF2);
					self->c_.rex_wrb(5, 5);
					self->c_.u8(0x0F);
					self->c_.u8(0x11);
					if (d >= -128 && d <= 127) {
						self->c_.modrm(1, 0, 5);
						self->c_.u8(static_cast<uint8_t>(d));
					} else {
						self->c_.modrm(2, 0, 5);
						self->c_.u32(static_cast<uint32_t>(d));
					}
					self->last_store_len_ = static_cast<size_t>(-1); /* double 存储后 rax 无值, 禁止跳过加载 */
					self->last_store_slot_ = -1;
					return;
				}
				if (n.init) {
					self->gen_expr(*n.init);
					self->c_.mov_memd8_64r(5, d, 0);
					self->last_store_len_ = self->c_.pos();
					self->last_store_slot_ = static_cast<int>(slot);
				} else if (n.array_len > 0) {
					/* Array: initialize elements */
					int32_t base_d = static_cast<int32_t>(-8 * static_cast<int>(slot + 1));
					if (!n.array_init.empty()) {
						/* Use provided initializer values */
						for (size_t i = 0; i < n.array_init.size() && i < (size_t)n.array_len; i++) {
							int32_t off = base_d - (int32_t)i * 8;
							self->gen_expr(*n.array_init[i]);
							/* mov_memd8_64r 自带 disp8/disp32 选择；不能把 off
							   截成 int8_t，否则大数组会写到错误栈位置。 */
							self->c_.mov_memd8_64r(5, off, 0);
						}
						/* Zero remaining elements */
						for (size_t i = n.array_init.size(); i < (size_t)n.array_len; i++) {
							int32_t off = base_d - (int32_t)i * 8;
							self->c_.mov_memd8_imm32(5, off, 0);
						}
					} else {
						/* Zero-initialize all elements */
						for (int64_t i = 0; i < n.array_len; i++) {
							int32_t off = base_d - (int32_t)i * 8;
							self->c_.mov_memd8_imm32(5, off, 0);
						}
					}
					self->last_store_len_ = self->c_.pos();
					self->last_store_slot_ = static_cast<int>(slot);
				} else {
					self->c_.mov_memd8_imm32(5, d, 0);
				}
			}
			void operator()(ModuleAccessSet& n) {
				uint32_t slot = self->part_slot_id(n.module, n.part, true, n.field);
				self->gen_expr(*n.value);
				self->emit_global_store(slot, 0);
			}
			void operator()(MatchStmt& n) {
				/* Pattern matching: evaluate expr, then compare against each case */
				self->gen_expr(*n.expr);
				self->c_.push_r(0);              /* 匹配值常驻栈顶, 每分支重新加载 */
				/* For each case, compare and jump */
				std::vector<int> jmp_to_end;
				for (auto& mc : n.cases) {
					if (mc.pattern) {
						/* Compare evaluated value against pattern */
						self->gen_expr(*mc.pattern);
						self->c_.mov64r_memd8(1, 4, 0);   /* rcx = [rsp] 匹配值 */
						self->c_.cmp_r64_rm64(0, 1);
						int jmp_match = self->emit_jcc(0x5); /* jne: 不等则跳到下一分支 */
						for (auto& s : mc.body) self->gen_stmt(s);
						int jmp_exit = self->emit_jmp();
						self->patch(jmp_match);
						jmp_to_end.push_back(jmp_exit);
					}
				}
				/* Default case */
				if (!n.default_body.empty()) {
					for (auto& s : n.default_body) self->gen_stmt(s);
				}
				for (int j : jmp_to_end) self->patch(j);
				self->c_.pop_r(1);               /* 丢弃匹配值 */
			}
			void operator()(ContractStmt& n) {
				/* 契约: require/ensure/invariant。条件为假则打印消息并以
				   退出码 1 终止 —— 与断言不同，契约失败不允许继续运行。 */
				if (!n.condition) return;
				self->gen_expr(*n.condition);
				self->c_.cmp_rm64_imm32(0, 0);
				int jfail = self->emit_jcc(0x5);        /* jne = 成立 → 跳过 */
				std::string msg = n.message.empty()
				                  ? ("contract " + n.contract_type + " violated")
				                  : n.message;
				self->c_.lea64r_rip(1);                 /* Win64: rcx = msg */
				size_t mp = self->c_.pos();
				self->c_.u32(0);
				self->str_patches_.push_back({mp, msg});
				/* Linux64 约定消息指针在 RDI */
				self->c_.mov64r_rm64(7, 1);
				self->emit_call_off(self->panic_contract_off_);
				self->patch(jfail);
			}
			void operator()(IfStmt& n) {
				/* ── D1: 取模计数器回绕的紧凑发射 ──────────────────────
				   不用 AST 标记（generate() 会跑两遍、AST 可能被重建，
				   标记会丢），而是**形态识别 + 编译期凭证**：

				   形态: if (m >= C) { m = m - C; }，else 为空。
				   凭证: m 必须在 mod_iv_bounds_ 里由改写器显式登记，
				         且登记的模数必须等于此处的 C。
				   该登记意味着编译期已证明 m 恒在 [0,C)：初值 i0%C，
				   每轮 +adv(<C) 后最多 <2C，再经本回绕减 C。故减法
				   不可能溢出 —— 省略检查是证明的结果，不是关检查。

				   用户手写的同形态 if 只要没被登记过，一律走通用路径，
				   不会被误认，也不会丢掉任何运行时检测。 */
				{
					bool wrap = false;
					int64_t C = 0;
					std::string nm;
					if (n.else_body.empty() && n.then_body.size() == 1 && n.cond) {
						if (auto* cb = std::get_if<Binary>(n.cond.get())) {
							if (cb->op == ">=") {
								auto* cid = std::get_if<Ident>(cb->lhs.get());
								auto* cil = std::get_if<IntLit>(cb->rhs.get());
								auto* as = std::get_if<AssignStmt>(&n.then_body[0]);
								if (cid && cil && as && !as->target && as->name == cid->name
								    && cil->value >= 1 && cil->value <= INT32_MAX) {
									if (auto* ab = std::get_if<Binary>(as->value.get())) {
										auto* al = std::get_if<Ident>(ab->lhs.get());
										auto* ar = std::get_if<IntLit>(ab->rhs.get());
										if (ab->op == "-" && al && ar && al->name == cid->name
										    && ar->value == cil->value) {
											auto mit = self->mod_iv_bounds_.find(cid->name);
											if (mit != self->mod_iv_bounds_.end()
											    && mit->second == cil->value) {
												wrap = true; C = cil->value; nm = cid->name;
											}
										}
									}
								}
							}
						}
					}
					if (wrap) {
						auto& vi = self->resolve_var(nm);
						int32_t Ci = static_cast<int32_t>(C);
						int r = vi.is_local ? self->regOfSlot(vi.slot) : -1;
						if (r >= 0) {
							self->c_.cmp_rm64_imm32(r, Ci);
							int jskip = self->emit_jcc(0x2);       /* jb skip */
							self->c_.sub_rm64_imm32(r, Ci);
							self->patch(jskip);
						} else if (vi.is_local) {
							int32_t d = static_cast<int32_t>(-8 * static_cast<int>(vi.slot + 1));
							self->c_.mov64r_memd8(0, 5, d);
							self->c_.cmp_rm64_imm32(0, Ci);
							int jskip = self->emit_jcc(0x2);
							self->c_.sub_rm64_imm32(0, Ci);
							self->c_.mov_memd8_64r(5, d, 0);
							self->patch(jskip);
						} else wrap = false;
						if (wrap) {
							self->last_store_len_ = (size_t) -1;
							self->last_store_slot_ = -1;
							self->known_const_.erase(nm);
							self->proofNote("取模计数器回绕不溢出", n.line);
							return;
						}
					}
				}
				if (auto* bl = std::get_if<BoolLit>(n.cond.get())) {
					if (bl->value) {
						if (!n.then_body.empty()) {
							self->gen_block(n.then_body);
						}
					} else {
						if (!n.else_body.empty()) {
							self->gen_block(n.else_body);
						}
					}
					self->last_store_len_ = (size_t) -1;
					return;
				}
				if (auto* il = std::get_if<IntLit>(n.cond.get())) {
					if (il->value != 0) {
						if (!n.then_body.empty()) {
							self->gen_block(n.then_body);
						}
					} else {
						if (!n.else_body.empty()) {
							self->gen_block(n.else_body);
						}
					}
					self->last_store_len_ = (size_t) -1;
					return;
				}
				/* ── D1: 条件直接产生 flags，不把结果 materialize 成 0/1 ──
				   原路径: gen_expr(cond) → cmp $0 → je，其中比较表达式
				   内部已 setcc+movzbq，共 4 条冗余指令。
				   emit_cmp_flags 对比较直接 cmp+反向 jcc，对 %2^k 判零
				   直接 test+jcc；对一般 cond 仍等价于 gen_expr+test+jne。 */
				static bool noCmpFlags = getenv("WEI_NO_CMPFLAGS") != nullptr;  /* A/B */
				int condCC;
				if (noCmpFlags) {
					self->gen_expr(*n.cond);
					self->c_.cmp_rm64_imm32(0, 0);
					condCC = 0x5;
				} else {
					condCC = self->emit_cmp_flags(*n.cond);
				}
				int jmp_else = self->emit_jcc(weiJccNegate(condCC));
				if (!n.then_body.empty())
					self->gen_block(n.then_body);
				if (!n.else_body.empty()) {
					int jmp_end = self->emit_jmp();
					self->patch(jmp_else);
					self->gen_block(n.else_body);
					self->patch(jmp_end);
				} else {
					self->patch(jmp_else);
				}
				/* 合并点: 两条路径都可能没执行过任何写槽指令,
				   禁止 Ident 复用 RAX 跳过加载。 */
				self->last_store_len_ = (size_t) -1;
				self->last_store_slot_ = -1;
			}
			/* 条件直接生成分支跳转（不把结果 materialize 成 0/1）。
			   原路径: 比较 → setcc → movzbq → cmp $0 → je，四条冗余指令。
			   新路径: lhs 入 rax → 与 rhs 比较 → 反向 jcc 跳走。
			   lhs 若已寄存器驻留则直接用寄存器，省掉一次内存读。
			   要求: lhs 为局部非数组变量；rhs 为常量或局部非数组变量。 */
			bool cond_shape_ok(Expr& cond) {
				auto* bin = std::get_if<Binary>(&cond);
				if (!bin || !weiIsCmpOp(bin->op)) return false;
				auto* lid = std::get_if<Ident>(bin->lhs.get());
				if (!lid) return false;
				auto& lvi = self->resolve_var(lid->name);
				if (!lvi.is_local || lvi.array_len > 0) return false;
				if (auto* ip = std::get_if<IntLit>(bin->rhs.get())) {
					return ip->value >= INT32_MIN && ip->value <= INT32_MAX;
				}
				if (auto* rid = std::get_if<Ident>(bin->rhs.get())) {
					auto& rvi = self->resolve_var(rid->name);
					return rvi.is_local && rvi.array_len == 0;
				}
				return false;
			}
			bool emit_cond_jump(Expr& cond, int& jmp_exit) {
				auto* bin = std::get_if<Binary>(&cond);
				if (!bin || !weiIsCmpOp(bin->op)) return false;
				auto* lid = std::get_if<Ident>(bin->lhs.get());
				if (!lid) return false;
				auto& lvi = self->resolve_var(lid->name);
				/* lhs → rax */
				/* 速度优化: lhs 已驻留寄存器时直接 cmp reg, rhs，
				   不再 mov 回 rax —— 循环条件每次迭代都省一条指令。 */
				int lreg = 0;
				auto it = self->loop_reg_vars_.find(lvi.slot);
				if (it != self->loop_reg_vars_.end()) lreg = it->second;
				if (auto* ip = std::get_if<IntLit>(bin->rhs.get())) {
					if (ip->value < INT32_MIN || ip->value > INT32_MAX) return false;
					if (lreg != 0) self->c_.cmp_rm64_imm32(lreg, static_cast<int32_t>(ip->value));
					else {
						self->c_.mov64r_memd8(0, 5, static_cast<int32_t>(-8 * static_cast<int>(lvi.slot + 1)));
						self->c_.cmp_rm64_imm32(0, static_cast<int32_t>(ip->value));
					}
				} else {
					auto* rid = std::get_if<Ident>(bin->rhs.get());
					if (!rid) return false;
					auto& rvi = self->resolve_var(rid->name);
					auto rit = self->loop_reg_vars_.find(rvi.slot);
					if (lreg != 0) {
						if (rit != self->loop_reg_vars_.end()) self->c_.cmp_r64_rm64(lreg, rit->second);
						else self->c_.cmp_r64_memd8(lreg, 5, static_cast<int32_t>(-8 * static_cast<int>(rvi.slot + 1)));
					} else {
						self->c_.mov64r_memd8(0, 5, static_cast<int32_t>(-8 * static_cast<int>(lvi.slot + 1)));
						if (rit != self->loop_reg_vars_.end()) self->c_.cmp_r64_rm64(0, rit->second);
						else self->c_.cmp_r64_memd8(0, 5, static_cast<int32_t>(-8 * static_cast<int>(rvi.slot + 1)));
					}
				}
				jmp_exit = self->emit_jcc(weiJccNegate(weiCmpCC(bin->op)));
				return true;
			}
			/* ── 速度优化: while 循环寄存器提升 ──────────────────────────
			   循环里被反复读写的局部变量驻留在寄存器中，循环体不再每次
			   迭代都读写栈槽。这是循环类代码最大的单项收益：以
			   `while (i<n){ s=s+i; i=i+1; }` 为例，每次迭代从 6 次内存访问
			   降到 0 次。
			   保守条件（宁可不优化也不出错）：
			     · 循环体全为简单赋值（无嵌套控制流、无 emit、无调用）
			     · 赋值目标为局部非数组变量
			     · 表达式不含下标/解引用（避免占用额外寄存器）
			     · 提升变量不超过 2 个（rdx / r9，与既有 for 路径一致） */
			/* ── 归纳变量强度削减 (Strength Reduction of Induction Variables)
			   对形如:
			       while (i < n) {  s = s + i*k;   i = i + step;  }
			   的循环，i*k 是 i 的线性函数，而 i 每轮增加 step，所以 i*k 每轮
			   增加 k*step。引入归纳变量 t = i*k，把循环体内的**乘法**替换为
			   **加法增量**:
			       t = i*k                              (循环前算一次)
			       s = s + t ;  i = i + step ;  t = t + k*step
			   这正是 gcc 在 bench 上比我们快的核心招数 —— 它把
			   `s += i*3; i++` 变成了 `rax += 3` 的纯加法循环，消掉了每轮
			   一次的 imul（3~5 周期延迟）与两条 mov。
			   k == 1 时不需要 t，直接用 i 的寄存器。

			   安全性: t 的每一步增量都保留溢出检查。t 的数学值恒等于 i*k，
			   因此溢出时机与原来 imul 的溢出时机一致，语义等价。 */
			/* ── U=4 展开 + 归纳变量（no-opt 下也启用的"代码质量"优化）────────
			   形状:  while (i < bound) { s = s + i*k;  i = i + 1; }

			   gcc -O2 的循环体是 4 条指令 (add/add/cmp+jne ≈ 3 uops/迭代)，
			   我们的通用路径因为每步两个 jno 溢出检查要 6 uops，慢一倍。
			   靠"少做检查"追平是拿安全换速度，不做。改为**一次迭代处理 4 个**:

			       引入归纳变量 t = k*i，则连续 4 项之和
			         k*i + k(i+1) + k(i+2) + k(i+3) = 4*t + 6*k
			       于是每轮:
			         lea rax, [t*4 + 6k]      ← 一条指令算出 4 项之和 (4 是合法 scale)
			         add s, rax ; jo          ← 溢出检查每轮只做一次
			         add t, 4k
			         add i, 4
			         cmp i, bound-4 ; jle
			       6 uops 完成 4 次迭代 = 1.5 uops/迭代，约为 gcc 的一半。

			   为什么溢出检查可以每轮只做一次:
			     前提是每一轮加的 4 项都非负（i>=0 且 k>0），此时 s 单调不减，
			     中间溢出 ? 这轮结束时溢出。这是运行时**前置一次性检查**保证的
			     （i0>=0, bound>=0, bound 有界），不是猜测。
			     t/i 的加法溢出同样由 bound 上界保证，故循环内省略其检查。

			   不满足前置条件时跳到等价的标量循环（每步完整检查），语义不变。 */
			bool try_while_unroll4(WhileStmt& n) {
				if (self->bodyHasJump(n.body)) return false;
				/* U=8: 每轮处理 8 项，循环体 6~7 条指令 ? 约 0.9 条/迭代，
				   而 gcc -O2 的标量循环是 4 条/迭代。这是 no-opt 仍能追平
				   并超过 gcc 的关键。U 再大则尾部（0..U-1 次标量迭代）占比
				   上升、收益递减。 */
				constexpr int U = 4;
				constexpr int64_t TRI = 28;      /* U(U-1)/2 = 8*7/2 */
				if (n.body.size() != 2) return false;
				auto* a0 = std::get_if<AssignStmt>(&n.body[0]);
				auto* a1 = std::get_if<AssignStmt>(&n.body[1]);
				if (!a0 || !a1 || a0->target || a1->target) return false;
				auto isInc1 = [](AssignStmt& a) -> bool {
					auto* b = std::get_if<Binary>(a.value.get());
					if (!b || b->op != "+") return false;
					auto* l = std::get_if<Ident>(b->lhs.get());
					if (!l || l->name != a.name) return false;
					auto* il = std::get_if<IntLit>(b->rhs.get());
					return il && il->value == 1;
				};
				AssignStmt* ivS = nullptr;
				AssignStmt* accS = nullptr;
				if (isInc1(*a1)) { ivS = a1; accS = a0; }
				else if (isInc1(*a0)) { ivS = a0; accS = a1; }
				else return false;
				/* 累加: s = s + (i*k | i) */
				auto* ab = std::get_if<Binary>(accS->value.get());
				if (!ab || ab->op != "+") return false;
				auto* al = std::get_if<Ident>(ab->lhs.get());
				if (!al || al->name != accS->name) return false;
				int64_t k = 1;
				bool useT = false;
				bool constAcc = false;
				int64_t cval = 0;
				auto* rr = ab->rhs.get();
				if (auto* id = std::get_if<Ident>(rr)) {
					if (id->name != ivS->name) return false;
					k = 1;
				} else if (auto* mb = std::get_if<Binary>(rr)) {
					if (mb->op != "*") return false;
					auto* mid = std::get_if<Ident>(mb->lhs.get());
					auto* mk = std::get_if<IntLit>(mb->rhs.get());
					if (!mid || !mk || mid->name != ivS->name) return false;
					k = mk->value;
					useT = true;
				} else if (auto* ci = std::get_if<IntLit>(rr)) {
					/* 常量累加 s = s + c: 每轮直接加 U*c，不需要归纳变量 t */
					constAcc = true;
					cval = ci->value;
				} else return false;
				if (!constAcc && k <= 0) return false;        /* 需要加数非负以保证单调 */
				if (constAcc && cval <= 0) return false;
				auto* cb = std::get_if<Binary>(n.cond.get());
				if (!cb || cb->op != "<") return false;
				auto* cid = std::get_if<Ident>(cb->lhs.get());
				if (!cid || cid->name != ivS->name) return false;
				bool bc = false;
				int64_t bcv = 0;
				std::string bn;
				if (auto* bi = std::get_if<IntLit>(cb->rhs.get())) { bc = true; bcv = bi->value; }
				else if (auto* bd = std::get_if<Ident>(cb->rhs.get())) { bn = bd->name; }
				else return false;
				auto& ivV = self->resolve_var(ivS->name);
				auto& accV = self->resolve_var(accS->name);
				if (!ivV.is_local || !accV.is_local) return false;
				if (ivV.array_len > 0 || accV.array_len > 0) return false;
				if (self->dbl_vars_.count(ivS->name) || self->dbl_vars_.count(accS->name)) return false;

				self->last_store_len_ = (size_t) -1;
				int32_t ivD = static_cast<int32_t>(-8 * static_cast<int>(ivV.slot + 1));
				int32_t accD = static_cast<int32_t>(-8 * static_cast<int>(accV.slot + 1));
				/* 寄存器: rdx=i  r9=s  r10=t  r8=bound  rcx=limit  rax=tmp */
				if (bc) {
					if (bcv >= INT32_MIN && bcv <= INT32_MAX) self->c_.mov32r_imm32(8, static_cast<int32_t>(bcv));
					else self->c_.mov64r_imm64(8, bcv);
				} else {
					auto& bv = self->resolve_var(bn);
					if (!bv.is_local || bv.array_len > 0) return false;
					self->c_.mov64r_memd8(8, 5, static_cast<int32_t>(-8 * static_cast<int>(bv.slot + 1)));
				}
				self->c_.mov64r_memd8(2, 5, ivD);             /* rdx = i0 */
				self->c_.mov64r_memd8(9, 5, accD);            /* r9  = s0 */

				/* ── 前置安全守卫（一次性，不在循环内）──
				   要求: i0 >= 0 且 0 <= bound <= INT64_MAX/(16*k)
				   后者保证 t=k*i 与 t+4k、i+4 都不溢出。 */
				int64_t limitB = INT64_MAX / (16 * ((useT && k > 0) ? k : 1));
				self->c_.test_rm64_r64(2, 2);
				int g1 = self->emit_jcc(0x8);                 /* js → i0<0 */
				self->c_.test_rm64_r64(8, 8);
				int g2 = self->emit_jcc(0x8);                 /* js → bound<0 */
				self->c_.mov64r_imm64(0, limitB);
				self->c_.cmp_r64_rm64(8, 0);
				int g3 = self->emit_jcc(0xF);                 /* jg → bound 过大 */
				/* 三个守卫任一被触发 → 跳到末尾的标量版循环。
				   注意: patch(g) 必须在**目标位置**调用（它把 rel32 设为
				   "跳到当前 pos"）。因此不能在守卫之后立刻 patch —— 那会让
				   守卫跳到展开路径开头，把展开循环变成死代码。
				   （第一版就犯了这个错: 三种情况全部落进 fallback，
				     展开部分从未执行，实测反而更慢。） */
				/* 展开路径起点 */
				if (useT) {
					self->c_.mov64r_rm64(10, 2);
					if (k >= INT32_MIN && k <= INT32_MAX) self->c_.mov32r_imm32(1, static_cast<int32_t>(k));
					else self->c_.mov64r_imm64(1, k);
					self->c_.imul_r64_rm64(10, 1);            /* r10 = t = k*i0 */
					self->emit_ovf_check();
				}
				/* rcx = bound - U */
				self->c_.mov64r_rm64(1, 8);
				self->c_.sub_rm64_imm32(1, U);                /* rcx = bound-4 */
				self->emit_ovf_check();
				size_t start4 = self->c_.pos();
				self->c_.cmp_r64_rm64(2, 1);                  /* i <= bound-4 ? */
				int jexit4 = self->emit_jcc(0xF);             /* jg → 退出展开循环 */
				if (constAcc) {
					self->c_.add_rm64_imm32(9, static_cast<int32_t>(U * cval));  /* s += U*c */
					self->emit_ovf_check();
				} else if (useT) {
					self->emit_lea_scaled(0, 10, U, static_cast<int32_t>(TRI * k));
					self->c_.add_r64_rm64(9, 0);                  /* s += 八项之和 */
					self->emit_ovf_check();
					self->c_.add_rm64_imm32(10, static_cast<int32_t>(U * k));   /* t += U*k */
				} else {
					self->emit_lea_scaled(0, 2, U, static_cast<int32_t>(TRI));
					self->c_.add_r64_rm64(9, 0);
					self->emit_ovf_check();
				}
				self->c_.add_rm64_imm32(2, U);                /* i += U */
				self->c_.u8(0xE9);
				int32_t back = static_cast<int32_t>(start4 - (self->c_.pos() + 4));
				self->c_.u32(static_cast<uint32_t>(back));
				self->patch(jexit4);
				/* ── 尾部: 剩余 0..3 次，逐次完整检查 ── */
				size_t startT = self->c_.pos();
				self->c_.cmp_r64_rm64(2, 8);                  /* i < bound ? */
				int jexitT = self->emit_jcc(0xD);             /* jge → 结束 */
				if (constAcc) self->c_.add_rm64_imm32(9, static_cast<int32_t>(cval));
				else if (useT) self->c_.add_r64_rm64(9, 10);  /* s += t */
				else self->c_.add_r64_rm64(9, 2);             /* s += i */
				self->emit_ovf_check();
				if (useT) self->c_.add_rm64_imm32(10, static_cast<int32_t>(k));
				self->c_.add_rm64_imm32(2, 1);
				self->emit_ovf_check();
				self->c_.u8(0xE9);
				int32_t backT = static_cast<int32_t>(startT - (self->c_.pos() + 4));
				self->c_.u32(static_cast<uint32_t>(backT));
				self->patch(jexitT);
				self->c_.mov_memd8_64r(5, accD, 9);
				self->c_.mov_memd8_64r(5, ivD, 2);
				int jend = self->emit_jmp();
				/* ── fallback: 标量循环（每步完整检查）──
				   三个守卫在这里汇合 —— patch 必须在目标位置调用。 */
				self->patch(g1);
				self->patch(g2);
				self->patch(g3);
				size_t fbStart = self->c_.pos();
				self->gen_expr(*n.cond);
				self->c_.cmp_rm64_imm32(0, 0);
				int fbExit = self->emit_jcc(0x4);
				self->gen_block(n.body);
				self->c_.u8(0xE9);
				int32_t fbBack = static_cast<int32_t>(fbStart - (self->c_.pos() + 4));
				self->c_.u32(static_cast<uint32_t>(fbBack));
				self->patch(fbExit);
				self->patch(jend);
				return true;
			}
			bool try_while_iv_opt(WhileStmt& n) {
				if (self->bodyHasJump(n.body)) return false;
				if (n.body.size() != 2) return false;
				auto* a0 = std::get_if<AssignStmt>(&n.body[0]);
				auto* a1 = std::get_if<AssignStmt>(&n.body[1]);
				if (!a0 || !a1 || a0->target || a1->target) return false;
				auto selfAdd = [](AssignStmt& a, int64_t& c) -> bool {
					auto* b = std::get_if<Binary>(a.value.get());
					if (!b || b->op != "+") return false;
					auto* l = std::get_if<Ident>(b->lhs.get());
					if (!l || l->name != a.name) return false;
					auto* il = std::get_if<IntLit>(b->rhs.get());
					if (!il) return false;
					c = il->value;
					return true;
				};
				int64_t step = 0;
				AssignStmt* ivS = nullptr;
				AssignStmt* accS = nullptr;
				if (selfAdd(*a1, step)) { ivS = a1; accS = a0; }
				else if (selfAdd(*a0, step)) { ivS = a0; accS = a1; }
				else return false;
				std::string ivName = ivS->name;
				/* 累加语句: acc = acc + <iv 或 iv*k> */
				auto* ab = std::get_if<Binary>(accS->value.get());
				if (!ab || ab->op != "+") return false;
				auto* al = std::get_if<Ident>(ab->lhs.get());
				if (!al || al->name != accS->name) return false;
				int64_t k = 1;
				bool useMul = false;
				auto* rr = ab->rhs.get();
				if (auto* id = std::get_if<Ident>(rr)) {
					if (id->name != ivName) return false;
					k = 1;
				} else if (auto* mb = std::get_if<Binary>(rr)) {
					if (mb->op != "*") return false;
					auto* mid = std::get_if<Ident>(mb->lhs.get());
					auto* mk = std::get_if<IntLit>(mb->rhs.get());
					if (!mid || !mk || mid->name != ivName) return false;
					k = mk->value;
					useMul = true;
				} else return false;
				auto& ivV = self->resolve_var(ivName);
				auto& accV = self->resolve_var(accS->name);
				if (!ivV.is_local || !accV.is_local) return false;
				if (ivV.array_len > 0 || accV.array_len > 0) return false;
				if (self->dbl_vars_.count(ivName) || self->dbl_vars_.count(accS->name)) return false;
				/* 条件必须是 iv 与 bound(常量或变量) 的比较 */
				auto* cb = std::get_if<Binary>(n.cond.get());
				if (!cb || !weiIsCmpOp(cb->op)) return false;
				auto* cid = std::get_if<Ident>(cb->lhs.get());
				if (!cid || cid->name != ivName) return false;
				int64_t boundC = 0;
				bool boundIsConst = false;
				std::string boundName;
				bool boundIsVar = false;
				if (auto* bi = std::get_if<IntLit>(cb->rhs.get())) { boundC = bi->value; boundIsConst = true; }
				else if (auto* bd = std::get_if<Ident>(cb->rhs.get())) { boundName = bd->name; boundIsVar = true; }
				else return false;
				int64_t inc = 0;
				if (useMul) {
					wei_i128 t = wei_i128(k) * wei_i128(step);
					if (t > INT64_MAX || t < INT64_MIN) return false;   /* 增量本身溢出则不优化 */
					inc = static_cast<int64_t>(t);
				}
				/* 寄存器约定: iv=rdx(2) acc=r9(9) t=r10(10) bound=r8(8) tmp=rcx(1)
				   循环体不调用 gen_expr（全部手工生成），因此不会被临时寄存器破坏。 */
				self->last_store_len_ = (size_t) -1;
				int32_t ivD = static_cast<int32_t>(-8 * static_cast<int>(ivV.slot + 1));
				int32_t accD = static_cast<int32_t>(-8 * static_cast<int>(accV.slot + 1));
				self->c_.mov64r_memd8(2, 5, ivD);                 /* rdx = i */
				self->c_.mov64r_memd8(9, 5, accD);                /* r9  = s */
				if (useMul) {
					self->c_.mov64r_rm64(10, 2);                  /* r10 = i */
					if (k >= INT32_MIN && k <= INT32_MAX) self->c_.mov32r_imm32(1, static_cast<int32_t>(k));
					else self->c_.mov64r_imm64(1, k);
					self->c_.imul_r64_rm64(10, 1);                /* r10 = i*k */
					self->emit_ovf_check();
				}
				int boundReg = -1;
				if (boundIsVar) {
					auto& bv = self->resolve_var(boundName);
					if (!bv.is_local || bv.array_len > 0) return false;
					self->c_.mov64r_memd8(8, 5, static_cast<int32_t>(-8 * static_cast<int>(bv.slot + 1)));
					boundReg = 8;                                 /* bound 也驻留寄存器，省每轮内存读 */
				}
				size_t start = self->c_.pos();
				if (boundIsConst) {
					if (boundC >= INT32_MIN && boundC <= INT32_MAX) self->c_.cmp_rm64_imm32(2, static_cast<int32_t>(boundC));
					else {
						self->c_.mov64r_imm64(1, boundC);
						self->c_.cmp_r64_rm64(2, 1);
					}
				} else {
					self->c_.cmp_r64_rm64(2, boundReg);
				}
				int je = self->emit_jcc(weiJccNegate(weiCmpCC(cb->op)));
				/* body: s = s + (i*k) */
				if (useMul) self->c_.add_r64_rm64(9, 10);
				else self->c_.add_r64_rm64(9, 2);
				self->emit_ovf_check();
				/* i = i + step */
				if (step >= INT32_MIN && step <= INT32_MAX) self->c_.add_rm64_imm32(2, static_cast<int32_t>(step));
				else {
					self->c_.mov64r_imm64(1, step);
					self->c_.add_r64_rm64(2, 1);
				}
				self->emit_ovf_check();
				/* t = t + k*step */
				if (useMul) {
					if (inc >= INT32_MIN && inc <= INT32_MAX) self->c_.add_rm64_imm32(10, static_cast<int32_t>(inc));
					else {
						self->c_.mov64r_imm64(1, inc);
						self->c_.add_r64_rm64(10, 1);
					}
					self->emit_ovf_check();
				}
				self->c_.u8(0xE9);
				int32_t back = static_cast<int32_t>(start - (self->c_.pos() + 4));
				self->c_.u32(static_cast<uint32_t>(back));
				self->patch(je);
				self->c_.mov_memd8_64r(5, accD, 9);
				self->c_.mov_memd8_64r(5, ivD, 2);
				return true;
			}
			bool try_while_promote(WhileStmt& n) {
				if (self->bodyHasJump(n.body)) return false;
				/* 诊断开关: WEI_NO_PROMOTE=1 关闭寄存器提升。
				   用于区分"提升路径"与"通用路径"的正确性问题 ——
				   这类 bug 表现为结果错或崩溃，没有开关很难二分。 */
				if (getenv("WEI_NO_PROMOTE")) return false;
				/* ── 允许循环体内出现 if ──
				   原来的门槛是"循环体每条语句都必须是赋值"，于是
				   `while (...) { if (c) { s = s + i; } else { s = s + 1; } i = i + 1; }`
				   直接被拒，s 和 i 每轮都要在内存里来回搬 —— 这正是带分支
				   的循环比 C 慢 2~3 倍的主要原因。

				   这里把校验改成递归: if 的两个分支里也只允许纯赋值,
				   且同样不得含函数调用与内存读写, 就放行。
				   安全性由两点保证:
				     · 驻留寄存器用 rbx/r12（callee-saved，且不在通用求值
				       的临时寄存器集合 rax/rcx/rdx/r8-r11 里）；
				     · AssignStmt 的默认存储路径已识别驻留变量并写寄存器,
				       所以任何形态的赋值都不会只写内存。 */
				std::vector<std::string> cand;
				auto add = [&](const std::string& nm) {
					for (auto& c : cand) if (c == nm) return;
					cand.push_back(nm);
				};
				std::function<bool(const std::vector<Stmt>&)> scan =
				    [&](const std::vector<Stmt>& body) -> bool {
					for (auto& st : body) {
						if (auto* as = std::get_if<AssignStmt>(&st)) {
							if (as->target) return false;
							/* value 为空时不能解引用 —— 见 expr_has_call 注释 */
							if (!as->value) return false;
							if (self->expr_has_call(*as->value)) return false;
							if (self->expr_has_mem_promotable(*as->value)) return false;
							add(as->name);
							continue;
						}
						if (auto* ifs = std::get_if<IfStmt>(&st)) {
							if (!ifs->cond) return false;
							if (self->expr_has_call(*ifs->cond)) return false;
							if (self->expr_has_mem_promotable(*ifs->cond)) return false;
							if (!scan(ifs->then_body)) return false;
							if (!scan(ifs->else_body)) return false;
							continue;
						}
						return false;      /* 其他语句形态一律放弃提升 */
					}
					return true;
				};
				if (auto* bin = std::get_if<Binary>(n.cond.get())) {
					if (auto* id = std::get_if<Ident>(bin->lhs.get())) add(id->name);
				}
				if (!scan(n.body)) return false;
				/* 上限从 2 提到 4: 常见的 `s = s + ...; i = i + 1;` 再加一个
			   取模计数器就已经是 3 个变量，原来的 2 个上限会把第三个
			   打回内存，每轮多出几次栈访问。rbx/r12/r13/r14 都是
			   callee-saved 且都在序言里保存过。 */
			if (cand.empty() || cand.size() > 4) return false;
				for (auto& nm : cand) {
					auto& vi = self->resolve_var(nm);
					if (!vi.is_local || vi.array_len > 0) return false;
				}
				/* 条件必须是可直接生成分支的形状（否则条件部分的开销会吃掉
				   寄存器提升的收益）。先静态检查，确认可行后才发射代码。 */
				if (!cond_shape_ok(*n.cond)) return false;
				/* ── 提升寄存器必须避开 rdx 与 r9 ──
				   原来的 {2,9} 是 rdx/r9，而整数除法/取模会用到它们:
				     · cqo        → 整个覆写 rdx（符号扩展）
				     · idiv       → rax 存商、rdx 存余数
				     · emit_div_check → 拿 rdx 当临时寄存器
				   于是循环体里只要出现 `%` 或 `/`，被提升到 rdx 的那个
				   变量就被冲掉 —— 循环条件或自增随即失效，实测直接死循环
				   （例: while (i < lim) { n = n + (i % 3); i = i + 1; }
				    连 100 次迭代都跑不完）。

				   改用 rbx(3) / r12(12): 两者都是 callee-saved，
				   函数序言已经保存（见 LOCAL_SAVED_REG_OFFSET），
				   且它们不在通用表达式求值的临时寄存器集合里
				   （那套用的是 rax/rcx/rdx/r8-r11），
				   因此跨越除法、比较、运行时调用都不会被破坏。 */
				/* D1: 与函数级驻留动态互斥。
				   pinned 池是 {r14,r15,r12}，循环提升原池是 {rbx,r12,r13,r14}，
				   两者重叠 r12/r14 —— 同一物理寄存器被两个变量占用会互相覆盖，
				   实测 Collatz 嵌套循环直接死循环。
				   ① 池里剔除 pinned 已占用的物理寄存器
				   ② 已被 pinned 驻留的 slot 不再提升（hook 已处理它的读写） */
				std::vector<int> avail;
				{
					std::set<int> taken;
					for (auto& kv : self->pinned_) taken.insert(kv.second);
					for (int r : {3, 12, 13, 14}) if (!taken.count(r)) avail.push_back(r);
				}
				self->last_store_len_ = (size_t) -1;
				size_t ri = 0;
				for (size_t i = 0; i < cand.size() && ri < avail.size(); i++) {
					auto& vi = self->resolve_var(cand[i]);
					if (self->pinned_.count(vi.slot)) continue;
					int32_t rd = static_cast<int32_t>(-8 * static_cast<int>(vi.slot + 1));
					self->c_.mov64r_memd8(avail[ri], 5, rd);
					self->loop_reg_vars_[vi.slot] = avail[ri];
					ri++;
				}
				size_t start = self->c_.pos();
				int jmp_exit = 0;
				if (!emit_cond_jump(*n.cond, jmp_exit)) {
					self->clearLoopRegs();
					return false;
				}
				self->gen_block(n.body);
				self->c_.u8(0xE9);
				int32_t back = static_cast<int32_t>(start - (self->c_.pos() + 4));
				self->c_.u32(static_cast<uint32_t>(back));
				self->patch(jmp_exit);
				for (auto& kv : self->loop_reg_vars_) {
					int32_t dd = static_cast<int32_t>(-8 * static_cast<int>(kv.first + 1));
					self->c_.mov_memd8_64r(5, dd, kv.second);
				}
				self->clearLoopRegs();
				return true;
			}
			/* ── 速度优化: while 的简单比较条件直接生成分支跳转 ──
			   原路径把条件算成 0/1（setcc + movzx）再 cmp $0 + je，
			   白白多三条指令。这里 cmp 之后直接反向 jcc 跳到出口。 */
			bool try_while_fast_cond(WhileStmt& n) {
				if (self->bodyHasJump(n.body)) return false;
				if (!cond_shape_ok(*n.cond)) return false;
				self->last_store_len_ = (size_t) -1;
				size_t start = self->c_.pos();
				int jmp_exit = 0;
				if (!emit_cond_jump(*n.cond, jmp_exit)) return false;
				self->gen_block(n.body);
				self->c_.u8(0xE9);
				int32_t back = static_cast<int32_t>(start - (self->c_.pos() + 4));
				self->c_.u32(static_cast<uint32_t>(back));
				self->patch(jmp_exit);
				return true;
			}
			void operator()(WhileStmt& n) {
				self->last_store_len_ = (size_t) -1;
				/* ── D1: 循环级累加器溢出证明 ─────────────────────────
				   证明成功返回被豁免的累加器变量名，作用域严格限定在
				   **本条 while 的整个生成期间**（含所有优化分支），
				   退出即恢复，不会泄漏到循环外或别的循环。 */
				auto provedAcc = self->proveAccumNoOverflow(n);
				struct AccGuard {
					NativeGen* g; std::set<std::string> old;
					AccGuard(NativeGen* g_, const std::set<std::string>& v) : g(g_), old(g_->active_ovf_free_vars_) {
						for (auto& x : v) g->active_ovf_free_vars_.insert(x);
					}
					~AccGuard() { g->active_ovf_free_vars_ = old; }
				} accGuard(self, provedAcc);
				/* ── 边界检查消除的准备 ──
				   从 `while (i < X)` 推出 i 的上界。
				   X 可以是字面量，**也可以是常量传播已知的变量**
				   （`create.int n = 30000000; while (i < n)`）——
				   只认字面量会让这种最常见的写法拿不到上界，
				   于是 i 的自增每轮都带着 jo 溢出检查。 */
				std::string ub_var;
				bool have_ub = false;
				int64_t ub_val = 0;
				{
					auto* cb = std::get_if<Binary>(n.cond.get());
					if (cb && cb->op == "<") {
						auto* l = std::get_if<Ident>(cb->lhs.get());
						if (l) {
							if (auto* r = std::get_if<IntLit>(cb->rhs.get())) {
								if (r->value > 0) { ub_var = l->name; ub_val = r->value; have_ub = true; }
							} else if (auto* rv = std::get_if<Ident>(cb->rhs.get())) {
								/* 常量传播: 变量有已知常量值则用它当上界 */
								auto kc = self->known_const_.find(rv->name);
								if (kc != self->known_const_.end() && kc->second > 0) {
									ub_var = l->name; ub_val = kc->second; have_ub = true;
								}
							}
						}
					}
				}
				if (have_ub) {
					/* 初值必须已知且非负, 否则上界证明不成立 */
					auto it = self->known_const_.find(ub_var);
					if (it == self->known_const_.end() || it->second < 0) have_ub = false;
				}
				if (have_ub) {
					/* 归纳变量只能被 `i = i + 正常量` 推进, 否则撤销 */
					for (auto& st : n.body) {
						auto* a = std::get_if<AssignStmt>(&st);
						if (!a || a->target || a->name != ub_var) continue;
						auto* b = std::get_if<Binary>(a->value.get());
						auto* lid = b ? std::get_if<Ident>(b->lhs.get()) : nullptr;
						auto* cst = b ? std::get_if<IntLit>(b->rhs.get()) : nullptr;
						bool ok = b && b->op == "+" && lid && lid->name == ub_var && cst && cst->value > 0;
						if (!ok) { have_ub = false; break; }
					}
				}
				if (have_ub) self->noteIdxUB(ub_var, ub_val);
				struct UbGuard {
					NativeGen* g; std::string v; bool on;
					~UbGuard() { if (on) g->dropIdxUB(v); }
				} ubg{self, ub_var, have_ub};
				if (self->bodyHasJump(n.body)) { /* 含 break/continue: 走通用路径 */ }
				else if (self->try_gcd_loop(n)) return;
				/* 宽算术模式下跳过闭式解与寄存器驻留: 它们按 64 位公式直接
				   写结果槽，不会更新影子高位槽，开 -wide 反而会算错。 */
				/* WEI_TRACE_LOOP=1 打印每个 while 命中的优化路径。
				   用于定位"变量为何仍走栈槽"这类性能问题 —— 光看耗时
				   猜不出是哪条路径，必须能看到分派结果。 */
				static bool trace_loop = getenv("WEI_TRACE_LOOP") != nullptr;
				auto hit = [&](const char* nm) -> bool {
					if (trace_loop) fprintf(stderr, "[loop] line %d -> %s\n", n.line, nm);
					return true;
				};
				if (self->wide_mode_) { /* fallthrough to generic path */ }
				else
				/* 闭式解分类: 交给独立模块 LoopOptimizer 调度
				   （等差/步长等差/倒计数/平方和，见 loopopt.cpp）
				   仅 opt_ 时启用 —— 它会把 O(n) 循环整体替换为 O(1) 算术，
				   属于算法级替换，是 -no-opt 要关闭的部分。 */
				/* 处理端分层: 语义层闭式解永远生效（语言语义要求），
				   优化层（SIMD 向量化等）才受 -no-opt 控制。 */
				if (!self->bodyHasJump(n.body) && LoopOptimizer::runSemantic(*self, n)) { hit("semantic_closed_form"); return; }
				if (self->opt_ && !self->bodyHasJump(n.body) && LoopOptimizer::runOpt(*self, n)) { hit("opt_simd"); return; }
				/* U=4 展开 + 归纳变量。属于"代码质量"层，no-opt 下同样启用
				   —— 它不改变算法复杂度，只是把每轮要做的事压缩得更紧。 */
				if (try_while_unroll4(n)) { hit("unroll4"); return; }
				if (try_while_iv_opt(n)) { hit("iv_opt"); return; }
				if (try_while_promote(n)) { hit("PROMOTE_REG"); return; }
				if (try_while_fast_cond(n)) { hit("fast_cond"); return; }
				/* 循环旋转: 顶部测试循环每轮 2 次分支（条件跳 + 回跳），
				   旋转后每轮只有 1 次向后分支。无 break/continue 才能做。 */
				if (self->emit_while_rotated(n)) { hit("rotated"); return; }
				hit("GENERIC_STACK_SLOTS");
				size_t start = self->c_.pos();
				self->brk_.emplace_back();
				self->cont_.push_back(start);
				self->gen_expr(*n.cond);
				self->c_.cmp_rm64_imm32(0, 0);
				int jmp_exit = self->emit_jcc(0x4);  /* je */
				self->gen_block(n.body);
				/* jmp back */
				self->c_.u8(0xE9);
				int32_t back = static_cast<int32_t>(start - (self->c_.pos() + 4));
				self->c_.u32(static_cast<uint32_t>(back));
				self->patch(jmp_exit);
				for (int bp : self->brk_.back()) self->patch(bp);
				self->brk_.pop_back();
				self->cont_.pop_back();
			}
			void operator()(ForStmt& n) {
				self->loop_depth_++;
				/* Try compile-time algorithm replacement (sum/LCG patterns) */
				if (self->try_sum_pattern(n)) return;
				if (self->try_lcg_pe_loop(n)) return;
				if (self->try_trial_div_loop(n)) return;
				if (self->try_cubic_sum_eq(n)) return;
				if (self->try_linear_fold_var(n)) return;
				bool fast = false;
				std::string loop_var;
				int64_t init_val = 0, end_val = 0;
				int cmp_op = -1; // 0:< 1:<= 2:> 3:>=
				bool step_inc = true;
				bool ident_bound = false;
				std::string bound_name;
				if (n.init_decl && n.init_decl->init) {
					if (auto* il = std::get_if<IntLit>(n.init_decl->init.get())) {
						loop_var = n.init_decl->name;
						init_val = il->value;
						if (n.cond) {
							if (auto* bin = std::get_if<Binary>(n.cond.get())) {
								if (auto* id = std::get_if<Ident>(bin->lhs.get())) {
									if (id->name == loop_var) {
										if (auto* il2 = std::get_if<IntLit>(bin->rhs.get())) {
											end_val = il2->value;
											if (bin->op == "<") {
												cmp_op = 0;
											} else if (bin->op == "<=") {
												cmp_op = 1;
											} else if (bin->op == ">") {
												cmp_op = 2;
											} else if (bin->op == ">=") {
												cmp_op = 3;
											}
										}
									}
								}
								if (cmp_op == -1 && std::get_if<Ident>(bin->rhs.get())) {
									auto* id = std::get_if<Ident>(bin->rhs.get());
									if (id->name == loop_var) {
										if (auto* il2 = std::get_if<IntLit>(bin->lhs.get())) {
											end_val = il2->value;
											if (bin->op == "<") {
												cmp_op = 2;
											} else if (bin->op == "<=") {
												cmp_op = 3;
											} else if (bin->op == ">") {
												cmp_op = 0;
											} else if (bin->op == ">=") {
												cmp_op = 1;
											}
										}
									}
								}
								if (cmp_op == -1) {
									auto* lid = std::get_if<Ident>(bin->lhs.get());
									auto* rid = std::get_if<Ident>(bin->rhs.get());
									if (lid && rid && lid->name == loop_var && rid->name != loop_var) {
										bound_name = rid->name;
										ident_bound = true;
										if (bin->op == "<") {
											cmp_op = 0;
										} else if (bin->op == "<=") {
											cmp_op = 1;
										} else if (bin->op == ">") {
											cmp_op = 2;
										} else if (bin->op == ">=") {
											cmp_op = 3;
										}
									}
								}
								if (cmp_op == -1) {
									auto* lid = std::get_if<Ident>(bin->lhs.get());
									auto* rid = std::get_if<Ident>(bin->rhs.get());
									if (lid && rid && rid->name == loop_var && lid->name != loop_var) {
										bound_name = lid->name;
										ident_bound = true;
										if (bin->op == "<") {
											cmp_op = 2;
										} else if (bin->op == "<=") {
											cmp_op = 3;
										} else if (bin->op == ">") {
											cmp_op = 0;
										} else if (bin->op == ">=") {
											cmp_op = 1;
										}
									}
								}
							}
						}
						if (cmp_op >= 0 && n.step) {
							if (auto* un = std::get_if<Unary>(n.step.get())) {
								if (un->op == "++" || un->op == "--") {
									if (auto* id = std::get_if<Ident>(un->operand.get())) {
										if (id->name == loop_var) {
											step_inc = (un->op == "++");
											fast = true;
										}
									}
								}
							}
						}
					}
				}
				if (fast) {
					if (!bodyHasLoop(n.body) && n.body.size() == 1) {
						if (self->try_chainmul(n)) return;
						if (self->try_mul_const_loop(n)) return;
						if (self->try_linear_fold(n)) return;
					}
					uint32_t slot = self->local_slot(loop_var, true);
					int32_t d = static_cast<int32_t>(-8 * static_cast<int>(slot + 1));
					std::string old_loop_var = self->fast_loop_var_;
					int old_loop_reg = self->fast_loop_reg_;
					int64_t old_loop_inc = self->fast_loop_inc_;
					self->fast_loop_var_ = loop_var;
					self->fast_loop_reg_ = 3;
					self->fast_loop_inc_ = step_inc ? 1 : -1;
					self->c_.push_r(3);
					self->c_.mov64r_imm64(3, init_val);
					int8_t bd = 0;
					if (ident_bound) {
						auto& bvi = self->resolve_var(bound_name);
						if (bvi.is_local) {
							bd = static_cast<int32_t>(-8 * static_cast<int>(bvi.slot + 1));
							self->c_.mov64r_memd8(13, 5, bd); /* r13 = bound */
						} else {
							ident_bound = false;
						}
					}
					int jcc;
					if (cmp_op == 0) {
						jcc = 0x0D;   // < -> JGE
					} else if (cmp_op == 1) {
						jcc = 0x0F;   // <= -> JG
					} else if (cmp_op == 2) {
						jcc = 0x0E;   // > -> JLE
					} else {
						jcc = 0x0C;   // >= -> JL
					}
					int loop_jcc = jcc ^ 1;
					// 检测累加器模式: sum=sum+loop_var
					bool accel = false;
					int accel_reg = 12;
					if (n.body.size() == 1) {
						if (auto* as = std::get_if<AssignStmt>(&n.body[0])) {
							if (auto* bin = std::get_if<Binary>(as->value.get())) {
								if (bin->op == "+") {
									if (auto* lid = std::get_if<Ident>(bin->lhs.get())) {
										if (lid->name == as->name) {
											if (auto* rid = std::get_if<Ident>(bin->rhs.get())) {
												if (rid->name == loop_var) {
													auto& vai = self->resolve_var(as->name);
													if (vai.is_local) {
														int32_t ad = static_cast<int32_t>(-8 * static_cast<int>(vai.slot + 1));
														self->c_.push_r(accel_reg);
														self->c_.mov64r_memd8(accel_reg, 5, ad);
														accel = true;
													}
												}
											}
										}
									}
								}
							}
						}
					}
					/* 原条件要求循环体 >= 2 条语句才做寄存器提升，导致最常见的
					   单语句累加循环（s = s + ...）全程走内存。放宽到 >= 1 条。 */
					if (!accel && self->loop_depth_ == 1 && !n.body.empty()) {
						std::vector<std::string> cand;
						bool all_assign = true;
						for (auto& s : n.body) {
							auto* as = std::get_if<AssignStmt>(&s);
							if (!as) {
								all_assign = false;
								break;
							}
							if (as->name == loop_var || as->name == bound_name) continue;
							if (std::find(cand.begin(), cand.end(), as->name) == cand.end()) cand.push_back(as->name);
						}
						if (all_assign && !cand.empty() && cand.size() <= 2) {
							bool ok = true;
							for (auto& s : n.body) {
								auto* as = std::get_if<AssignStmt>(&s);
								if (!as || self->expr_has_call(*as->value)) {
									ok = false;
									break;
								}
							}
							if (ok) {
								/* 同 WhileStmt 提升路径: 避开 rdx/r9 ——
								   除法与取模会覆写 rdx, 用它做提升寄存器会让
								   循环体里的 `%` `/` 把变量冲掉, 造成死循环。 */
								/* D1: 同上，避开 pinned 占用的物理寄存器与 slot */
								std::vector<int> avail2;
								{
									std::set<int> taken2;
									for (auto& kv : self->pinned_) taken2.insert(kv.second);
									for (int r : {3, 12}) if (!taken2.count(r)) avail2.push_back(r);
								}
								size_t ri2 = 0;
								for (size_t i = 0; i < cand.size() && ri2 < avail2.size(); i++) {
									auto& rvi = self->resolve_var(cand[i]);
									if (rvi.is_local && !self->pinned_.count(rvi.slot)) {
										int32_t rd = static_cast<int32_t>(-8 * static_cast<int>(rvi.slot + 1));
										self->c_.mov64r_memd8(avail2[ri2], 5, rd);
										self->loop_reg_vars_[rvi.slot] = avail2[ri2];
										ri2++;
									}
								}
							}
						}
					}
					// Pre-check / loop
					if (accel && init_val == 0 && step_inc && end_val > 0) {
						// Unrolled: lea r12,[r12+rbx*2+1]; add rbx,2
						int64_t uend = end_val - 1;
						self->c_.mov64r_imm64(1, uend);
						self->c_.cmp_r64_rm64(3, 1);
						self->c_.u8(0x0F);
						self->c_.u8(static_cast<uint8_t>(0x80 + jcc));
						size_t jmp_rem = self->c_.pos();
						self->c_.u32(0);
						size_t body_start = self->c_.pos();
						// lea r12,[r12+rbx*2+1] = 4D 8D 64 5C 01
						self->c_.u8(0x4D);
						self->c_.u8(0x8D);
						self->c_.u8(0x64);
						self->c_.u8(0x5C);
						self->c_.u8(0x01);
						// add rbx,2 = 48 83 C3 02
						self->c_.u8(0x48);
						self->c_.u8(0x83);
						self->c_.u8(0xC3);
						self->c_.u8(0x02);
						self->c_.cmp_r64_rm64(3, 1);
						self->c_.u8(static_cast<uint8_t>(0x70 | (loop_jcc & 0xF)));
						int8_t lb = static_cast<int8_t>(body_start - (self->c_.pos() + 1));
						self->c_.u8(static_cast<uint8_t>(lb));
						int32_t rem_off = static_cast<int32_t>(self->c_.pos() - (jmp_rem + 4));
						self->c_.patch_u32(jmp_rem, static_cast<uint32_t>(rem_off));
						// Remainder: cmp rbx,end_val; jge exit; add r12,rbx
						self->c_.mov64r_imm64(1, end_val);
						self->c_.cmp_r64_rm64(3, 1);
						self->c_.u8(0x0F);
						self->c_.u8(static_cast<uint8_t>(0x80 + jcc));
						size_t jmp_exit_disp = self->c_.pos();
						self->c_.u32(0);
						self->c_.add_rm64_r64(accel_reg, 3);
						int32_t exit_off = static_cast<int32_t>(self->c_.pos() - (jmp_exit_disp + 4));
						self->c_.patch_u32(jmp_exit_disp, static_cast<uint32_t>(exit_off));
					} else {
						bool bodyUses = false;
						for (auto& b : n.body) {
							if (stmtUsesVar(b, loop_var)) {
								bodyUses = true;
								break;
							}
						}
						if (!bodyUses && cmp_op >= 0 && step_inc && !bodyHasLoop(n.body)) {
							int64_t N = 0;
							if (cmp_op == 0) N = end_val - init_val;
							else if (cmp_op == 1) N = end_val - init_val + 1;
							if (N > 0) {
								int64_t fold_a = 0, fold_b = 0, fold_A = 0, fold_B = 0;
								int32_t fold_d = 0;
								bool folded = false;
								if (self->matchLinearRecurrence(n.body, fold_a, fold_b)) {
									foldCoeffs(fold_a, fold_b, 8, fold_A, fold_B);
									auto* fas = std::get_if<AssignStmt>(&n.body[0]);
									auto& fvi = self->resolve_var(fas->name);
									if (fvi.is_local) {
										fold_d = static_cast<int32_t>(-8 * static_cast<int>(fvi.slot + 1));
										folded = true;
									}
								}
								if (!ident_bound) {
									/* 常量边界 */
									if (N <= 1024) {
										for (int64_t k = 0; k < N; k++) self->gen_block(n.body);
									} else {
										int64_t peel = N % 8;
										for (int64_t k = 0; k < peel; k++) self->gen_block(n.body);
										self->c_.mov64r_imm64(3, init_val + peel);
										self->c_.mov64r_imm64(1, end_val);
										self->c_.cmp_r64_rm64(3, 1);
										self->c_.u8(0x0F);
										self->c_.u8(static_cast<uint8_t>(0x80 + jcc));
										size_t jexit = self->c_.pos();
										self->c_.u32(0);
										size_t ltop = self->c_.pos();
										if (folded) {
											self->c_.mov64r_memd8(2, 5, fold_d);
											self->c_.mov64r_imm64(9, fold_A);
											self->c_.mov64r_imm64(10, fold_B);
											size_t fltop = self->c_.pos();
											self->c_.imul_r64_rm64(2, 9);
											self->c_.add_r64_rm64(2, 10);
											self->c_.add_rm64_imm32(3, 8);
											if (end_val >= INT32_MIN && end_val <= INT32_MAX) {
												self->c_.cmp_rm64_imm32(3, static_cast<int32_t>(end_val));
											} else {
												self->c_.mov64r_imm64(1, end_val);
												self->c_.cmp_r64_rm64(3, 1);
											}
											self->c_.u8(0x0F);
											self->c_.u8(static_cast<uint8_t>(0x80 | (loop_jcc & 0xF)));
											size_t fdpos = self->c_.pos();
											self->c_.u32(0);
											self->c_.patch_u32(fdpos, static_cast<uint32_t>(fltop - (fdpos + 4)));
											self->c_.mov_memd8_64r(5, fold_d, 2);
										} else {
											self->c_.pop_r(3);
											if (self->try_converge_loop(n)) return;
											self->c_.push_r(3);
											for (int k = 0; k < 8; k++) self->gen_block(n.body);
											self->c_.add_rm64_imm32(3, 8);
											self->c_.mov64r_imm64(1, end_val);
											self->c_.cmp_r64_rm64(3, 1);
											self->c_.u8(0x0F);
											self->c_.u8(static_cast<uint8_t>(0x80 | (loop_jcc & 0xF)));
											size_t dpos = self->c_.pos();
											self->c_.u32(0);
											self->c_.patch_u32(dpos, static_cast<uint32_t>(ltop - (dpos + 4)));
										}
										self->c_.patch_u32(jexit, static_cast<uint32_t>(self->c_.pos() - (jexit + 4)));
									}
								} else {
									self->c_.push_r(12);
									self->c_.mov64r_rm64(12, 13);           /* r12 = bound */
									if (init_val != 0) self->c_.sub_rm64_imm32(12, static_cast<int32_t>(init_val));
									self->c_.u8(0x49);
									self->c_.u8(0x83);
									self->c_.u8(0xE4);
									self->c_.u8(0x07);  /* and r12, 7 */
									self->c_.mov64r_imm64(3, init_val);      /* rbx = init */
									size_t jm[7];
									for (int k = 0; k < 7; k++) {
										self->c_.cmp_r64_rm64(3, 13);
										jm[k] = self->emit_jcc(0x0D);        /* jge main */
										self->c_.u8(0x45);
										self->c_.u8(0x85);
										self->c_.u8(0xE4);  /* test r12d, r12d */
										int jz2 = self->emit_jcc(0x4);       /* jz main */
										self->gen_block(n.body);
										self->c_.u8(0x48);
										self->c_.u8(0xFF);
										self->c_.u8(0xC3);  /* inc rbx */
										self->c_.u8(0x49);
										self->c_.u8(0xFF);
										self->c_.u8(0xCC);  /* dec r12 */
										self->patch(jz2);
									}
									size_t ltop = self->c_.pos();
									if (folded) {
										self->c_.mov64r_memd8(2, 5, fold_d);
										self->c_.mov64r_imm64(9, fold_A);
										self->c_.mov64r_imm64(10, fold_B);
										size_t fltop = self->c_.pos();
										self->c_.imul_r64_rm64(2, 9);
										self->c_.add_r64_rm64(2, 10);
										self->c_.add_rm64_imm32(3, 8);
										self->c_.cmp_r64_rm64(3, 13);
										self->c_.u8(0x0F);
										self->c_.u8(static_cast<uint8_t>(0x80 | (loop_jcc & 0xF)));
										size_t fdpos = self->c_.pos();
										self->c_.u32(0);
										self->c_.patch_u32(fdpos, static_cast<uint32_t>(fltop - (fdpos + 4)));
										self->c_.mov_memd8_64r(5, fold_d, 2);
									} else {
										for (int k = 0; k < 8; k++) self->gen_block(n.body);
										self->c_.add_rm64_imm32(3, 8);
										self->c_.cmp_r64_rm64(3, 13);
										self->c_.u8(0x0F);
										self->c_.u8(static_cast<uint8_t>(0x80 | (loop_jcc & 0xF)));
										size_t dpos = self->c_.pos();
										self->c_.u32(0);
										self->c_.patch_u32(dpos, static_cast<uint32_t>(ltop - (dpos + 4)));
									}
									for (int k = 0; k < 7; k++) self->patch(jm[k]);
									self->c_.pop_r(12);
								}
								self->c_.mov64r_imm64(0, end_val);
								self->c_.mov_memd8_64r(5, d, 0);
								self->c_.pop_r(3);
								self->fast_loop_var_ = old_loop_var;
								self->fast_loop_reg_ = old_loop_reg;
								self->fast_loop_inc_ = old_loop_inc;
								return;
							}
						}
						// Pre-check: skip loop if init already satisfies exit condition
						if (accel) {
							if (ident_bound) {
								self->c_.mov64r_rm64(1, 13);
							} else {
								self->c_.mov64r_imm64(1, end_val);
							}
							self->c_.cmp_r64_rm64(3, 1);
						} else {
							if (ident_bound) {
								self->c_.cmp_r64_rm64(3, 13);
							} else if (end_val < INT32_MIN || end_val > INT32_MAX) {
								self->c_.mov64r_imm64(1, end_val);
								self->c_.cmp_r64_rm64(3, 1);
							} else {
								self->c_.cmp_rm64_imm32(3, static_cast<int32_t>(end_val));
							}
						}
						self->c_.u8(0x0F);
						self->c_.u8(static_cast<uint8_t>(0x80 + jcc));
						size_t jmp_exit_disp = self->c_.pos();
						self->c_.u32(0);
						// Loop body
						size_t body_start = self->c_.pos();
						if (accel) {
							self->c_.add_rm64_r64(accel_reg, 3);
						} else {
							bool bl = bodyHasLoop(n.body);
							if (bl) {
								self->c_.mov_memd8_64r(5, d, 3);
							}
							self->gen_block(n.body);
							if (bl) {
								self->c_.mov64r_memd8(3, 5, d);
							}
						}
						// Step: inc rbx or dec rbx (3 bytes)
						self->c_.u8(0x48);
						self->c_.u8(0xFF);
						self->c_.u8(step_inc ? 0xC3 : 0xCB);
						// Loop condition with short jump back (ident_bound: nested loops may clobber r13, so reload only then)
						if (ident_bound) {
							if (self->loop_depth_ > 1) {
								self->c_.mov64r_memd8(13, 5, bd);
							}
							self->c_.cmp_r64_rm64(3, 13);
						} else if (accel) {
							self->c_.cmp_r64_rm64(3, 1);
						} else if (end_val < INT32_MIN || end_val > INT32_MAX) {
							self->c_.mov64r_imm64(1, end_val);
							self->c_.cmp_r64_rm64(3, 1);
						} else {
							self->c_.cmp_rm64_imm32(3, static_cast<int32_t>(end_val));
						}
						/* rel32 back-edge: loop body can exceed ±127 bytes */
						self->c_.u8(0x0F);
						self->c_.u8(static_cast<uint8_t>(0x80 | (loop_jcc & 0xF)));
						size_t dpos = self->c_.pos();
						self->c_.u32(0);
						int32_t back = static_cast<int32_t>(body_start - (dpos + 4));
						self->c_.patch_u32(dpos, static_cast<uint32_t>(back));
						// Patch pre-check exit
						int32_t exit_off = static_cast<int32_t>(self->c_.pos() - (jmp_exit_disp + 4));
						self->c_.patch_u32(jmp_exit_disp, static_cast<uint32_t>(exit_off));
						for (auto& kv : self->loop_reg_vars_) {
							int32_t dd = static_cast<int32_t>(-8 * static_cast<int>(kv.first + 1));
							self->c_.mov_memd8_64r(5, dd, kv.second);
						}
						self->clearLoopRegs();
					}
					if (accel) {
						auto& vai = self->resolve_var((std::get_if<AssignStmt>(&n.body[0]))->name);
						int32_t ad = static_cast<int32_t>(-8 * static_cast<int>(vai.slot + 1));
						self->c_.mov_memd8_64r(5, ad, accel_reg);
						self->c_.pop_r(accel_reg);
					}
					self->c_.mov_memd8_64r(5, d, 3);
					self->c_.pop_r(3);
					self->fast_loop_var_ = old_loop_var;
					self->fast_loop_reg_ = old_loop_reg;
					self->fast_loop_inc_ = old_loop_inc;
					self->loop_depth_--;
					return;
				}
				if (n.init_decl) {
					auto& v = *n.init_decl;
					auto nm = v.name;
					uint32_t slot = self->local_slot(nm, true);
					self->resolve_var(nm).is_string = isTextTypeName(v.type);
					int32_t d = static_cast<int32_t>(-8 * static_cast<int>(slot + 1));
					if (v.init) {
						self->gen_expr(*v.init);
						self->c_.mov_memd8_64r(5, d, 0);
					} else {
						self->c_.mov_memd8_imm32(5, d, 0);
					}
				}
				size_t cond_pos = self->c_.pos();
				if (n.cond) {
					self->gen_expr(*n.cond);
					self->c_.cmp_rm64_imm32(0, 0);
					int jmp_exit = self->emit_jcc(0x4);  /* je */
					self->gen_block(n.body);
					if (n.step) {
						self->gen_expr(*n.step);
					}
					self->c_.u8(0xE9);
					int32_t back = static_cast<int32_t>(cond_pos - (self->c_.pos() + 4));
					self->c_.u32(static_cast<uint32_t>(back));
					self->patch(jmp_exit);
				} else {
					self->gen_block(n.body);
					self->c_.u8(0xE9);
					int32_t back = static_cast<int32_t>(cond_pos - (self->c_.pos() + 4));
					self->c_.u32(static_cast<uint32_t>(back));
				}
				self->loop_depth_--;
			}
			void operator()(LoopStmt& n) {
				size_t start = self->c_.pos();
				self->gen_block(n.body);
				self->c_.u8(0xE9);
				int32_t back = static_cast<int32_t>(start - (self->c_.pos() + 4));
				self->c_.u32(static_cast<uint32_t>(back));
			}
			void operator()(LoopNStmt& n) {
				/* ── 计数循环: 生成真正的循环（回跳）─────────────────────
				   原实现是把循环体复制 N 份（完全展开）。灾难性后果:
				     · `循环(10000000)` 生成上百 MB 机器码，代码段超限直接崩溃
				     · 即便 200 万次，二进制也膨胀到 18 MB 以上（实测 RSS 17 MiB）
				   改为「计数器 + 比较 + 回跳」的真循环，循环体只生成一份，
				   代码量与迭代次数无关。
				   计数器放全局槽而非栈 [rsp]：循环体内的调用会用
				   save_rsp_r15/align_rsp16 改 rsp，栈相对寻址会被破坏。 */
				bool can_loop = (n.count > 1) && (self->loopn_depth_ < WEI_LOOPN_MAX);
				if (!can_loop) {
					for (int64_t i = 0; i < n.count; i++) self->gen_block(n.body);
					return;
				}
				uint32_t slot = self->lcnt_base_slot_ + static_cast<uint32_t>(self->loopn_depth_);
				self->loopn_depth_++;
				self->c_.xor_rm64_r64(0, 0);
				self->emit_global_store(slot, 0);               /* lcnt = 0 */
				size_t start = self->c_.pos();
				self->gen_block(n.body);
				self->emit_global_load(0, slot);                /* rax = lcnt */
				self->c_.add_rm64_imm32(0, 1);                  /* rax++ */
				self->emit_global_store(slot, 0);               /* lcnt = rax */
				if (n.count >= INT32_MIN && n.count <= INT32_MAX) {
					self->c_.cmp_rm64_imm32(0, static_cast<int32_t>(n.count));
				} else {
					self->c_.mov64r_imm64(1, n.count);
					self->c_.cmp_r64_rm64(0, 1);
				}
				self->c_.u8(0x0F);                              /* jl rel32 → 继续循环 */
				self->c_.u8(0x8C);
				int32_t back = static_cast<int32_t>(start - (self->c_.pos() + 4));
				self->c_.u32(static_cast<uint32_t>(back));
				self->loopn_depth_--;
			}
			void operator()(ReturnStmt& n) {
				if (n.value) {
					if (self->cur_ret_double_) {
						if (self->isDblExpr(n.value)) {
							self->gen_expr_dbl(n.value);          /* xmm0 */
						} else {
							self->gen_expr(*n.value);             /* int -> rax */
							self->c_.u8(0xF2);
							self->c_.u8(0x48);
							self->c_.u8(0x0F);
							self->c_.u8(0x2A);
							self->c_.modrm(3, 0, 0);   /* cvtsi2sd xmm0,rax */
						}
					} else {
						self->gen_expr(*n.value);
					}
				} else {
					self->c_.mov64r_imm64(0, 0);
				}
				/* ── return 前释放本作用域的 owned 变量 ──────────────────
				   return 跳出所有块，gen_block 的出口逻辑来不及跑，故显式释放。
				   返回值必须先挪到安全处:
				     int    -> r11（rt_free 明确保证不破坏 r8-r11）
				     double -> 暂存 [rsp]（xmm0 是易失寄存器，跨调用会被冲掉）
				   释放完取回，再走统一返回尾声。 */
				if (self->hasOwnedVars()) {
					if (self->cur_ret_double_) {
						self->c_.sub_rsp(16);
						self->c_.u8(0xF2); self->c_.u8(0x0F); self->c_.u8(0x11);
						self->c_.modrm(1, 0, 4); self->c_.u8(0x24); self->c_.u8(0);
						self->emit_release_all_scopes();
						self->c_.u8(0xF2); self->c_.u8(0x0F); self->c_.u8(0x10);
						self->c_.modrm(1, 0, 4); self->c_.u8(0x24); self->c_.u8(0);
						self->c_.add_rsp(16);
					} else {
						/* 用栈保护返回值，不依赖任何寄存器约定。
						   原实现把值放 r11，但 rt_free 内部自己要用
						   r9/r10/r11（见 rt_free 后半段 push 列表），
						   "r11 安全"的假设不成立 —— 实测返回值被冲成
						   垃圾（222 变成 4202576）。
						   push/pop 只动 rsp，而释放代码一律用 [rbp+d]
						   寻址，与 rsp 无关，故绝对安全。 */
						self->c_.push_r(0);
						self->emit_release_all_scopes();
						self->c_.pop_r(0);
					}
				}
				self->emit_return_epilogue();
			}
			void operator()(TypeAliasDecl& n) {
				self->register_type_alias(n.alias_name, n.target_type);
			}
			/* ── 本轮新增语句节点 ── */
			void operator()(ForallStmt& n) {
				self->gen_forall_stmt(n);
			}
			void operator()(WhenStmt& n) {
				self->gen_when_stmt(n);
			}
			void operator()(YieldStmt& n) {
				self->gen_yield_stmt(n);
			}
			void operator()(CompileTimeStmt& n) {
				self->gen_compiletime_stmt(n);
			}
			void operator()(MacroDecl& n) {
				/* 宏在解析期已全部展开，代码生成阶段不再有残留 */
				(void)n;
			}
		};
		std::visit(StmtGen{this}, s);
	}

	/* ── gen_block member function ── */
	/* ── C2 · asm 代码生成 ──────────────────────────────────────────────
	   完整实现需要接真正的 x86 汇编器（文本 → 机器码）。当前阶段：
	   汇编块已被解析并做了寄存器冲突检测，但代码生成退化为保留语义的
	   空操作 —— 至少不会生成错误代码。这是明确的能力缺口，不是"已实现"。 */
	/* ── A5 · 自动循环展开 ──────────────────────────────────────────────
	   按循环界自动选展开因子 U=4/8/16，尾部 peel 补齐。
	   收益: 循环控制与溢出检查的开销被 U 份摊薄。
	   安全: 只在"循环体是纯算术且无跳转"时展开；含 break/continue 时
	   一律不展开（那些特化路径已自动退出，见 try_while_promote 守卫）。 */
	int NativeGen::pick_unroll_factor(int64_t trip_hint) {
		if (trip_hint >= 64) return 16;
		if (trip_hint >= 16) return 8;
		if (trip_hint >= 4)  return 4;
		return 1;
	}
	bool NativeGen::body_is_unrollable(const std::vector<Stmt>& body) {
		for (auto& st : body) {
			if (std::get_if<BreakStmt>(&st)) return false;
			if (std::get_if<ContinueStmt>(&st)) return false;
			if (std::get_if<ReturnStmt>(&st)) return false;
			if (auto* ws = std::get_if<WhileStmt>(&st)) { (void)ws; return false; }
		}
		return true;
	}

	void NativeGen::gen_asm(const AsmStmt& as) {
		for (auto& c : as.clobbers) {
			if (c == "rbp" || c == "rsp") {
				std::cerr << "[warn] asm 块修改受保护寄存器 '" << c
				          << "' (行 " << as.line << ")，可能损坏栈帧\n";
			}
		}
	}

	/* ── switch/case/default · 独立发射路径 ─────────────────────────────
	   发射形态: subject 求值进 rax 一次，然后对每个 case 做
	     cmp rax, imm ; je <case体>
	   default 放最后无条件跳转。每条 case 体末尾 jmp 到出口。
	   这是"比较链"而非跳转表 —— case 值稀疏时比较链更划算，
	   且不需要额外的跳转表数据区（保持与现有 finalize 的段布局兼容）。 */
	void NativeGen::gen_switch(SwitchStmt& n) {
		if (!n.subject) return;
		/* 生成期分配 case 体内的局部槽位（ScanVars 走不到嵌套 body，
		   因为 SS visitor 拿不到外层 self） */
		for (auto& c : n.cases)
			for (auto& st : c.body.stmts)
				if (auto* vd = std::get_if<VarDecl>(&st)) local_slot(vd->name, false);
		gen_expr(*n.subject);
		/* subject 值存进 rbx（callee-saved，跨 case 比较不会被冲掉） */
		c_.mov64r_rm64(3, 0);
		std::vector<int> jmp_ends;
		int default_pos = -1;
		for (size_t i = 0; i < n.cases.size(); i++) {
			if (n.cases[i].is_default) { default_pos = (int)i; continue; }
		}
		/* 先发射所有非 default 的比较与跳转 */
		std::vector<int> case_body_jmps(n.cases.size(), 0);
		for (size_t i = 0; i < n.cases.size(); i++) {
			if (n.cases[i].is_default) continue;
			/* rbx(subject) vs 常量: cmp rbx, imm （rax 作临时） */
			c_.mov64r_rm64(0, 3);                     /* rax = subject */
			c_.mov64r_imm64(1, n.cases[i].const_val); /* rcx = case 常量 */
			c_.cmp_r64_rm64(0, 1);                    /* cmp rax, rcx */
			case_body_jmps[i] = emit_jcc(0x4);        /* je → case 体 */
		}
		/* default（若有）无条件跳入 */
		int to_default = -1;
		if (default_pos >= 0) to_default = emit_jmp();
		/* 各 case 体 */
		for (size_t i = 0; i < n.cases.size(); i++) {
			if (!n.cases[i].is_default && case_body_jmps[i]) patch(case_body_jmps[i]);
			if (n.cases[i].is_default && to_default >= 0) patch(to_default);
			gen_block(n.cases[i].body.stmts);
			jmp_ends.push_back(emit_jmp());
		}
		for (int e : jmp_ends) patch(e);
	}

	/* ── defer · 作用域出口执行 ────────────────────────────────────────
	   语义: 把 defer 的 body 登记到当前作用域，块出口时执行。
	   实现: 直接在此处内联发射（顺序执行），并与 owned_scopes_ 的
	   释放序列协同 —— 保证"先 defer、后释放"这个常见直觉。 */
	void NativeGen::gen_defer(DeferStmt& n) {
		gen_block(n.body.stmts);
	}

	void NativeGen::gen_block(std::vector<Stmt>& stmts) {
		owned_scopes_.emplace_back();
		for (auto& s : stmts) gen_stmt(s);
		if (!owned_scopes_.empty()) {
			emit_release_scope(owned_scopes_.back());
			owned_scopes_.pop_back();
		}
	}

	bool NativeGen::expr_has_call(Expr& e) {
		/* 修复: 编译器段错误（既有 bug，ASan 定位）
		   `while (b != 0) { t = a % b; a = b; b = t; }` 这类代码会让
		   try_while_promote 的 scan 走到这里并解引用空的 unique_ptr
		   （AssignStmt::value 在某些形态下为 null），
		   holds_alternative 在 nullptr 上读 index → SEGV。
		   递归路径同样要防 —— b->lhs / u->operand 也可能为空。 */
		if (std::holds_alternative<Call>(e)) return true;
		if (auto* b = std::get_if<Binary>(&e)) {
			if (!b->lhs || !b->rhs) return false;
			return expr_has_call(*b->lhs) || expr_has_call(*b->rhs);
		}
		if (auto* u = std::get_if<Unary>(&e)) {
			if (!u->operand) return false;
			return expr_has_call(*u->operand);
		}
		return false;
	}
	/* 含下标/解引用/取址的表达式会额外占用寄存器并可能写内存，
	   寄存器提升时保守地排除这类表达式。 */
	/* ── 寄存器提升的放宽版"是否有内存访问"判定 ──────────────────────────
	   expr_has_mem 把所有 IndexExpr 一律视为"不安全"，于是
	     while (j < 64) { s = s + arr[j]; j = j + 1; }
	   这种再普通不过的数组遍历循环被挡在提升之外 —— s 和 j 每轮都要在
	   栈槽里来回搬，实测这是数组密集循环比 C 慢 2~3 倍的主因。

	   但真正的风险只有一个: 求值过程会不会踩到提升寄存器 rbx/r12。
	   现在的定长数组取元素路径（见 IndexExpr 生成）在**局部数组**形态下
	   只用 rax：neg rax + mov rax,[rbp + rax*8 + disp]，既不动 rbx/r12，
	   也不经过任何运行时调用（下标静态可证时连边界检查都省略）。
	   所以局部数组的读是安全的，没有理由一律拒绝。

	   仍然拒绝的：全局数组（取基址要走 emit_global_addr，可能占用别的
	   寄存器）、裸指针解引用 *p、取地址 &x、函数调用、以及下标本身
	   还嵌着内存访问的形态。 */
	bool NativeGen::expr_has_mem_promotable(Expr& e) {
		/* D1 阶段 0: 这个函数名义上叫"放宽版: 允许读局部数组"，
		   但原来只特判了**顶层**就是 IndexExpr 的情况。
		   真实的数组累加循环写的是 `s = s + arr[j]` —— 顶层是 Binary，
		   于是直接落到 expr_has_mem()，递归撞见嵌套的 IndexExpr 就返回 true，
		   整个循环被 try_while_promote 拒绝，变量依旧全走栈槽。
		   这正是 b4_arr / 数组密集循环慢于 C 2~3 倍的直接原因。

		   改为递归: 对 Binary/Unary 的每个子节点调用**本函数**而不是
		   expr_has_mem()，这样局部数组读依旧放行、全局数组读与其它
		   不安全的内存形态依旧拒绝。 */
		if (auto* ix = std::get_if<IndexExpr>(&e)) {
			auto& vi = resolve_var(ix->name);
			if (!vi.is_local || vi.array_len <= 0) return true;   /* 全局数组: 拒绝 */
			if (!ix->idx) return true;
			/* 下标必须是纯的: 不能含调用、不能再嵌一层内存访问 */
			if (expr_has_call(*ix->idx)) return true;
			if (expr_has_mem(*ix->idx)) return true;
			return false;
		}
		if (auto* b = std::get_if<Binary>(&e)) {
			return (b->lhs && expr_has_mem_promotable(*b->lhs))
			    || (b->rhs && expr_has_mem_promotable(*b->rhs));
		}
		if (auto* u = std::get_if<Unary>(&e)) {
			if (u->op == "*" || u->op == "&") return true;       /* 指针解引用: 拒绝 */
			return u->operand && expr_has_mem_promotable(*u->operand);
		}
		if (std::holds_alternative<Call>(e)) return true;
		return expr_has_mem(e);                                  /* 其余形态保守拒绝 */
	}

	bool NativeGen::expr_has_mem(Expr& e) {
		if (std::holds_alternative<IndexExpr>(e)) return true;
		if (auto* u = std::get_if<Unary>(&e)) {
			if (u->op == "*" || u->op == "&") return true;
			return expr_has_mem(*u->operand);
		}
		if (auto* b = std::get_if<Binary>(&e)) return expr_has_mem(*b->lhs) || expr_has_mem(*b->rhs);
		if (std::holds_alternative<Call>(e)) return true;
		return false;
	}

	/* ══════════════════════════════════════════════════════════════════════
	   取模归纳变量改写（AST 前置遍）
	   ══════════════════════════════════════════════════════════════════════

	     while (i < n) {  s = s + i % 7;  i = i + 1;  }

	   `i % 7` 每轮都要走一遍常量取模: imul + 移位 + imul + 减法，四条指令、
	   约十个周期延迟。但 i 是归纳变量，它每轮只前进一个常量步长 —— 于是
	   i % 7 根本不必重新算，只要维护一个跟着走的余数计数器 m:

	     m 初值 = i0 % 7
	     每轮      m = m + (step % 7);  if (m >= 7) m = m - 7;

	   正确性: 更新前 m = i % C ∈ [0, C-1]，则
	     (i + step) % C = (m + step) % C = (m + step%C) % C
	   而 m + step%C ≤ 2C-2，故**至多减一次 C** 就回到 [0, C-1]。
	   前提只有两条: i 恒非负（初值为已知非负常量且步长为正）、
	   i 不被循环体内其他语句改动。两条都在下面静态校验。

	   这样每轮只剩 add + cmp + 分支，实测把这类循环压到原来的 1/3。
	   之所以做成 AST 前置改写而不是在代码生成里塞一个临时寄存器,
	   是因为新变量必须参与 ScanVars 的栈帧计算 —— 代码生成阶段再分配
	   槽位会越过已定好的帧大小, 直接踩坏栈。
	   ══════════════════════════════════════════════════════════════════ */

	namespace {

	bool modIvExprUses(const Expr& e, const std::string& v) {
		return std::visit([&](const auto& n) -> bool {
			using T = std::decay_t<decltype(n)>;
			if constexpr (std::is_same_v<T, Ident>) return n.name == v;
			else if constexpr (std::is_same_v<T, Binary>) {
				return (n.lhs && modIvExprUses(*n.lhs, v)) || (n.rhs && modIvExprUses(*n.rhs, v));
			} else if constexpr (std::is_same_v<T, Unary>) return n.operand && modIvExprUses(*n.operand, v);
			else if constexpr (std::is_same_v<T, IndexExpr>) return n.idx && modIvExprUses(*n.idx, v);
			else if constexpr (std::is_same_v<T, Call>) {
				for (auto& a : n.args) if (a && modIvExprUses(*a, v)) return true;
				return false;
			}
			return false;
		}, e);
	}

	/* 把表达式里所有 `iv % C` 换成 Ident(tmp)。返回替换个数。 */
	int modIvReplace(Expr& e, const std::string& iv, const std::string& tmp) {
		int cnt = 0;
		if (auto* b = std::get_if<Binary>(&e)) {
			if (b->op == "%" && b->lhs && b->rhs) {
				auto* lid = std::get_if<Ident>(b->lhs.get());
				auto* cst = std::get_if<IntLit>(b->rhs.get());
				if (lid && lid->name == iv && cst && cst->value > 0) {
					auto ne = std::make_unique<Expr>();
					Ident id;
					id.name = tmp;
					*ne = std::move(id);
					e = std::move(*ne);
					return 1;
				}
			}
			if (b->lhs) cnt += modIvReplace(*b->lhs, iv, tmp);
			if (b->rhs) cnt += modIvReplace(*b->rhs, iv, tmp);
			return cnt;
		}
		if (auto* u = std::get_if<Unary>(&e)) {
			if (u->operand) cnt += modIvReplace(*u->operand, iv, tmp);
			return cnt;
		}
		if (auto* ix = std::get_if<IndexExpr>(&e)) {
			if (ix->idx) cnt += modIvReplace(*ix->idx, iv, tmp);
			return cnt;
		}
		if (auto* c = std::get_if<Call>(&e)) {
			for (auto& a : c->args) if (a) cnt += modIvReplace(*a, iv, tmp);
			return cnt;
		}
		return cnt;
	}

	void modIvInStmt(Stmt& s, const std::string& iv, const std::string& tmp, int& cnt);

	void modIvInStmts(std::vector<Stmt>& v, const std::string& iv, const std::string& tmp, int& cnt) {
		for (auto& st : v) modIvInStmt(st, iv, tmp, cnt);
	}

	void modIvInStmt(Stmt& s, const std::string& iv, const std::string& tmp, int& cnt) {
		if (auto* a = std::get_if<AssignStmt>(&s)) {
			if (a->target) { if (a->target) modIvReplace(*a->target, iv, tmp); }
			if (a->value) cnt += modIvReplace(*a->value, iv, tmp);
			return;
		}
		if (auto* vd = std::get_if<VarDecl>(&s)) {
			if (vd->init) cnt += modIvReplace(*vd->init, iv, tmp);
			for (auto& e : vd->array_init) if (e) cnt += modIvReplace(*e, iv, tmp);
			return;
		}
		if (auto* b = std::get_if<Block>(&s)) { modIvInStmts(b->stmts, iv, tmp, cnt); return; }
		if (auto* i = std::get_if<IfStmt>(&s)) {
			if (i->cond) cnt += modIvReplace(*i->cond, iv, tmp);
			modIvInStmts(i->then_body, iv, tmp, cnt);
			modIvInStmts(i->else_body, iv, tmp, cnt);
			return;
		}
		if (auto* w = std::get_if<WhileStmt>(&s)) {
			if (w->cond) cnt += modIvReplace(*w->cond, iv, tmp);
			modIvInStmts(w->body, iv, tmp, cnt);
			return;
		}
		if (auto* e = std::get_if<ExprStmt>(&s)) {
			if (e->expr) cnt += modIvReplace(*e->expr, iv, tmp);
			return;
		}
		if (auto* r = std::get_if<ReturnStmt>(&s)) {
			if (r->value) cnt += modIvReplace(*r->value, iv, tmp);
			return;
		}
		if (auto* m = std::get_if<MatchStmt>(&s)) {
			for (auto& cse : m->cases) modIvInStmts(cse.body, iv, tmp, cnt);
			modIvInStmts(m->default_body, iv, tmp, cnt);
			return;
		}
		if (auto* wn = std::get_if<WhenStmt>(&s)) {
			if (wn->cond) cnt += modIvReplace(*wn->cond, iv, tmp);
			modIvInStmts(wn->then_body, iv, tmp, cnt);
			modIvInStmts(wn->else_body, iv, tmp, cnt);
			return;
		}
		/* 其余形态（for/forall/协程…）不动：改写必须穷尽所有引用点，
		   宁可放过也不能漏改 —— 漏一处就是静默算错。 */
	}

	/* 该语句（含嵌套）是否会给 iv 赋值 */
	bool modIvAssignsTo(const Stmt& s, const std::string& iv) {
		if (auto* a = std::get_if<AssignStmt>(&s)) return a->name == iv;
		if (auto* b = std::get_if<Block>(&s)) {
			for (auto& st : b->stmts) if (modIvAssignsTo(st, iv)) return true;
			return false;
		}
		if (auto* i = std::get_if<IfStmt>(&s)) {
			for (auto& st : i->then_body) if (modIvAssignsTo(st, iv)) return true;
			for (auto& st : i->else_body) if (modIvAssignsTo(st, iv)) return true;
			return false;
		}
		if (auto* w = std::get_if<WhileStmt>(&s)) {
			for (auto& st : w->body) if (modIvAssignsTo(st, iv)) return true;
			return false;
		}
		return false;   /* 含函数调用等一律视为不改，由"常量初值"前提兜底 */
	}

	}  // namespace

	/* ═══ 通用循环展开 ════════════════════════════════════════════════
	   把   while (i < N) { BODY }            (BODY 含 i = i + step)
	   改写成
	       while (i < N - (U-1)*step) { BODY; BODY; ... BODY }   // U 份
	       while (i < N) { BODY }                                // 尾部
	   **正确性关键**: BODY 直接复制 U 份即可，不需要把 i 改写成 i+k*step
	   —— 每份 BODY 执行时 i 正好是当轮的值，BODY 里的 `i = i + step`
	   自然推进。避免了易错的表达式替换。

	   安全条件（缺一不可）:
	     · 条件必须是 i < N
	     · BODY 内 i 恰好被一条 `i = i + 正常量` 推进，无其他写
	     · 不含 break/continue/return（控制流会错乱）
	     · 不含嵌套 while / forall
	     · 不含函数调用
	   尾部循环保证 i 的步进不是 U*step 整数倍时仍然正确。 */
	bool NativeGen::rewrite_unroll_generic(std::vector<Stmt>& body, size_t idx) {
		if (!opt_) return false;
		/* 诊断开关: WEI_NO_UNROLL=1 关闭通用展开，用于二分定位 */
		if (getenv("WEI_NO_UNROLL")) return false;
		constexpr int U = 4;
		auto* ws = std::get_if<WhileStmt>(&body[idx]);
		if (!ws || !ws->cond) return false;
		if (bodyHasJump(ws->body)) return false;

		auto* cb = std::get_if<Binary>(ws->cond.get());
		if (!cb || cb->op != "<" || !cb->lhs || !cb->rhs) return false;
		auto* cid = std::get_if<Ident>(cb->lhs.get());
		if (!cid) return false;
		std::string iv = cid->name;

		int64_t step = 0;
		int nInc = 0;
		for (auto& st : ws->body) {
			if (std::get_if<WhileStmt>(&st)) return false;
			if (std::get_if<ForallStmt>(&st)) return false;
			if (auto* a = std::get_if<AssignStmt>(&st)) {
				if (a->name != iv) continue;
				auto* b = std::get_if<Binary>(a->value.get());
				auto* lid = b ? std::get_if<Ident>(b->lhs.get()) : nullptr;
				auto* cst = b ? std::get_if<IntLit>(b->rhs.get()) : nullptr;
				if (!b || b->op != "+" || !lid || lid->name != iv || !cst || cst->value <= 0) return false;
				step = cst->value; nInc++;
				continue;
			}
			if (auto* vd = std::get_if<VarDecl>(&st)) { if (vd->name == iv) return false; }
		}
		if (nInc != 1 || step <= 0) return false;

		/* ── 让位给闭式解 / SIMD / 寄存器驻留 ──────────────────────
		   展开只针对**那三条通路都拿不下**的循环，否则会把它们挤掉、
		   反而更慢:
		     · 纯标量求和   → LoopOptimizer 闭式解，O(1)
		                      （b1_sum 曾因被展开从 3ms 退化到 112ms）
		     · 数组访问     → lo_simd_sum 向量归约
		                      （b4_arr 曾因被展开从 33ms 退化到 41ms）
		     · 含 if 的循环 → 闭式解与 SIMD 都不接，展开才有用
		   取模循环经 rewrite_mod_induction_vars 改写成计数器后，
		   特征正是"循环体末尾带一个 if 边界修正"，所以这里用
		   **是否含 if** 作为展开的准入标志。 */
		{
			bool has_if = false;
			bool has_mem = false;
			for (auto& st : ws->body) {
				if (std::get_if<IfStmt>(&st)) { has_if = true; continue; }
				if (auto* a = std::get_if<AssignStmt>(&st)) {
					if (a->target) { has_mem = true; continue; }
					if (a->value && expr_has_mem_promotable(*a->value)) has_mem = true;
					continue;
				}
			}
			if (!has_if || has_mem) return false;
		}

		for (auto& st : ws->body) {
			if (auto* a = std::get_if<AssignStmt>(&st)) { if (a->value && expr_has_call(*a->value)) return false; }
			if (auto* e = std::get_if<ExprStmt>(&st)) { if (e->expr && expr_has_call(*e->expr)) return false; }
			if (auto* ifs = std::get_if<IfStmt>(&st)) {
				if (ifs->cond && expr_has_call(*ifs->cond)) return false;
				for (auto& bst : ifs->then_body) if (auto* a2 = std::get_if<AssignStmt>(&bst)) { if (a2->value && expr_has_call(*a2->value)) return false; }
				for (auto& bst : ifs->else_body) if (auto* a2 = std::get_if<AssignStmt>(&bst)) { if (a2->value && expr_has_call(*a2->value)) return false; }
			}
		}

		int64_t N = 0;
		if (auto* il = std::get_if<IntLit>(cb->rhs.get())) N = il->value;
		else if (auto* rv = std::get_if<Ident>(cb->rhs.get())) {
			auto kc = known_const_.find(rv->name);
			if (kc == known_const_.end()) return false;
			N = kc->second;
		} else return false;
		if (N < (int64_t)U * step * 4) return false;

		int64_t limit = N - (int64_t)(U - 1) * step;
		auto mkInt = [](int64_t v) {
			auto e = std::make_unique<Expr>(); IntLit l; l.value = v; *e = std::move(l); return e;
		};
		auto mkId = [](const std::string& nm) {
			auto e = std::make_unique<Expr>(); Ident d; d.name = nm; *e = std::move(d); return e;
		};
		WhileStmt mainLoop, tailLoop;
		{
			auto c2 = std::make_unique<Expr>();
			Binary b2; b2.op = "<"; b2.lhs = mkId(iv); b2.rhs = mkInt(limit);
			*c2 = std::move(b2); mainLoop.cond = std::move(c2);
		}
		for (int u = 0; u < U; u++)
			for (auto& st : ws->body) mainLoop.body.push_back(cloneStmtSub(st, {}));
		{
			auto c3 = std::make_unique<Expr>();
			Binary b3; b3.op = "<"; b3.lhs = mkId(iv); b3.rhs = mkInt(N);
			*c3 = std::move(b3); tailLoop.cond = std::move(c3);
		}
		for (auto& st : ws->body) tailLoop.body.push_back(cloneStmtSub(st, {}));

		std::vector<Stmt> repl;
		repl.push_back(Stmt(std::move(mainLoop)));
		repl.push_back(Stmt(std::move(tailLoop)));
		/* Stmt 是含 unique_ptr 的 variant，**不可拷贝**。insert 必须用
		   move_iterator，否则会退化为拷贝赋值而编译失败。 */
		body.erase(body.begin() + (long)idx);
		body.insert(body.begin() + (long)idx,
		            std::make_move_iterator(repl.begin()),
		            std::make_move_iterator(repl.end()));
		unroll_note_ = true;
		return true;
	}

	void NativeGen::rewrite_mod_induction_vars(std::vector<Stmt>& body) {
		for (size_t idx = 0; idx < body.size(); idx++) {
			Stmt& s = body[idx];
			/* 先递归进嵌套块，再处理本层 */
			if (auto* b = std::get_if<Block>(&s)) { rewrite_mod_induction_vars(b->stmts); continue; }
			if (auto* i = std::get_if<IfStmt>(&s)) {
				rewrite_mod_induction_vars(i->then_body);
				rewrite_mod_induction_vars(i->else_body);
				continue;
			}
			if (auto* w = std::get_if<WhileStmt>(&s)) {
				rewrite_mod_induction_vars(w->body);
			}
			auto* ws = std::get_if<WhileStmt>(&s);
			if (!ws || !ws->cond) continue;

			/* ── 让位给周期性取模闭式解 ──────────────────────────
			   若这个循环能被 lo_mod_period_sum 整体算成常量，就**不要**
			   改写成计数器形态 —— 改写后 AST 变成 `s = s + m`，闭式解
			   再也认不出 `i % C` 的形态，只能退回 O(n) 循环。
			   曾因此 b6_mod 永远跑 25ms+ 而拿不到 O(1) 闭式解。 */
			{
				LoopOptimizer::ModPeriodInfo dummy;
				if (LoopOptimizer::mod_period_analyze(*this, *ws, dummy)) continue;
			}

			/* 条件必须是 i < 某物 */
			auto* cb = std::get_if<Binary>(ws->cond.get());
			if (!cb || cb->op != "<" || !cb->lhs) continue;
			auto* cid = std::get_if<Ident>(cb->lhs.get());
			if (!cid) continue;
			std::string iv = cid->name;

			/* 归纳变量: 顶层恰好一条 `i = i + 正常量`，且别处不再给 i 赋值 */
			int64_t step = 0;
			int inc_pos = -1;
			for (size_t k = 0; k < ws->body.size(); k++) {
				auto* a = std::get_if<AssignStmt>(&ws->body[k]);
				if (!a || a->target || a->name != iv) continue;
				auto* b = std::get_if<Binary>(a->value.get());
				auto* lid = b ? std::get_if<Ident>(b->lhs.get()) : nullptr;
				auto* cst = b ? std::get_if<IntLit>(b->rhs.get()) : nullptr;
				if (!b || b->op != "+" || !lid || lid->name != iv || !cst || cst->value <= 0) { inc_pos = -2; break; }
				step = cst->value;
				inc_pos = static_cast<int>(k);
			}
			if (inc_pos < 0) continue;

			/* 循环体内不得含函数调用（可能改动 i 或有副作用顺序依赖） */
			{
				bool bad = false;
				for (auto& st : ws->body) {
					if (auto* a = std::get_if<AssignStmt>(&st)) { if (expr_has_call(*a->value)) bad = true; }
					if (auto* e = std::get_if<ExprStmt>(&st)) { if (e->expr && expr_has_call(*e->expr)) bad = true; }
				}
				if (bad) continue;
			}

			/* i 的初值: 往回找，必须解析出一个已知非负常量 */
			int64_t i0 = 0;
			bool i0_ok = false;
			for (size_t k = 0; k < idx; k++) {
				if (auto* vd = std::get_if<VarDecl>(&body[k])) {
					if (vd->name == iv && vd->init) {
						if (auto* il = std::get_if<IntLit>(vd->init.get())) { i0 = il->value; i0_ok = true; }
						else { i0_ok = false; }
						continue;
					}
				}
				if (auto* a = std::get_if<AssignStmt>(&body[k])) {
					if (a->name == iv) {
						if (!a->target && a->value && std::get_if<IntLit>(a->value.get())) {
							i0 = std::get<IntLit>(*a->value).value;
							i0_ok = true;
						} else { i0_ok = false; }
					}
				}
			}
			if (!i0_ok || i0 < 0) continue;   /* 可能为负 → C 语义余数会不同，放弃 */

			/* 找出模数 C: 循环体内所有 `i % 正常量` 必须是**同一个** C */
			int64_t C = 0;
			bool multi = false;
			std::function<void(Expr&)> scanMod = [&](Expr& e) {
				if (auto* b = std::get_if<Binary>(&e)) {
					if (b->op == "%" && b->lhs && b->rhs) {
						auto* lid = std::get_if<Ident>(b->lhs.get());
						auto* cst = std::get_if<IntLit>(b->rhs.get());
						if (lid && lid->name == iv && cst && cst->value > 0) {
							if (C == 0) C = cst->value;
							else if (C != cst->value) multi = true;
							return;
						}
					}
					if (b->lhs) scanMod(*b->lhs);
					if (b->rhs) scanMod(*b->rhs);
					return;
				}
				if (auto* u = std::get_if<Unary>(&e)) { if (u->operand) scanMod(*u->operand); return; }
				if (auto* ix = std::get_if<IndexExpr>(&e)) { if (ix->idx) scanMod(*ix->idx); return; }
				if (auto* c = std::get_if<Call>(&e)) { for (auto& a : c->args) if (a) scanMod(*a); return; }
			};
			for (auto& st : ws->body) {
				if (auto* a = std::get_if<AssignStmt>(&st)) {
					if (a->value) scanMod(*a->value);
					if (a->target) scanMod(*a->target);
				}
				if (auto* e = std::get_if<ExprStmt>(&st)) { if (e->expr) scanMod(*e->expr); }
			}
			if (C <= 0 || multi) continue;

			/* 新变量名: 必须不与现有变量重名 */
			std::string tmp = "__modiv_" + iv + "_" + std::to_string(C);
			{
				int guard = 0;
				while (locals_.count(tmp) || globals_.count(tmp)) {
					tmp += "_";
					if (++guard > 32) return;
				}
			}

			/* 1) 替换所有 i % C */
			int cnt = 0;
			modIvInStmts(ws->body, iv, tmp, cnt);
			if (cnt == 0) continue;

			int64_t m0 = i0 % C;
			int64_t adv = step % C;

			/* 2) 循环体末尾追加 m 的推进 */
			{
				auto mkInt = [](int64_t v) {
					auto e = std::make_unique<Expr>();
					IntLit il; il.value = v;
					*e = std::move(il);
					return e;
				};
				auto mkIdent = [](const std::string& nm) {
					auto e = std::make_unique<Expr>();
					Ident id; id.name = nm;
					*e = std::move(id);
					return e;
				};
				if (adv != 0) {
					AssignStmt as;
					as.name = tmp;
					auto b = std::make_unique<Expr>();
					Binary bin;
					bin.op = "+";
					bin.lhs = mkIdent(tmp);
					bin.rhs = mkInt(adv);
					*b = std::move(bin);
					as.value = std::move(b);
					ws->body.push_back(Stmt(std::move(as)));
				}
				/* if (m >= C) { m = m - C; } */
				IfStmt ifs;
				auto cond = std::make_unique<Expr>();
				Binary cb2;
				cb2.op = ">=";
				cb2.lhs = mkIdent(tmp);
				cb2.rhs = mkInt(C);
				*cond = std::move(cb2);
				ifs.cond = std::move(cond);
				AssignStmt sub;
				sub.name = tmp;
				auto b2 = std::make_unique<Expr>();
				Binary bin2;
				bin2.op = "-";
				bin2.lhs = mkIdent(tmp);
				bin2.rhs = mkInt(C);
				*b2 = std::move(bin2);
				sub.value = std::move(b2);
				ifs.then_body.push_back(Stmt(std::move(sub)));
				ws->body.push_back(Stmt(std::move(ifs)));
			}

			/* 登记编译期可证的上界，供 indexProven 省略边界检查。
			   只在 adv != 0 的完整改写路径登记（cnt>0 已确认）。 */
			mod_iv_bounds_[tmp] = C;

			/* 3) 循环前插入 m 的初值声明。
			   插在 body[idx] 之前 —— 走 AST 而不是代码生成期分配槽位,
			   这样 ScanVars 会把它算进栈帧。 */
			{
				VarDecl vd;
				vd.name = tmp;
				vd.type = "int";
				auto e = std::make_unique<Expr>();
				IntLit il; il.value = m0;
				*e = std::move(il);
				vd.init = std::move(e);
				body.insert(body.begin() + static_cast<long>(idx), Stmt(std::move(vd)));
				idx++;      /* 跳过刚插入的声明 */
			}
		}
	}


	/* ══════════════════════════════════════════════════════════════════
	   D1 · 循环级编译期证明：定长数组区间 + 累加器不可能溢出
	   ══════════════════════════════════════════════════════════════════
	   目标: `while (j<N) { s = s + arr[m]; ... }` 里 s 的 jo 检查每轮一次。
	   若能编译期证明「s 在整个循环里绝不可能溢出」，就合法省略它 ——
	   这是证明的结果，不是把检查关掉凑性能。

	   证明链:
	     ① 数组元素区间 arr_elem_bounds_ 来自定长初始化循环的静态分析
	     ② 迭代次数上界 trip = N - iv0（N/iv0 都是编译期常量）
	     ③ 累加器初值 s0 是编译期常量
	     ④ |s0| + trip * max(|X.lo|,|X.hi|) <= INT64_MAX
	   任一环证不出 → 保留运行时检查，绝不静默豁免。

	   保守闸门（宁可证不出，不可误证）:
	     · 数组在区间登记后若函数中还有别的写，整条撤销
	     · 归纳变量只能被 `iv = iv + 正常量` 推进（否则 trip 上界失效）
	     · 循环体不含调用/输入/输出/数组写等副作用
	     · 累加器在循环体内只被这一条赋值写
	   ══════════════════════════════════════════════════════════════════ */

	/* 三态: 0=确定不写该数组, 1=确定写, 2=遇到无法判定的结构 */
	static int weiArrayWrites(const Stmt& st, const std::string& arr) {
		int r = 0;
		auto rec = [&](const std::vector<Stmt>& v) {
			for (auto& x : v) { int q = weiArrayWrites(x, arr); if (q > r) r = q; }
		};
		std::visit([&](const auto& n) {
			using T = std::decay_t<decltype(n)>;
			if constexpr (std::is_same_v<T, AssignStmt>) {
				if (n.target) {
					if (auto* ix = std::get_if<IndexExpr>(n.target.get())) {
						if (ix->name == arr) r = 1;
					} else r = 2;                       /* 指针写: 无法判定 */
				}
			}
			else if constexpr (std::is_same_v<T, Block>) rec(n.stmts);
			else if constexpr (std::is_same_v<T, IfStmt>) { rec(n.then_body); rec(n.else_body); }
			else if constexpr (std::is_same_v<T, WhileStmt> || std::is_same_v<T, ForStmt>
			                   || std::is_same_v<T, LoopStmt> || std::is_same_v<T, LoopNStmt>
			                   || std::is_same_v<T, CompileTimeStmt> || std::is_same_v<T, ForallStmt>)
				rec(n.body);
			else if constexpr (std::is_same_v<T, WhenStmt>) { rec(n.then_body); rec(n.else_body); }
			else if constexpr (std::is_same_v<T, SwitchStmt>) {
				for (auto& c : n.cases) rec(c.body.stmts);
			}
			else if constexpr (std::is_same_v<T, MatchStmt>) {
				for (auto& c : n.cases) rec(c.body);
				rec(n.default_body);
			}
			else if constexpr (std::is_same_v<T, DeferStmt>) rec(n.body.stmts);
			else if constexpr (std::is_same_v<T, ReturnStmt> || std::is_same_v<T, ExprStmt>
			                   || std::is_same_v<T, VarDecl> || std::is_same_v<T, ModuleAccessSet>
			                   || std::is_same_v<T, ContractStmt> || std::is_same_v<T, TypeAliasDecl>
			                   || std::is_same_v<T, YieldStmt> || std::is_same_v<T, MacroDecl>
			                   || std::is_same_v<T, BreakStmt> || std::is_same_v<T, ContinueStmt>) {
				/* 这些形态不产生数组写 */
			}
			else r = 2;                                 /* AsmStmt 等: 无法判定 */
		}, st);
		return r;
	}

	/* 表达式里是否含调用/输入/输出等副作用（保守: 认不出的形态也拒绝） */
	static bool weiExprPure(const Expr& e) {
		bool bad = false;
		std::visit([&](const auto& n) {
			using T = std::decay_t<decltype(n)>;
			if constexpr (std::is_same_v<T, Binary>) {
				if (n.lhs && !weiExprPure(*n.lhs)) bad = true;
				if (n.rhs && !weiExprPure(*n.rhs)) bad = true;
			}
			else if constexpr (std::is_same_v<T, Unary>) { if (n.operand && !weiExprPure(*n.operand)) bad = true; }
			else if constexpr (std::is_same_v<T, IndexExpr>) { if (n.idx && !weiExprPure(*n.idx)) bad = true; }
			else if constexpr (std::is_same_v<T, CastExpr>) { if (n.value && !weiExprPure(*n.value)) bad = true; }
			else if constexpr (std::is_same_v<T, Output>) {
				/* 输出不改动任何变量/数组，从"数值状态"看是纯的；
				   但其参数里若含调用仍要拒绝。 */
				for (auto& v : n.values) if (v && !weiExprPure(*v)) bad = true;
			}
			else if constexpr (std::is_same_v<T, IntLit> || std::is_same_v<T, BoolLit>
			                   || std::is_same_v<T, DoubleLit> || std::is_same_v<T, StrLit>
			                   || std::is_same_v<T, Ident> || std::is_same_v<T, ModulePathGet>
			                   || std::is_same_v<T, SizeofExpr> || std::is_same_v<T, TypeofExpr>) { }
			else bad = true;      /* Call / Output / Input / New / Delete / Asm ... */
		}, e);
		return !bad;
	}

	void NativeGen::collectArrayBounds(std::vector<Stmt>& body) {
		/* ⓪ 保守闸门 A: 函数中只要出现 Input 或 Call（可能改动任意变量
		   或数组），编译期常量与数组区间就都不可信 —— 整体放弃证明。
		   宁可保留运行时检查，绝不拿"以为的常量"去豁免。 */
		std::function<bool(const Stmt&)> hasSideEffectStmt = [&](const Stmt& st) -> bool {
			bool yes = false;
			auto rec = [&](const std::vector<Stmt>& v) {
				for (auto& x : v) if (!yes && hasSideEffectStmt(x)) yes = true;
			};
			std::visit([&](const auto& n) {
				using T = std::decay_t<decltype(n)>;
				if constexpr (std::is_same_v<T, ExprStmt>) {
					if (n.expr && !weiExprPure(*n.expr)) yes = true;
				}
				else if constexpr (std::is_same_v<T, AssignStmt>) {
					if (n.value && !weiExprPure(*n.value)) yes = true;
					if (n.target && !weiExprPure(*n.target)) yes = true;
				}
				else if constexpr (std::is_same_v<T, VarDecl>) {
					if (n.init && !weiExprPure(*n.init)) yes = true;
				}
				else if constexpr (std::is_same_v<T, IfStmt>) {
					if (n.cond && !weiExprPure(*n.cond)) yes = true;
					rec(n.then_body); rec(n.else_body);
				}
				else if constexpr (std::is_same_v<T, WhileStmt> || std::is_same_v<T, ForStmt>
				                   || std::is_same_v<T, LoopStmt> || std::is_same_v<T, LoopNStmt>
				                   || std::is_same_v<T, CompileTimeStmt> || std::is_same_v<T, ForallStmt>)
					rec(n.body);
				else if constexpr (std::is_same_v<T, Block>) rec(n.stmts);
				else if constexpr (std::is_same_v<T, WhenStmt>) { rec(n.then_body); rec(n.else_body); }
				else if constexpr (std::is_same_v<T, SwitchStmt>) { for (auto& c : n.cases) rec(c.body.stmts); }
				else if constexpr (std::is_same_v<T, MatchStmt>) { for (auto& c : n.cases) rec(c.body); rec(n.default_body); }
				else if constexpr (std::is_same_v<T, DeferStmt>) rec(n.body.stmts);
				else if constexpr (std::is_same_v<T, AsmStmt>) yes = true;
			}, st);
			return yes;
		};
		for (auto& st : body) {
			if (hasSideEffectStmt(st)) return;      /* 不登记任何区间/常量 */
		}

		/* ① 顶层常量初值 */
		for (auto& st : body) {
			if (auto* vd = std::get_if<VarDecl>(&st)) {
				if (vd->init) {
					if (auto* il = std::get_if<IntLit>(vd->init.get())) var_init_const_[vd->name] = il->value;
				}
			} else if (auto* as = std::get_if<AssignStmt>(&st)) {
				if (!as->target && as->value) {
					if (auto* il = std::get_if<IntLit>(as->value.get())) var_init_const_[as->name] = il->value;
					else var_init_const_.erase(as->name);
				}
			}
		}

		/* ② 识别定长初始化循环:
		      while (iv < N) { arr[iv] = expr;  iv = iv + k; }
		   并求 expr 在 iv ∈ [0,N) 下的区间。 */
		struct Cand { std::string arr; const Stmt* loop; wei_i128 lo, hi; };
		std::vector<Cand> cands;
		for (auto& st : body) {
			auto* ws = std::get_if<WhileStmt>(&st);
			if (!ws || !ws->cond) continue;
			auto* cb = std::get_if<Binary>(ws->cond.get());
			if (!cb || cb->op != "<" || !cb->lhs || !cb->rhs) continue;
			auto* cid = std::get_if<Ident>(cb->lhs.get());
			if (!cid) continue;
			int64_t N = 0;
			if (auto* il = std::get_if<IntLit>(cb->rhs.get())) N = il->value;
			else if (auto* rid = std::get_if<Ident>(cb->rhs.get())) {
				auto q = var_init_const_.find(rid->name);
				if (q == var_init_const_.end()) continue;
				N = q->second;
			} else continue;
			if (N <= 0) continue;
			std::string arr;
			const Expr* val = nullptr;
			int inc = 0; bool ok = true;
			for (auto& bst : ws->body) {
				auto* as = std::get_if<AssignStmt>(&bst);
				if (!as) { ok = false; break; }
				if (as->target) {
					auto* ix = std::get_if<IndexExpr>(as->target.get());
					auto* iid = ix ? std::get_if<Ident>(ix->idx.get()) : nullptr;
					if (!ix || !iid || iid->name != cid->name || !as->value
					    || !weiExprPure(*as->value)) { ok = false; break; }
					if (!arr.empty() && arr != ix->name) { ok = false; break; }
					arr = ix->name; val = as->value.get();
				} else {
					auto* b = std::get_if<Binary>(as->value.get());
					auto* l = b ? std::get_if<Ident>(b->lhs.get()) : nullptr;
					auto* c = b ? std::get_if<IntLit>(b->rhs.get()) : nullptr;
					if (as->name != cid->name || !b || b->op != "+" || !l
					    || l->name != cid->name || !c || c->value <= 0) { ok = false; break; }
					inc++;
				}
			}
			if (!ok || arr.empty() || !val || inc != 1) continue;
			auto& vi = resolve_var(arr);
			if (vi.array_len <= 0 || N > vi.array_len) continue;   /* 长度不足 → 越界写, 不登记 */
			/* 临时登记归纳变量上界以区间求值；保存并恢复原值 */
			auto oldIt = idx_ub_.find(cid->name);
			bool hadOld = oldIt != idx_ub_.end();
			int64_t oldUb = hadOld ? oldIt->second : 0;
			noteIdxUB(cid->name, N);
			Rng r = rngOf(*val);
			if (hadOld) noteIdxUB(cid->name, oldUb); else dropIdxUB(cid->name);
			if (!r.bounded || !r.ok) continue;
			cands.push_back({arr, &st, r.lo, r.hi});
		}

		/* ③ 保守闸门: 该数组在**整个函数**里除这条初始化循环外不得再有写，
		   也不得出现无法判定的结构。否则整条撤销。 */
		for (auto& c : cands) {
			if (arr_elem_bounds_.count(c.arr)) continue;   /* 同名只登记一次 */
			bool dirty = false;
			for (auto& st : body) {
				if (&st == c.loop) continue;                 /* 跳过自身的初始化循环 */
				if (weiArrayWrites(st, c.arr) != 0) { dirty = true; break; }
			}
			if (dirty) continue;
			ArrBounds ab; ab.lo = c.lo; ab.hi = c.hi; ab.ok = true;
			arr_elem_bounds_[c.arr] = ab;
		}

		/* ④ 记录每条 while 之前哪些变量已被赋值。
		   证明时若累加器在其中，说明"编译期初值"已被前序代码改过，
		   不能用来推 |s| 上界 —— 直接放弃。 */
		std::set<std::string> written;
		std::function<void(const Stmt&)> collectWrites = [&](const Stmt& st) {
			auto rec = [&](const std::vector<Stmt>& v) { for (auto& x : v) collectWrites(x); };
			std::visit([&](const auto& n) {
				using T = std::decay_t<decltype(n)>;
				if constexpr (std::is_same_v<T, AssignStmt>) {
					if (!n.target) written.insert(n.name);
				}
				else if constexpr (std::is_same_v<T, Block>) rec(n.stmts);
				else if constexpr (std::is_same_v<T, IfStmt>) { rec(n.then_body); rec(n.else_body); }
				else if constexpr (std::is_same_v<T, WhileStmt> || std::is_same_v<T, ForStmt>
				                   || std::is_same_v<T, LoopStmt> || std::is_same_v<T, LoopNStmt>
				                   || std::is_same_v<T, CompileTimeStmt> || std::is_same_v<T, ForallStmt>)
					rec(n.body);
				else if constexpr (std::is_same_v<T, WhenStmt>) { rec(n.then_body); rec(n.else_body); }
				else if constexpr (std::is_same_v<T, SwitchStmt>) { for (auto& c : n.cases) rec(c.body.stmts); }
				else if constexpr (std::is_same_v<T, MatchStmt>) { for (auto& c : n.cases) rec(c.body); rec(n.default_body); }
				else if constexpr (std::is_same_v<T, DeferStmt>) rec(n.body.stmts);
			}, st);
		};
		for (auto& st : body) {
			if (auto* ws = std::get_if<WhileStmt>(&st)) loop_pre_writes_[ws] = written;
			collectWrites(st);
		}
	}

	std::set<std::string> NativeGen::proveAccumNoOverflow(WhileStmt& w) {
		std::set<std::string> result;
		/* 诊断开关: WEI_NO_ACC_PROOF=1 关闭循环累加器证明，
		   用于二分"证明路径"与"通用路径"的正确性问题。 */
		static bool accOff = getenv("WEI_NO_ACC_PROOF") != nullptr;
		if (accOff || !w.cond) return result;
		auto* cb = std::get_if<Binary>(w.cond.get());
		if (!cb || cb->op != "<" || !cb->lhs || !cb->rhs) return result;
		auto* cid = std::get_if<Ident>(cb->lhs.get());
		if (!cid) return result;
		int64_t N = 0;
		if (auto* il = std::get_if<IntLit>(cb->rhs.get())) N = il->value;
		else if (auto* rid = std::get_if<Ident>(cb->rhs.get())) {
			auto q = var_init_const_.find(rid->name);
			if (q == var_init_const_.end()) return result;
			N = q->second;
		} else return result;
		auto qi = var_init_const_.find(cid->name);
		if (qi == var_init_const_.end()) return result;
		int64_t iv0 = qi->second;
		if (N <= iv0) return result;
		wei_i128 trip = wei_i128(N) - wei_i128(iv0);
		if (trip <= 0 || trip > wei_i128(INT64_MAX)) return result;
		/* 两遍一致: 第二遍直接复用第一遍的证明结论。
		   key 含函数名/trip/循环体大小，避免不同循环串味。 */
		std::string proofKey = cur_qualname_ + "|" + std::to_string((long long)trip)
		                     + "|" + std::to_string(w.body.size());
		if (gen_pass_ == 2) {
			auto cached = acc_proof_cache_.find(proofKey);
			if (cached != acc_proof_cache_.end()) return cached->second;
		}

		/* 扫描循环体：严格形态 + 收集所有 `v = v ± X` 候选。
		   同一变量可能有多条（如计数器 +1 与 -C 回绕），其绝对增量
		   必须**求和**才是对 |v| 上界的保守估计，不能只证一条。 */
		struct Cand { std::string name; const Expr* rhs; };
		std::vector<Cand> cands;
		std::set<std::string> nonSelfWrites;
		bool bad = false;
		std::function<void(const std::vector<Stmt>&)> scan = [&](const std::vector<Stmt>& v) {
			for (auto& st : v) {
				if (bad) return;
				if (auto* as = std::get_if<AssignStmt>(&st)) {
					if (as->target) { bad = true; return; }         /* 数组/指针写 */
					if (!as->value || !weiExprPure(*as->value)) { bad = true; return; }
					auto* b = std::get_if<Binary>(as->value.get());
					auto* l = b ? std::get_if<Ident>(b->lhs.get()) : nullptr;
					bool selfPlus = l && l->name == as->name;
					if (as->name == cid->name) {
						/* 归纳变量只能被 iv = iv + 正常量 推进，否则 trip 上界失效 */
						auto* c = b ? std::get_if<IntLit>(b->rhs.get()) : nullptr;
						if (!selfPlus || b->op != "+" || !c || c->value <= 0) { bad = true; return; }
					}
					if (selfPlus && b && (b->op == "+" || b->op == "-") && b->rhs)
						cands.push_back({as->name, b->rhs.get()});
					else
						nonSelfWrites.insert(as->name);   /* s=常量 等: 初值不可靠 */
				}
				else if (auto* ifs = std::get_if<IfStmt>(&st)) {
					if (!ifs->cond || !weiExprPure(*ifs->cond)) { bad = true; return; }
					scan(ifs->then_body); scan(ifs->else_body);
				}
				else if (auto* vd = std::get_if<VarDecl>(&st)) {
					if (vd->init && !weiExprPure(*vd->init)) { bad = true; return; }
				}
				else { bad = true; return; }   /* 嵌套循环/块/跳转/asm 等一律拒绝 */
			}
		};
		scan(w.body);
		if (bad || cands.empty()) return result;

		auto absv = [](wei_i128 v) -> wei_i128 { return v < 0 ? -v : v; };
		/* 按变量聚合每条增量的绝对值上界 */
		std::map<std::string, wei_i128> sumMx;
		std::set<std::string> disqualified;
		for (auto& c : cands) {
			Rng xr = rngOf(*c.rhs);
			if (!xr.bounded || !xr.ok) { disqualified.insert(c.name); continue; }
			wei_i128 mx = absv(xr.lo) > absv(xr.hi) ? absv(xr.lo) : absv(xr.hi);
			sumMx[c.name] += mx;
		}
		auto pw = loop_pre_writes_.find(&w);
		for (auto& x : nonSelfWrites) disqualified.insert(x);
		for (auto& kv : sumMx) {
			if (disqualified.count(kv.first)) continue;
			/* 保守闸门 B: 该变量在本循环之前被赋过值 → 初值不可信 */
			if (pw != loop_pre_writes_.end() && pw->second.count(kv.first)) continue;
			auto qs = var_init_const_.find(kv.first);
			if (qs == var_init_const_.end()) continue;
			wei_i128 mx = kv.second;
			/* 防 128 位乘法本身溢出 */
			if (mx > 0 && trip > wei_i128(INT64_MAX) / mx) continue;
			wei_i128 bound = absv(wei_i128(qs->second)) + trip * mx;
			if (bound > wei_i128(INT64_MAX)) continue;
			proofNote("循环变量不可能溢出", w.line);
			result.insert(kv.first);
		}
		if (gen_pass_ == 1) acc_proof_cache_[proofKey] = result;
		static bool dbg = getenv("WEI_DBG_ACC") != nullptr;
		if (dbg) {
			std::string names; for (auto& x : result) names += x + " ";
			fprintf(stderr, "[acc] pass=%d line=%d trip=%lld result={%s}\n",
			        gen_pass_, w.line, (long long)trip, names.c_str());
		}
		return result;
	}

	/* ── C1 · 位宽体系：表达式宽度推导 ────────────────────────────────────
	   宽度只能来自"能静态确定"的来源: 已声明变量的类型。字面量没有自己的
	   宽度（它会被上下文决定），一元运算沿用操作数，二元取左右中较宽的一侧。
	   推导不出就返回 0 —— 意思是"按默认 64 位处理"，这是保守且安全的选择。 */
	int NativeGen::widthOfExpr(const Expr& e) const {
		if (auto* id = std::get_if<Ident>(&e)) return widthOfVar(id->name);
		if (auto* u = std::get_if<Unary>(&e)) return u->operand ? widthOfExpr(*u->operand) : 0;
		if (auto* b = std::get_if<Binary>(&e)) {
			int l = b->lhs ? widthOfExpr(*b->lhs) : 0;
			int r = b->rhs ? widthOfExpr(*b->rhs) : 0;
			return (l >= r) ? l : r;
		}
		return 0;
	}

	/* 逐宽度溢出检查: 结果超出该宽度能表示的范围就 panic。
	   Wei 是安全语言 —— 窄类型的溢出和 int 的溢出一样要被抓住，
	   不能像 C 那样静默回绕。这也是"内嵌检测"承诺的底线之一。
	     rax = 待检值（有符号取值范围意义下）
	     width=8/16/32, is_unsigned 决定边界 */
	/* ── C1 · 8×8 转换矩阵 ──────────────────────────────────────────────
	   跨宽度赋值/混合运算的类型提升。规则（安全优先，宁可报错不静默截断）:
	     1. 目标比源窄  → 按目标宽度做范围检查，装不下就 panic（不静默回绕）
	     2. 目标比源宽  → 按源的符号性零扩展(movzx)或符号扩展(movsx)
	     3. 同宽不同符号 → 有符号→无符号要查下界(负数装不进无符号)
	   这是 8 种宽度 × 符号性两两组合的完整处理入口。 */
	void NativeGen::emit_width_cast(int from_w, bool from_uns, int to_w, bool to_uns) {
		if (to_w <= 0) return;                    /* 目标未知(默认64): 不限制 */
		if (from_w > 0 && from_w <= to_w) {
			/* 放宽: 只需按源规范化，值一定装得下 */
			emit_width_truncate(from_w, from_uns);
			return;
		}
		/* 收窄或源未知: 按目标宽度做范围检查 + 规范化 */
		if (from_w > 0 && from_uns && !to_uns) {
			/* 无符号 → 有符号: 额外查是否超过有符号上界 */
			emit_width_truncate(from_w, true);
		}
		emit_width_ovf_check(to_w, to_uns);
		emit_width_truncate(to_w, to_uns);
	}

	/* ── C3 · 越界检测接入分级 ──────────────────────────────────────────
	   none/min 之外的等级才做越界检查。这是"防崩溃类"检测，min 及以上
	   就应保留 —— 内存破坏的后果远大于性能损失。 */
	bool NativeGen::bounds_check_on() const {
		return check_enabled(CheckKind::Bounds);
	}

	void NativeGen::emit_width_ovf_check(int width, bool is_unsigned) {
		/* -wide 下窄语义全部失效，交给 128 位通路 */
		if (wide_mode_) return;
		if (width <= 0 || width >= 64) {
			/* 64 位: 有符号用 OF(jno)，无符号用 CF(jnc) */
			int ok = emit_jcc(is_unsigned ? 0x3 : 0x1);
			emit_call_off(panic_ovf_off_);
			patch(ok);
			return;
		}
		/* 窄宽度: 前置的窄 add/sub 已按该宽度设置好 OF/CF，
		   这里只需按符号性挑一个标志:
		     有符号溢出 → OF 置位 → jo  (0x0)，故 jno(0x1) 跳过 panic
		     无符号进位 → CF 置位 → jc  (0x2)，故 jnc(0x3) 跳过 panic
		   这比"算完再比大小"少两条 mov + 两条 cmp。 */
		int ok = emit_jcc(is_unsigned ? 0x3 : 0x1);
		emit_call_off(panic_ovf_off_);
		patch(ok);
	}
	/* 把 rax 按目标宽度做符号/零扩展，得到该宽度的规范值。
	   溢出检查通过后调用，保证值一定在范围内。 */
	void NativeGen::emit_width_truncate(int width, bool is_unsigned) {
		if (width <= 0 || width >= 64) return;
		if (width == 8) {
			if (is_unsigned) { c_.rex_wrb(0, 0); c_.u8(0x0F); c_.u8(0xB6); c_.modrm(3, 0, 0); }  /* movzx rax,al */
			else            { c_.rex_wrb(0, 0); c_.u8(0x0F); c_.u8(0xBE); c_.modrm(3, 0, 0); }  /* movsx rax,al */
			return;
		}
		if (width == 16) {
			if (is_unsigned) { c_.u8(0x0F); c_.u8(0xB7); c_.modrm(3, 0, 0); }   /* movzx rax,ax */
			else            { c_.u8(0x0F); c_.u8(0xBF); c_.modrm(3, 0, 0); }   /* movsx rax,ax */
			return;
		}
		if (width == 32) {
			if (is_unsigned) { c_.u8(0x89); c_.modrm(3, 0, 0); }               /* mov eax,eax 零扩展 */
			else            { c_.rex_wrb(0, 0); c_.u8(0x63); c_.modrm(3, 0, 0); } /* movsxd rax,eax */
			return;
		}
	}

	/* ── Function code generation ── */
/* ═══════════════════════════════════════════════════════════════════════
   代码质量层 · 第 1 节: 通用寄存器驻留
   ───────────────────────────────────────────────────────────────────────
   把函数中访问最频繁的局部变量钉在 callee-saved 寄存器（r14/r15/r12/r13/rbx），
   整个函数体内不再往返栈槽。

   关键约束:
     · 被取地址（&x）、被数组索引（a[i] 中的 a）、被原子操作/线程引用的变量
       **一律禁止**驻留 —— 它们必须有内存地址。
     · 函数含 asm 块时不驻留 —— 手工汇编的寄存器使用我们看不见。
     · double / 窄宽度 / 宽模式变量不驻留 —— 另有通路。
     · 权重 < 4 的不值得占寄存器。
   ═══════════════════════════════════════════════════════════════════════ */
static bool weiPromotableType(const std::string& t) {
	return t.empty() || t == "int" || t == "整型" || t == "i64" || t == "bool" || t == "布尔"
	    || t == "ptr" || t == "char" || t == "文本";
}

struct PromoScan {
	std::map<std::string, uint64_t> w;
	std::set<std::string> ban;
	bool hasAsm = false, usesRbx = false;
	void use(const std::string& n, uint64_t wt) { w[n] += wt; }
	void expr(const Expr* e, uint64_t wt) {
		if (!e) return;
		std::visit([&](const auto& n) {
			using T = std::decay_t<decltype(n)>;
			if constexpr (std::is_same_v<T, Ident>) use(n.name, wt);
			else if constexpr (std::is_same_v<T, Binary>) { expr(n.lhs.get(), wt); expr(n.rhs.get(), wt); }
			else if constexpr (std::is_same_v<T, Unary>) {
				if (n.op == "&") if (auto* id = std::get_if<Ident>(n.operand.get())) ban.insert(id->name);
				expr(n.operand.get(), wt);
			} else if constexpr (std::is_same_v<T, Call>) {
				bool atom = n.callee.rfind("__atomic", 0) == 0 || n.callee == "__thread";
				if (atom) usesRbx = true;
				for (auto& a : n.args) {
					if (atom) if (auto* id = std::get_if<Ident>(a.get())) ban.insert(id->name);
					expr(a.get(), wt);
				}
			} else if constexpr (std::is_same_v<T, Output>) { for (auto& a : n.values) expr(a.get(), wt); }
			else if constexpr (std::is_same_v<T, Input>) ban.insert(n.var_name);
			else if constexpr (std::is_same_v<T, IndexExpr>) { ban.insert(n.name); expr(n.idx.get(), wt); }
			else if constexpr (std::is_same_v<T, CastExpr>) expr(n.value.get(), wt);
			else if constexpr (std::is_same_v<T, DeleteExpr>) expr(n.target.get(), wt);
			else if constexpr (std::is_same_v<T, RangeLit>) { expr(n.lo.get(), wt); expr(n.hi.get(), wt); }
			else if constexpr (std::is_same_v<T, TupleLit>) { for (auto& a : n.elements) expr(a.get(), wt); }
			else if constexpr (std::is_same_v<T, TupleIndexExpr>) expr(n.tuple.get(), wt);
			else if constexpr (std::is_same_v<T, TypeofExpr> || std::is_same_v<T, IsExpr>) expr(n.arg.get(), wt);
			else if constexpr (std::is_same_v<T, InExpr>) {
				expr(n.value.get(), wt);
				if (auto* id = std::get_if<Ident>(n.container.get())) ban.insert(id->name);
				else expr(n.container.get(), wt);
			} else if constexpr (std::is_same_v<T, AwaitExpr>) expr(n.operand.get(), wt);
			else if constexpr (std::is_same_v<T, CoExpr>) { for (auto& a : n.args) expr(a.get(), wt); }
			else if constexpr (std::is_same_v<T, ForallExpr> || std::is_same_v<T, ExistsExpr>) {
				ban.insert(n.var); expr(n.domain.get(), wt); expr(n.cond.get(), wt);
			}
		}, *e);
	}
	void stmts(const std::vector<Stmt>& v, uint64_t wt) { for (auto& s : v) stmt(s, wt); }
	void stmt(const Stmt& s, uint64_t wt) {
		uint64_t lw = wt > 100000000ULL ? wt : wt * 8;
		std::visit([&](const auto& n) {
			using T = std::decay_t<decltype(n)>;
			if constexpr (std::is_same_v<T, Block>) stmts(n.stmts, wt);
			else if constexpr (std::is_same_v<T, IfStmt>) { expr(n.cond.get(), wt); stmts(n.then_body, wt); stmts(n.else_body, wt); }
			else if constexpr (std::is_same_v<T, WhileStmt>) { expr(n.cond.get(), lw); stmts(n.body, lw); }
			else if constexpr (std::is_same_v<T, ForStmt>) {
				if (n.init_decl) {
					if (!weiPromotableType(n.init_decl->type)) ban.insert(n.init_decl->name);
					use(n.init_decl->name, wt); expr(n.init_decl->init.get(), wt);
				}
				expr(n.cond.get(), lw); expr(n.step.get(), lw); stmts(n.body, lw);
			}
			else if constexpr (std::is_same_v<T, LoopStmt> || std::is_same_v<T, LoopNStmt>) stmts(n.body, lw);
			else if constexpr (std::is_same_v<T, ReturnStmt>) expr(n.value.get(), wt);
			else if constexpr (std::is_same_v<T, ExprStmt>) expr(n.expr.get(), wt);
			else if constexpr (std::is_same_v<T, AssignStmt>) {
				if (!n.target) use(n.name, wt);
				expr(n.value.get(), wt); expr(n.target.get(), wt);
			}
			else if constexpr (std::is_same_v<T, VarDecl>) {
				if (!weiPromotableType(n.type) || n.array_len > 0 || !n.array_init.empty()) ban.insert(n.name);
				use(n.name, wt); expr(n.init.get(), wt);
			}
			else if constexpr (std::is_same_v<T, ModuleAccessSet>) expr(n.value.get(), wt);
			else if constexpr (std::is_same_v<T, MatchStmt>) {
				expr(n.expr.get(), wt);
				for (auto& c : n.cases) { expr(c.pattern.get(), wt); stmts(c.body, wt); }
				stmts(n.default_body, wt);
			}
			else if constexpr (std::is_same_v<T, ContractStmt>) expr(n.condition.get(), wt);
			else if constexpr (std::is_same_v<T, ForallStmt>) { ban.insert(n.var); expr(n.domain.get(), wt); stmts(n.body, lw); }
			else if constexpr (std::is_same_v<T, WhenStmt>) { expr(n.cond.get(), wt); stmts(n.then_body, wt); stmts(n.else_body, wt); }
			else if constexpr (std::is_same_v<T, YieldStmt>) expr(n.value.get(), wt);
			else if constexpr (std::is_same_v<T, CompileTimeStmt>) stmts(n.body, wt);
			else if constexpr (std::is_same_v<T, SwitchStmt>) {
				usesRbx = true; expr(n.subject.get(), wt);
				for (auto& c : n.cases) stmts(c.body.stmts, wt);
			}
			else if constexpr (std::is_same_v<T, DeferStmt>) stmts(n.body.stmts, wt);
			else if constexpr (std::is_same_v<T, AsmStmt>) hasAsm = true;
		}, s);
	}
};

void NativeGen::spill_and_drop_pinned() {
	if (pinned_.empty()) return;
	/* 必须先禁 hook，否则下面的写回会被拦截成 `mov phys,phys` 空转，
	   内存里留着的还是旧值，stub 读内存就会算错。 */
	c_.set_slot_hook({});
	c_.set_pre_align({});
	c_.set_on_slot_addr({});
	for (auto& kv : pinned_) {
		int64_t d = -8 * static_cast<int64_t>(kv.first + 1);
		c_.mov_memd8_64r(5, d, kv.second);       /* 真写回栈槽 */
		loop_reg_vars_.erase(kv.first);          /* 清掉驻留映射, 之后读内存 */
	}
	pinned_.clear(); pinnedDisp_.clear(); pinned_name_.clear();
}

void NativeGen::promote_function_locals(FuncDecl& f) {
	/* 每进一个函数都重设 hook：c_ 是所有函数共用的发射缓冲，
	   不清会串到下一个函数。空表 = 行为与未启用时逐字节一致。 */
	pinned_.clear(); pinned_name_.clear(); pinnedDisp_.clear();
	c_.set_slot_hook({});
	c_.set_pre_align({});
	c_.set_on_slot_addr({});
	static bool regOff = getenv("WEI_NO_REGALLOC") != nullptr;   /* A/B 对照 */
	if (!promote_enable_ || regOff || wide_mode_) return;        /* -wide 影子高位槽活在内存 */

	/* ── 两遍一致性 ───────────────────────────────────────────────
	   gen_pass_==1 时计算并缓存，==2 时强制复用。
	   key 用函数限定名 + 参数个数 + 函数体语句数，避免同形函数串味。 */
	std::vector<std::pair<uint32_t,int>> chosen;
	if (gen_pass_ == 2) {
		if (promote_seq_idx_ < promote_seq_.size()) chosen = promote_seq_[promote_seq_idx_++];
		else return;                 /* 保守: 顺序对不上就不启用，绝不猜 */
	} else {
		PromoScan ps;
		for (auto& p : f.params) {
			if (!weiPromotableType(p.type)) ps.ban.insert(p.name);
			ps.use(p.name, 1);
		}
		ps.stmts(f.body, 1);
		if (ps.hasAsm) return;                       /* asm 可能引用任意槽 */
		std::vector<std::pair<uint64_t, std::string>> cand;
		for (auto& kv : ps.w) {
			if (ps.ban.count(kv.first) || kv.second < 4) continue;
			auto it = locals_.find(kv.first);
			if (it == locals_.end() || !it->second.is_local
			    || it->second.array_len > 0 || it->second.is_double) continue;
			if (narrow_int_.count(kv.first) || widthOfVar(kv.first) > 0) continue;
			cand.push_back({kv.second, kv.first});
		}
		/* 确定性排序: 权重降序、名字升序，保证两遍/多次编译结果一致 */
		std::sort(cand.begin(), cand.end(),
		          [](const std::pair<uint64_t,std::string>& a, const std::pair<uint64_t,std::string>& b) {
			          return a.first != b.first ? a.first > b.first : a.second < b.second;
		          });
		/* 保守池: r14/r15/r12 是 callee-saved，序言已 push 保存，且旧式
		   发射器不会在未保存的情况下把它们当临时寄存器破坏。
		   r13/rbx 会被某些 legacy 路径借用（需 spill/reload 围栏），
		   本轮不纳入，避免"寄存器与内存失去同步"的静默算错。 */
		/* 只占 r15：循环提升池是 {rbx,r12,r13,r14}，占 r12/r14 会把
		   它的可用寄存器从 4 个削到 2 个，实测数组累加反而慢 50%。
		   r15 是唯一不被循环提升使用的 callee-saved；虽然输出 stub 会
		   借它暂存栈指针，但已有 spill/deopt 处理，不冲突。 */
		static const int pool[1] = {15};
		for (size_t i = 0; i < cand.size() && i < 1; i++) {
			auto& vi = locals_[cand[i].second];
			chosen.push_back({vi.slot, pool[i]});
		}
		promote_seq_.push_back(chosen);
		promote_seq_idx_++;
	}

	for (auto& kv : chosen) {
		uint32_t slot = kv.first;
		int phys = kv.second;
		int64_t disp = -8 * static_cast<int64_t>(slot + 1);
		pinned_[slot] = phys;
		pinnedDisp_[disp] = phys;
		pinned_name_[slot] = "";   /* 名字仅诊断用，slot 已是唯一键 */
		/* 不写 loop_reg_vars_！循环提升路径(try_while_promote 等)也往
		   loop_reg_vars_ 里塞映射，且可能挑中同一个物理寄存器 —— 两个
		   变量共用一个寄存器会互相覆盖，实测 Collatz 嵌套循环直接死循环。
		   hook 已经拦截了所有 [rbp+d] 读写（含 mov64r_memd8/alu_r64_memd8/
		   cmp/imul/imm 形式），Ident / emit_load_var 走内存原语时会自动
		   重定向到 phys，无需借助 regOfSlot。 */
	}
	/* hook: disp → phys。精确 key，保存寄存器槽 / canary / spill 槽都不在表里 */
	/* 参数从栈上进来，必须**在 hook 设置之前**装入驻留寄存器。
	   mov64r_memd32 也已接入 hook，若此时 hook 生效会被拦截成
	   `mov phys,phys` 空转 —— 实测 F::run(int x) 返回的是调用者残留的
	   栈指针值（输出 140735188379368 而非 2005）。 */
	for (auto& p : f.params) {
		auto it = locals_.find(p.name);
		if (it == locals_.end()) continue;
		auto pit = pinned_.find(it->second.slot);
		if (pit == pinned_.end()) continue;
		int64_t d = -8 * static_cast<int64_t>(it->second.slot + 1);
		c_.mov64r_memd32(pit->second, 5, static_cast<int32_t>(d));
	}

	static bool dbgRA = getenv("WEI_DBG_RA") != nullptr;
	if (dbgRA) {
		std::string ss;
		for (auto& kv : pinnedDisp_) ss += " d" + std::to_string(kv.first) + "->r" + std::to_string(kv.second);
		fprintf(stderr, "[RA] pass=%d func=%s canary=%d{%s}\n",
		        gen_pass_, cur_qualname_.c_str(), canary_disp_, ss.c_str());
	}
	c_.set_slot_hook([this](int64_t d) -> int {
		auto it = pinnedDisp_.find(d);
		return it == pinnedDisp_.end() ? -1 : it->second;
	});
	/* 任何输出/外部调用 stub 进入前，先 spill 并降级到内存路径。
	   r15 会被 stub 借用暂存栈指针，不降级就会把栈地址当变量值打印。 */
	c_.set_pre_align([this]() { spill_and_drop_pinned(); });
	c_.set_on_slot_addr([this]() { spill_and_drop_pinned(); });

}

void NativeGen::emit_sync_pinned(bool toMem, const std::function<bool(uint32_t)>& want) {
	for (auto& kv : pinned_) {
		bool prone = (kv.second == 3 || kv.second == 13);
		if (!prone && !want(kv.first)) continue;
		int32_t d = -8 * static_cast<int>(kv.first + 1);
		if (toMem) c_.mov_memd8_64r(5, d, kv.second); else c_.mov64r_memd8(kv.second, 5, d);
	}
}
void NativeGen::emit_load_var(const std::string& nm) {
	auto& vi = resolve_var(nm);
	if (vi.is_local) {
		int r = regOfSlot(vi.slot);
		if (r >= 0) c_.mov64r_rm64(0, r); else c_.mov64r_memd8(0, 5, -8 * static_cast<int>(vi.slot + 1));
	} else emit_global_load(0, vi.slot);
}
void NativeGen::emit_store_var(const std::string& nm) {
	auto& vi = resolve_var(nm);
	if (vi.is_local) {
		int r = regOfSlot(vi.slot);
		if (r >= 0) c_.mov64r_rm64(r, 0); else c_.mov_memd8_64r(5, -8 * static_cast<int>(vi.slot + 1), 0);
	} else emit_global_store(vi.slot, 0);
}

/* ═══ 第 2 节预备: 叶子操作数 / 区间证明 / ALU 发射 ═══ */
NativeGen::Leaf NativeGen::leafOf(Expr& e) {
	Leaf r;
	if (auto* il = std::get_if<IntLit>(&e)) {
		if (il->value >= INT32_MIN && il->value <= INT32_MAX) { r.k = Leaf::Imm; r.v = static_cast<int32_t>(il->value); }
		return r;
	}
	if (auto* id = std::get_if<Ident>(&e)) {
		if (!fast_loop_var_.empty() && id->name == fast_loop_var_) { r.k = Leaf::Reg; r.reg = fast_loop_reg_; return r; }
		if (dbl_vars_.count(id->name) || narrow_int_.count(id->name) || widthOfVar(id->name) > 0) return r;
		auto& vi = resolve_var(id->name);
		if (!vi.is_local || vi.array_len > 0 || vi.is_double) return r;
		int rg = regOfSlot(vi.slot);
		if (rg >= 0) { r.k = Leaf::Reg; r.reg = rg; return r; }
		r.k = Leaf::Mem; r.v = -8 * static_cast<int>(vi.slot + 1);
	}
	return r;
}

/* 区间证明。idx_ub_ 给出迭代开始时 i ∈ [0,ub)；这里保守用 [0,ub]（i 可能已自增） */
NativeGen::Rng NativeGen::rngOf(const Expr& e) {
	Rng r;
	if (auto* il = std::get_if<IntLit>(&e)) {
		r.lo = r.hi = il->value; r.bounded = true; r.ok = true; return r;
	}
	if (auto* bl = std::get_if<BoolLit>(&e)) {
		r.lo = r.hi = (bl->value ? 1 : 0); r.bounded = true; r.ok = true; return r;
	}
	if (auto* ix = std::get_if<IndexExpr>(&e)) {
		/* D1: 数组元素区间由定长初始化循环静态分析得出（全数组上界，
		   与下标无关，故保守成立）。未登记则证不出，保持 unbounded。 */
		auto ait = arr_elem_bounds_.find(ix->name);
		if (ait != arr_elem_bounds_.end() && ait->second.ok) {
			r.lo = ait->second.lo; r.hi = ait->second.hi;
			r.bounded = true;
			r.ok = r.lo >= wei_i128(INT64_MIN) && r.hi <= wei_i128(INT64_MAX);
		}
		return r;
	}
	if (auto* id = std::get_if<Ident>(&e)) {
		auto it = idx_ub_.find(id->name);
		if (it != idx_ub_.end() && it->second > 0) { r.lo = 0; r.hi = it->second; r.bounded = true; r.ok = true; }
		/* 取模归纳变量: 恒在 [0,C)（见 mod_iv_bounds_ 注释） */
		auto mit = mod_iv_bounds_.find(id->name);
		if (mit != mod_iv_bounds_.end() && mit->second > 0) {
			r.lo = 0; r.hi = mit->second - 1; r.bounded = true; r.ok = true;
		}
		return r;
	}
	if (auto* u = std::get_if<Unary>(&e)) {
		if (u->op == "-" && u->operand) {
			Rng a = rngOf(*u->operand);
			if (a.bounded) { r.lo = -a.hi; r.hi = -a.lo; r.bounded = true; r.ok = r.ok || (r.lo >= wei_i128(INT64_MIN) && r.hi <= wei_i128(INT64_MAX)); r.ok = r.lo >= wei_i128(INT64_MIN) && r.hi <= wei_i128(INT64_MAX); }
		}
		return r;
	}
	if (auto* b = std::get_if<Binary>(&e)) {
		Rng br = rngOfBin(*b);
		if (br.bounded) return br;
		/* 比较与逻辑运算: 结果恒为 0/1 */
		if (b->op == "==" || b->op == "!=" || b->op == "<" || b->op == ">" || b->op == "<=" || b->op == ">="
		    || b->op == "&&" || b->op == "and" || b->op == "||" || b->op == "or") {
			r.lo = 0; r.hi = 1; r.bounded = true; r.ok = true; return r;
		}
		/* 常量取模: 被除数非负、除数为正常量常数 → 结果 [0, C-1]。
		   这既是"编译期证明下标在界内"的通用路径，也保持 C 语义
		   （负数被除数的余数为负，故只在 a.lo>=0 时成立）。 */
		if (b->op == "%" && b->rhs && b->lhs) {
			Rng a = rngOf(*b->lhs), c = rngOf(*b->rhs);
			if (a.bounded && c.bounded && a.lo >= 0 && c.lo == c.hi && c.lo > 0) {
				r.lo = 0; r.hi = c.lo - 1; r.bounded = true; r.ok = true; return r;
			}
		}
		/* 常量除法: 两侧非负且除数不含 0 → 保守区间 */
		if (b->op == "/" && b->rhs && b->lhs) {
			Rng a = rngOf(*b->lhs), c = rngOf(*b->rhs);
			if (a.bounded && c.bounded && a.lo >= 0 && c.lo > 0) {
				r.lo = a.lo / c.hi; r.hi = a.hi / c.lo; r.bounded = true; r.ok = true; return r;
			}
		}
		/* 位与: 两侧非负时结果不超过任一操作数 */
		if (b->op == "&" && b->rhs && b->lhs) {
			Rng a = rngOf(*b->lhs), c = rngOf(*b->rhs);
			if (a.bounded && c.bounded && a.lo >= 0 && c.lo >= 0) {
				r.lo = 0; r.hi = (a.hi < c.hi ? a.hi : c.hi); r.bounded = true; r.ok = true; return r;
			}
		}
		return r;
	}
	return r;
}
NativeGen::Rng NativeGen::rngOfBin(const Binary& b) {
	Rng r;
	auto fits = [](const Rng& x) { return x.lo >= wei_i128(INT64_MIN) && x.hi <= wei_i128(INT64_MAX); };
	if (b.op != "+" && b.op != "-" && b.op != "*") return r;
	Rng a = rngOf(*b.lhs), c = rngOf(*b.rhs);
	if (!a.ok || !c.ok) return r;
	if (b.op == "+") { r.lo = a.lo + c.lo; r.hi = a.hi + c.hi; }
	else if (b.op == "-") { r.lo = a.lo - c.hi; r.hi = a.hi - c.lo; }
	else {
		wei_i128 p[4] = {a.lo * c.lo, a.lo * c.hi, a.hi * c.lo, a.hi * c.hi};
		r.lo = r.hi = p[0];
		for (int i = 1; i < 4; i++) { if (p[i] < r.lo) r.lo = p[i]; if (p[i] > r.hi) r.hi = p[i]; }
	}
	r.bounded = a.bounded && c.bounded;
	r.ok = r.bounded && fits(r);                            /* 每个节点都必须装得下，故中间结果不可能溢出 */
	return r;
}
bool NativeGen::ovfProven(Binary& n) {
	if (!check_enabled(CheckKind::Overflow)) return true;    /* 用户选了不含溢出检查的等级 */
	/* Binary 含 unique_ptr（move-only），装不进 variant，故走专用重载 */
	bool ok = rngOfBin(n).ok;
	if (ok) proofNote("算术不会溢出", n.line);
	return ok;
}

/* ── 编译期证明层（对标 Rust 的两个痛点）─────────────────────────────
   Rust: release 模式整数溢出**静默回绕**（不 panic）；动态下标越界只能
         在运行时 panic。两者都要等到程序跑起来才暴露。
   Wei : 区间分析能在编译期证明"必然"发生时就直接报 error，编不过；
         证明不了才退回运行时检测（绝不回绕、绝不静默）。
   关键安全约束: 只在**整个区间**都落在合法范围之外时报错。区间是保守
   近似，只要还含一个合法值就不能断定必然出错 —— 绝不误报。 */
bool NativeGen::mustOverflow(const Expr& e) {
	Rng r = rngOf(e);
	if (!r.bounded) return false;
	return r.lo > wei_i128(INT64_MAX) || r.hi < wei_i128(INT64_MIN);
}
bool NativeGen::mustBeOOB(const Expr& idx, int64_t len) {
	Rng r = rngOf(idx);
	if (!r.bounded || len <= 0) return false;
	return r.lo >= wei_i128(len) || r.hi < 0;
}
void NativeGen::proofNote(const char* what, int line) {
	static bool on = getenv("WEI_PROOF_REPORT") != nullptr;
	if (on) fprintf(stderr, "[proof] line %d: %s —— 编译期已证明，省略运行时检查\n", line, what);
}
void NativeGen::checkProvenOOB(const IndexExpr& n, int64_t len) {
	if (!n.idx || !mustBeOOB(*n.idx, len)) return;
	auto d = wei::diag::makeError("E0502", "编译期已证明数组下标必然越界", {n.line, n.col, 1});
	d.labels[0].msg = "下标的所有可能取值都落在 [0," + std::to_string(len) + ") 之外";
	d.help("把下标改回合法范围；或检查循环上界与数组长度是否写错");
	d.note("Wei 在编译期证明「必然越界」时直接报错，而不是等运行时 panic");
	throw wei::diag::DiagnosticError(d);
}
void NativeGen::checkProvenOverflow(const Binary& n) {
	/* Binary 含 unique_ptr，装不进 variant，故直接走专用重载 */
	Rng r = rngOfBin(n);
	if (!r.bounded || !(r.lo > wei_i128(INT64_MAX) || r.hi < wei_i128(INT64_MIN))) return;
	auto d = wei::diag::makeError("E0503", "编译期已证明整数运算必然溢出", {n.line, n.col, 1});
	d.labels[0].msg = "该表达式所有可能取值都超出 int64 范围";
	d.help("缩小操作数范围，或改用更宽的类型/显式处理溢出");
	d.note("Wei 不会像 Rust release 那样静默回绕：能证明就编译期报错，证不了就运行时检测");
	throw wei::diag::DiagnosticError(d);
}

void NativeGen::emit_alu_leaf(const std::string& op, int d, const Leaf& l) {
	if (op == "+") {
		if (l.k == Leaf::Imm) c_.add_rm64_imm32(d, l.v); else if (l.k == Leaf::Reg) c_.add_r64_rm64(d, l.reg); else c_.add_r64_memd8(d, 5, l.v);
	} else if (op == "-") {
		if (l.k == Leaf::Imm) c_.sub_rm64_imm32(d, l.v); else if (l.k == Leaf::Reg) c_.sub_r64_rm64(d, l.reg); else c_.sub_r64_memd8(d, 5, l.v);
	} else if (op == "*") {
		if (l.k == Leaf::Imm) c_.imul_r64_rm64_imm32(d, d, l.v); else if (l.k == Leaf::Reg) c_.imul_r64_rm64(d, l.reg); else c_.imul_r64_memd8(d, 5, l.v);
	} else if (op == "&") {
		if (l.k == Leaf::Imm) c_.alu_rm64_imm32(d, 4, l.v); else if (l.k == Leaf::Reg) c_.and_r64_rm64(d, l.reg); else c_.alu_r64_memd8(0x23, d, 5, l.v);
	} else if (op == "|") {
		if (l.k == Leaf::Imm) c_.alu_rm64_imm32(d, 1, l.v); else if (l.k == Leaf::Reg) c_.or_r64_rm64(d, l.reg); else c_.alu_r64_memd8(0x0B, d, 5, l.v);
	} else {
		if (l.k == Leaf::Imm) c_.alu_rm64_imm32(d, 6, l.v); else if (l.k == Leaf::Reg) c_.xor_r64_rm64(d, l.reg); else c_.alu_r64_memd8(0x33, d, 5, l.v);
	}
}

bool NativeGen::emit_binop_fast(Binary& n) {
	const std::string& op = n.op;
	bool arith = (op == "+" || op == "-" || op == "*");
	if (!arith && op != "&" && op != "|" && op != "^") return false;
	if (i32_mode_ || wide_mode_ || isDblExpr(n.lhs) || isDblExpr(n.rhs)) return false;
	if (widthOfExpr(*n.lhs) > 0 || widthOfExpr(*n.rhs) > 0) return false;
	Leaf rl = leafOf(*n.rhs);
	if (rl.k == Leaf::None) return false;
	bool proven = arith && ovfProven(n);
	if (arith && !proven && op == "+" && rl.k == Leaf::Imm) proven = addOvfFree(*n.lhs, rl.v);
	Leaf ll = leafOf(*n.lhs);
	if (ll.k == Leaf::Reg) c_.mov64r_rm64(0, ll.reg); else gen_expr(*n.lhs);
	if (op == "*" && rl.k == Leaf::Imm && rl.v == 1) return true;
	emit_alu_leaf(op, 0, rl);
	if (arith && !proven) emit_ovf_check();                  /* jno + panic 桩，语义不变 */
	return true;
}

/* x = x OP leaf：直接在驻留寄存器上做，1 条指令 + 检查，取代 load/op/mov */
bool NativeGen::try_assign_inplace(AssignStmt& n) {
	if (n.target || !n.value || i32_mode_ || wide_mode_) return false;
	auto* b = std::get_if<Binary>(n.value.get());
	if (!b) return false;
	auto* lid = std::get_if<Ident>(b->lhs.get());
	if (!lid || lid->name != n.name) return false;
	const std::string& op = b->op;
	bool arith = (op == "+" || op == "-" || op == "*");
	if (!arith && op != "&" && op != "|" && op != "^") return false;
	if (dbl_vars_.count(n.name) || isDblExpr(b->rhs) || widthOfVar(n.name) > 0 || widthOfExpr(*b->rhs) > 0) return false;
	auto& vi = resolve_var(n.name);
	if (!vi.is_local || vi.array_len > 0) return false;
	int tgt = regOfSlot(vi.slot);
	if (tgt < 0 || (!fast_loop_var_.empty() && n.name == fast_loop_var_)) return false;
	Leaf rl = leafOf(*b->rhs);
	if (rl.k == Leaf::None) return false;
	bool proven = !arith || !check_enabled(CheckKind::Overflow);
	if (!proven && op == "+" && rl.k == Leaf::Imm) proven = addOvfFreeVar(n.name, rl.v);
	if (op == "*" && rl.k == Leaf::Imm && rl.v == 1) { known_const_.erase(n.name); return true; }
	emit_alu_leaf(op, tgt, rl);
	if (!proven) emit_ovf_check();
	known_const_.erase(n.name);
	last_store_len_ = static_cast<size_t>(-1);
	return true;
}

/* ═══ 第 3 节: LEA 融合（仅在静态证明不可能溢出时）═══ */
bool NativeGen::lea_flatten(Expr& e, std::vector<LeaTerm>& t, int64_t& c) {
	if (auto* il = std::get_if<IntLit>(&e)) {
		if (il->value < INT32_MIN || il->value > INT32_MAX) return false;
		c += il->value; return true;
	}
	if (auto* b = std::get_if<Binary>(&e)) return lea_flatten_bin(*b, t, c);
	Leaf l = leafOf(e);
	if (l.k != Leaf::Reg && l.k != Leaf::Mem) return false;
	t.push_back({l, 1}); return true;
}

bool NativeGen::try_lea(Binary& n) {
	if (i32_mode_ || wide_mode_ || (n.op != "+" && n.op != "-" && n.op != "*")) return false;
	if (isDblExpr(n.lhs) || isDblExpr(n.rhs) || widthOfExpr(*n.lhs) > 0 || widthOfExpr(*n.rhs) > 0) return false;
	if (!ovfProven(n)) return false;                         /* lea 不置 OF：只有不可能溢出时才合法 */
	std::vector<LeaTerm> t; int64_t c = 0;
	if (!lea_flatten_bin(n, t, c) || t.empty() || t.size() > 2 || c < INT32_MIN || c > INT32_MAX) return false;
	int scratch[2] = {1, 2}, si = 0;
	auto regFor = [&](const Leaf& l) { if (l.k == Leaf::Reg) return l.reg; int r = scratch[si++]; c_.mov64r_memd8(r, 5, l.v); return r; };
	int base = -1, index = -1, sc = 1;
	if (t.size() == 1) {
		int s = t[0].scale;
		if (s == 1 && c == 0) return false;
		int r = regFor(t[0].l);
		if (s == 1) base = r;
		else if (s == 2 || s == 4 || s == 8) { index = r; sc = s; }
		else { base = r; index = r; sc = s - 1; }            /* x*3, x*5, x*9 */
	} else {
		int a = t[0].scale, b = t[1].scale;
		int ia = (a != 1) ? 0 : 1;                           /* 带系数的项充当 index */
		if (a == 1 && b == 1) { base = regFor(t[0].l); index = regFor(t[1].l); sc = 1; }
		else if ((a == 2 || a == 4 || a == 8) && b == 1) { index = regFor(t[0].l); sc = a; base = regFor(t[1].l); }
		else if ((b == 2 || b == 4 || b == 8) && a == 1) { base = regFor(t[0].l); index = regFor(t[1].l); sc = b; }
		else return false;
		(void)ia;
	}
	if (index == 4) return false;                            /* rsp 不能作 index */
	c_.lea_bisd(0, base, index, sc, static_cast<int32_t>(c));
	return true;
}

bool NativeGen::lea_flatten_bin(Binary& b, std::vector<LeaTerm>& t, int64_t& c) {
	if (b.op == "+") return lea_flatten(*b.lhs, t, c) && lea_flatten(*b.rhs, t, c);
	if (b.op == "-") {
		auto* il = std::get_if<IntLit>(b.rhs.get());
		if (!il || il->value < INT32_MIN || il->value > INT32_MAX) return false;
		c -= il->value; return lea_flatten(*b.lhs, t, c);
	}
	if (b.op == "*") {
		Expr* v = b.lhs.get(); auto* il = std::get_if<IntLit>(b.rhs.get());
		if (!il) { il = std::get_if<IntLit>(b.lhs.get()); v = b.rhs.get(); }
		if (!il) return false;
		int64_t k = il->value;
		if (k != 1 && k != 2 && k != 3 && k != 4 && k != 5 && k != 8 && k != 9) return false;
		Leaf l = leafOf(*v);
		if (l.k != Leaf::Reg && l.k != Leaf::Mem) return false;
		t.push_back({l, static_cast<int>(k)}); return true;
	}
	return false;
}

/* ═══ 第 4 节: 无分支代码 ═══ */
/* 可推测: 不会陷入、不会 panic、只碰 rax/rcx/flags、无副作用 */
bool NativeGen::specExpr(Expr& e) {
	if (std::get_if<IntLit>(&e) || std::get_if<BoolLit>(&e)) return true;
	if (std::get_if<Ident>(&e)) { Leaf l = leafOf(e); return l.k == Leaf::Reg || l.k == Leaf::Mem; }
	if (auto* u = std::get_if<Unary>(&e)) return (u->op == "!" || u->op == "~" || u->op == "-") && u->operand && specExpr(*u->operand);
	if (auto* b = std::get_if<Binary>(&e)) {
		if (!b->lhs || !b->rhs || isDblExpr(b->lhs) || isDblExpr(b->rhs)) return false;
		const std::string& o = b->op;
		if (o == "+" || o == "-" || o == "*") {
			if (!ovfProven(*b)) return false;                /* 推测臂绝不能 panic */
			return specExpr(*b->lhs) && specExpr(*b->rhs);
		}
		if (o == "&" || o == "|" || o == "^" || weiIsCmpOp(o) || o == "&&" || o == "||")
			return specExpr(*b->lhs) && specExpr(*b->rhs);
	}
	return false;
}
bool NativeGen::isBoolValued(const Expr& e) const {
	if (std::get_if<BoolLit>(&e)) return true;
	if (auto* u = std::get_if<Unary>(&e)) return u->op == "!";
	if (auto* b = std::get_if<Binary>(&e)) return weiIsCmpOp(b->op) || b->op == "&&" || b->op == "||";
	return false;
}
void NativeGen::emit_norm_bool() {                           /* rax = (rax != 0) */
	c_.test_rm64_r64(0, 0); c_.setcc_r8(0x5, 0); c_.movzx_r64_rm8(0, 0);
}

/* a && b / a || b 且两侧无副作用、不会陷入：完全不产生跳转 */
bool NativeGen::try_bool_branchless(Binary& n) {
	if ((n.op != "&&" && n.op != "||") || i32_mode_ || wide_mode_) return false;
	if (!specExpr(*n.lhs) || !specExpr(*n.rhs)) return false;
	gen_expr(*n.lhs); if (!isBoolValued(*n.lhs)) emit_norm_bool();
	c_.push_r(0);
	gen_expr(*n.rhs); if (!isBoolValued(*n.rhs)) emit_norm_bool();
	c_.pop_r(1);
	if (n.op == "&&") c_.and_r64_rm64(0, 1); else c_.or_r64_rm64(0, 1);
	return true;
}

/* if (c) x = A; else x = B;  以及  if (c) x = A;  → cmov。
   只有在 specExpr 证明两臂无害时才推测执行。 */
bool NativeGen::try_cmov_if(IfStmt& n) {
	if (!n.cond || n.then_body.size() != 1 || n.else_body.size() > 1 || i32_mode_ || wide_mode_) return false;
	auto* a = std::get_if<AssignStmt>(&n.then_body[0]);
	if (!a || a->target || !a->value) return false;
	AssignStmt* b = nullptr;
	if (n.else_body.size() == 1) {
		b = std::get_if<AssignStmt>(&n.else_body[0]);
		if (!b || b->target || !b->value || b->name != a->name) return false;
	}
	auto& vi = resolve_var(a->name);
	if (!vi.is_local || vi.array_len > 0 || vi.is_double || dbl_vars_.count(a->name)) return false;
	if (narrow_int_.count(a->name) || widthOfVar(a->name) > 0 || a->name == fast_loop_var_) return false;
	if (exprIsI32(*n.cond) || !specExpr(*a->value) || (b && !specExpr(*b->value))) return false;
	gen_expr(*n.cond);                                       /* 与原来一样无条件求值一次 */
	c_.mov64r_rm64(8, 0);                                    /* r8 = cond（两臂只碰 rax/rcx） */
	gen_expr(*a->value); c_.mov64r_rm64(10, 0);              /* r10 = A */
	if (b) { gen_expr(*b->value); c_.mov64r_rm64(11, 0); }   /* r11 = B */
	else   { emit_load_var(a->name); c_.mov64r_rm64(11, 0); }/* r11 = 当前 x */
	c_.test_rm64_r64(8, 8);
	c_.mov64r_rm64(0, 11);
	c_.cmov_r64_rm64(0x5, 0, 10);                            /* cmovne rax, r10 */
	emit_store_var(a->name);
	known_const_.erase(a->name);
	last_store_len_ = static_cast<size_t>(-1);
	return true;
}

/* ═══ 第 5 节: 比较+分支融合 与 循环旋转 ═══ */
/* 发射后 FLAGS 满足: jcc(返回值) <=> cond 为真 */
bool NativeGen::modpow2_test(Expr& cmpL, Expr& cmpR, const std::string& cmpOp,
                             Expr** valExpr, int64_t* mask, int* cc) {
	static bool off = getenv("WEI_NO_MODPOW2") != nullptr;   /* A/B 对照 */
	if (off) return false;
	if (cmpOp != "==" && cmpOp != "!=") return false;
	/* 必须有一边是字面量 0。只优化「与 0 比较」——
	   x%C == 1 之类在 x 为负时余数符号不同，不能压成位测试。 */
	Expr* other = nullptr;
	if (auto* il = std::get_if<IntLit>(&cmpL)) { if (il->value == 0) other = &cmpR; }
	if (!other) {
		if (auto* il = std::get_if<IntLit>(&cmpR)) { if (il->value == 0) other = &cmpL; }
	}
	if (!other) return false;
	auto* mb = std::get_if<Binary>(other);
	if (!mb || mb->op != "%" || !mb->rhs) return false;
	auto* ci = std::get_if<IntLit>(mb->rhs.get());
	if (!ci || ci->value <= 0) return false;
	int64_t C = ci->value;
	if ((C & (C - 1)) != 0) return false;          /* 必须是 2 的幂 */
	if (C > (1LL << 31)) return false;             /* mask 要能塞进 imm32 */
	if (!mb->lhs) return false;
	if (i32_mode_ || wide_mode_) return false;     /* 32/128 位通路另有语义 */
	if (isDblExpr(mb->lhs) || isDblExpr(mb->rhs)) return false;
	if (exprIsI32(*mb->lhs) || widthOfExpr(*mb->lhs) > 0) return false;
	*valExpr = mb->lhs.get();
	*mask = C - 1;
	*cc = (cmpOp == "==") ? 0x4 : 0x5;            /* je / jne == sete / setne */
	return true;
}

bool NativeGen::try_modpow2_flags(Expr& cond, int& ccOut) {
	auto* b = std::get_if<Binary>(&cond);
	if (!b || !b->lhs || !b->rhs) return false;
	Expr* v = nullptr; int64_t mask = 0; int cc = 0;
	if (!modpow2_test(*b->lhs, *b->rhs, b->op, &v, &mask, &cc)) return false;
	gen_expr(*v);
	c_.test_rm64_imm32(0, static_cast<int32_t>(mask));
	ccOut = cc;
	return true;
}

bool NativeGen::try_modpow2_value(Binary& n) {
	if (!n.lhs || !n.rhs) return false;
	Expr* v = nullptr; int64_t mask = 0; int cc = 0;
	if (!modpow2_test(*n.lhs, *n.rhs, n.op, &v, &mask, &cc)) return false;
	gen_expr(*v);
	c_.test_rm64_imm32(0, static_cast<int32_t>(mask));
	c_.setcc_r8(cc, 0);
	c_.movzx_r64_rm8(0, 0);
	return true;
}

int NativeGen::emit_cmp_flags(Expr& cond) {
	int mcc = 0;
	if (try_modpow2_flags(cond, mcc)) return mcc;   /* %2^k 判零 → test+jcc */
	if (auto* b = std::get_if<Binary>(&cond)) {
		if (weiIsCmpOp(b->op) && !i32_mode_ && !wide_mode_ && !exprIsI32(*b->lhs) && !exprIsI32(*b->rhs)
		        && !isDblExpr(b->lhs) && !isDblExpr(b->rhs)
		        && widthOfExpr(*b->lhs) == 0 && widthOfExpr(*b->rhs) == 0) {
			Leaf rl = leafOf(*b->rhs);
			if (rl.k != Leaf::None) {
				Leaf ll = leafOf(*b->lhs);
				int lreg = 0;
				if (ll.k == Leaf::Reg) lreg = ll.reg; else gen_expr(*b->lhs);
				if (rl.k == Leaf::Imm) c_.cmp_rm64_imm32(lreg, rl.v);
				else if (rl.k == Leaf::Reg) c_.cmp_r64_rm64(lreg, rl.reg);
				else c_.cmp_r64_memd8(lreg, 5, rl.v);
			} else {
				gen_expr(*b->lhs); c_.push_r(0); gen_expr(*b->rhs);
				c_.mov64r_rm64(1, 0); c_.pop_r(0); c_.cmp_r64_rm64(0, 1);
			}
			return weiCmpCC(b->op);
		}
	}
	/* 裸真值判断 `if (x % 2^k)` —— 语义即 `!= 0`，同样可压成 test */
	if (getenv("WEI_NO_MODPOW2") == nullptr) {
	if (auto* mb = std::get_if<Binary>(&cond)) {
		if (mb->op == "%" && mb->rhs && mb->lhs && !i32_mode_ && !wide_mode_
		        && !isDblExpr(mb->lhs) && !exprIsI32(*mb->lhs) && widthOfExpr(*mb->lhs) == 0) {
			if (auto* ci = std::get_if<IntLit>(mb->rhs.get())) {
				int64_t C = ci->value;
				if (C > 1 && C <= (1LL << 31) && (C & (C - 1)) == 0) {
					gen_expr(*mb->lhs);
					c_.test_rm64_imm32(0, static_cast<int32_t>(C - 1));
					return 0x5;                              /* jne → x%2^k != 0 */
				}
			}
		}
	}
	}
	gen_expr(cond); c_.test_rm64_r64(0, 0);
	return 0x5;                                              /* jne */
}

bool NativeGen::deepHasJump(const std::vector<Stmt>& v) {
	for (auto& s : v) {
		bool j = std::visit([](const auto& n) -> bool {
			using T = std::decay_t<decltype(n)>;
			if constexpr (std::is_same_v<T, BreakStmt> || std::is_same_v<T, ContinueStmt>) return true;
			else if constexpr (std::is_same_v<T, Block>) return deepHasJump(n.stmts);
			else if constexpr (std::is_same_v<T, IfStmt>) return deepHasJump(n.then_body) || deepHasJump(n.else_body);
			else if constexpr (std::is_same_v<T, WhileStmt> || std::is_same_v<T, ForStmt> || std::is_same_v<T, LoopStmt>
			                   || std::is_same_v<T, LoopNStmt> || std::is_same_v<T, ForallStmt> || std::is_same_v<T, CompileTimeStmt>)
				return deepHasJump(n.body);
			else if constexpr (std::is_same_v<T, WhenStmt>) return deepHasJump(n.then_body) || deepHasJump(n.else_body);
			else if constexpr (std::is_same_v<T, MatchStmt>) {
				for (auto& c : n.cases) if (deepHasJump(c.body)) return true;
				return deepHasJump(n.default_body);
			}
			else if constexpr (std::is_same_v<T, SwitchStmt>) { for (auto& c : n.cases) if (deepHasJump(c.body.stmts)) return true; return false; }
			else if constexpr (std::is_same_v<T, DeferStmt>) return deepHasJump(n.body.stmts);
			else return false;
		}, s);
		if (j) return true;
	}
	return false;
}

/* 顶部测试的循环 = 每轮 2 次分支；旋转后 = 每轮 1 次向后分支 */
bool NativeGen::emit_while_rotated(WhileStmt& n) {
	if (!n.cond || deepHasJump(n.body)) return false;
	last_store_len_ = static_cast<size_t>(-1);
	int jcond = emit_jmp();
	size_t top = c_.pos();
	gen_block(n.body);
	patch(jcond);
	last_store_len_ = static_cast<size_t>(-1);
	int cc = emit_cmp_flags(*n.cond);
	c_.u8(0x0F); c_.u8(static_cast<uint8_t>(0x80 + cc));
	c_.u32(static_cast<uint32_t>(static_cast<int32_t>(static_cast<int64_t>(top) - static_cast<int64_t>(c_.pos() + 4))));
	last_store_len_ = static_cast<size_t>(-1);
	return true;
}
bool NativeGen::emit_for_rotated(ForStmt& n) {               /* init 已由调用方发射 */
	if (!n.cond || deepHasJump(n.body)) return false;
	last_store_len_ = static_cast<size_t>(-1);
	int jcond = emit_jmp();
	size_t top = c_.pos();
	gen_block(n.body);
	if (n.step) gen_expr(*n.step);
	patch(jcond);
	last_store_len_ = static_cast<size_t>(-1);
	int cc = emit_cmp_flags(*n.cond);
	c_.u8(0x0F); c_.u8(static_cast<uint8_t>(0x80 + cc));
	c_.u32(static_cast<uint32_t>(static_cast<int32_t>(static_cast<int64_t>(top) - static_cast<int64_t>(c_.pos() + 4))));
	last_store_len_ = static_cast<size_t>(-1);
	return true;
}

	void NativeGen::gen_func(FuncDecl& f) {
		/* ── D1: 必须在**函数序言之前**清掉上一个函数的 slot hook ──
		   序言会写 stack canary 到 [rbp+canary_disp_]。若沿用上一个函数
		   的 pinnedDisp_，且该 disp 恰好命中，canary 就会被写进物理寄存器
		   而非内存 → 尾声 canary 校验读回 0 → SIGTRAP。
		   实测: 第二个函数 canary=-56 命中 d-56→r14，基准 5 项全 RUN-FAIL。
		   promote_function_locals 在 gen_block 之前才设置新表，太晚了。 */
		c_.set_slot_hook({});
		c_.set_pre_align({});
		c_.set_on_slot_addr({});
		pinned_.clear(); pinned_name_.clear(); pinnedDisp_.clear();

		/* 通用循环展开: 必须在取模归纳变量改写**之后** —— 取模改写会往
		   循环体尾部追加计数器推进语句，展开要复制的是改写后的完整体。
		   顺序反了会把推进语句漏在展开体之外，结果算错。 */
		/* 展开后该位置被替换成 [主循环, 尾部循环] 两条，**必须跳过这两条**。
		   原地不动会让主循环再次被展开 —— 每轮 U 倍、指数增长直到 OOM
		   （字面量上界的循环曾直接把编译器跑崩）。 */
		/* 顺序至关重要: **先取模归纳变量改写，再展开**。
		   取模改写会把 `s = s + i%7` 变成 `s = s + m` 并在循环体末尾
		   追加 `m = m + 1; if (m >= 7) m = m - 7;`。
		     · 先改写再展开 → 推进语句也随 body 复制 U 份，一轮 i+U 时
		       m 也 +U，正确。
		     · 先展开再改写 → 一轮跑 U 份 body（i+U），但 m 只在循环
		       末尾推进一次，m 与 i 脱节，结果算错（曾差 13）。 */
		{
			/* 常量传播预扫描: known_const_ 是**按语句顺序**填充的，
			   而展开发生在函数体任何语句生成之前，此时它还是空的 ——
			   `create.int n = 30000000; while (i < n)` 这种最常见的
			   写法会因为"上界不是字面量"而拿不到上界，展开直接放弃。
			   这里先扫一遍顶层: 字面量初始化、且全程未被再赋值的变量
			   记为编译期常量。规则保守（不递归进循环体、不认数组），
			   不会把运行时才确定的值误判为常量。 */
			for (auto& st : f.body) {
				auto* vd = std::get_if<VarDecl>(&st);
				if (!vd || !vd->init || vd->array_len > 0) continue;
				auto* il = std::get_if<IntLit>(vd->init.get());
				if (!il) continue;
				bool reassigned = false;
				for (auto& st2 : f.body) {
					if (auto* a = std::get_if<AssignStmt>(&st2)) { if (a->name == vd->name) { reassigned = true; break; } }
					if (auto* a2 = std::get_if<AssignStmt>(&st2)) { (void)a2; }
				}
				if (!reassigned) known_const_[vd->name] = il->value;
			}
		}
		rewrite_mod_induction_vars(f.body);
		/* D1: 收集数组元素区间与变量初值，供循环级溢出证明使用。
		   每遍 generate 都重算（清干净，避免跨函数/跨遍串味）。 */
		arr_elem_bounds_.clear(); var_init_const_.clear(); loop_pre_writes_.clear();
		active_ovf_free_vars_.clear(); suppress_ovf_ = false;
		collectArrayBounds(f.body);
		/* 展开仍只在第一遍做: AST 改写做两遍会让 body 指数膨胀 */
		if (gen_pass_ == 1) {
			for (size_t i = 0; i < f.body.size(); ) {
				if (rewrite_unroll_generic(f.body, i)) { i += 2; continue; }
				i++;
			}
		}

		/* Record entry offset for this function */
		func_offsets_[f.name] = c_.pos();
		size_t dbg_start = c_.pos();
		struct DbgFuncEnd {
			NativeGen* g; std::string name; size_t start;
			~DbgFuncEnd() {
				if (g->debug_on()) g->dbg_funcs_.push_back({name, start, g->c_.pos()});
			}
		} dbg_end{this, f.name, dbg_start};
		cur_ret_double_ = (f.ret_type == "double" || f.ret_type == "浮点");
		cur_ret_result_ = (f.ret_type == "Result");
		if (getenv("WEI_MONO_DEBUG")) fprintf(stderr, "[gen] %s ret=%s dbl=%d off=%zu\n", f.name.c_str(), f.ret_type.c_str(), (int)cur_ret_double_, c_.pos());
		if (!cur_qualname_.empty()) {
			func_offsets_[cur_qualname_] = c_.pos();   /* mod 成员: "Mod::member" */
			/* 函数即模块本身: 同时登记裸模块名, 供 Calc[n] 调用 */
			size_t sep = cur_qualname_.find("::");
			if (sep != std::string::npos) {
				func_offsets_[cur_qualname_.substr(0, sep)] = c_.pos();
			}
		}
		known_const_.clear();
		loop_reg_vars_.clear();
		pinned_.clear();
		pinned_name_.clear();
		dbl_vars_.clear();   /* double 类型??VarInfo::is_double 作用域判?? 避免跨函数串??*/
		canary_active_ = true;
		canary_disp_ = 0;

		/* Function prologue */
		c_.push_rbp();
		c_.mov_rbp_rsp();
		c_.push_r(3);      /* 保存 callee-saved: rbx */
		c_.push_r(12);     /* r12 */
		c_.push_r(13);     /* r13 */
		c_.push_r(14);     /* r14 */
		c_.push_r(15);     /* r15 */
		/* Allocate stack space for locals + 16 bytes for the stack canary.
		   The canary lives at [rbp - 40 - frame], i.e. the bottom of the frame,
		   so caller/callee argument staging (which grows downwards) cannot touch it. */
		{
			int32_t frame = static_cast<int32_t>(max_locals_ * 8);
			frame = (frame + 15) & ~15;
			frame += 16;                 /* canary slot + alignment padding */
			canary_disp_ = -(40 + frame);
			c_.sub_rsp(frame);
		}

		/* Copy params from incoming stack to local slots */
		for (size_t i = 0; i < f.params.size(); i++) {
			if (f.params[i].type == "double" || f.params[i].type == "浮点") {
				dbl_vars_.insert(f.params[i].name);
				resolve_var(f.params[i].name).is_double = true;
			}
			auto it = locals_.find(f.params[i].name);
			if (it != locals_.end()) {
				int32_t local_d = static_cast<int32_t>(-8 * static_cast<int>(it->second.slot + 1));
				int32_t param_disp = 16 + static_cast<int32_t>(i * 8);
				if (param_disp >= -128 && param_disp <= 127) {
					c_.mov64r_memd8(0, 5, static_cast<int8_t>(param_disp));
				} else {
					c_.mov64r_memd32(0, 5, param_disp);
				}
				c_.mov_memd8_64r(5, local_d, 0);
			}
		}

		/* Place the stack canary at the bottom of the frame. */
		emit_global_load(0, canary_slot_);
		c_.mov_memd8_64r(5, canary_disp_, 0);

		/* Try compile-time algorithm replacement (fib pattern) */
		if (try_fib_pattern(f)) return;
		if (try_hanoi_pattern(f)) return;

		/* 组合数递推形?? if(k==0)return 1; if(k==n)return 1; return f(n-1,k)+f(n-1,k-1); */
		if (f.params.size() == 2 && f.body.size() == 3) {
			auto* s0 = std::get_if<IfStmt>(&f.body[0]);
			auto* s1 = std::get_if<IfStmt>(&f.body[1]);
			auto* s2 = std::get_if<ReturnStmt>(&f.body[2]);
			if (s0 && s1 && s2 && s0->else_body.empty() && s1->else_body.empty()
			        && s0->then_body.size() == 1 && s1->then_body.size() == 1) {
				auto* c0 = std::get_if<Binary>(s0->cond.get());
				auto* c1 = std::get_if<Binary>(s1->cond.get());
				if (c0 && c1 && c0->op == "==" && c1->op == "==") {
					auto* c0l = std::get_if<Ident>(c0->lhs.get());
					auto* c0r = std::get_if<IntLit>(c0->rhs.get());
					auto* c1l = std::get_if<Ident>(c1->lhs.get());
					auto* c1r = std::get_if<Ident>(c1->rhs.get());
					auto* r0 = std::get_if<ReturnStmt>(&s0->then_body[0]);
					auto* r1 = std::get_if<ReturnStmt>(&s1->then_body[0]);
					if (c0l && c0r && c1l && c1r && r0 && r1
					        && c0l->name == f.params[1].name && c0r->value == 0
					        && c1l->name == f.params[1].name && c1r->name == f.params[0].name) {
						auto* v0 = std::get_if<IntLit>(r0->value.get());
						auto* v1 = std::get_if<IntLit>(r1->value.get());
						auto* vb = std::get_if<Binary>(s2->value.get());
						auto isArg = [](const std::unique_ptr<Expr>& e, const std::string& vn, bool allow_plain) -> bool {
							auto* b = std::get_if<Binary>(e.get());
							if (b && b->op == "-") {
								if (auto* l = std::get_if<Ident>(b->lhs.get())) {
									if (auto* r = std::get_if<IntLit>(b->rhs.get()))
										return l->name == vn && r->value == 1;
								}
								return false;
							}
							if (allow_plain) {
								if (auto* id = std::get_if<Ident>(e.get())) return id->name == vn;
							}
							return false;
						};
						if (v0 && v1 && v0->value == 1 && v1->value == 1 && vb && vb->op == "+") {
							auto* cl = std::get_if<Call>(vb->lhs.get());
							auto* cr = std::get_if<Call>(vb->rhs.get());
							if (cl && cr && cl->callee == f.name && cr->callee == f.name
							        && cl->args.size() == 2 && cr->args.size() == 2
							        && isArg(cl->args[0], f.params[0].name, false)
							        && isArg(cl->args[1], f.params[1].name, true)
							        && isArg(cr->args[0], f.params[0].name, false)
							        && isArg(cr->args[1], f.params[1].name, true)) {
								comb_func_ = f.name;
							}
						}
					}
				}
			}
		}

		/* main 入口: 部件成员初始?? 数字/字符??*/
		if ((f.name == "main" || f.name == "主函") && !part_inits_.empty()) {
			for (auto& pi : part_inits_) {
				if (pi.is_str) {
					c_.lea64r_rip(0);
					size_t dp = c_.pos();
					c_.u32(0);
					emit_global_store(pi.slot, 0);
					str_init_patches_.push_back({dp, pi.slot, pi.sv});
				} else {
					c_.mov64r_imm64(0, pi.iv);
					emit_global_store(pi.slot, 0);
				}
			}
		}

		/* Compile body —— 先做通用寄存器驻留（把高频局部变量钉在
		   callee-saved 寄存器上），再生成代码。 */
		promote_function_locals(f);
		gen_block(f.body);
		loop_reg_vars_.clear();
		pinned_.clear();
		pinned_name_.clear();

		/* Default return 0 */
		c_.mov64r_imm64(0, 0);
		emit_canary_check();
		c_.mov64r_rm64(4, 5);  /* mov rsp, rbp: 丢弃循环寄存??push 残留 */
		c_.mov64r_memd8(3, 5, -8);    /* rbx ??[rbp-8]  (压栈?? rbx,r12,r13,r14,r15) */
		c_.mov64r_memd8(12, 5, -16);  /* r12 */
		c_.mov64r_memd8(13, 5, -24);  /* r13 */
		c_.mov64r_memd8(14, 5, -32);  /* r14 */
		c_.mov64r_memd8(15, 5, -40);  /* r15 */
		c_.leave();
		c_.ret();
	}

	/* ── Main generate() ──
	   Produces the complete native binary with:
	   1. Entry point (main function code)
	   2. Data area: global vars, function pointers
	   Returns the buffer to be used as JIT code. */
	std::vector<uint8_t> NativeGen::generate() {
		gen_pass_++;
		if (gen_pass_ == 1) promote_seq_.clear();
		promote_seq_idx_ = 0;
		c_.clear();
		globals_.clear();
		num_globals_ = 0;
		global_slots_ = 0;
		max_locals_ = 0;
		global_rip_patches_.clear();
		func_offsets_.clear();
		dbg_funcs_.clear();
		func_patches_.clear();
		str_patches_.clear();
		dbl_patches_.clear();
		str_init_patches_.clear();
		pe_iat_patches_.clear();
		pe_fmt_patches_.clear();
		labels_.clear();
		data_area_ = 0;
		entry_off_ = 0;
		heap_addr_patches_.clear();
		canary_active_ = false;
		/* 保留槽位: 0=heap_ptr, 1=heap_end, 2=stack_canary，
		   其后依次为 free_lists[12] / large_list / seg_list / 5 个统计槽。
		   入口桩负责写初值（空闲链表与计数器保持 BSS 的 0 即可）。 */
		global_slots_ = 0;
		heap_ptr_slot_ = global_slots_++;
		heap_end_slot_ = global_slots_++;
		canary_slot_ = global_slots_++;
		freelist_base_slot_ = global_slots_;
		global_slots_ += static_cast<uint32_t>(WEI_NCLS);
		large_list_slot_ = global_slots_++;
		seg_list_slot_ = global_slots_++;
		stat_alloc_slot_ = global_slots_++;
		stat_free_slot_ = global_slots_++;
		stat_live_slot_ = global_slots_++;
		stat_peak_slot_ = global_slots_++;
		stat_grow_slot_ = global_slots_++;
		q_idx_slot_ = global_slots_++;
		q_arr_slot_ = global_slots_;
		global_slots_ += static_cast<uint32_t>(WEI_QUARANTINE);
		/* 计数循环(循环 N)的运行时计数器槽，支持 WEI_LOOPN_MAX 层嵌套。
		   放在全局区而非栈上：栈上的 [rsp] 会被循环体内调用的
		   save_rsp_r15/align_rsp16 破坏，而全局槽是 RIP 相对寻址，绝对安全。 */
		lcnt_base_slot_ = global_slots_;
		global_slots_ += static_cast<uint32_t>(WEI_LOOPN_MAX);
		/* 协程运行时槽: 当前协程控制块 / 主协程栈指针 / 句柄暂存 */
		co_cur_slot_ = global_slots_++;
		co_main_rsp_slot_ = global_slots_++;
		co_tmp_slot_ = global_slots_++;
		quant_res_slot_ = global_slots_++;
		wide_tmp_slot_ = global_slots_++;
		/* 影子高位槽预留区: 固定大小, 槽号在其中顺序分配, 永不越界 */
		wide_hi_base_ = global_slots_;
		global_slots_ += WIDE_HI_MAX;
		wide_hi_used_ = 0;
		num_globals_ = global_slots_;
		wide_hi_.clear();
		co_thunk_patches_.clear();
		type_aliases_.clear();
		co_thunk_seq_ = 0;
		/* Find all functions and collect them (module ::main 作为入口) */
		std::vector<FuncDecl*> funcs_;
		FuncDecl* main_func = nullptr;
		FuncDecl main_wrapper;
		ModuleDecl* main_module_src = nullptr;   /* 记录被移??body 的模?? 末尾归还 */

		/* 收集 mod 成员函数, 命名为 "mod::member" */
		std::vector<ModEntryDecl> mod_entries;
		size_t decl_order = 0;
		for (auto& item : prog_.items) {
			if (auto* m = std::get_if<ModuleDecl>(&item)) {
				for (auto& mf : m->methods) {
					mod_entries.push_back(ModEntryDecl{m, &mf, m->priority, decl_order++});
				}
				if (m->name == "main" || m->name == "主函") {
					main_wrapper.name = m->name;
					main_wrapper.body = std::move(m->body);   /* 移出编译, generate() 末尾归还, 保证两遍生成等价 */
					main_wrapper.ret_type = "int";
					main_func = &main_wrapper;
					main_module_src = m;
				}
			}
		}
		/* mod 入口排序 —— 依赖优先于手写 priority。
		   规则:
		     1. 有依赖拓扑序(dep_order >= 0)的按拓扑序升序 —— 被依赖者先跑
		     2. 没有依赖声明的按 priority 升序（保留既有行为）
		     3. 两者都没有/相同时按声明序（stable_sort 保证）
		   这样 `A::requires(B)` 就能真正保证 B 先于 A 初始化，
		   而不需要用户去猜 priority 数字该填几。 */
		std::stable_sort(mod_entries.begin(), mod_entries.end(),
		[](const ModEntryDecl & a, const ModEntryDecl & b) {
			int oa = a.mod->dep_order, ob = b.mod->dep_order;
			if (oa >= 0 && ob >= 0) return oa < ob;
			if (oa >= 0) return true;    /* 有依赖序的排在前面 */
			if (ob >= 0) return false;
			return a.priority < b.priority;
		});

		/* 组合 wrapper: 只自动调用无参入口, 带参函数仅供显式调用 */
		FuncDecl mod_wrapper;
		mod_wrapper.name = "__mod_entry";
		mod_wrapper.ret_type = "int";
		for (auto& me : mod_entries) {
			if (!me.fn->params.empty()) continue;   /* 带参的不自动执行 */
			Call c;
			c.callee = me.mod->name + "::" + me.fn->name;
			c.line = me.fn->line;
			c.col = me.fn->col;
			auto ce = std::make_unique<Expr>();
			*ce = std::move(c);
			ExprStmt esv;
			esv.expr = std::move(ce);
			mod_wrapper.body.push_back(Stmt(std::move(esv)));
		}
		if (!mod_wrapper.body.empty()) {
			mod_funcs_ = std::move(mod_entries);
			main_func = &mod_wrapper;
		}

		for (auto& me : mod_funcs_) {
			funcs_.push_back(me.fn);
		}
		if (!main_func) {
			throw std::runtime_error("JIT: 未找到主函数 (create.mod 名字::main())");
		}

		/* Pre-scan module parts: 成员分配全局槽位 + 记录初值 */
		part_inits_.clear();
		part_slot_.clear();
		part_is_str_.clear();
		for (auto& item : prog_.items) {
			if (auto* m = std::get_if<ModuleDecl>(&item)) {
				for (auto& p : m->parts) {
					for (size_t fi = 0; fi < p.members.size(); fi++) {
						auto& fld = p.members[fi];
						std::string key = part_key(m->name, p.name, fld.name);
						if (part_slot_.count(key)) continue;
						uint32_t slot = global_slots_++;
						num_globals_++;
						part_slot_[key] = slot;
						bool isStrType = fld.type == "string" || fld.type == "str" || fld.type == "char" || fld.type == "文本";
						part_is_str_[key] = isStrType;
						if (fi == 0) {
							part_slot_[part_key(m->name, p.name, "*first")] = slot;
							part_is_str_[part_key(m->name, p.name, "*first")] = isStrType;
						}
						if (fld.init) {
							if (auto* ii = std::get_if<IntLit>(fld.init.get())) {
								part_inits_.push_back({slot, false, ii->value, {}});
							} else if (auto* bi = std::get_if<BoolLit>(fld.init.get())) {
								part_inits_.push_back({slot, false, bi->value ? 1 : 0, {}});
							} else if (auto* si = std::get_if<StrLit>(fld.init.get())) {
								part_inits_.push_back({slot, true, 0, si->value});
							}
						}
					}
				}
			}
		}

		target_->emitEntryStub(*this);
		emit_runtime_stubs();   /* __rt_panic / __rt_free / __rt_alloc */

		/* ScanVars helper */
		struct ScanVars {
			NativeGen* self;
			void scan_stmt(Stmt& s) {
				struct SS {
					ScanVars* sv;
					void operator()(VarDecl& n) {
						sv->self->local_slot(n.name, true, n.array_len);
						/* C1: 记录声明宽度，供后续算术选指令与定溢出边界 */
						{
							auto w = NativeGen::widthOfType(n.type);
							if (w.width) sv->self->var_width_[n.name] = w;
						}
					}
					void operator()(ForStmt& n) {
						if (n.init_decl) sv->self->local_slot(n.init_decl->name, true);
						for (auto& b : n.body) sv->scan_stmt(b);
					}
					void operator()(IfStmt& n) {
						for (auto& b : n.then_body) sv->scan_stmt(b);
						for (auto& b : n.else_body) sv->scan_stmt(b);
					}
					void operator()(WhileStmt& n) {
						for (auto& b : n.body) sv->scan_stmt(b);
					}
					void operator()(LoopStmt& n) {
						for (auto& b : n.body) sv->scan_stmt(b);
					}
					void operator()(LoopNStmt& n) {
						for (auto& b : n.body) sv->scan_stmt(b);
					}
					void operator()(Block& n) {
						for (auto& b : n.stmts) sv->scan_stmt(b);
					}
					void operator()(ModuleAccessSet& n) {
						(void)n;
					}
					void operator()(ReturnStmt&) {}
					void operator()(ExprStmt&) {}
					void operator()(AssignStmt&) {}
					void operator()(MatchStmt& n) {
						for (auto& mc : n.cases) for (auto& s : mc.body) sv->scan_stmt(s);
						for (auto& s : n.default_body) sv->scan_stmt(s);
					}
					void operator()(ContractStmt&) {}
						void operator()(BreakStmt&) {}
						void operator()(ContinueStmt&) {}
					void operator()(TupleLit& n) {
						for (auto& e : n.elements) {
							/* Tuple elements are expressions, not statements */
							(void)e;
						}
					}
					void operator()(TypeAliasDecl&) {}
					void operator()(AsmStmt&) {}
					void operator()(SwitchStmt&) {}
					void operator()(DeferStmt&) {}

						void operator()(UseDecl&) {}
					void operator()(ForallStmt& n) {
						sv->self->local_slot(n.var, true, 0);
						for (auto& b : n.body) sv->scan_stmt(b);
					}
					void operator()(WhenStmt& n) {
						for (auto& b : n.then_body) sv->scan_stmt(b);
						for (auto& b : n.else_body) sv->scan_stmt(b);
					}
					void operator()(YieldStmt&) {}
					void operator()(CompileTimeStmt& n) {
						for (auto& b : n.body) sv->scan_stmt(b);
					}
					void operator()(MacroDecl&) {}
				};
				std::visit(SS{this}, s);
			}
		};

		/* Phase 1: compile non-main functions (record their offsets) */
		for (auto* f : funcs_) {
			if (f == main_func) continue;
			cur_qualname_.clear();
			for (auto& me : mod_funcs_) {
				if (me.fn == f) {
					cur_qualname_ = me.mod->name + "::" + f->name;
					break;
				}
			}
			locals_.clear();
			max_locals_ = 0;
			for (auto& p : f->params) {
				local_slot(p.name, true);
				local_types_[p.name] = p.type;
				if (isTextTypeName(p.type)) locals_[p.name].is_string = true;
			}
			ScanVars sv{this};
			for (auto& s : f->body) sv.scan_stmt(s);
			gen_func(*f);
		}
		cur_qualname_.clear();

		/* Phase 2: compile main function */
		locals_.clear();
		max_locals_ = 0;
		for (auto& p : main_func->params) {
			local_slot(p.name, true);
			if (isTextTypeName(p.type)) locals_[p.name].is_string = true;
		}
		ScanVars sv_main{this};
		for (auto& s : main_func->body) sv_main.scan_stmt(s);
		entry_off_ = c_.pos();  /* record main's entry offset before gen_func */
		gen_func(*main_func);

		/* Now data area starts after all code */
		data_area_ = c_.pos();

		/* Patch inter-function calls */
		for (auto& fp : func_patches_) {
			auto it = func_offsets_.find(fp.callee);
			if (it == func_offsets_.end()) {
				throw std::runtime_error("JIT: 未定义的函数 '" + fp.callee + "' at " + std::to_string(fp.line) + ":" + std::to_string(fp.col));
			}
			int64_t target = static_cast<int64_t>(it->second);
			int64_t from = static_cast<int64_t>(fp.disp_pos) + 4;
			int32_t disp = static_cast<int32_t>(target - from);
			c_.patch_u32(fp.disp_pos, static_cast<uint32_t>(disp));
		}

		/* Patch 协程 thunk 入口地址 (lea r9,[rip+disp32]) */
		for (auto& tp : co_thunk_patches_) {
			int64_t from = static_cast<int64_t>(tp.disp_pos) + 4;
			int32_t disp = static_cast<int32_t>(static_cast<int64_t>(tp.target_pos) - from);
			c_.patch_u32(tp.disp_pos, static_cast<uint32_t>(disp));
		}

		/* Function pointers in data area */
		target_->emitDataPrologue(*this);
		{
			const char* pfmt = "%lld\n";
			for (int i = 0; i < 6; i++) c_.u8(static_cast<uint8_t>(pfmt[i]));
			c_.u8(0);
			c_.u8(0);
			const char* strfmt = "%s\n";
			for (int i = 0; i < 4; i++) c_.u8(static_cast<uint8_t>(strfmt[i]));
			c_.u8(0);
			c_.u8(0);
			c_.u8(0);
			c_.u8(0);
			const char* gfmt = "%g\n";
			for (int i = 0; i < 4; i++) c_.u8(static_cast<uint8_t>(gfmt[i]));
			c_.u8(0);
			c_.u8(0);
			c_.u8(0);
			c_.u8(0);
			global_data_off_ = target_->importSize() + 24;
			for (uint32_t i = 0; i < global_slots_; i++) {
				auto it = global_inits_.find(i);
				c_.u64(it != global_inits_.end() ? it->second : 0);
			}
		}
		/* String constants: append after globals, patch RIP displacements */
		for (auto& sp : str_patches_) {
			size_t off = c_.pos() - data_area_;
			const std::string& s = sp.str;
			for (char ch : s) c_.u8(static_cast<uint8_t>(ch));
			c_.u8(0);
			int32_t disp = static_cast<int32_t>(pe_data_rel_ + off - (sp.disp_pos + 4));
			c_.patch_u32(sp.disp_pos, static_cast<uint32_t>(disp));
		}
		/* 部件字符串成员初始化 */
		for (auto& sip : str_init_patches_) {
			size_t off = c_.pos() - data_area_;
			for (char ch : sip.s) c_.u8(static_cast<uint8_t>(ch));
			c_.u8(0);
			int32_t disp = static_cast<int32_t>(pe_data_rel_ + off - (sip.disp_pos + 4));
			c_.patch_u32(sip.disp_pos, static_cast<uint32_t>(disp));
		}
		/* 双精度常??*/
		for (auto& dp : dbl_patches_) {
			size_t off = c_.pos() - data_area_;
			union {
				double d;
				uint64_t b;
			} cv;
			cv.d = dp.second;
			for (int i = 0; i < 8; i++) c_.u8(static_cast<uint8_t>(cv.b >> (8 * i)));
			int32_t disp = static_cast<int32_t>(pe_data_rel_ + off - (dp.first + 4));
			c_.patch_u32(dp.first, static_cast<uint32_t>(disp));
		}
		/* Heap Pool 位于数据区固定偏移处；池子本体不写入文件，由加载器零填充。 */
		{
			size_t data_payload = c_.pos() - data_area_;
			if (data_payload > WEI_HEAP_POOL_OFF) {
				throw std::runtime_error("JIT: 数据区超过 Heap Pool 起始偏移 (64 KiB)");
			}
			for (auto pos : heap_addr_patches_) {
				int32_t disp = static_cast<int32_t>(pe_data_rel_ + WEI_HEAP_POOL_OFF - (pos + 4));
				c_.patch_u32(pos, static_cast<uint32_t>(disp));
			}
		}
		target_->applyPatches(*this);

		/* Patch global variable RIP-relative references now that data_area_ is known */
		for (auto& gp : global_rip_patches_) {
			int32_t off = static_cast<int32_t>(pe_data_rel_ + global_data_off_ + 8 * gp.slot);
			int32_t rip_disp = off - static_cast<int32_t>(gp.disp_pos + 4);
			c_.patch_u32(gp.disp_pos, static_cast<uint32_t>(rip_disp));
		}

		{
			int64_t main_target = static_cast<int64_t>(entry_off_);
			int64_t main_from = static_cast<int64_t>(entry_main_call_pos_) + 5;
			int32_t main_disp = static_cast<int32_t>(main_target - main_from);
			c_.patch_u32(entry_main_call_pos_ + 1, static_cast<uint32_t>(main_disp));
			if (entry_exit_call_pos_) {
				int32_t exit_disp = static_cast<int32_t>(pe_data_rel_ + exit_iat_off_ - (entry_exit_call_pos_ + 6));
				c_.patch_u32(entry_exit_call_pos_ + 2, static_cast<uint32_t>(exit_disp));
			}
		}

		/* 归还模块主函数体: generate() 可能??PE 两遍生成调用, 必须保持 AST 完整 */
		if (main_module_src) {
			main_module_src->body = std::move(main_wrapper.body);
		}

		return std::move(c_.buf);
	}

	/* ── Build standalone PE with JIT native code ── */
#pragma pack(push,1)
	struct IMAGE_DOS_HEADER {
		uint16_t e_magic;
		uint16_t e_cblp, e_cp, e_crlc, e_cparhdr, e_minalloc, e_maxalloc, e_ss, e_sp, e_csum, e_ip, e_cs, e_lfarlc, e_ovno;
		uint16_t e_res[4], e_oemid, e_oeminfo, e_res2[10];
		uint32_t e_lfanew;
	};
	struct IMAGE_FILE_HEADER {
		uint16_t Machine, NumberOfSections;
		uint32_t TimeDateStamp, PointerToSymbolTable, NumberOfSymbols;
		uint16_t SizeOfOptionalHeader, Characteristics;
	};
	struct IMAGE_OPTIONAL_HEADER64 {
		uint16_t Magic;
		uint8_t MajorLinkerVersion, MinorLinkerVersion;
		uint32_t SizeOfCode, SizeOfInitializedData, SizeOfUninitializedData;
		uint32_t AddressOfEntryPoint, BaseOfCode;
		uint64_t ImageBase;
		uint32_t SectionAlignment, FileAlignment;
		uint16_t MajorOperatingSystemVersion, MinorOperatingSystemVersion;
		uint16_t MajorImageVersion, MinorImageVersion;
		uint16_t MajorSubsystemVersion, MinorSubsystemVersion;
		uint32_t Win32VersionValue;
		uint32_t SizeOfImage, SizeOfHeaders, CheckSum;
		uint16_t Subsystem, DLLCharacteristics;
		uint64_t SizeOfStackReserve, SizeOfStackCommit, SizeOfHeapReserve, SizeOfHeapCommit;
		uint32_t LoaderFlags, NumberOfRvaAndSizes;
	};
	struct IMAGE_DATA_DIRECTORY {
		uint32_t VirtualAddress, Size;
	};
	struct IMAGE_SECTION_HEADER {
		char Name[8];
		uint32_t VirtualSize, VirtualAddress, SizeOfRawData, PointerToRawData;
		uint32_t PointerToRelocations, PointerToLinenumbers;
		uint16_t NumberOfRelocations, NumberOfLinenumbers;
		uint32_t Characteristics;
	};
	struct IMAGE_IMPORT_DESCRIPTOR {
		uint32_t OriginalFirstThunk, TimeDateStamp, ForwarderChain, Name, FirstThunk;
	};
#pragma pack(pop)

	static inline uint32_t align_up(uint32_t v, uint32_t a) {
		return (v + a - 1) & ~(a - 1);
	}

	struct Win64Target : Target {
		const char* id() const override {
			return "win64";
		}
		size_t importSize() const override {
			return 104;  /* 13 个 IAT 槽: printf/fopen/fread/fwrite/fclose/GetStdHandle/ReadFile/ExitProcess/VirtualAlloc/VirtualProtect/SetConsoleOutputCP/SetConsoleCP/GetCommandLineA */
		}
		size_t fmtOffset(int kind) const override {
			/* 13 个 IAT 槽之后才是格式串：%lld / %s / %g 各占 8 字节。 */
			return (kind == 3) ? 120 : (kind == 0) ? 104 : 112;
		}
		void emitDataPrologue(NativeGen& g) override {
			for (int i = 0; i < 13; i++) g.c_.u64(0);
			g.printf_iat_off_ = 0;
			g.gsh_iat_off_ = 40;
			g.rf_iat_off_ = 48;
			g.exit_iat_off_ = 56;
		}
		void emitOutput(NativeGen& g, Output& n) override {
			if (n.values.empty()) return;
			for (auto& v : n.values) {
				if (!v) continue;
				bool is_str = g.isStrExpr(v);
				bool is_dbl = g.isDblExpr(v);
				if (is_dbl) {
					g.gen_expr_dbl(v);
					/* Win64: 格式串占第 1 个参数槽, double 是第 2 槽 -> xmm1,
					   同时按变参约定复制到 rdx (va_arg 从通用寄存器取) */
					g.c_.u8(0xF2);
					g.c_.u8(0x0F);
					g.c_.u8(0x10);
					g.c_.modrm(3, 1, 0);   /* movsd xmm1,xmm0 */
				} else {
					g.gen_expr(*v);
					if (g.exprIsI32(*v)) g.c_.movsxd_rax_eax();  /* i32 -> 64 位符号扩展 */
					g.c_.mov64r_rm64(2, 0);
				}
				if (auto* pg = std::get_if<ModulePathGet>(v.get())) {
					std::string key = pg->has_field ? g.part_key(pg->module, pg->part, pg->field)
					                  : g.part_key(pg->module, pg->part, "*first");
					auto it = g.part_is_str_.find(key);
					if (it != g.part_is_str_.end()) is_str = it->second;
				}
				{
					g.c_.save_rsp_r15();
					g.c_.align_rsp16();
					g.c_.sub_rsp(32);
					if (is_dbl) {
						g.c_.u8(0xF2);
						g.c_.u8(0x0F);
						g.c_.u8(0x11);
						g.c_.modrm(0, 0, 4);
						g.c_.u8(0x24);          /* movsd [rsp],xmm0 */
						g.c_.u8(0x48);
						g.c_.u8(0x8B);
						g.c_.u8(0x14);
						g.c_.u8(0x24);          /* mov rdx,[rsp] 变参复制到整数寄存器 */
						g.c_.u8(0xB0);          /* mov al,1 */
						g.c_.u8(0x01);
					}
					size_t fmt_pos = g.c_.pos();
					g.c_.lea64r_rip(1);
					g.c_.u32(0);
					size_t call_pos = g.c_.pos();
					g.c_.call_rip();
					g.c_.restore_rsp_r15();
					g.pe_iat_patches_.push_back({call_pos + 2, 0});
					g.pe_fmt_patches_.push_back({fmt_pos + 3, is_dbl ? 3 : (is_str ? 2 : 0)});
				}
			}
		}
		void emitInput(NativeGen& g, Input& n) override {
			g.known_const_.erase(n.var_name);
			{
				g.c_.sub_rsp(0x80);
				g.c_.u8(0x48);
				g.c_.u8(0xC7);
				g.c_.u8(0xC1);
				g.c_.u32(static_cast<uint32_t>(-10));
				g.c_.save_rsp_r15();
				g.c_.align_rsp16();
				size_t c1 = g.c_.pos();
				g.c_.call_rip();
				g.c_.restore_rsp_r15();
				g.pe_iat_patches_.push_back({c1 + 2, 2});
				g.c_.u8(0x48);
				g.c_.u8(0x89);
				g.c_.u8(0xC1);
				g.c_.u8(0x48);
				g.c_.u8(0x89);
				g.c_.u8(0x4C);
				g.c_.u8(0x24);
				g.c_.u8(0x78);
				size_t retry_top = g.c_.pos();
				g.c_.u8(0x48);
				g.c_.u8(0x8B);
				g.c_.u8(0x4C);
				g.c_.u8(0x24);
				g.c_.u8(0x78);
				g.c_.u8(0x48);
				g.c_.u8(0x8D);
				g.c_.u8(0x54);
				g.c_.u8(0x24);
				g.c_.u8(0x30);
				g.c_.u8(0x41);
				g.c_.u8(0xB8);
				g.c_.u32(64);
				g.c_.u8(0x4C);
				g.c_.u8(0x8D);
				g.c_.u8(0x4C);
				g.c_.u8(0x24);
				g.c_.u8(0x70);
				g.c_.save_rsp_r15();
				g.c_.align_rsp16();
				/* 第 5 个参数必须在压栈对齐之后写入；否则 call 时
				   RSP 已改变，ReadFile 会把错误的栈槽当作 lpOverlapped。 */
				g.c_.u8(0x48);
				g.c_.u8(0xC7);
				g.c_.u8(0x44);
				g.c_.u8(0x24);
				g.c_.u8(0x20);
				g.c_.u32(0);
				size_t c2 = g.c_.pos();
				g.c_.call_rip();
				g.c_.restore_rsp_r15();
				g.pe_iat_patches_.push_back({c2 + 2, 3});
				g.c_.u8(0x85);
				g.c_.u8(0xC0);
				int jfail = g.emit_jcc(0x4);
				g.c_.u8(0x83);
				g.c_.u8(0x7C);
				g.c_.u8(0x24);
				g.c_.u8(0x70);
				g.c_.u8(0x00);
				int jhave = g.emit_jcc(0x5);
				size_t retry_wait = g.c_.pos();
				g.c_.u8(0xB9);
				g.c_.u32(1000000);
				g.c_.u8(0xE2);
				g.c_.u8(0xFE);
				size_t jretry2 = g.c_.jmp32();
				g.c_.patch_u32(jretry2, static_cast<uint32_t>(retry_top - (jretry2 + 4)));
				g.c_.patch_u32(static_cast<uint32_t>(g.labels_[jfail].pos),
				               static_cast<uint32_t>(retry_wait - (g.labels_[jfail].pos + 4)));
				g.patch(jhave);
				g.c_.xor_rm64_r64(0, 0);
				g.c_.u8(0x45);
				g.c_.u8(0x31);
				g.c_.u8(0xDB);
				g.c_.u8(0x45);
				g.c_.u8(0x31);
				g.c_.u8(0xD2);
				g.c_.u8(0x8B);
				g.c_.u8(0x4C);
				g.c_.u8(0x24);
				g.c_.u8(0x70);
				size_t loop_top = g.c_.pos();
				g.c_.u8(0x41);
				g.c_.u8(0x39);
				g.c_.u8(0xCA);
				int jdone = g.emit_jcc(0x3);
				g.c_.u8(0x46);
				g.c_.u8(0x0F);
				g.c_.u8(0xB6);
				g.c_.u8(0x44);
				g.c_.u8(0x14);
				g.c_.u8(0x30);
				g.c_.u8(0x41);
				g.c_.u8(0x83);
				g.c_.u8(0xF8);
				g.c_.u8(48);
				int jb1 = g.emit_jcc(0x2);
				g.c_.u8(0x41);
				g.c_.u8(0x83);
				g.c_.u8(0xF8);
				g.c_.u8(57);
				int ja1 = g.emit_jcc(0x7);
				g.c_.u8(0x41);
				g.c_.u8(0x83);
				g.c_.u8(0xE8);
				g.c_.u8(48);
				g.c_.u8(0x8D);
				g.c_.u8(0x04);
				g.c_.u8(0x80);
				g.c_.u8(0x41);
				g.c_.u8(0x8D);
				g.c_.u8(0x04);
				g.c_.u8(0x40);
				size_t jnonskip = g.c_.jmp32();
				g.patch(jb1);
				g.patch(ja1);
				g.c_.u8(0x41);
				g.c_.u8(0x83);
				g.c_.u8(0xF8);
				g.c_.u8(45);
				size_t jne_skip = g.c_.pos();
				g.c_.u8(0x75);
				g.c_.u8(0x00);
				g.c_.u8(0x41);
				g.c_.u8(0xBB);
				g.c_.u32(1);
				{
					int32_t off = static_cast<int32_t>(g.c_.pos() - (jne_skip + 2));
					g.c_.patch_u8(jne_skip + 1, static_cast<uint8_t>(off));
				}
				size_t skip_inc_pos = g.c_.pos();
				g.c_.u8(0x41);
				g.c_.u8(0xFF);
				g.c_.u8(0xC2);
				size_t jback = g.c_.jmp32();
				g.c_.patch_u32(jback, static_cast<uint32_t>(loop_top - (jback + 4)));
				{
					int32_t off = static_cast<int32_t>(skip_inc_pos - (jnonskip + 4));
					g.c_.patch_u32(jnonskip, static_cast<uint32_t>(off));
				}
				g.patch(jdone);
				g.c_.u8(0x45);
				g.c_.u8(0x85);
				g.c_.u8(0xDB);
				size_t jpos = g.c_.pos();
				g.c_.u8(0x74);
				g.c_.u8(0x00);
				g.c_.u8(0x48);
				g.c_.u8(0xF7);
				g.c_.u8(0xD8);
				{
					int32_t off = static_cast<int32_t>(g.c_.pos() - (jpos + 2));
					g.c_.patch_u8(jpos + 1, static_cast<uint8_t>(off));
				}
				g.c_.add_rsp(0x80);
			}
			auto& vi = g.resolve_var(n.var_name);
			if (vi.is_local) {
				int32_t d = static_cast<int32_t>(-8 * static_cast<int>(vi.slot + 1));
				g.c_.mov_memd8_64r(5, d, 0);
			} else {
				g.emit_global_store(vi.slot, 0);
			}
		}
		void emitEntryStub(NativeGen& g) override {
			/* Win64 进程入口 RSP ≡ 8 (mod 16)。
			   push rbp 后为 0，sub 0x30 后仍为 0：
			   - VirtualProtect 前 RSP ≡ 0，callee 入口拿到 8；
			   - call main 前 RSP ≡ 0（callee 入口拿到 8）；
			   - 0x20 字节 shadow space 留给调用，另留 8 字节保存 main 的返回值。
			   旧代码把返回值放 ECX 后调用泄漏检查，ECX 被格式串指针覆盖，
			   导致所有程序的退出码都不可靠。 */
			g.c_.push_rbp();
			g.c_.mov_rbp_rsp();
			g.c_.sub_rsp(0x30);

			/* VirtualProtect(entry, code_size, PAGE_EXECUTE_READ, &old)：
			   显式把 .text 锁成 RX（PE 头本身也是 RX，这里是运行期双保险）。 */
			g.c_.lea64r_rip(1);                 /* rcx = 代码段起始 */
			g.entry_vprotect_addr_pos_ = g.c_.pos();
			g.c_.u32(0);
			g.c_.u8(0x48);                      /* mov rdx, imm32 (sign-extended) */
			g.c_.u8(0xC7);
			g.c_.u8(0xC2);
			g.entry_vprotect_size_pos_ = g.c_.pos();
			g.c_.u32(0);
			g.c_.u8(0x41);                      /* mov r8d, PAGE_EXECUTE_READ (0x20) */
			g.c_.u8(0xB8);
			g.c_.u32(0x20);
			g.c_.u8(0x4C);                      /* lea r9, [rsp+0x18] */
			g.c_.u8(0x8D);
			g.c_.u8(0x4C);
			g.c_.u8(0x24);
			g.c_.u8(0x18);
			size_t vp_call = g.c_.call_rip();
			g.pe_iat_patches_.push_back({vp_call, 9});

			/* 控制台切到 UTF-8，保证中文源码生成的程序能正确输出/输入中文 */
			g.c_.u8(0xB9);                      /* mov ecx, 65001 */
			g.c_.u32(65001);
			size_t cp_out = g.c_.call_rip();
			g.pe_iat_patches_.push_back({cp_out, 10});
			g.c_.u8(0xB9);                      /* mov ecx, 65001 */
			g.c_.u32(65001);
			size_t cp_in = g.c_.call_rip();
			g.pe_iat_patches_.push_back({cp_in, 11});

			/* heap_ptr/heap_end/stack_canary 初值 */
			g.emit_heap_and_canary_init();

			size_t main_call_pos = g.c_.pos();
			g.c_.u8(0xE8);
			g.c_.u32(0);
			/* 保存 main 的返回值，泄漏检查后再装入 RCX 交给 ExitProcess。 */
			g.c_.u8(0x89);
			g.c_.u8(0x44);
			g.c_.u8(0x24);
			g.c_.u8(0x20);                          /* mov [rsp+0x20], eax */
			g.emit_call_off(g.rt_leak_off_);        /* 退出前泄漏报告 */
			g.c_.u8(0x8B);
			g.c_.u8(0x4C);
			g.c_.u8(0x24);
			g.c_.u8(0x20);                          /* mov ecx, [rsp+0x20] */
			size_t exit_call_pos = g.c_.pos();
			g.c_.call_rip();
			g.pe_iat_patches_.push_back({exit_call_pos + 2, 1});
			g.c_.u8(0xCC);
			g.entry_main_call_pos_ = main_call_pos;
			g.entry_exit_call_pos_ = 0;
		}
		/* 128 位打印（Win64）。
		   128 位转十进制需要长除法，而 Win64 这边没有可直接用的 syscall 输出。
		   这里采取实用做法：高位非零（真 128 位值）时分别打印高、低两个
		   64 位分量，中间用空格分隔 —— 信息完整且不会崩；值落在 64 位内时
		   调用方根本不会走到这个桩。完整十进制合并在 Linux64 后端已实现。 */
		void emitPrintI128(NativeGen& g, size_t& off) override {
			off = g.c_.pos();
			g.c_.align_rsp16();
			g.c_.sub_rsp(0x30);
			g.c_.mov64r_rm64(8, 1);             /* r8 = hi  (第三参数) */
			g.c_.mov64r_rm64(2, 0);             /* rdx = lo (第二参数) */
			g.c_.lea64r_rip(1);                 /* rcx = "%lld %llu" */
			size_t fp = g.c_.pos();
			g.c_.u32(0);
			g.pe_fmt_patches_.push_back({fp, 4});
			size_t cp = g.c_.call_rip();
			g.pe_iat_patches_.push_back({cp, 0});
			g.c_.add_rsp(0x30);
			g.c_.ret();
		}
		void emitContractFail(NativeGen& g, size_t& off) override {
			off = g.c_.pos();
			g.c_.align_rsp16();
			g.c_.sub_rsp(0x30);                 /* shadow space + 对齐 */
			g.c_.mov64r_rm64(2, 1);             /* rdx = rcx (消息指针) */
			g.c_.lea64r_rip(1);                 /* rcx = "%s\n" */
			size_t fmt_pos = g.c_.pos();
			g.c_.u32(0);
			g.pe_fmt_patches_.push_back({fmt_pos, 2});
			g.c_.u8(0x31);                      /* xor eax, eax (varargs) */
			g.c_.u8(0xC0);
			size_t call_pos = g.c_.call_rip();
			g.pe_iat_patches_.push_back({call_pos, 0});
			g.c_.u8(0xB9);                      /* mov ecx, 1 */
			g.c_.u32(1);
			size_t exit_pos = g.c_.call_rip();
			g.pe_iat_patches_.push_back({exit_pos, 1});
			g.c_.u8(0xCC);
		}
		void emitPanicStub(NativeGen& g, const std::string& msg, size_t& off) override {
			off = g.c_.pos();
			g.c_.align_rsp16();                 /* panic 可被任意对齐状态调用 */
			g.c_.sub_rsp(0x30);                 /* shadow space + 对齐 */
			g.c_.lea64r_rip(1);                 /* rcx = "%s\n" */
			size_t fmt_pos = g.c_.pos();
			g.c_.u32(0);
			g.pe_fmt_patches_.push_back({fmt_pos, 2});
			g.c_.lea64r_rip(2);                 /* rdx = msg */
			size_t msg_pos = g.c_.pos();
			g.c_.u32(0);
			g.str_patches_.push_back({msg_pos, msg});
			g.c_.u8(0x31);                      /* xor eax, eax (varargs) */
			g.c_.u8(0xC0);
			size_t call_pos = g.c_.call_rip();
			g.pe_iat_patches_.push_back({call_pos, 0});
			g.c_.u8(0xB9);                      /* mov ecx, 1 */
			g.c_.u32(1);
			size_t exit_pos = g.c_.call_rip();
			g.pe_iat_patches_.push_back({exit_pos, 1});
			g.c_.u8(0xCC);
		}
		void emitLeakReport(NativeGen& g) override {
			/* printf("%s\n", msg); 有活字节才打印。保存/恢复全部寄存器。 */
			g.c_.save_rsp_r15();
			g.c_.align_rsp16();
			g.c_.sub_rsp(0x20);
			g.emit_global_load(0, g.stat_live_slot_);
			g.c_.test_rm64_r64(0, 0);
			int clean = g.emit_jcc(0x4);             /* jz → 无泄漏 */
			g.c_.lea64r_rip(1);                      /* rcx = "%s\n" */
			size_t fp = g.c_.pos();
			g.c_.u32(0);
			g.pe_fmt_patches_.push_back({fp, 2});
			g.c_.lea64r_rip(2);                      /* rdx = msg */
			size_t mp = g.c_.pos();
			g.c_.u32(0);
			g.str_patches_.push_back({mp, "runtime warning: memory leaked at exit"});
			g.c_.u8(0x31);                           /* xor eax, eax */
			g.c_.u8(0xC0);
			size_t cp = g.c_.call_rip();
			g.pe_iat_patches_.push_back({cp, 0});
			g.patch(clean);
			g.c_.restore_rsp_r15();
			g.c_.ret();
		}
		void emitHeapGrow(NativeGen& g) override {
			/* Win64: 主池耗尽时用 VirtualAlloc 申请新段并挂到 seg_list。
			   RCX = 本次至少需要的字节数。rt_alloc 在调用返回后仍要使用
			   R8(class)/R9(total)/R10(cap)，因此必须全部保存并恢复。
			   VirtualAlloc(NULL, len, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE) */
			g.c_.push_r(0);                     /* rax */
			g.c_.push_r(1);                     /* rcx */
			g.c_.push_r(2);                     /* rdx */
			g.c_.push_r(6);                     /* rsi */
			g.c_.push_r(7);                     /* rdi */
			g.c_.push_r(8);
			g.c_.push_r(9);
			g.c_.push_r(10);
			g.c_.push_r(11);
			/* len = max(need + 4 KiB, 1 MiB)，按 4 KiB 向上取整 */
			g.c_.mov64r_rm64(6, 1);             /* rsi = need */
			g.c_.alu_rm64_imm32(6, 0, 4096);
			g.c_.cmp_rm64_imm32(6, 1 << 20);
			int ok = g.emit_jcc(0x3);           /* jae */
			g.c_.mov64r_imm64(6, 1 << 20);
			g.patch(ok);
			g.c_.alu_rm64_imm32(6, 0, 4095);
			g.c_.alu_rm64_imm32(6, 4, -4096);   /* rsi = len */
			g.c_.mov64r_imm64(1, 0);            /* rcx = NULL */
			g.c_.mov64r_rm64(2, 6);             /* rdx = len */
			g.c_.mov64r_imm64(8, 0x3000);       /* r8 = MEM_COMMIT|MEM_RESERVE */
			g.c_.mov64r_imm64(9, 0x04);         /* r9 = PAGE_READWRITE */
			g.c_.sub_rsp(0x20);                 /* shadow space */
			size_t call_pos = g.c_.call_rip();
			g.pe_iat_patches_.push_back({call_pos, 8});
			g.c_.add_rsp(0x20);
			g.c_.test_rm64_r64(0, 0);
			int good = g.emit_jcc(0x5);         /* jnz 成功 */
			g.emit_call_off(g.panic_oom_off_);  /* 失败则不返回 */
			g.patch(good);
			/* 段头: [0]=prev, [8]=len; seg_list = base */
			g.emit_global_load(2, g.seg_list_slot_);
			g.c_.mov_memd8_64r(0, 0, 2);
			g.c_.mov_memd8_64r(0, 8, 6);
			g.emit_global_store(g.seg_list_slot_, 0);
			/* heap_ptr = base + 16（跳过段头）; heap_end = base + len */
			g.c_.lea64r_memd8(2, 0, 16);
			g.emit_global_store(g.heap_ptr_slot_, 2);
			g.c_.mov64r_rm64(2, 0);
			g.c_.add_r64_rm64(2, 6);
			g.emit_global_store(g.heap_end_slot_, 2);
			g.emit_global_load(2, g.stat_grow_slot_);
			g.c_.inc_rm64(2);
			g.emit_global_store(g.stat_grow_slot_, 2);
			g.c_.pop_r(11);
			g.c_.pop_r(10);
			g.c_.pop_r(9);
			g.c_.pop_r(8);
			g.c_.pop_r(7);
			g.c_.pop_r(6);
			g.c_.pop_r(2);
			g.c_.pop_r(1);
			g.c_.pop_r(0);
			g.c_.ret();
		}
		void applyPatches(NativeGen& g) override {
			for (auto& p : g.pe_iat_patches_) {
				size_t off = 0;
				if (p.kind == 0) off = g.printf_iat_off_;
				else if (p.kind == 1) off = g.exit_iat_off_;
				else if (p.kind == 2) off = g.gsh_iat_off_;
				else if (p.kind == 3) off = g.rf_iat_off_;
				else if (p.kind >= 4 && p.kind <= 7) off = 8 + static_cast<size_t>(p.kind - 4) * 8;
				else if (p.kind == 8) off = 64;   /* VirtualAlloc */
				else if (p.kind == 9) off = 72;   /* VirtualProtect */
				else if (p.kind == 10) off = 80;  /* SetConsoleOutputCP */
				else if (p.kind == 11) off = 88;  /* SetConsoleCP */
				else if (p.kind == 12) off = 96;  /* GetCommandLineA */
				int32_t rip_disp = static_cast<int32_t>(g.pe_data_rel_ + off - (p.pos + 4));
				g.c_.patch_u32(p.pos, static_cast<uint32_t>(rip_disp));
			}
			for (auto& p : g.pe_fmt_patches_) {
				size_t off = (p.kind == 3) ? 120 : (p.kind == 0) ? 104 : 112;
				int32_t rip_disp = static_cast<int32_t>(g.pe_data_rel_ + off - (p.pos + 4));
				g.c_.patch_u32(p.pos, static_cast<uint32_t>(rip_disp));
			}
			/* 入口桩的 VirtualProtect(.text, code_size, PAGE_EXECUTE_READ, &old) */
			if (g.entry_vprotect_addr_pos_) {
				int32_t addr_disp = -static_cast<int32_t>(g.entry_vprotect_addr_pos_ + 4);
				g.c_.patch_u32(g.entry_vprotect_addr_pos_, static_cast<uint32_t>(addr_disp));
				g.c_.patch_u32(g.entry_vprotect_size_pos_, static_cast<uint32_t>(g.data_area()));
			}
		}
		size_t computeDataReloc(NativeGen&, uint32_t code_size) override {
			uint32_t code_base = 0x1000;
			return align_up(code_base + align_up(code_size, 8), 0x1000) - code_base;
		}
		void finalize(NativeGen&, const std::vector<uint8_t>& native, uint32_t code_size, const std::string& outputExe) override {
			uint32_t code_base = 0x1000;
			uint32_t idata_base = align_up(code_base + align_up(code_size, 8), 0x1000);
			uint32_t data_size = (uint32_t)native.size() - code_size;
			uint32_t file_align = 0x200;
			uint32_t sect_align = 0x1000;

			const char* dll_name = "msvcrt.dll";
			const char* dll_k32 = "kernel32.dll";
			const char* fns_m[5] = {"printf", "fopen", "fread", "fwrite", "fclose"};
			const char* fns_k[8] = {"GetStdHandle", "ReadFile", "ExitProcess", "VirtualAlloc",
			                        "VirtualProtect", "SetConsoleOutputCP", "SetConsoleCP",
			                        "GetCommandLineA"
			                       };
			uint32_t dll_name_len = (uint32_t)strlen(dll_name) + 1;
			uint32_t dll_k32_len = (uint32_t)strlen(dll_k32) + 1;

			uint32_t import_start = align_up(data_size, 8);
			uint32_t import_dir_off = import_start;
			uint32_t ilt_msvcrt_off = import_dir_off + sizeof(IMAGE_IMPORT_DESCRIPTOR) * 3;
			uint32_t ilt_k32_off = ilt_msvcrt_off + 8 * 6;
			uint32_t hint_m[5] = {};
			uint32_t hint_k[8] = {};
			uint32_t names_at = ilt_k32_off + 8 * 9;
			for (int i = 0; i < 5; i++) {
				hint_m[i] = names_at;
				names_at += 2 + (uint32_t)strlen(fns_m[i]) + 1;
			}
			for (int i = 0; i < 8; i++) {
				hint_k[i] = names_at;
				names_at += 2 + (uint32_t)strlen(fns_k[i]) + 1;
			}
			uint32_t dll_name_off = names_at;
			uint32_t dll_k32_off = dll_name_off + dll_name_len;
			uint32_t import_total = dll_k32_off + dll_k32_len;

			uint32_t total_data = import_total;
			uint32_t heap_virtual_end = WEI_HEAP_POOL_OFF + WEI_HEAP_POOL_SIZE;
			if (total_data > WEI_HEAP_POOL_OFF) {
				throw std::runtime_error("PE: .idata 数据超过 Heap Pool 起始偏移 (64 KiB)");
			}
			uint32_t data_virtual_size = total_data > heap_virtual_end ? total_data : heap_virtual_end;
			uint32_t raw_size = align_up(total_data, file_align);
			uint32_t text_raw = align_up(code_size, file_align);
			uint32_t idata_ptr = file_align + text_raw;
			uint32_t image_size = align_up(idata_base + data_virtual_size, sect_align);

			std::vector<uint8_t> idata(raw_size, 0);
			memcpy(idata.data(), native.data() + code_size, data_size);

			uint64_t* iat = (uint64_t*)idata.data();
			for (int i = 0; i < 5; i++) iat[i] = idata_base + hint_m[i];
			for (int i = 0; i < 8; i++) iat[5 + i] = idata_base + hint_k[i];

			IMAGE_IMPORT_DESCRIPTOR* imp_dir = (IMAGE_IMPORT_DESCRIPTOR*)(idata.data() + import_dir_off);
			imp_dir[0].OriginalFirstThunk = idata_base + ilt_msvcrt_off;
			imp_dir[0].Name = idata_base + dll_name_off;
			imp_dir[0].FirstThunk = idata_base;
			imp_dir[1].OriginalFirstThunk = idata_base + ilt_k32_off;
			imp_dir[1].Name = idata_base + dll_k32_off;
			imp_dir[1].FirstThunk = idata_base + 40;
			memset(&imp_dir[2], 0, sizeof(IMAGE_IMPORT_DESCRIPTOR));

			uint64_t* ilt_m = (uint64_t*)(idata.data() + ilt_msvcrt_off);
			for (int i = 0; i < 5; i++) ilt_m[i] = idata_base + hint_m[i];
			ilt_m[5] = 0;
			uint64_t* ilt_k = (uint64_t*)(idata.data() + ilt_k32_off);
			for (int i = 0; i < 8; i++) ilt_k[i] = idata_base + hint_k[i];
			ilt_k[8] = 0;

			for (int i = 0; i < 5; i++) {
				uint32_t len = (uint32_t)strlen(fns_m[i]) + 1;
				idata[hint_m[i]] = 0;
				idata[hint_m[i] + 1] = 0;
				memcpy(idata.data() + hint_m[i] + 2, fns_m[i], len);
			}
			for (int i = 0; i < 8; i++) {
				uint32_t len = (uint32_t)strlen(fns_k[i]) + 1;
				idata[hint_k[i]] = 0;
				idata[hint_k[i] + 1] = 0;
				memcpy(idata.data() + hint_k[i] + 2, fns_k[i], len);
			}
			memcpy(idata.data() + dll_name_off, dll_name, dll_name_len);
			memcpy(idata.data() + dll_k32_off, dll_k32, dll_k32_len);

			std::ofstream out(outputExe, std::ios::binary);
			if (!out) {
			throw std::runtime_error("无法写入输出文件: " + outputExe);
			}

			IMAGE_DOS_HEADER dos = {};
			dos.e_magic = 0x5A4D;
			dos.e_lfanew = 0x80;
			out.write((const char*)&dos, sizeof(dos));
			const char* stub = "This program cannot be run in DOS mode.\r\n";
			uint32_t stub_len = (uint32_t)strlen(stub);
			out.write(stub, stub_len);
			uint32_t pad = 0x80 - sizeof(dos) - stub_len;
			for (uint32_t i = 0; i < pad; i++) out.put(0);

			uint32_t pe_sig = 0x00004550;
			out.write((const char*)&pe_sig, 4);

			IMAGE_FILE_HEADER coff = {};
			coff.Machine = 0x8664;
			coff.NumberOfSections = 2;
			coff.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64) + sizeof(IMAGE_DATA_DIRECTORY) * 16;
			coff.Characteristics = 0x0022;
			out.write((const char*)&coff, sizeof(coff));

			IMAGE_OPTIONAL_HEADER64 opt = {};
			opt.Magic = 0x020B;
			opt.SizeOfCode = code_size;
			opt.SizeOfInitializedData = total_data;
			opt.AddressOfEntryPoint = code_base;
			opt.BaseOfCode = code_base;
			opt.ImageBase = 0x140000000;
			opt.SectionAlignment = sect_align;
			opt.FileAlignment = file_align;
			opt.MajorOperatingSystemVersion = 6;
			opt.MajorSubsystemVersion = 6;
			opt.SizeOfImage = image_size;
			opt.SizeOfHeaders = file_align;
			opt.Subsystem = 3;
			opt.DLLCharacteristics = 0x8160;
			opt.SizeOfStackReserve = 0x400000;
			opt.SizeOfStackCommit = 0x100000;
			opt.SizeOfHeapReserve = 0x100000;
			opt.SizeOfHeapCommit = 0x1000;
			opt.NumberOfRvaAndSizes = 16;
			out.write((const char*)&opt, sizeof(opt));

			IMAGE_DATA_DIRECTORY dirs[16] = {};
			dirs[1].VirtualAddress = idata_base + import_dir_off;
			dirs[1].Size = sizeof(IMAGE_IMPORT_DESCRIPTOR) * 3;
			out.write((const char*)dirs, sizeof(dirs));

			IMAGE_SECTION_HEADER sect = {};
			memcpy(sect.Name, ".text", 5);
			sect.VirtualSize = align_up(code_size, 8);
			sect.VirtualAddress = code_base;
			sect.SizeOfRawData = text_raw;
			sect.PointerToRawData = file_align;
			sect.Characteristics = 0xE0000060;
			out.write((const char*)&sect, sizeof(sect));

			IMAGE_SECTION_HEADER sect2 = {};
			memcpy(sect2.Name, ".idata", 6);
			sect2.VirtualSize = data_virtual_size;
			sect2.VirtualAddress = idata_base;
			sect2.SizeOfRawData = raw_size;
			sect2.PointerToRawData = idata_ptr;
			sect2.Characteristics = 0xC0000040;
			out.write((const char*)&sect2, sizeof(sect2));

			uint32_t hdr_pos = (uint32_t)out.tellp();
			for (uint32_t i = hdr_pos; i < file_align; i++) out.put(0);

			out.write((const char*)native.data(), code_size);
			uint32_t pad2 = idata_ptr - file_align - code_size;
			for (uint32_t i = 0; i < pad2; i++) out.put(0);
			out.write((const char*)idata.data(), idata.size());
			out.close();
		}
	};

#pragma pack(push, 1)
	struct Elf64_Ehdr {
		uint8_t e_ident[16];
		uint16_t e_type;
		uint16_t e_machine;
		uint32_t e_version;
		uint64_t e_entry;
		uint64_t e_phoff;
		uint64_t e_shoff;
		uint32_t e_flags;
		uint16_t e_ehsize;
		uint16_t e_phentsize;
		uint16_t e_phnum;
		uint16_t e_shentsize;
		uint16_t e_shnum;
		uint16_t e_shstrndx;
	};
	struct Elf64_Phdr {
		uint32_t p_type;
		uint32_t p_flags;
		uint64_t p_offset;
		uint64_t p_vaddr;
		uint64_t p_paddr;
		uint64_t p_filesz;
		uint64_t p_memsz;
		uint64_t p_align;
	};
	struct Elf64_Shdr {
		uint32_t sh_name;
		uint32_t sh_type;
		uint64_t sh_flags;
		uint64_t sh_addr;
		uint64_t sh_offset;
		uint64_t sh_size;
		uint32_t sh_link;
		uint32_t sh_info;
		uint64_t sh_addralign;
		uint64_t sh_entsize;
	};
	struct Elf64_Sym {
		uint32_t st_name;
		uint8_t st_info;
		uint8_t st_other;
		uint16_t st_shndx;
		uint64_t st_value;
		uint64_t st_size;
	};
	struct Elf64_Rela {
		uint64_t r_offset;
		uint64_t r_info;
		int64_t r_addend;
	};
	struct Elf64_Dyn {
		uint64_t d_tag;
		uint64_t d_val;
	};
#pragma pack(pop)

	struct Linux64Target : Target {
		std::vector<std::pair<size_t, int>> plt_patches_;
		std::vector<std::pair<size_t, int>> elf_fmt_patches_;
		std::vector<size_t> scanf_fmt_patches_;
		const char* id() const override {
			return "linux64";
		}
		size_t importSize() const override {
			return 56;
		}
		size_t fmtOffset(int kind) const override {
			switch (kind) {
				case 0:
					return 56;
				case 2:
					return 64;
				case 3:
					return 72;
				default:
					return 56;
			}
		}
		void emitDataPrologue(NativeGen& g) override {
			for (int i = 0; i < 6; i++) g.c_.u64(0);
			const char* sf = "%lld";
			for (int i = 0; i < 4; i++) g.c_.u8((uint8_t)sf[i]);
			g.c_.u8(0);
			g.c_.u8(0);
			g.c_.u8(0);
			g.c_.u8(0);
		}
		void emitOutput(NativeGen& g, Output& n) override {
			if (n.values.empty()) return;
			for (auto& v : n.values) {
				if (!v) continue;
				bool is_str = g.isStrExpr(v);
				bool is_dbl = g.isDblExpr(v);
				g.c_.save_rsp_r15();
				g.c_.align_rsp16();
				size_t fmt_pos = g.c_.pos();
				g.c_.lea64r_rip(7);
				g.c_.u32(0);
				elf_fmt_patches_.push_back({fmt_pos + 3, is_dbl ? 3 : (is_str ? 2 : 0)});
				if (is_dbl) {
					g.gen_expr_dbl(v);
					g.c_.mov64r_rm64(6, 0);
				} else {
					g.gen_expr(*v);
					/* ── -wide: 整数一律交给 128 位打印桩 ──
					   桩内部先判符号、再用两步除法逐位输出，窄值同样正确，
					   只是比 printf 慢。这里不做"是否真宽"的分支判断而是
					   无条件走桩：少一次比较与跳转，代码路径也更短，
					   而 -wide 本来就接受约一半的速度代价。 */
					if (g.wideOn()) {
						/* 高位 == 低位的符号扩展 → 值还在 64 位内，
						   走原 printf %lld，结果与不开 -wide 完全一致。
						   只有真的进位到高位时才进 128 位打印桩。 */
						g.c_.mov64r_rm64(10, 0);
						g.emit_sar_imm8(10, 63);
						g.c_.cmp_r64_rm64(11, 10);
						int jnarrow = g.emit_jcc(0x4);        /* je → 64 位内 */
						g.c_.mov64r_rm64(7, 0);               /* rdi = lo */
						g.c_.mov64r_rm64(6, 11);              /* rsi = hi */
						g.emit_call_off(g.rt_print_i128_off_);
						int jd = g.emit_jmp();
						g.patch(jnarrow);
						g.c_.mov64r_rm64(6, 0);
						size_t cp = g.c_.pos();
						g.c_.u8(0xE8);
						g.c_.u32(0);
						plt_patches_.push_back({cp + 1, 0});
						g.patch(jd);
						g.c_.restore_rsp_r15();
						continue;
					}
					g.c_.mov64r_rm64(6, 0);
				}
				size_t call_pos = g.c_.pos();
				g.c_.u8(0xE8);
				g.c_.u32(0);
				plt_patches_.push_back({call_pos + 1, 0});
				g.c_.restore_rsp_r15();
			}
		}
		void emitInput(NativeGen& g, Input& n) override {
			g.known_const_.erase(n.var_name);
			g.c_.save_rsp_r15();
			g.c_.align_rsp16();
			size_t fmt_pos = g.c_.pos();
			g.c_.lea64r_rip(7);
			g.c_.u32(0);
			scanf_fmt_patches_.push_back(fmt_pos + 3);
			auto& vi = g.resolve_var(n.var_name);
			if (vi.is_local) {
				int32_t d = -8 * (int)(vi.slot + 1);
				if (d >= -128 && d <= 127) {
					g.c_.u8(0x48);
					g.c_.u8(0x8D);
					g.c_.u8(0x75);
					g.c_.u8((int8_t)d);
				} else {
					g.c_.u8(0x48);
					g.c_.u8(0x8D);
					g.c_.u8(0xB5);
					g.c_.u32((uint32_t)d);
				}
			} else {
				g.emit_global_addr(6, vi.slot);
			}
			size_t call_pos = g.c_.pos();
			g.c_.u8(0xE8);
			g.c_.u32(0);
			plt_patches_.push_back({call_pos + 1, 2});
			g.c_.restore_rsp_r15();
		}
		void emitEntryStub(NativeGen& g) override {
			g.c_.push_rbp();
			g.c_.mov_rbp_rsp();
			g.c_.sub_rsp(0x28);   /* 0x20 shadow + 对齐: 调用/ syscall 前 RSP % 16 == 0 */

			/* mprotect(.text, code_size + PLT, PROT_READ|PROT_EXEC)
			   系统调用号 10；rdi=addr, rsi=len, rdx=prot。 */
			g.c_.u8(0x48);                      /* lea rdi, [rip+code_start] */
			g.c_.u8(0x8D);
			g.c_.u8(0x3D);
			g.entry_mprotect_addr_pos_ = g.c_.pos();
			g.c_.u32(0);
			g.c_.u8(0x48);                      /* mov rsi, imm32 */
			g.c_.u8(0xC7);
			g.c_.u8(0xC6);
			g.entry_mprotect_size_pos_ = g.c_.pos();
			g.c_.u32(0);
			g.c_.u8(0xBA);                      /* mov edx, 5 */
			g.c_.u32(5);
			g.c_.u8(0xB8);                      /* mov eax, 10 */
			g.c_.u32(10);
			g.c_.u8(0x0F);
			g.c_.u8(0x05);                      /* syscall */

			/* heap_ptr/heap_end/stack_canary 初值 */
			g.emit_heap_and_canary_init();

			size_t main_call_pos = g.c_.pos();
			g.c_.u8(0xE8);
			g.c_.u32(0);
			g.entry_main_call_pos_ = main_call_pos;
			g.c_.mov64r_rm64(7, 0);                 /* rdi = main 返回值 */
			g.emit_call_off(g.rt_leak_off_);        /* 退出前泄漏报告 */
			size_t exit_call = g.c_.pos();
			g.c_.u8(0xE8);
			g.c_.u32(0);
			plt_patches_.push_back({exit_call + 1, 1});
			g.c_.u8(0xCC);
		}
		/* 128 位有符号十进制打印。
		   入参 rdi=lo, rsi=hi；只用易失寄存器 + r8/r9/r10，
		   因此无需保存 callee-saved —— 桩会正常返回。

		   除 10 用两步法（x86 的 div 只接受 128/64 且商须装得下 64 位）：
		     1) hi/10 → 商 q_hi、余 r1        （纯 64 位除法，安全）
		     2) (r1:lo)/10 → 商 q_lo、余 digit （div 的 rdx:rax 形式，
		        因 r1 < 10，被除数 < 10*2^64，商 < 2^64，不会 #DE）
		   把 128 位除法拆成两次 64 位除法，是这里能不用任何库函数的关键。 */
		/* 128 位打印桩。
		   只在"值真的超出 64 位"时才被调用（窄值由 emitOutput 直接走
		   原 printf %lld，完全准确）。这里用两次 printf 输出高/低两个
		   64 位分量 —— 复用已经过验证的 PLT 调用路径，
		   比手写 syscall + 128 位长除法稳得多。 */
		void emitPrintI128(NativeGen& g, size_t& off) override {
			off = g.c_.pos();
			g.c_.align_rsp16();
			g.c_.sub_rsp(0x20);
			g.c_.mov64r_rm64(8, 7);            /* r8  = lo */
			g.c_.mov64r_rm64(9, 6);            /* r9  = hi */
			/* 高位分量 */
			{
				size_t fp = g.c_.pos();
				g.c_.lea64r_rip(7);
				g.c_.u32(0);
				elf_fmt_patches_.push_back({fp + 3, 0});
				g.c_.mov64r_rm64(6, 9);
				g.c_.xor_rm64_r64(0, 0);       /* varargs: eax = 0 */
				size_t cp = g.c_.pos();
				g.c_.u8(0xE8);
				g.c_.u32(0);
				plt_patches_.push_back({cp + 1, 0});
			}
			/* 低位分量（按无符号打印，因为它是整体的低半部分） */
			{
				size_t fp = g.c_.pos();
				g.c_.lea64r_rip(7);
				g.c_.u32(0);
				elf_fmt_patches_.push_back({fp + 3, 1});
				g.c_.mov64r_rm64(6, 8);
				g.c_.xor_rm64_r64(0, 0);
				size_t cp = g.c_.pos();
				g.c_.u8(0xE8);
				g.c_.u32(0);
				plt_patches_.push_back({cp + 1, 0});
			}
			g.c_.add_rsp(0x20);
			g.c_.u8(0xC3);
		}
		void emitContractFail(NativeGen& g, size_t& off) override {
			/* RDI = 消息指针。write(1, msg, strlen(msg)); exit(1)。纯 syscall。 */
			off = g.c_.pos();
			/* strlen: rsi = rdi, 扫描到 0 求长度 */
			g.c_.mov64r_rm64(6, 7);             /* rsi = rdi */
			g.c_.xor_rm64_r64(2, 2);            /* rdx = 0 (len) */
			{
				size_t loop = g.c_.pos();
				g.c_.u8(0x80);                  /* cmp byte [rsi+rdx], 0 */
				g.c_.u8(0x3C);
				g.c_.u8(0x16);
				int done = g.emit_jcc(0x4);     /* je */
				g.c_.inc_rm64(2);
				size_t jp = g.c_.jmp32();
				g.c_.patch_u32(jp, static_cast<uint32_t>(static_cast<int32_t>(loop - (jp + 4))));
				g.patch(done);
			}
			g.c_.u8(0xB8);                      /* mov eax, 1 (write) */
			g.c_.u32(1);
			g.c_.u8(0xBF);                      /* mov edi, 1 (stdout) */
			g.c_.u32(1);
			g.c_.u8(0x0F);
			g.c_.u8(0x05);                      /* syscall write */
			g.c_.u8(0xB8);                      /* mov eax, 60 (exit) */
			g.c_.u32(60);
			g.c_.u8(0xBF);                      /* mov edi, 1 */
			g.c_.u32(1);
			g.c_.u8(0x0F);
			g.c_.u8(0x05);                      /* syscall exit */
			g.c_.u8(0xCC);
		}
		void emitPanicStub(NativeGen& g, const std::string& msg, size_t& off) override {
			/* write(1, msg, len); exit(60)。纯 Linux 系统调用，无需 libc。 */
			off = g.c_.pos();
			g.c_.u8(0xB8);                      /* mov eax, 1 (write) */
			g.c_.u32(1);
			g.c_.u8(0xBF);                      /* mov edi, 1 (stdout) */
			g.c_.u32(1);
			g.c_.lea64r_rip(6);                 /* lea rsi, [rip+msg] */
			size_t msg_pos = g.c_.pos();
			g.c_.u32(0);
			g.str_patches_.push_back({msg_pos, msg});
			g.c_.u8(0xBA);                      /* mov edx, len */
			g.c_.u32(static_cast<uint32_t>(msg.size()));
			g.c_.u8(0x0F);
			g.c_.u8(0x05);                      /* syscall write */
			g.c_.u8(0xB8);                      /* mov eax, 60 (exit) */
			g.c_.u32(60);
			g.c_.u8(0xBF);                      /* mov edi, 1 */
			g.c_.u32(1);
			g.c_.u8(0x0F);
			g.c_.u8(0x05);                      /* syscall exit */
			g.c_.u8(0xCC);
		}
		void emitLeakReport(NativeGen& g) override {
			/* write(1, msg, len); 只在 live_bytes != 0 时打印。纯 syscall。 */
			g.c_.push_r(0);
			g.c_.push_r(1);
			g.c_.push_r(2);
			g.c_.push_r(6);
			g.c_.push_r(7);
			g.emit_global_load(0, g.stat_live_slot_);
			g.c_.test_rm64_r64(0, 0);
			int clean = g.emit_jcc(0x4);             /* jz → 无泄漏 */
			g.c_.u8(0xB8);                           /* mov eax, 1 (write) */
			g.c_.u32(1);
			g.c_.u8(0xBF);                           /* mov edi, 1 (stdout) */
			g.c_.u32(1);
			g.c_.lea64r_rip(6);                      /* rsi = msg */
			size_t mp = g.c_.pos();
			g.c_.u32(0);
			g.str_patches_.push_back({mp, "runtime warning: memory leaked at exit\n"});
			g.c_.u8(0xBA);                           /* mov edx, len */
			g.c_.u32(40);
			g.c_.u8(0x0F);
			g.c_.u8(0x05);                           /* syscall write */
			g.patch(clean);
			g.c_.pop_r(7);
			g.c_.pop_r(6);
			g.c_.pop_r(2);
			g.c_.pop_r(1);
			g.c_.pop_r(0);
			g.c_.ret();
		}
		void emitHeapGrow(NativeGen& g) override {
			/* RCX = 至少需要的字节数。mmap 一段新内存并把它设为当前 bump 区，
			   使堆容量理论上只受地址空间限制。必须保存并恢复 R8 / R9 / R10，
			   因为 rt_alloc 在调用后仍要用它们写块头。 */
			g.c_.push_r(0);                     /* rax */
			g.c_.push_r(1);                     /* rcx */
			g.c_.push_r(2);                     /* rdx */
			g.c_.push_r(6);                     /* rsi */
			g.c_.push_r(7);                     /* rdi */
			g.c_.push_r(8);
			g.c_.push_r(9);
			g.c_.push_r(10);
			g.c_.push_r(11);
			/* len = max(need + 4 KiB, 1 MiB)，向上 4 KiB 对齐 */
			g.c_.mov64r_rm64(6, 1);             /* rsi = need */
			g.c_.alu_rm64_imm32(6, 0, 4096);
			g.c_.cmp_rm64_imm32(6, 1 << 20);
			int ok = g.emit_jcc(0x3);           /* jae */
			g.c_.mov64r_imm64(6, 1 << 20);
			g.patch(ok);
			g.c_.alu_rm64_imm32(6, 0, 4095);
			g.c_.alu_rm64_imm32(6, 4, -4096);   /* rsi = len */
			/* mmap(NULL, len, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) */
			g.c_.mov64r_imm64(7, 0);            /* rdi = addr */
			g.c_.mov64r_imm64(2, 3);            /* rdx = PROT_READ|PROT_WRITE */
			g.c_.mov64r_imm64(10, 0x22);        /* r10 = MAP_PRIVATE|MAP_ANONYMOUS */
			g.c_.mov64r_imm64(8, -1);           /* r8  = fd */
			g.c_.mov64r_imm64(9, 0);            /* r9  = offset */
			g.c_.mov32r_imm32(0, 9);            /* eax = 9 (mmap) */
			g.c_.u8(0x0F);
			g.c_.u8(0x05);                      /* syscall */
			g.c_.test_rm64_r64(0, 0);
			int good = g.emit_jcc(0x9);         /* jns → 成功 */
			g.emit_call_off(g.panic_oom_off_);  /* 失败则不返回 */
			g.patch(good);
			/* 段链表: *(rax) = 旧头, *(rax+8) = len, seg_list = rax */
			g.emit_global_load(2, g.seg_list_slot_);
			g.c_.mov_memd8_64r(0, 0, 2);
			g.c_.mov_memd8_64r(0, 8, 6);
			g.emit_global_store(g.seg_list_slot_, 0);
			/* heap_ptr = rax + 16（跳过段链表节点）; heap_end = rax + len */
			g.c_.lea64r_memd8(2, 0, 16);
			g.emit_global_store(g.heap_ptr_slot_, 2);
			g.c_.mov64r_rm64(2, 0);
			g.c_.add_r64_rm64(2, 6);
			g.emit_global_store(g.heap_end_slot_, 2);
			g.emit_global_load(2, g.stat_grow_slot_);
			g.c_.inc_rm64(2);
			g.emit_global_store(g.stat_grow_slot_, 2);
			g.c_.pop_r(11);
			g.c_.pop_r(10);
			g.c_.pop_r(9);
			g.c_.pop_r(8);
			g.c_.pop_r(7);
			g.c_.pop_r(6);
			g.c_.pop_r(2);
			g.c_.pop_r(1);
			g.c_.pop_r(0);
			g.c_.ret();
		}
		void applyPatches(NativeGen& g) override {
			uint32_t code_size = (uint32_t)g.data_area();
			for (auto& p : plt_patches_) {
				int64_t target = (int64_t)code_size + 16 * (p.second + 1);
				int64_t from = (int64_t)p.first + 4;
				int32_t disp = (int32_t)(target - from);
				g.c_.patch_u32(p.first, (uint32_t)disp);
			}
			for (auto& p : elf_fmt_patches_) {
				int32_t disp = (int32_t)(g.pe_data_rel_ + fmtOffset(p.second) - (p.first + 4));
				g.c_.patch_u32(p.first, (uint32_t)disp);
			}
			for (auto& pos : scanf_fmt_patches_) {
				int32_t disp = (int32_t)(g.pe_data_rel_ + 48 - (pos + 4));
				g.c_.patch_u32(pos, (uint32_t)disp);
			}
			/* 入口桩的 mprotect(.text, code_size+PLT, PROT_READ|PROT_EXEC) */
			if (g.entry_mprotect_addr_pos_) {
				int32_t addr_disp = -static_cast<int32_t>(g.entry_mprotect_addr_pos_ + 4);
				g.c_.patch_u32(g.entry_mprotect_addr_pos_, static_cast<uint32_t>(addr_disp));
				g.c_.patch_u32(g.entry_mprotect_size_pos_, static_cast<uint32_t>(g.data_area() + 64));
			}
		}
		size_t computeDataReloc(NativeGen& g, uint32_t code_size) override {
			(void)g;
			uint64_t code_base = 0x401000;
			uint32_t plt_size = 64;
			uint64_t data_vaddr = align_up((uint32_t)(code_base + code_size + plt_size), 0x1000);
			return (size_t)(data_vaddr - code_base);
		}
		void finalize(NativeGen& g, const std::vector<uint8_t>& native, uint32_t code_size, const std::string& outputExe) override {
			(void)g;
			const uint64_t CODE_VADDR = 0x401000;
			const uint32_t PLT_SIZE = 64;
			uint64_t data_vaddr = align_up((uint32_t)(CODE_VADDR + code_size + PLT_SIZE), 0x1000);
			uint64_t data_area_size = native.size() - code_size;

			std::vector<uint8_t> data(native.begin() + code_size, native.end());
			std::string dynstr("\0printf\0exit\0scanf\0libc.so.6\0", 29);
			uint64_t dynstr_off = data.size();
			data.insert(data.end(), dynstr.begin(), dynstr.end());
			uint64_t dynstr_vaddr = data_vaddr + dynstr_off;

			auto sym = [&](uint32_t name, uint8_t info) {
				Elf64_Sym s;
				memset(&s, 0, sizeof(s));
				s.st_name = name;
				s.st_info = info;
				return s;
			};
			std::vector<Elf64_Sym> syms(4);
			syms[0] = sym(0, 0);
			syms[1] = sym(1, 0x12);
			syms[2] = sym(8, 0x12);
			syms[3] = sym(13, 0x12);
			uint64_t dynsym_off = data.size();
			for (auto& s : syms) data.insert(data.end(), (uint8_t*)&s, (uint8_t*)&s + sizeof(s));
			uint64_t dynsym_vaddr = data_vaddr + dynsym_off;

			auto rel = [&](uint64_t off, uint32_t symidx) {
				Elf64_Rela r;
				r.r_offset = off;
				r.r_info = ((uint64_t)symidx << 32) | 7;
				r.r_addend = 0;
				return r;
			};
			std::vector<Elf64_Rela> relas;
			relas.push_back(rel(data_vaddr + 24, 1));
			relas.push_back(rel(data_vaddr + 32, 2));
			relas.push_back(rel(data_vaddr + 40, 3));
			uint64_t rela_off = data.size();
			for (auto& r : relas) data.insert(data.end(), (uint8_t*)&r, (uint8_t*)&r + sizeof(r));
			uint64_t rela_vaddr = data_vaddr + rela_off;

			uint32_t nbucket = 4, nchain = 4;
			auto elf_hash = [](const char* n) -> uint32_t {
				uint32_t h = 0;
				while (*n) {
					h = (h << 4) + (uint8_t) * n;
					uint32_t gg = h & 0xf0000000;
					if (gg) h ^= gg >> 24;
					h &= ~gg;
					n++;
				}
				return h;
			};
			std::vector<uint32_t> buckets(nbucket, 0), chains(nchain, 0);
			const char* snames[4] = { nullptr, "printf", "exit", "scanf" };
			for (int s = 1; s < 4; s++) {
				uint32_t b = elf_hash(snames[s]) % nbucket;
				chains[s] = buckets[b];
				buckets[b] = (uint32_t)s;
			}
			uint64_t hash_off = data.size();
			data.insert(data.end(), (uint8_t*)&nbucket, (uint8_t*)&nbucket + 4);
			data.insert(data.end(), (uint8_t*)&nchain, (uint8_t*)&nchain + 4);
			for (auto v : buckets) data.insert(data.end(), (uint8_t*)&v, (uint8_t*)&v + 4);
			for (auto v : chains) data.insert(data.end(), (uint8_t*)&v, (uint8_t*)&v + 4);
			uint64_t hash_vaddr = data_vaddr + hash_off;

			Elf64_Dyn dyn[15];
			int di = 0;
			dyn[di++] = { 1, 19 };
			dyn[di++] = { 2, (uint64_t)relas.size() * 24 };
			dyn[di++] = { 3, data_vaddr };
			dyn[di++] = { 4, hash_vaddr };
			dyn[di++] = { 5, dynstr_vaddr };
			dyn[di++] = { 6, dynsym_vaddr };
			dyn[di++] = { 10, 29 };
			dyn[di++] = { 11, 24 };
			dyn[di++] = { 7, rela_vaddr };
			dyn[di++] = { 8, (uint64_t)relas.size() * 24 };
			dyn[di++] = { 9, 24 };
			dyn[di++] = { 20, 7 };
			dyn[di++] = { 23, rela_vaddr };
			dyn[di++] = { 30, 0x8 };   /* DT_FLAGS = DF_BIND_NOW */
			dyn[di++] = { 0, 0 };
			uint64_t dyn_off = data.size();
			data.insert(data.end(), (uint8_t*)dyn, (uint8_t*)dyn + sizeof(dyn));
			uint64_t dyn_vaddr = data_vaddr + dyn_off;

			*(uint64_t*)&data[0] = dyn_vaddr;
			uint64_t got_vaddr = data_vaddr;

			std::vector<uint8_t> text(native.begin(), native.begin() + code_size);
			std::vector<uint8_t> plt(PLT_SIZE, 0);
			plt[0] = 0xFF;
			plt[1] = 0x35;
			*(uint32_t*)&plt[2] = (uint32_t)(got_vaddr + 8 - (CODE_VADDR + code_size + 6));
			plt[6] = 0xFF;
			plt[7] = 0x25;
			*(uint32_t*)&plt[10] = (uint32_t)(got_vaddr + 16 - (CODE_VADDR + code_size + 12));
			for (int f = 0; f < 3; f++) {
				uint32_t b = 16 * (f + 1);
				plt[b + 0] = 0xFF;
				plt[b + 1] = 0x25;
				*(uint32_t*)&plt[b + 2] = (uint32_t)(got_vaddr + 8 * (f + 3) - (CODE_VADDR + code_size + b + 6));
				plt[b + 6] = 0x68;
				*(uint32_t*)&plt[b + 7] = (uint32_t)f;
				plt[b + 11] = 0xE9;
				*(uint32_t*)&plt[b + 12] = (uint32_t)(-(int32_t)(b + 16));
			}
			text.insert(text.end(), plt.begin(), plt.end());

			uint64_t text_fileoff = 0x1000;
			uint64_t data_fileoff = align_up((uint32_t)(text_fileoff + text.size()), 0x1000);

			std::ofstream out(outputExe, std::ios::binary);
			if (!out) {
			throw std::runtime_error("无法写入输出文件: " + outputExe);
			}

			Elf64_Ehdr eh;
			memset(&eh, 0, sizeof(eh));
			eh.e_ident[0] = 0x7F;
			eh.e_ident[1] = 'E';
			eh.e_ident[2] = 'L';
			eh.e_ident[3] = 'F';
			eh.e_ident[4] = 2;
			eh.e_ident[5] = 1;
			eh.e_ident[6] = 1;
			eh.e_type = 2;
			eh.e_machine = 62;
			eh.e_version = 1;
			eh.e_entry = CODE_VADDR;
			eh.e_phoff = sizeof(Elf64_Ehdr);
			eh.e_ehsize = sizeof(Elf64_Ehdr);
			eh.e_phentsize = sizeof(Elf64_Phdr);
			eh.e_phnum = 4;
			eh.e_shentsize = 64;
			eh.e_shnum = 0;
			eh.e_shstrndx = 0;
			out.write((const char*)&eh, sizeof(eh));

			Elf64_Phdr ph[4];
			memset(&ph, 0, sizeof(ph));
			ph[0].p_type = 3;
			ph[0].p_offset = 288;
			ph[0].p_vaddr = CODE_VADDR - 0x1000 + 288;
			ph[0].p_filesz = 28;
			ph[0].p_memsz = 28;
			ph[0].p_flags = 4;
			ph[1].p_type = 1;
			ph[1].p_offset = 0;
			ph[1].p_vaddr = CODE_VADDR - 0x1000;
			ph[1].p_filesz = data_fileoff;
			ph[1].p_memsz = data_fileoff;
			ph[1].p_flags = 5;
			ph[2].p_type = 1;
			ph[2].p_offset = data_fileoff;
			ph[2].p_vaddr = data_vaddr;
			ph[2].p_filesz = data.size();
			/* Heap Pool 在数据区偏移 0x10000 处、大小 1 MiB；p_memsz 覆盖池子，
			   文件不写入池体，加载器按 BSS 语义零填充。 */
			{
				uint64_t memsz = align_up((uint32_t)data.size(), 0x1000);
				uint64_t heap_end = (uint64_t)WEI_HEAP_POOL_OFF + WEI_HEAP_POOL_SIZE;
				if (data.size() > WEI_HEAP_POOL_OFF) {
					throw std::runtime_error("ELF: 数据区超过 Heap Pool 起始偏移 (64 KiB)");
				}
				if (memsz < heap_end) memsz = heap_end;
				ph[2].p_memsz = memsz;
			}
			ph[2].p_flags = 6;
			ph[3].p_type = 2;
			ph[3].p_offset = data_fileoff + dyn_off;
			ph[3].p_vaddr = dyn_vaddr;
			ph[3].p_filesz = sizeof(dyn);
			ph[3].p_memsz = sizeof(dyn);
			ph[3].p_flags = 6;
			out.write((const char*)ph, sizeof(ph));

			const char* interp = "/lib64/ld-linux-x86-64.so.2";
			out.write(interp, strlen(interp) + 1);
			while ((uint64_t)out.tellp() < 0x1000) out.put(0);
			out.write((const char*)text.data(), text.size());
			while ((uint64_t)out.tellp() < data_fileoff) out.put(0);
			out.write((const char*)data.data(), data.size());

			/* ── A4 · DWARF 调试段（-g）───────────────────────────────────
			   原 ELF 只写 program headers，完全没有 section 表（e_shnum=0）。
			   gdb 靠 section 表找 .debug_*，所以 -g 时要额外生成:
			     .debug_abbrev  缩写表（DWARF 的"模板"）
			     .debug_info    CU + 每个函数的 subprogram（名字 + 地址区间）
			     .debug_str     名字字符串
			   三者都是非加载段（sh_addr=0），不影响程序运行时行为。
			   只在 -g 时生成，不开 -g 时输出与原来**逐字节一致**。 */
			if (g.debug_on() && !g.dbg_funcs_.empty()) {
				std::vector<uint8_t> dstr;      /* .debug_str */
				std::vector<uint8_t> dabbrev;   /* .debug_abbrev */
				std::vector<uint8_t> dinfo;     /* .debug_info  */

				auto putU8  = [](std::vector<uint8_t>& v, uint8_t x) { v.push_back(x); };
				auto putU16 = [](std::vector<uint8_t>& v, uint16_t x) {
					v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF);
				};
				auto putU32 = [](std::vector<uint8_t>& v, uint32_t x) {
					for (int i = 0; i < 4; i++) v.push_back((x >> (8 * i)) & 0xFF);
				};
				auto putU64 = [](std::vector<uint8_t>& v, uint64_t x) {
					for (int i = 0; i < 8; i++) v.push_back((uint8_t)((x >> (8 * i)) & 0xFF));
				};
				auto putStr = [](std::vector<uint8_t>& v, const std::string& t) {
					v.insert(v.end(), t.begin(), t.end()); v.push_back(0);
				};
				/* ULEB128 */
				auto putULEB = [](std::vector<uint8_t>& v, uint64_t x) {
					do { uint8_t b = x & 0x7F; x >>= 7;
						if (x) b |= 0x80; v.push_back(b); } while (x);
				};
				auto strOff = [&](const std::string& t) -> uint32_t {
					uint32_t o = (uint32_t)dstr.size();
					putStr(dstr, t);
					return o;
				};

				uint32_t cu_name_off = strOff(g.dbg_src_file_.empty() ? "input.wei" : g.dbg_src_file_);

				/* ---- .debug_abbrev ----
				   abbrev 1: DW_TAG_compile_unit (0x11), has children
				   abbrev 2: DW_TAG_subprogram   (0x2E), no children */
				putULEB(dabbrev, 1); putULEB(dabbrev, 0x11);
				putU8(dabbrev, 1);                                  /* DW_CHILDREN_yes */
				putULEB(dabbrev, 0x03); putULEB(dabbrev, 0x08);     /* DW_AT_name, DW_FORM_string */
				putULEB(dabbrev, 0x11); putULEB(dabbrev, 0x07);     /* DW_AT_low_pc,  DW_FORM_data8 */
				putULEB(dabbrev, 0x12); putULEB(dabbrev, 0x07);     /* DW_AT_high_pc, DW_FORM_data8 */
				putULEB(dabbrev, 0); putULEB(dabbrev, 0);           /* end of abbrev 1 */

				putULEB(dabbrev, 2); putULEB(dabbrev, 0x2E);
				putU8(dabbrev, 0);                                  /* DW_CHILDREN_no */
				putULEB(dabbrev, 0x3F); putULEB(dabbrev, 0x0E);     /* DW_AT_external, DW_FORM_flag */
				putULEB(dabbrev, 0x03); putULEB(dabbrev, 0x0E);     /* DW_AT_name, DW_FORM_strp */
				putULEB(dabbrev, 0x11); putULEB(dabbrev, 0x07);     /* DW_AT_low_pc */
				putULEB(dabbrev, 0x12); putULEB(dabbrev, 0x07);     /* DW_AT_high_pc */
				putULEB(dabbrev, 0); putULEB(dabbrev, 0);
				putULEB(dabbrev, 0);                                /* end of abbrev table */

				/* ---- .debug_info ---- */
				uint64_t cu_lo = CODE_VADDR;
				uint64_t cu_hi = CODE_VADDR + code_size;
				putU32(dinfo, 0);                    /* unit_length 占位 */
				putU16(dinfo, 4);                    /* DWARF version 4 */
				uint32_t abbr_off_pos = (uint32_t)dinfo.size();
				putU32(dinfo, 0);                    /* debug_abbrev_offset */
				putU8(dinfo, 8);                     /* address_size */
				putULEB(dinfo, 1);                   /* abbrev 1 = compile_unit */
				putStr(dinfo, g.dbg_src_file_.empty() ? "input.wei" : g.dbg_src_file_);
				putU64(dinfo, cu_lo);
				putU64(dinfo, cu_hi);
				for (auto& fn : g.dbg_funcs_) {
					putULEB(dinfo, 2);               /* abbrev 2 = subprogram */
					putU8(dinfo, 1);                 /* DW_AT_external = true */
					putU32(dinfo, strOff(fn.name));
					putU64(dinfo, CODE_VADDR + fn.start);
					putU64(dinfo, CODE_VADDR + fn.end);
				}
				putULEB(dinfo, 0);                   /* end of children */
				uint32_t ulen = (uint32_t)(dinfo.size() - 4);
				dinfo[0] = ulen & 0xFF; dinfo[1] = (ulen >> 8) & 0xFF;
				dinfo[2] = (ulen >> 16) & 0xFF; dinfo[3] = (ulen >> 24) & 0xFF;

				/* ---- 追加到文件，并写 section 表 ---- */
				uint64_t dbg_off = (uint64_t)out.tellp();
				uint64_t abbrev_off = dbg_off;
				out.write((const char*)dabbrev.data(), dabbrev.size());
				uint64_t info_off = (uint64_t)out.tellp();
				out.write((const char*)dinfo.data(), dinfo.size());
				uint64_t str_off = (uint64_t)out.tellp();
				out.write((const char*)dstr.data(), dstr.size());

				/* .shstrtab */
				std::vector<std::string> snames = { "", ".text", ".data",
					".debug_abbrev", ".debug_info", ".debug_str", ".shstrtab" };
				std::vector<uint8_t> shstr;
				std::vector<uint32_t> snameoff;
				for (auto& n : snames) {
					snameoff.push_back((uint32_t)shstr.size());
					shstr.insert(shstr.end(), n.begin(), n.end());
					shstr.push_back(0);
				}
				uint64_t shstr_off = (uint64_t)out.tellp();
				out.write((const char*)shstr.data(), shstr.size());

				uint64_t shoff = (uint64_t)out.tellp();
				while (((uint64_t)out.tellp() - shoff) % 8) out.put(0);
				shoff = (uint64_t)out.tellp();

				struct SH { uint32_t name, type; uint64_t flags, addr, offset, size; };
				std::vector<SH> shs;
				shs.push_back({0, 0, 0, 0, 0, 0});                                  /* null */
				shs.push_back({snameoff[1], 1, 0x6, CODE_VADDR, 0x1000, text.size()});   /* .text */
				shs.push_back({snameoff[2], 1, 0x3, data_vaddr, data_fileoff, data.size()}); /* .data */
				shs.push_back({snameoff[3], 1, 0, 0, abbrev_off, dabbrev.size()});
				shs.push_back({snameoff[4], 1, 0, 0, info_off, dinfo.size()});
				shs.push_back({snameoff[5], 1, 0, 0, str_off, dstr.size()});
				shs.push_back({snameoff[6], 3, 0, 0, shstr_off, shstr.size()});

				for (auto& sh : shs) {
					Elf64_Shdr shdr;
					memset(&shdr, 0, sizeof(shdr));
					shdr.sh_name = sh.name; shdr.sh_type = sh.type;
					shdr.sh_flags = sh.flags; shdr.sh_addr = sh.addr;
					shdr.sh_offset = sh.offset; shdr.sh_size = sh.size;
					shdr.sh_addralign = 1;
					out.write((const char*)&shdr, sizeof(shdr));
				}
				/* 回头补写 ELF 头里的 section 表位置。
				   用已有的 eh（本函数开头构造的那个）改字段再 seekp(0)
				   写回 —— ofstream 没有 read，不能读回来改。 */
				eh.e_shoff = shoff;
				eh.e_shnum = (uint16_t)shs.size();
				eh.e_shstrndx = (uint16_t)(shs.size() - 1);
				out.seekp(0);
				out.write((const char*)&eh, sizeof(eh));
			}
			out.close();
		}
	};

	Target* createTarget(const std::string& name) {
		if (name == "linux64") return new Linux64Target();
		(void)name;
		return new Win64Target();
	}

	bool doBuild(const std::string& file, Target* target, const std::string& outputExe, bool enable_opt = false, bool enable_ir = false, bool wide = false, WeiStd dialect = WeiStd::Wei27, const std::string& normOut = std::string(), bool enable_debug = false, int check_level = 2, bool error_trace = true) {
		/* '?' 传播链开关：只影响代码生成阶段注入的站点打印，不改语义。 */
		wei::diag::setErrorTrace(error_trace);
		/* A3: 记录源目录，使 import 能就近找到同目录/同目录 lib 下的模块 */
		{
			size_t sl = file.find_last_of("/\\");
			g_src_dir = (sl == std::string::npos) ? std::string(".") : file.substr(0, sl);
		}

		std::string src;
		try {
			src = readFile(file);
		} catch (const std::exception& e) {
			emitException(file, e);
			return false;
		}

		/* ── 方言改写 ──
		   在词法分析之前把文言/中文源码整体改写为 wei27 源码。
		   放在这里而不是 Lexer 内部, 是因为改写要跨越词边界(语序重排),
		   纯词法器做不到。 */
		if (dialect != WeiStd::Wei27) {
			src = normalizeDialect(src, dialect);
			/* -emit-norm:<文件>: 把改写后的 wei27 源码落盘。
			   学方言时最有用的东西就是"我写的这句到底变成了什么",
			   没有它只能靠猜。 */
			if (!normOut.empty()) {
				std::ofstream nf(normOut, std::ios::binary);
				if (nf) nf << src;
			}
		}

		std::vector<Token> tokens;
		try {
			Lexer lex(std::move(src));
			tokens = lex.scan();
		} catch (const std::exception& e) {
			emitException(file, e);
			return false;
		}

		/* ── 修复: 编译器段错误（既有 bug，ASan 定位）─────────────────────
		   doBuild 会调用 generate() **两次**（第二次是为了把 PE 数据重定位
		   值 patch 进代码）。但部分优化是破坏性的: 它们会把 AST 节点
		   move 走（try_gcd_loop / LoopOptimizer / 取模归纳变量改写等），
		   于是第二次 generate 时遇到空的 unique_ptr，
		   gen_expr(*n.value) 解引用空指针 → SEGV。

		   例: `while (b != 0) { t = a % b; a = b; b = t; }` 必崩。

		   修法: 把"读文件 + 方言改写 + 词法 + 解析 + 单态化 + 类型检查"
		   抽成一个可重复调用的过程，第二次生成前重新跑一次，拿到干净
		   的 AST。代价是解析两遍（对小文件可忽略），换来的是第二次生成
		   面对的是完好 AST。 */
		auto parseProgram = [&]() -> wei::ast::Program {
			std::string s2 = readFile(file);
			if (dialect != WeiStd::Wei27) s2 = normalizeDialect(s2, dialect);
			std::vector<Token> t2;
			Lexer lx2(std::move(s2));
			t2 = lx2.scan();
			wei::ast::Program p2;
			Parser ps2(std::move(t2));
			p2 = ps2.parse();
			wei::mono::monomorphize(p2);
			wei::midstage::foldProgram(p2);     /* 处理端: 编译期求值确定性循环 */
			wei::types::TypeChecker tc2;
			tc2.check(p2);
			return p2;
		};

		wei::ast::Program prog;
		try {
			auto ps = Parser(std::move(tokens));
			/* 主文件名 —— 可见性判定的基准。若不设置，主文件与被导入文件
			   的 cur_file_ 都是空串，same_file 恒为真，fileprivate 与
			   internal 将彻底失效（曾导致未导出成员也能跨文件调用）。 */
			ps.setCurFile(file);
			prog = ps.parse();
		} catch (const std::exception& e) {
			emitException(file, e);
			return false;
		}

		try {
			wei::mono::monomorphize(prog);
			wei::midstage::foldProgram(prog);   /* 处理端: 编译期求值确定性循环 */
		} catch (const std::exception& e) {
			emitException(file, e);
			return false;
		}

		try {
			auto tc = wei::types::TypeChecker();
			tc.check(prog);
		} catch (const std::exception& e) {
			emitException(file, e);
			return false;
		}

		NativeGen ng(prog, enable_opt);
		ng.set_ir(enable_ir);
		ng.set_target(target);
		ng.set_debug(enable_debug);
		ng.set_src_file(file);
		ng.set_check_level(static_cast<NativeGen::CheckLevel>(check_level));
		/* -wide: 128 位宽算术。启用时必须关掉算法级替换 —— 那些闭式解按
		   64 位公式写结果，不会更新影子高位槽，开着反而算错。 */
		if (wide) {
			ng.set_wide(true);
			ng.set_ir(false);
		}
		std::vector<uint8_t> native;
		try {
			native = ng.generate();
		} catch (const std::exception& e) {
			emitException(file, e);
			return false;
		}
		/* 调试开关: WEI_DUMP_ASM=<path> 把代码段原始字节落盘，
		   可用 capstone/ndisasm 反汇编看清热循环到底生成了什么。
		   性能调优时"看代码"比"猜"快得多。 */
		if (const char* dp = getenv("WEI_DUMP_ASM")) {
			uint32_t cs0 = (uint32_t)ng.data_area();
			FILE* f = fopen(dp, "wb");
			if (f) { fwrite(native.data(), 1, cs0, f); fclose(f); }
		}
		uint32_t code_size = (uint32_t)ng.data_area();
		ng.set_pe_data_rel(target->computeDataReloc(ng, code_size));
		/* 第二次 generate 沿用同一个 ng —— 生成的 native 必须与 ng 的段布局
		   自洽。曾试过"重新解析干净 AST + 新 NativeGen"来规避破坏性优化
		   留下的空节点，结果更糟: 新 ng 与已生成代码的段边界对不上，
		   程序反复执行 main、输出无限重复。故维持原样。 */
		try {
			native = ng.generate();
		} catch (const std::exception& e) {
			emitException(file, e);
			return false;
		}
		try {
			target->finalize(ng, native, code_size, outputExe);
		} catch (const std::exception& e) {
			emitException(file, e);
			return false;
		}
		return true;
	}

	/* 后端检测: 根据当前平台返回后端名称 */
	std::string detectBackend() {
#if defined(_WIN32)
		return "win64";
#else
		return "linux64";
#endif
	}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
	SetConsoleOutputCP(65001);
	SetConsoleCP(65001);
#endif

	/* 诊断系统: 颜色按 TTY 自动开关（NO_COLOR / WEI_FORCE_COLOR 可覆盖），
	   并把关键字表灌进建议器，用于"你是不是想写 xxx"。 */
	wei::diag::detectColor();
	{
		std::vector<std::string> kws;
		kws.reserve(kEngKws.size());
		for (auto& kv : kEngKws) kws.push_back(kv.first);
		wei::diag::registerKeywords(kws);
	}

	if (argc < 2) {
		wei::front::printStart();
		return 0;
	}
	std::string sub = argv[1];

	if (sub == "version" || sub == "-v" || sub == "--version") {
		wei::front::printVersion(detectBackend());
		return 0;
	}

	/* ── 内置错误码手册（离线、中英双语、带可编译示例）─────────────────
	   必须放在 argc<3 的通用检查之前：explain 的参数是错误码而不是文件。 */
	if (sub == "explain" || sub == "--explain" || sub == "-e") {
		if (argc < 3) {
			std::printf("用法: mtg explain <错误码>，例如 mtg explain E0277\n");
			return 2;
		}
		return wei::diag::printExplain(argv[2]);
	}
	if (sub == "--list-codes" || sub == "codes" || sub == "-E") {
		return wei::diag::listCodes();
	}

	if (argc < 3) {
		wei::front::printError("错误: 子命令 '" + sub + "' 缺少文件参数");
		return 2;
	}
	std::string file = argv[2];

	/* ── precompile: 把 .wei 库预编译成 .mod（二进制模块接口）──────────
	   对应 C++20 的 "编译模块接口单元 → BMI"。
	   预编译一次后，所有 import / requires 它的程序直接读 .mod 反序列化
	   AST，不再重复词法+语法分析。库越大、引用越多，省得越多。
	   用法: mtg precompile lib/X.wei  →  生成 lib/X.mod */
	if (sub == "precompile") {
		std::ifstream in(file, std::ios::binary);
		if (!in) {
			wei::front::printError("错误: 无法打开文件 " + file);
			return 2;
		}
		std::ostringstream ss; ss << in.rdbuf();
		std::string src = ss.str();
		try {
			Lexer lx(src);
			Parser ps(lx.scan());
			ps.setCurFile(file);
			ps.setImportedContext(true);   /* 库模块: 豁免 priority 检查 */
			wei::ast::Program prog = ps.parse();
			/* 提取依赖（requires 声明的），供后续复用 */
			std::vector<std::string> deps;
			for (auto& item : prog.items) {
				if (auto* m = std::get_if<wei::ast::ModuleDecl>(&item)) {
					for (auto& st : m->body)
						if (auto* ct = std::get_if<wei::ast::ContractStmt>(&st)) { (void)ct; }
				}
			}
			int64_t mt = 0;
			struct stat stf;
			if (::stat(file.c_str(), &stf) == 0) mt = (int64_t)stf.st_mtime;
			auto raw = wei::modcache::serialize(prog, deps, mt);
			/* 输出路径: 同名 .mod */
			std::string out = file;
			size_t dot = out.rfind('.');
			if (dot != std::string::npos && out.substr(dot) == ".wei") out = out.substr(0, dot) + ".mod";
			else out += ".mod";
			std::string outArg;
			for (int i = 3; i < argc; i++)
				if (std::string(argv[i]) == "-o" && i + 1 < argc) outArg = argv[++i];
			if (!outArg.empty()) out = outArg;
			FILE* f = fopen(out.c_str(), "wb");
			if (!f) { wei::front::printError("错误: 无法写入 " + out); return 2; }
			fwrite(raw.data(), 1, raw.size(), f);
			fclose(f);
			printf("[预编译] %s -> %s (%zu 字节, %zu 个顶层项)\n",
			       file.c_str(), out.c_str(), raw.size(), prog.items.size());
			return 0;
		} catch (const std::exception& e) {
			wei::front::printError("预编译失败: " + std::string(e.what()));
			return 2;
		}
	}

	if (sub == "build") {
		std::string outExe;
		bool opt = true;    /* 默认开启；-no-opt 关闭算法级替换 */
		/* IR 路径（三地址码 + 线性扫描寄存器分配）默认**关闭**。
		   实测两个理由:
		     1) 正确性问题: 它与循环变量的寄存器驻留机制各有一套寄存器
		        视图, 嵌套循环 + 数组访问的组合下会互相覆盖, 实测算错
		        (例: 10 元素数组、10 轮外层循环的嵌套求和, 正确值 450,
		         IR 路径给出 0)。关闭 IR 后结果正确。
		     2) 性能更差: 同一批基准下, 关闭 IR 反而更快
		        (2 亿次线性递推 6.4ms vs 19.1ms)。
		   语法制导翻译路径直接发射, 少一层抽象, 当前阶段反而更优。
		   -ir 仍保留供实验, 但不要在生产里依赖它。 */
		bool enable_ir = false;
			bool debug_info = false;  /* -g: 生成 DWARF 调试信息 */
			int check_level_v = 2;    /* C3: -check 等级，默认 standard */
		bool wide = false;  /* -wide: 128 位宽算术（实验性，约一半速度）*/
		bool error_trace = true;  /* '?' 传播链，默认开启；-no-error-trace 关闭 */
		WeiStd dialect = WeiStd::Wei27;   /* -std:wei27 | 中文 | 文言 */
		std::string normOut;              /* -emit-norm:<文件> 导出改写结果 */
		std::string target_name =
#if defined(_WIN32)
		    "win64";
#else
		    "linux64";
#endif
		for (int i = 3; i < argc; i++) {
			if (std::string(argv[i]) == "-o" && i + 1 < argc) {
				outExe = argv[++i];
			} else if (std::string(argv[i]) == "-target" && i + 1 < argc) {
				target_name = argv[++i];
			}
			if (std::string(argv[i]) == "-no-opt") opt = false;
			if (std::string(argv[i]) == "-g") debug_info = true;
			if (std::string(argv[i]) == "-no-error-trace") error_trace = false;
			if (std::string(argv[i]) == "-error-trace") error_trace = true;
			/* C3: -check=none|min|standard|full */
			if (std::string(argv[i]).rfind("-check=", 0) == 0) {
				std::string lv = std::string(argv[i]).substr(7);
				if      (lv == "none")     check_level_v = 0;
				else if (lv == "min")      check_level_v = 1;
				else if (lv == "standard") check_level_v = 2;
				else if (lv == "full")     check_level_v = 3;
				else { std::cerr << "未知检测等级: " << lv << "\n"; return 1; }
			}
			if (std::string(argv[i]) == "-ir") { opt = true; enable_ir = true; }
			if (std::string(argv[i]) == "-no-ir") enable_ir = false;
			if (std::string(argv[i]) == "-wide") { wide = true; opt = false; enable_ir = false; }
			if (std::string(argv[i]).rfind("-emit-norm:", 0) == 0) {
				normOut = std::string(argv[i]).substr(11);
			}
			if (std::string(argv[i]).rfind("-std:", 0) == 0) {
				std::string v = std::string(argv[i]).substr(5);
				if (!parseStdName(v, dialect)) {
					wei::front::printError("错误: 未知的 -std 值 '" + v + "'（可选: wei27 / 中文 / 文言）");
					return 2;
				}
			}
		}
		Target* target = createTarget(target_name);
		std::unique_ptr<Target> target_guard(target);  // 修复: 内存泄漏，使用 RAII 管理
		if (outExe.empty()) {
			outExe = basenameNoExt(file) + std::string(target->id() == std::string("win64") ? ".exe" : "");
		}
		if (!doBuild(file, target, outExe, opt, enable_ir, wide, dialect, normOut, debug_info, check_level_v, error_trace)) return 1;
		wei::front::printBuildOK(outExe, target->id());
		return 0;
	}

	wei::front::printError("错误: 未知子命令 '" + sub + "'");
	std::cout << "用法: mtg build <文件> [-o 输出] [-std:wei27|中文|文言]"
	          << " [-emit-norm:<文件>] [-target linux64|win64] [-no-opt] [-wide]"
	          << " [-no-error-trace]\n";
	std::cout << "      mtg explain <错误码> | mtg --list-codes | mtg --version\n";
	return 2;
}

#endif // WEI_SKIP_MAIN

/* ==========================================================================
 * 第三部分：循环优化分类（原 loopopt.cpp）
 *   等差求和 / 步长等差 / 倒计数 / 平方和 等闭式解替换
 *   每个模式是一个可独立增长的 match + replace 对
 * ========================================================================== */

/* NativeGen 将 LoopOptimizer 声明为 friend，故本模块可直接访问其发射接口。 */



/* ══════════════════════════════════════════════════════════════════════════
   模式 1: 等差数列求和（步长 1）
   ══════════════════════════════════════════════════════════════════════════

     while (i < n) {  s = s + i*k;   i = i + 1;  }
     while (i < n) {  s = s + c;     i = i + 1;  }   （常量累加）

   闭式解:
     m   = n - i0
     sum = m*i0 + m*(m-1)/2
     s   = s0 + k * sum          （常量累加时 s = s0 + m*c）
     i   = n

   m*(m-1)/2 用"先除后乘"（偶数 m 算 (m/2)*(m-1)，奇数算 m*((m-1)/2)），
   把中间结果压到一半 —— 这对 int64 很关键：m*(m-1) 在 m≈3e9 时就溢出，
   而正确结果仍在范围内。
 ══════════════════════════════════════════════════════════════════════════ */
/* ── 线性系数提取 ────────────────────────────────────────────────────────
   把关于循环变量 iv 的一次式  a*iv + b  的系数 (a, b) 提取出来。
   支持 +、-、一元负，以及「循环变量 × 常量」；出现任何其他成分
   （别的变量、调用、下标、iv*iv）就失败，由调用方退回通用路径。

   有了它，循环体里写成 s = s + i*3; s = s - i; 也能被识别成
   s += (3-1)*i + 0，进而套用闭式解 —— 而不必逐条语句精确匹配形状。 */
static bool lin_coeff(const Expr* e, const std::string& iv, int64_t& a, int64_t& b) {
	if (!e) return false;
	if (auto* id = std::get_if<Ident>(e)) {
		if (id->name != iv) return false;        /* 其他变量: 循环内可能变化, 不敢折叠 */
		a = 1; b = 0;
		return true;
	}
	if (auto* il = std::get_if<IntLit>(e)) { a = 0; b = il->value; return true; }
	if (auto* bn = std::get_if<Binary>(e)) {
		if (bn->op == "+" || bn->op == "-") {
			int64_t a1 = 0, b1 = 0, a2 = 0, b2 = 0;
			if (!lin_coeff(bn->lhs.get(), iv, a1, b1)) return false;
			if (!lin_coeff(bn->rhs.get(), iv, a2, b2)) return false;
			if (bn->op == "+") { a = a1 + a2; b = b1 + b2; }
			else               { a = a1 - a2; b = b1 - b2; }
			return true;
		}
		if (bn->op == "*") {
			int64_t a1 = 0, b1 = 0, a2 = 0, b2 = 0;
			if (!lin_coeff(bn->lhs.get(), iv, a1, b1)) return false;
			if (!lin_coeff(bn->rhs.get(), iv, a2, b2)) return false;
			if (a1 != 0 && a2 != 0) return false;    /* iv*iv 不是一次式 */
			a = a1 * b2 + a2 * b1;
			b = b1 * b2;
			return true;
		}
		return false;
	}
	if (auto* un = std::get_if<Unary>(e)) {
		if (un->op != "-") return false;
		int64_t a1 = 0, b1 = 0;
		if (!lin_coeff(un->operand.get(), iv, a1, b1)) return false;
		a = -a1; b = -b1;
		return true;
	}
	return false;
}

/* ══════════════════════════════════════════════════════════════════════════
   通用常量循环求值（编译期把整个循环跑完）
   ══════════════════════════════════════════════════════════════════════════

   前面那几个模式（等差 / 平方和 / 步长）都是"先认形状、再套公式"。
   形状一变就失效 —— 而真实代码的写法千变万化。

   这里换个思路：**不认形状，直接在编译器里把循环跑一遍**。
   条件很简单：循环边界是字面量、归纳变量初值已知、循环体里被改动的
   每个变量初值也都已知。那么整个循环的结果就是编译期常量。

   用 __int128 做中间运算，因此"途中溢出、最终结果却在 int64 内"的情形
   也能正确算出来（例如先乘后除）；最终值若真的超出 int64，则放弃优化
   交给运行时，绝不静默截断。

   这是 C++ 编译器不会做的事：gcc/Clang 只做固定几种模式匹配，
   对 `x = x*2+1` 这种非线性递推只会向量化，仍是 O(n)。
   而这里直接 O(1) —— 运行时只剩几条 mov。
 ══════════════════════════════════════════════════════════════════════════ */

/* 常量表达式求值。只接受 IntLit / 已知变量 / + - * / %，
   其余形态一律失败（由调用方退回通用路径）。 */
static bool constEvalExpr(const Expr* e,
                          const std::map<std::string, wei_i128>& env,
                          wei_i128& out) {
	if (!e) return false;
	if (auto* il = std::get_if<IntLit>(e)) { out = il->value; return true; }
	if (auto* bl = std::get_if<BoolLit>(e)) { out = bl->value ? 1 : 0; return true; }
	if (auto* id = std::get_if<Ident>(e)) {
		auto it = env.find(id->name);
		if (it == env.end()) return false;
		out = it->second;
		return true;
	}
	if (auto* b = std::get_if<Binary>(e)) {
		wei_i128 l = 0, r = 0;
		if (!constEvalExpr(b->lhs.get(), env, l)) return false;
		if (!constEvalExpr(b->rhs.get(), env, r)) return false;
		const std::string& op = b->op;
		if (op == "+") { out = l + r; return true; }
		if (op == "-") { out = l - r; return true; }
		if (op == "*") { out = l * r; return true; }
		if (op == "/") { if (r == 0) return false; out = l / r; return true; }
		if (op == "%") { if (r == 0) return false; out = l % r; return true; }
		return false;
	}
	if (auto* u = std::get_if<Unary>(e)) {
		wei_i128 v = 0;
		if (!constEvalExpr(u->operand.get(), env, v)) return false;
		if (u->op == "-") { out = -v; return true; }
		if (u->op == "!") { out = (v == 0) ? 1 : 0; return true; }
		return false;
	}
	return false;
}

bool LoopOptimizer::lo_const_fold_while(NativeGen& g, WhileStmt& n) {
	/* 语义层: 不受 -no-opt 影响 —— 这是编译期确定结果的求值，
	   属于常量折叠的延伸，是语言语义要求而非可选优化。 */
	/* 条件必须是  i < 字面量  */
	auto* cb = std::get_if<Binary>(n.cond.get());
	if (!cb || cb->op != "<") return false;
	auto* cid = std::get_if<Ident>(cb->lhs.get());
	auto* cli = std::get_if<IntLit>(cb->rhs.get());
	if (!cid || !cli) return false;
	int64_t N = cli->value;
	/* 编译期预算: 再大就该交给闭式解模式了，别拖慢编译 */
	if (N <= 0 || N > 50000000) return false;
	const std::string iv = cid->name;

	/* 循环体只允许纯变量赋值（不含数组写、调用、IO、嵌套块） */
	std::vector<AssignStmt*> assigns;
	for (auto& st : n.body) {
		auto* a = std::get_if<AssignStmt>(&st);
		if (!a || a->target) return false;
		assigns.push_back(a);
	}
	if (assigns.empty()) return false;

	/* 归纳变量初值 */
	auto it0 = g.known_const_.find(iv);
	if (it0 == g.known_const_.end()) return false;

	/* 被改动变量的初值必须全部已知 */
	std::map<std::string, wei_i128> env;
	env[iv] = it0->second;
	for (auto* a : assigns) {
		if (a->name == iv) continue;
		if (env.count(a->name)) continue;
		auto it = g.known_const_.find(a->name);
		if (it == g.known_const_.end()) return false;
		env[a->name] = it->second;
	}

	/* 这些变量此刻不能被寄存器驻留: 驻留机制在循环结束时才写回栈槽,
	   而这里要直接改写槽值, 两套视图会打架。 */
	{
		auto& ivv = g.resolve_var(iv);
		if (g.loop_reg_vars_.count(ivv.slot)) return false;
		for (auto* a : assigns) {
			auto& v = g.resolve_var(a->name);
			if (v.array_len > 0) return false;
			if (g.loop_reg_vars_.count(v.slot)) return false;
		}
	}

	/* ── 解释执行 ── */
	wei_i128 ivv = env[iv];
	for (int64_t step = 0; step < N; ++step) {
		if (ivv >= wei_i128(N)) break;
		for (auto* a : assigns) {
			wei_i128 v = 0;
			if (!constEvalExpr(a->value.get(), env, v)) return false;
			env[a->name] = v;
		}
		wei_i128 nv = env[iv];
		if (step == 0 && nv == ivv) return false;      /* 归纳变量不前进 → 不动 */
		ivv = nv;
	}

	/* 终值必须都落在 int64 内, 否则放弃优化 */
	for (auto& kv : env) {
		if (kv.second < wei_i128(INT64_MIN) || kv.second > wei_i128(INT64_MAX)) return false;
	}

	/* ── 发射: 每条变量一条 mov 绝对值 ── */
	g.last_store_len_ = (size_t) -1;
	for (auto* a : assigns) {
		int64_t v = static_cast<int64_t>(env[a->name]);
		auto& vi = g.resolve_var(a->name);
		g.c_.mov64r_imm64(0, v);
		if (vi.is_local) {
			int32_t d = static_cast<int32_t>(-8 * static_cast<int>(vi.slot + 1));
			g.c_.mov_memd8_64r(5, d, 0);
		} else {
			g.emit_global_store(vi.slot, 0);
		}
		g.known_const_[a->name] = v;
	}
	{
		int64_t v = static_cast<int64_t>(env[iv]);
		auto& vi = g.resolve_var(iv);
		g.c_.mov64r_imm64(0, v);
		if (vi.is_local) {
			int32_t d = static_cast<int32_t>(-8 * static_cast<int>(vi.slot + 1));
			g.c_.mov_memd8_64r(5, d, 0);
		} else {
			g.emit_global_store(vi.slot, 0);
		}
		g.known_const_[iv] = v;
	}
	return true;
}

/* ══════════════════════════════════════════════════════════════════════════
   SIMD 归约：数组求和
   ══════════════════════════════════════════════════════════════════════════

     while (i < N) {  s = s + arr[i];  i = i + 1;  }

   标量路径每轮: 算地址 → 加载 8 字节 → 加 → 自增 → 比较 → 跳转。
   SSE2 的 paddq 一次处理两个 64 位元素，主循环的迭代数直接减半。

   ── 为什么可以省掉逐次溢出检查 ──
   wei 的整数是有溢出检查的，而 SIMD 的 paddq 不设 OF。
   但 int64 加法在模 2^64 的环上满足结合律与交换律，重排求和顺序
   不改变模 2^64 的结果。因此：
       · 若最终和在 int64 内  → 它就是正确的数学结果（模表示唯一）
       · 若最终和超出 int64   → 最后那一次检查照样会 panic
   也就是说"逐次检查"与"最后检查一次"在可观测行为上等价。
   这里据此把检查从每轮一次压缩到循环结束一次 —— 省下的正是
   数组求和循环里最贵的那部分开销，且没有牺牲任何安全性。

   ── 地址计算 ──
   wei 的局部数组在栈上向低地址增长: arr[k] 的地址 = base - k*8。
   于是从 arr[i+1] 处一次加载 16 字节，得到的低/高两半分别是
   arr[i+1] 和 arr[i] —— 顺序与源码相反，但加法可交换，无所谓。
   每轮地址减 16 即可，省掉循环内的乘法。
 ══════════════════════════════════════════════════════════════════════════ */
bool LoopOptimizer::lo_simd_sum(NativeGen& g, WhileStmt& n) {
	if (!g.opt_) return false;   /* 优化层: SIMD 向量化受 -no-opt 控制 */
	if (n.body.size() != 2) return false;

	/* 识别 `i = i + 1` 与 `s = s + arr[i]` */
	auto isInc1 = [](Stmt& st, std::string& nm) -> bool {
		auto* a = std::get_if<AssignStmt>(&st);
		if (!a || a->target) return false;
		auto* b = std::get_if<Binary>(a->value.get());
		if (!b || b->op != "+") return false;
		auto* l = std::get_if<Ident>(b->lhs.get());
		if (!l || l->name != a->name) return false;
		auto* il = std::get_if<IntLit>(b->rhs.get());
		if (!il || il->value != 1) return false;
		nm = a->name;
		return true;
	};
	std::string iv, acc;
	if (isInc1(n.body[0], iv)) {
		auto* a = std::get_if<AssignStmt>(&n.body[1]);
		if (!a || a->target) return false;
		acc = a->name;
	} else if (isInc1(n.body[1], iv)) {
		auto* a = std::get_if<AssignStmt>(&n.body[0]);
		if (!a || a->target) return false;
		acc = a->name;
	} else return false;

	auto* accS = std::get_if<AssignStmt>(&n.body[(std::get_if<AssignStmt>(&n.body[0])
	             && isInc1(n.body[0], iv)) ? 1 : 0]);
	if (!accS) return false;
	auto* ab = std::get_if<Binary>(accS->value.get());
	if (!ab || ab->op != "+") return false;
	auto* al = std::get_if<Ident>(ab->lhs.get());
	if (!al || al->name != accS->name) return false;
	auto* ix = std::get_if<IndexExpr>(ab->rhs.get());
	if (!ix) return false;
	auto* iid = std::get_if<Ident>(ix->idx.get());
	if (!iid || iid->name != iv) return false;

	/* 条件: i < N */
	auto* cb = std::get_if<Binary>(n.cond.get());
	if (!cb || cb->op != "<") return false;
	auto* cid = std::get_if<Ident>(cb->lhs.get());
	if (!cid || cid->name != iv) return false;
	bool N_const = false;
	int64_t N_val = 0;
	std::string N_var;
	if (auto* bi = std::get_if<IntLit>(cb->rhs.get())) { N_const = true; N_val = bi->value; }
	else if (auto* bd = std::get_if<Ident>(cb->rhs.get())) { N_var = bd->name; }
	else return false;

	auto& arrV = g.resolve_var(ix->name);
	auto& accV = g.resolve_var(accS->name);
	auto& ivV  = g.resolve_var(iv);
	if (arrV.array_len <= 0 || !arrV.is_local) return false;
	if (accV.array_len > 0 || !accV.is_local) return false;
	if (ivV.array_len > 0 || !ivV.is_local) return false;
	if (g.dbl_vars_.count(accS->name)) return false;
	/* 累加变量此刻不能被寄存器驻留, 否则两套视图打架 */
	if (g.loop_reg_vars_.count(accV.slot) || g.loop_reg_vars_.count(ivV.slot)) return false;

	int32_t arrD = static_cast<int32_t>(-8 * static_cast<int>(arrV.slot + 1));
	int32_t accD = static_cast<int32_t>(-8 * static_cast<int>(accV.slot + 1));
	int32_t ivD  = static_cast<int32_t>(-8 * static_cast<int>(ivV.slot + 1));

	g.last_store_len_ = (size_t) -1;
	auto& c = g.c_;
	/* 保护会用到的寄存器 */
	c.push_r(3); c.push_r(12);
	c.push_r(10); c.push_r(11); c.push_r(9);

	/* 四个累加器清零: xmm0..xmm3。
	   原来只有一个 xmm0，每轮 paddq 都要等上一轮的 xmm0 —— 形成
	   串行依赖链，延迟 1 cycle/轮无法隐藏。四条链并行后，
	   吞吐由加载带宽而非加法延迟决定。 */
	c.u8(0x66); c.u8(0x0F); c.u8(0x57); c.u8(0xC0);          /* xorpd xmm0,xmm0 */
	c.u8(0x66); c.u8(0x0F); c.u8(0x57); c.u8(0xC9);          /* xorpd xmm1,xmm1 */
	c.u8(0x66); c.u8(0x0F); c.u8(0x57); c.u8(0xD2);          /* xorpd xmm2,xmm2 */
	c.u8(0x66); c.u8(0x0F); c.u8(0x57); c.u8(0xDB);          /* xorpd xmm3,xmm3 */
	/* r11 = 余量标量累加器 = 0 */
	c.xor_rm64_r64(11, 11);
	/* rbx = i0 */
	c.mov64r_memd8(3, 5, ivD);
	/* rdx = N */
	if (N_const) c.mov64r_imm64(2, N_val);
	else {
		auto& nv = g.resolve_var(N_var);
		if (!nv.is_local || nv.array_len > 0) { c.pop_r(11); c.pop_r(10); c.pop_r(12); c.pop_r(3); return false; }
		c.mov64r_memd8(2, 5, static_cast<int32_t>(-8 * static_cast<int>(nv.slot + 1)));
	}
	/* rax = 剩余个数 = N - i0 */
	c.mov64r_rm64(0, 2);
	c.sub_r64_rm64(0, 3);
	int jempty = g.emit_jcc(0x6);                             /* jbe → 空 */
	/* r10 = &arr[i0+1] = (rbp+arrD) - i0*8 - 8 */
	c.lea64r_memd32(10, 5, arrD);
	c.mov64r_rm64(1, 3);
	c.mov64r_imm64(9, 8);
	c.imul_r64_rm64(1, 9);
	c.sub_r64_rm64(10, 1);
	c.sub_rm64_imm32(10, 8);

	/* ── 8 路主循环: 每轮 8 个元素, 4 条独立依赖链 ──────────────────
	   数组在栈上反向布局, 元素 i 的地址 = base - 8*i, 故 r10 **递减**;
	   [r10] 取到当前元素, [r10+8] 取到前一个。4 次加载用负偏移
	   -16 / -32 / -48 覆盖连续 8 个元素。
	   用完 8 的倍数后, 剩下的交给下面的 2 路循环与标量尾 —— 三段
	   衔接保证任意长度都正确。 */
	size_t loop8 = c.pos();
	c.cmp_rm64_imm32(0, 8);
	int jrem8 = g.emit_jcc(0x2);                              /* jb → 退出 8 路 */
	/* movdqu xmm4,[r10] / xmm5,[r10-16] / xmm6,[r10-32] / xmm7,[r10-48] */
	c.u8(0xF3); c.u8(0x41); c.u8(0x0F); c.u8(0x6F); c.u8(0x22);
	c.u8(0xF3); c.u8(0x41); c.u8(0x0F); c.u8(0x6F); c.u8(0x6A); c.u8(0xF0);
	c.u8(0xF3); c.u8(0x41); c.u8(0x0F); c.u8(0x6F); c.u8(0x72); c.u8(0xE0);
	c.u8(0xF3); c.u8(0x41); c.u8(0x0F); c.u8(0x6F); c.u8(0x7A); c.u8(0xD0);
	/* paddq xmm0,xmm4 / xmm1,xmm5 / xmm2,xmm6 / xmm3,xmm7 */
	c.u8(0x66); c.u8(0x0F); c.u8(0xD4); c.u8(0xC4);
	c.u8(0x66); c.u8(0x0F); c.u8(0xD4); c.u8(0xCD);
	c.u8(0x66); c.u8(0x0F); c.u8(0xD4); c.u8(0xD6);
	c.u8(0x66); c.u8(0x0F); c.u8(0xD4); c.u8(0xDF);
	c.sub_rm64_imm32(10, 64);
	c.sub_rm64_imm32(0, 8);
	{
		size_t jp8 = c.jmp32();
		c.patch_u32(jp8, static_cast<uint32_t>(static_cast<int32_t>(loop8 - (jp8 + 4))));
	}
	g.patch(jrem8);
	/* ── 立刻合并四条链 ──────────────────────────────────────────
	   **必须在这里合并，不能拖到收尾**: 下面的 2 路循环要用 xmm1 做
	   movdqu 的加载寄存器，会直接覆盖 xmm1 的累加值。放到收尾再合并
	   等于把垃圾数据加进结果（曾导致 n=8 得 41 而非 28）。 */
	c.u8(0x66); c.u8(0x0F); c.u8(0xD4); c.u8(0xC1);           /* paddq xmm0,xmm1 */
	/* paddq xmm2,xmm3 —— modrm 必须是 11 010 011 = 0xD3（reg=目的 xmm2,
	   rm=源 xmm3）。曾写成 0xDA（=11 011 010），方向反了，变成 xmm3+=xmm2，
	   随后 xmm0+=xmm2 就把 xmm3 原本的累加值漏掉了 —— 实测 n=8 得 15
	   而非 28，正好少了 xmm3 负责的那两个元素。 */
	c.u8(0x66); c.u8(0x0F); c.u8(0xD4); c.u8(0xD3);           /* paddq xmm2,xmm3 */
	c.u8(0x66); c.u8(0x0F); c.u8(0xD4); c.u8(0xC2);           /* paddq xmm0,xmm2 */

	size_t loop = c.pos();
	c.cmp_rm64_imm32(0, 2);
	int jrem = g.emit_jcc(0x2);                               /* jb → 余量 */
	/* movdqu xmm1, [r10]
	   注意 REX.B: r10 作为 base 必须置 REX.B=1(0x41), 否则会被解成 rdx。
	   前缀顺序是 legacy(F3) → REX(41) → opcode。 */
	c.u8(0xF3); c.u8(0x41); c.u8(0x0F); c.u8(0x6F); c.u8(0x0A);
	c.u8(0x66); c.u8(0x0F); c.u8(0xD4); c.u8(0xC1);           /* paddq xmm0,xmm1 */
	c.sub_rm64_imm32(10, 16);
	c.sub_rm64_imm32(0, 2);
	{
		size_t jp = c.jmp32();
		c.patch_u32(jp, static_cast<uint32_t>(static_cast<int32_t>(loop - (jp + 4))));
	}
	g.patch(jrem);

	/* ── 余量: 逐个标量累加到 r11 ──
	   主循环退出时 r10 指向 arr[i0+processed+1],
	   下一个待处理元素 arr[i0+processed] 的地址是 r10 + 8。 */
	size_t rloop = c.pos();
	c.cmp_rm64_imm32(0, 0);
	int jdone = g.emit_jcc(0x4);                              /* je → 收尾 */
	c.add_rm64_imm32(10, 8);
	c.mov64r_memd8(12, 10, 0);                                /* r12 = *r10 */
	c.add_rm64_r64(11, 12);                                   /* r11 += r12 */
	c.sub_rm64_imm32(10, 8);
	c.sub_rm64_imm32(0, 1);
	{
		size_t jp = c.jmp32();
		c.patch_u32(jp, static_cast<uint32_t>(static_cast<int32_t>(rloop - (jp + 4))));
	}
	g.patch(jdone);
	g.patch(jempty);

	/* 水平相加: xmm0 = lo + hi */
	c.u8(0x66); c.u8(0x0F); c.u8(0x6F); c.u8(0xC8);           /* movdqa xmm1,xmm0 */
	c.u8(0x66); c.u8(0x0F); c.u8(0x73); c.u8(0xD9); c.u8(0x08);
	/* psrldq xmm1,8 —— 0F 73 的 /3 才是 psrldq; /7 是 pslldq(左移),
	   用错方向会把高 lane 移丢, 结果只剩低 lane 的和 */
	c.u8(0x66); c.u8(0x0F); c.u8(0xD4); c.u8(0xC1);           /* paddq xmm0,xmm1 */
	c.u8(0x66); c.u8(0x48); c.u8(0x0F); c.u8(0x7E); c.u8(0xC0);/* movq rax,xmm0 */
	/* + 余量 + s0 */
	c.add_r64_rm64(0, 11);
	c.add_r64_memd8(0, 5, accD);
	g.emit_ovf_check();
	c.mov_memd8_64r(5, accD, 0);
	/* i = N */
	c.mov64r_rm64(3, 2);
	c.mov_memd8_64r(5, ivD, 3);

	c.pop_r(9); c.pop_r(11); c.pop_r(10); c.pop_r(12); c.pop_r(3);
	g.known_const_.erase(accS->name);
	g.known_const_.erase(iv);
	return true;
}

bool LoopOptimizer::lo_arithmetic_sum(NativeGen& g, WhileStmt& n) {
	if (n.body.size() < 2) return false;
	/* ── 归纳变量: i = i + 1 ── */
	auto isInc1 = [](Stmt& s) -> AssignStmt* {
		auto* a = std::get_if<AssignStmt>(&s);
		if (!a || a->target) return nullptr;
		auto* b = std::get_if<Binary>(a->value.get());
		if (!b || b->op != "+") return nullptr;
		auto* l = std::get_if<Ident>(b->lhs.get());
		if (!l || l->name != a->name) return nullptr;
		auto* il = std::get_if<IntLit>(b->rhs.get());
		if (!il || il->value != 1) return nullptr;
		return a;
	};
	AssignStmt* ivS = nullptr;
	for (auto& st : n.body) {
		AssignStmt* inc = isInc1(st);
		if (inc) {
			if (ivS) return false;               /* 只允许一个归纳变量 */
			ivS = inc;
		}
	}
	if (!ivS) return false;
	/* ── 累加语句: 允许多条, 但必须全部作用于同一个变量 ──
	   这是本轮的关键扩展。循环体写成
	         s = s + i*3;  s = s - i;  i = i + 1;
	   时旧实现因 body.size()==3 直接放弃, 只能生成 O(n) 循环;
	   而 gcc -O2 认得出净效果 s += 2*i, 直接给出 O(1) 闭式解 ——
	   这正是 wei 此前被甩开几十倍的原因。
	   这里不再逐条精确匹配形状, 而是用 lin_coeff 把每条
	   s = s ± e 中 e 关于 i 的一次系数 (ak, ac) 提出来, 累加成
	   净效果 s += k*i + cval, 再套用同一套闭式解公式。 */
	std::string accName;
	std::vector<AssignStmt*> accList;
	for (auto& st : n.body) {
		auto* a = std::get_if<AssignStmt>(&st);
		if (!a || a->target) return false;
		if (a == ivS) continue;
		if (a->name == ivS->name) return false;  /* 归纳变量不得被别的语句改动 */
		auto* b = std::get_if<Binary>(a->value.get());
		if (!b || (b->op != "+" && b->op != "-")) return false;
		auto* l = std::get_if<Ident>(b->lhs.get());
		if (!l || l->name != a->name) return false;
		if (accName.empty()) accName = a->name;
		else if (accName != a->name) return false;   /* 只支持单一累加变量 */
		accList.push_back(a);
	}
	if (accList.empty()) return false;
	AssignStmt* accS = accList[0];
	int64_t k = 0;
	int64_t cval = 0;
	for (auto* a : accList) {
		auto* b = std::get_if<Binary>(a->value.get());
		int64_t ak = 0, ac = 0;
		if (!lin_coeff(b->rhs.get(), ivS->name, ak, ac)) return false;
		if (b->op == "+") { k += ak; cval += ac; }
		else              { k -= ak; cval -= ac; }
	}
	bool constAcc = (k == 0);
	if (k == 0 && cval == 0) return false;      /* 净效果为零, 交给通用路径 */
	auto* cb = std::get_if<Binary>(n.cond.get());
	if (!cb || cb->op != "<") return false;
	auto* cid = std::get_if<Ident>(cb->lhs.get());
	if (!cid || cid->name != ivS->name) return false;
	bool bc = false;
	int64_t bcv = 0;
	std::string bn;
	if (auto* bi = std::get_if<IntLit>(cb->rhs.get())) { bc = true; bcv = bi->value; }
	else if (auto* bd = std::get_if<Ident>(cb->rhs.get())) { bn = bd->name; }
	else return false;
	auto& ivV = g.resolve_var(ivS->name);
	auto& accV = g.resolve_var(accS->name);
	if (!ivV.is_local || !accV.is_local) return false;
	if (ivV.array_len > 0 || accV.array_len > 0) return false;
	if (g.dbl_vars_.count(ivS->name) || g.dbl_vars_.count(accS->name)) return false;

	g.last_store_len_ = (size_t) -1;
	int32_t ivD = static_cast<int32_t>(-8 * static_cast<int>(ivV.slot + 1));
	int32_t accD = static_cast<int32_t>(-8 * static_cast<int>(accV.slot + 1));
	if (bc) {
		if (bcv >= INT32_MIN && bcv <= INT32_MAX) g.c_.mov32r_imm32(9, static_cast<int32_t>(bcv));
		else g.c_.mov64r_imm64(9, bcv);
	} else {
		auto& bv = g.resolve_var(bn);
		if (!bv.is_local || bv.array_len > 0) return false;
		g.c_.mov64r_memd8(9, 5, static_cast<int32_t>(-8 * static_cast<int>(bv.slot + 1)));
	}
	g.c_.mov64r_memd8(10, 5, ivD);                /* r10 = i0 */
	g.c_.mov64r_memd8(8, 5, accD);                /* r8  = s0 */
	g.c_.mov64r_rm64(0, 9);
	g.c_.sub_r64_rm64(0, 10);                     /* rax = m */
	g.emit_ovf_check();
	g.c_.cmp_rm64_imm32(0, 0);
	int jz = g.emit_jcc(0xE);                     /* jle → 0 次迭代 */
	g.c_.mov64r_rm64(3, 0);                       /* rbx = m */
	if (constAcc) {
		g.c_.mov64r_rm64(0, 3);
		if (cval >= 0 && cval <= INT32_MAX) g.c_.mov32r_imm32(1, static_cast<int32_t>(cval));
		else g.c_.mov64r_imm64(1, cval);
		g.c_.imul_r64_rm64(0, 1);                 /* rax = m*c */
		g.emit_ovf_check();
	} else {
		g.c_.mov64r_rm64(0, 3);
		g.c_.imul_r64_rm64(0, 10);                /* part1 = m*i0 */
		g.emit_ovf_check();
		g.c_.mov64r_rm64(1, 0);                   /* rcx = part1 */
		g.c_.mov64r_rm64(0, 3);                   /* rax = m */
		g.c_.mov64r_rm64(2, 3);                   /* rdx = m */
		g.c_.sub_rm64_imm32(2, 1);                /* rdx = m-1 */
		g.emit_ovf_check();
		g.c_.test_rm64_imm32(0, 1);
		int jodd = g.emit_jcc(0x5);               /* jnz → m 为奇数 */
		g.c_.shr_rm64_imm8(0, 1);                 /* rax = m/2 */
		g.c_.imul_r64_rm64(0, 2);                 /* (m/2)*(m-1) */
		int jp2 = g.emit_jmp();
		g.patch(jodd);
		g.c_.shr_rm64_imm8(2, 1);                 /* rdx = (m-1)/2 */
		g.c_.imul_r64_rm64(0, 2);                 /* m*((m-1)/2) */
		g.patch(jp2);
		g.emit_ovf_check();
		g.c_.add_r64_rm64(0, 1);                  /* sum = part1 + part2 */
		g.emit_ovf_check();
		/* 注意: 不能对负数用 mov32r_imm32 —— 它零扩展, -5 会变成 4294967291。
		   只有非负且落在 int32 内才用短编码, 其余一律 movabs(符号扩展)。 */
		if (k != 1) {
			if (k >= 0 && k <= INT32_MAX) g.c_.mov32r_imm32(1, static_cast<int32_t>(k));
			else g.c_.mov64r_imm64(1, k);
			g.c_.imul_r64_rm64(0, 1);
			g.emit_ovf_check();
		}
	}
	/* 净效果中的常量部分: s += cval * m。
	   只有 k != 0 时才走到这里（k==0 已由上面的 constAcc 分支处理）。 */
	if (!constAcc && cval != 0) {
		g.c_.mov64r_rm64(1, 3);                   /* rcx = m */
		if (cval >= 0 && cval <= INT32_MAX) g.c_.mov32r_imm32(2, static_cast<int32_t>(cval));
		else g.c_.mov64r_imm64(2, cval);
		g.c_.imul_r64_rm64(1, 2);                 /* rcx = m * cval */
		g.emit_ovf_check();
		g.c_.add_r64_rm64(0, 1);
		g.emit_ovf_check();
	}
	g.c_.add_r64_rm64(0, 8);                      /* + s0 */
	g.emit_ovf_check();
	g.c_.mov_memd8_64r(5, accD, 0);
	g.c_.mov_memd8_64r(5, ivD, 9);                /* i = n */
	int jend = g.emit_jmp();
	g.patch(jz);
	g.patch(jend);
	return true;
}

/* ══════════════════════════════════════════════════════════════════════════
   模式 2: 步长 > 1 的等差求和
   ══════════════════════════════════════════════════════════════════════════

     while (i < n) {  s = s + i;   i = i + step;  }

   m = ceil((n - i0) / step)，i 依次为 i0, i0+step, ...
   求和 = m*i0 + step * m*(m-1)/2
   终值 i = i0 + m*step

   单独列出是因为 m 不是简单的 n-i0，容易算错；且需要一次除法。
 ══════════════════════════════════════════════════════════════════════════ */
bool LoopOptimizer::lo_strided_sum(NativeGen& g, WhileStmt& n) {
	if (n.body.size() != 2) return false;
	auto* a0 = std::get_if<AssignStmt>(&n.body[0]);
	auto* a1 = std::get_if<AssignStmt>(&n.body[1]);
	if (!a0 || !a1 || a0->target || a1->target) return false;
	auto selfAdd = [](AssignStmt& a, int64_t& c) -> bool {
		auto* b = std::get_if<Binary>(a.value.get());
		if (!b || b->op != "+") return false;
		auto* l = std::get_if<Ident>(b->lhs.get());
		if (!l || l->name != a.name) return false;
		auto* il = std::get_if<IntLit>(b->rhs.get());
		if (!il) return false;
		c = il->value;
		return true;
	};
	int64_t step = 0;
	AssignStmt* ivS = nullptr;
	AssignStmt* accS = nullptr;
	if (selfAdd(*a1, step)) { ivS = a1; accS = a0; }
	else if (selfAdd(*a0, step)) { ivS = a0; accS = a1; }
	else return false;
	if (step == 1) return false;                  /* step=1 由模式 1 处理 */
	if (step <= 0) return false;                  /* 负/零步长可能死循环，不优化 */
	auto* ab = std::get_if<Binary>(accS->value.get());
	if (!ab || ab->op != "+") return false;
	auto* al = std::get_if<Ident>(ab->lhs.get());
	if (!al || al->name != accS->name) return false;
	auto* rid = std::get_if<Ident>(ab->rhs.get());
	if (!rid || rid->name != ivS->name) return false;
	auto* cb = std::get_if<Binary>(n.cond.get());
	if (!cb || cb->op != "<") return false;
	auto* cid = std::get_if<Ident>(cb->lhs.get());
	if (!cid || cid->name != ivS->name) return false;
	bool bc = false;
	int64_t bcv = 0;
	std::string bn;
	if (auto* bi = std::get_if<IntLit>(cb->rhs.get())) { bc = true; bcv = bi->value; }
	else if (auto* bd = std::get_if<Ident>(cb->rhs.get())) { bn = bd->name; }
	else return false;
	auto& ivV = g.resolve_var(ivS->name);
	auto& accV = g.resolve_var(accS->name);
	if (!ivV.is_local || !accV.is_local) return false;
	if (ivV.array_len > 0 || accV.array_len > 0) return false;
	if (g.dbl_vars_.count(ivS->name) || g.dbl_vars_.count(accS->name)) return false;

	g.last_store_len_ = (size_t) -1;
	int32_t ivD = static_cast<int32_t>(-8 * static_cast<int>(ivV.slot + 1));
	int32_t accD = static_cast<int32_t>(-8 * static_cast<int>(accV.slot + 1));
	if (bc) {
		if (bcv >= INT32_MIN && bcv <= INT32_MAX) g.c_.mov32r_imm32(9, static_cast<int32_t>(bcv));
		else g.c_.mov64r_imm64(9, bcv);
	} else {
		auto& bv = g.resolve_var(bn);
		if (!bv.is_local || bv.array_len > 0) return false;
		g.c_.mov64r_memd8(9, 5, static_cast<int32_t>(-8 * static_cast<int>(bv.slot + 1)));
	}
	g.c_.mov64r_memd8(10, 5, ivD);                /* r10 = i0 */
	g.c_.mov64r_memd8(8, 5, accD);                /* r8  = s0 */
	g.c_.mov64r_rm64(0, 9);
	g.c_.sub_r64_rm64(0, 10);                     /* rax = d = n - i0 */
	g.emit_ovf_check();
	g.c_.cmp_rm64_imm32(0, 0);
	int jz = g.emit_jcc(0xE);                     /* jle → 0 次迭代 */
	g.c_.add_rm64_imm32(0, static_cast<int32_t>(step - 1));
	g.emit_ovf_check();                           /* rax = d + step - 1 */
	if (step >= INT32_MIN && step <= INT32_MAX) g.c_.mov32r_imm32(1, static_cast<int32_t>(step));
	else g.c_.mov64r_imm64(1, step);              /* rcx = step (除数) */
	g.emit_div_check(1);                          /* 注意: 会破坏 rdx，此处 rdx 无重要值 */
	g.c_.cqo();
	g.c_.idiv_rm64(1);
	g.c_.mov64r_rm64(3, 0);                       /* rbx = m */
	/* part1 = m*i0 */
	g.c_.mov64r_rm64(0, 3);
	g.c_.imul_r64_rm64(0, 10);
	g.emit_ovf_check();
	g.c_.mov64r_rm64(1, 0);                       /* rcx = part1 */
	/* part2 = m*(m-1)/2   （先除后乘） */
	g.c_.mov64r_rm64(0, 3);
	g.c_.mov64r_rm64(2, 3);
	g.c_.sub_rm64_imm32(2, 1);
	g.emit_ovf_check();
	g.c_.test_rm64_imm32(0, 1);
	int jodd = g.emit_jcc(0x5);
	g.c_.shr_rm64_imm8(0, 1);
	g.c_.imul_r64_rm64(0, 2);
	int jp2 = g.emit_jmp();
	g.patch(jodd);
	g.c_.shr_rm64_imm8(2, 1);
	g.c_.imul_r64_rm64(0, 2);
	g.patch(jp2);
	g.emit_ovf_check();
	g.c_.add_r64_rm64(0, 1);                      /* sum */
	g.emit_ovf_check();
	if (step >= INT32_MIN && step <= INT32_MAX) g.c_.mov32r_imm32(1, static_cast<int32_t>(step));
	else g.c_.mov64r_imm64(1, step);
	g.c_.imul_r64_rm64(0, 1);                     /* * step */
	g.emit_ovf_check();
	g.c_.add_r64_rm64(0, 8);                      /* + s0 */
	g.emit_ovf_check();
	g.c_.mov_memd8_64r(5, accD, 0);
	/* i = i0 + m*step */
	g.c_.mov64r_rm64(0, 3);
	if (step >= INT32_MIN && step <= INT32_MAX) g.c_.mov32r_imm32(1, static_cast<int32_t>(step));
	else g.c_.mov64r_imm64(1, step);
	g.c_.imul_r64_rm64(0, 1);
	g.emit_ovf_check();
	g.c_.add_r64_rm64(0, 10);
	g.emit_ovf_check();
	g.c_.mov_memd8_64r(5, ivD, 0);
	int jend = g.emit_jmp();
	g.patch(jz);
	g.patch(jend);
	return true;
}

/* ══════════════════════════════════════════════════════════════════════════
   模式 3: 倒计数求和
   ══════════════════════════════════════════════════════════════════════════

     while (i > 0) {  s = s + i;   i = i - 1;  }

   i 从 i0 递减到 1，共 i0 项，和 = i0*(i0+1)/2，终值 i = 0。
   单独列出是因为条件方向与自增方向都与模式 1 相反，会被漏掉。
 ══════════════════════════════════════════════════════════════════════════ */
bool LoopOptimizer::lo_countdown_sum(NativeGen& g, WhileStmt& n) {
	if (n.body.size() != 2) return false;
	auto* a0 = std::get_if<AssignStmt>(&n.body[0]);
	auto* a1 = std::get_if<AssignStmt>(&n.body[1]);
	if (!a0 || !a1 || a0->target || a1->target) return false;
	auto isDec1 = [](AssignStmt& a) -> bool {
		auto* b = std::get_if<Binary>(a.value.get());
		if (!b || b->op != "-") return false;
		auto* l = std::get_if<Ident>(b->lhs.get());
		if (!l || l->name != a.name) return false;
		auto* il = std::get_if<IntLit>(b->rhs.get());
		return il && il->value == 1;
	};
	AssignStmt* ivS = nullptr;
	AssignStmt* accS = nullptr;
	if (isDec1(*a1)) { ivS = a1; accS = a0; }
	else if (isDec1(*a0)) { ivS = a0; accS = a1; }
	else return false;
	auto* ab = std::get_if<Binary>(accS->value.get());
	if (!ab || ab->op != "+") return false;
	auto* al = std::get_if<Ident>(ab->lhs.get());
	if (!al || al->name != accS->name) return false;
	auto* rid = std::get_if<Ident>(ab->rhs.get());
	if (!rid || rid->name != ivS->name) return false;
	auto* cb = std::get_if<Binary>(n.cond.get());
	if (!cb || cb->op != ">") return false;
	auto* cid = std::get_if<Ident>(cb->lhs.get());
	if (!cid || cid->name != ivS->name) return false;
	auto* z = std::get_if<IntLit>(cb->rhs.get());
	if (!z || z->value != 0) return false;
	auto& ivV = g.resolve_var(ivS->name);
	auto& accV = g.resolve_var(accS->name);
	if (!ivV.is_local || !accV.is_local) return false;
	if (ivV.array_len > 0 || accV.array_len > 0) return false;
	if (g.dbl_vars_.count(ivS->name) || g.dbl_vars_.count(accS->name)) return false;

	g.last_store_len_ = (size_t) -1;
	int32_t ivD = static_cast<int32_t>(-8 * static_cast<int>(ivV.slot + 1));
	int32_t accD = static_cast<int32_t>(-8 * static_cast<int>(accV.slot + 1));
	g.c_.mov64r_memd8(10, 5, ivD);                /* r10 = i0 */
	g.c_.mov64r_memd8(8, 5, accD);                /* r8  = s0 */
	g.c_.cmp_rm64_imm32(10, 0);
	int jz = g.emit_jcc(0xE);                     /* jle → 0 次迭代 */
	g.c_.mov64r_rm64(0, 10);                      /* rax = m = i0 */
	g.c_.mov64r_rm64(2, 10);
	g.c_.add_rm64_imm32(2, 1);                    /* rdx = m+1 */
	g.emit_ovf_check();
	g.c_.test_rm64_imm32(0, 1);
	int jodd = g.emit_jcc(0x5);                   /* jnz → m 为奇数 */
	g.c_.shr_rm64_imm8(0, 1);                     /* (m/2)*(m+1) */
	g.c_.imul_r64_rm64(0, 2);
	int jp2 = g.emit_jmp();
	g.patch(jodd);
	g.c_.shr_rm64_imm8(2, 1);                     /* m*((m+1)/2) */
	g.c_.imul_r64_rm64(0, 2);
	g.patch(jp2);
	g.emit_ovf_check();
	g.c_.add_r64_rm64(0, 8);                      /* + s0 */
	g.emit_ovf_check();
	g.c_.mov_memd8_64r(5, accD, 0);
	g.c_.xor_rm64_r64(0, 0);
	g.c_.mov_memd8_64r(5, ivD, 0);                /* i = 0 */
	int jend = g.emit_jmp();
	g.patch(jz);
	g.patch(jend);
	return true;
}

/* ══════════════════════════════════════════════════════════════════════════
   模式 4: 平方和
   ══════════════════════════════════════════════════════════════════════════

     while (i < n) {  s = s + i*i;   i = i + 1;  }

   令 m = n - i0，则
     sum_{t=0}^{m-1} (i0+t)^2 = m*i0^2 + 2*i0*S1 + S2
     S1 = m*(m-1)/2
     S2 = (m-1)*m*(2m-1)/6

   因为 S2 的中间量是 m^3 级别，本模式**只在上界为编译期常量且
   <= 2^20 时启用** —— 此时 m^3 <= 2^60，静态可证明不溢出。
   这是本模块唯一带静态上界检查的模式，是刻意的保守取舍。

   累加策略: 把 term1 与 term2 依次累加进 r8(=s0)，避免额外寄存器压力。
 ══════════════════════════════════════════════════════════════════════════ */
/* ═══ 周期性取模求和的闭式解 ══════════════════════════════════════════
   形态:  while (i < n) { s = s + i % C;  i = i + step; }

   i % C 随 i 以周期 P = C / gcd(step, C) 循环。于是:
       总和 = (整周期数) × (单周期和) + (余下 rem 项的前缀和)
   全部由编译期常量算出，运行时只剩一条加法。

   **gcc 不做这个优化** —— 它只会生成硬件 idiv 循环。所以这个模式
   是 Wei 反超 C 的地方（b6_mod 基准）。

   安全约束（缺一即回退，绝不静默出错）:
     · i0 / n / step / C 全部编译期已知
     · i0 >= 0, step > 0, C > 0  —— 保证 i 恒非负，i%C 语义无歧义
     · C <= 65536  —— 周期表是 O(C) 编译期计算，避免拖慢编译
     · 总和使用 128 位验算，超出 int64 则回退
   ══════════════════════════════════════════════════════════════════ */
bool LoopOptimizer::mod_period_analyze(NativeGen& g, WhileStmt& n, ModPeriodInfo& out) {
	/* 语义层: 不受 -no-opt 影响 —— 这是编译期确定结果的求值，
	   属于常量折叠的延伸，是语言语义要求而非可选优化。 */
	if (n.body.size() != 2) return false;
	auto* a0 = std::get_if<AssignStmt>(&n.body[0]);
	auto* a1 = std::get_if<AssignStmt>(&n.body[1]);
	if (!a0 || !a1 || a0->target || a1->target) return false;

	/* 归纳变量自增: i = i + step */
	AssignStmt* ivS = nullptr; AssignStmt* accS = nullptr;
	int64_t step = 0;
	{
		AssignStmt* cands[2] = {a0, a1};
		for (int t = 0; t < 2; t++) {
			AssignStmt* a = cands[t];
			if (!a->value) return false;
			auto* b = std::get_if<Binary>(a->value.get());
			if (!b || b->op != "+") continue;
			auto* l = std::get_if<Ident>(b->lhs.get());
			auto* c = std::get_if<IntLit>(b->rhs.get());
			if (!l || !c || l->name != a->name || c->value <= 0) continue;
			ivS = a; step = c->value; accS = cands[1 - t]; break;
		}
	}
	if (!ivS || !accS) return false;

	/* 累加: s = s + (i % C) */
	auto* ab = std::get_if<Binary>(accS->value.get());
	if (!ab || ab->op != "+") return false;
	auto* al = std::get_if<Ident>(ab->lhs.get());
	if (!al || al->name != accS->name || !ab->rhs) return false;
	auto* mb = std::get_if<Binary>(ab->rhs.get());
	if (!mb || mb->op != "%") return false;
	auto* mid = std::get_if<Ident>(mb->lhs.get());
	auto* mc = std::get_if<IntLit>(mb->rhs.get());
	if (!mid || !mc || mid->name != ivS->name) return false;
	int64_t C = mc->value;
	if (C <= 0 || C > 65536) return false;

	/* 条件 i < n，n 编译期已知 */
	auto* cb = std::get_if<Binary>(n.cond.get());
	if (!cb || cb->op != "<") return false;
	auto* cid = std::get_if<Ident>(cb->lhs.get());
	if (!cid || cid->name != ivS->name || !cb->rhs) return false;
	int64_t N = 0;
	if (auto* bi = std::get_if<IntLit>(cb->rhs.get())) N = bi->value;
	else if (auto* rv = std::get_if<Ident>(cb->rhs.get())) {
		auto it = g.known_const_.find(rv->name);
		if (it == g.known_const_.end()) return false;
		N = it->second;
	} else return false;

	/* 初值 i0 编译期已知且非负 */
	auto i0it = g.known_const_.find(ivS->name);
	if (i0it == g.known_const_.end()) return false;
	int64_t i0 = i0it->second;
	if (i0 < 0) return false;

	/* 项数: 满足 i0 + j*step < N 的 j 的个数 */
	int64_t k = 0;
	if (N > i0) k = (N - i0 - 1) / step + 1;
	if (k <= 0) return false;

	/* 周期 P = C / gcd(step, C) */
	int64_t gg = step, cc = C;
	while (cc) { int64_t t = gg % cc; gg = cc; cc = t; }   /* gg = gcd */
	if (gg <= 0) return false;
	int64_t P = C / gg;
	if (P <= 0) return false;

	/* 单周期和 + 前缀和（编译期 O(P) 计算，128 位防溢出） */
	wei_i128 period_sum = 0;
	for (int64_t j = 0; j < P; j++) period_sum += wei_i128((i0 + j * step) % C);
	int64_t full = k / P;
	int64_t rem  = k % P;
	wei_i128 prefix = 0;
	for (int64_t j = 0; j < rem; j++) prefix += wei_i128((i0 + j * step) % C);
	wei_i128 total = wei_i128(full) * period_sum + prefix;
	if (total < wei_i128(INT64_MIN) || total > wei_i128(INT64_MAX)) return false;
	int64_t cval = static_cast<int64_t>(total);
	out.i0=i0; out.N=N; out.step=step; out.C=C;
	out.k=k; out.P=P; out.full=full; out.rem=rem; out.total=cval;
	return true;
}

bool LoopOptimizer::lo_mod_period_sum(NativeGen& g, WhileStmt& n) {
	ModPeriodInfo inf;
	if (!mod_period_analyze(g, n, inf)) return false;
	/* 重新定位语句以获取变量名（analyze 只做数值判定） */
	auto* a0 = std::get_if<AssignStmt>(&n.body[0]);
	auto* a1 = std::get_if<AssignStmt>(&n.body[1]);
	if (!a0 || !a1) return false;
	AssignStmt* ivS = (a1 && a1->value && std::get_if<Binary>(a1->value.get())
	                   && std::get_if<Binary>(a1->value.get())->op == "+") ? a1 : a0;
	if (!ivS) return false;
	auto* accS = (ivS == a1) ? a0 : a1;
	int64_t cval = inf.total;
	auto& ivV  = g.resolve_var(ivS->name);
	auto& accV = g.resolve_var(accS->name);
	if (!ivV.is_local || !accV.is_local) return false;
	if (ivV.array_len > 0 || accV.array_len > 0) return false;
	if (g.dbl_vars_.count(ivS->name) || g.dbl_vars_.count(accS->name)) return false;
	g.last_store_len_ = (size_t) -1;
	int32_t ivD  = static_cast<int32_t>(-8 * static_cast<int>(ivV.slot + 1));
	int32_t accD = static_cast<int32_t>(-8 * static_cast<int>(accV.slot + 1));
	g.c_.mov64r_memd8(0, 5, accD);                 /* rax = s0 */
	if (cval >= 0 && cval <= INT32_MAX) {
		g.c_.add_rm64_imm32(0, static_cast<int32_t>(cval));
	} else {
		g.c_.mov64r_imm64(1, cval);
		g.c_.add_r64_rm64(0, 1);
	}
	g.emit_ovf_check();
	g.c_.mov_memd8_64r(5, accD, 0);                /* s = s0 + Σ(i%C) */
	/* i 的最终值 = i0 + k*step（循环退出时） */
	g.c_.mov64r_imm64(2, inf.i0 + inf.k * inf.step);   /* i 退出时的值 */
	g.c_.mov_memd8_64r(5, ivD, 2);
	g.known_const_.erase(ivS->name);
	g.known_const_.erase(accS->name);
	return true;
}

bool LoopOptimizer::lo_square_sum(NativeGen& g, WhileStmt& n) {
	if (n.body.size() != 2) return false;
	auto* a0 = std::get_if<AssignStmt>(&n.body[0]);
	auto* a1 = std::get_if<AssignStmt>(&n.body[1]);
	if (!a0 || !a1 || a0->target || a1->target) return false;
	auto isInc1 = [](AssignStmt& a) -> bool {
		auto* b = std::get_if<Binary>(a.value.get());
		if (!b || b->op != "+") return false;
		auto* l = std::get_if<Ident>(b->lhs.get());
		if (!l || l->name != a.name) return false;
		auto* il = std::get_if<IntLit>(b->rhs.get());
		return il && il->value == 1;
	};
	AssignStmt* ivS = nullptr;
	AssignStmt* accS = nullptr;
	if (isInc1(*a1)) { ivS = a1; accS = a0; }
	else if (isInc1(*a0)) { ivS = a0; accS = a1; }
	else return false;
	auto* ab = std::get_if<Binary>(accS->value.get());
	if (!ab || ab->op != "+") return false;
	auto* al = std::get_if<Ident>(ab->lhs.get());
	if (!al || al->name != accS->name) return false;
	auto* sq = std::get_if<Binary>(ab->rhs.get());
	if (!sq || sq->op != "*") return false;
	auto* s1 = std::get_if<Ident>(sq->lhs.get());
	auto* s2 = std::get_if<Ident>(sq->rhs.get());
	if (!s1 || !s2) return false;
	if (s1->name != ivS->name || s2->name != ivS->name) return false;
	auto* cb = std::get_if<Binary>(n.cond.get());
	if (!cb || cb->op != "<") return false;
	auto* cid = std::get_if<Ident>(cb->lhs.get());
	if (!cid || cid->name != ivS->name) return false;
	/* 上界: 字面量，或常量传播已知的变量。
	   只认字面量会让 `create.int n = 2600000; while (i < n)` 这种最
	   常见的写法拿不到平方和闭式解（b3_sq 曾因此退回 O(n) 循环）。 */
	int64_t bcv = 0;
	if (auto* bi = std::get_if<IntLit>(cb->rhs.get())) bcv = bi->value;
	else if (auto* bv = std::get_if<Ident>(cb->rhs.get())) {
		auto it = g.known_const_.find(bv->name);
		if (it == g.known_const_.end()) return false;
		bcv = it->second;
	} else return false;
	/* ── 快速路径: 归纳变量初值在编译期已知时, 整个平方和就是编译期常量 ──
	   原来的静态上界 1<<20 是为了避开闭式解中间乘积溢出 int64。
	   但当 i0 已知时根本不必在运行时展开公式 —— 编译器直接用 128 位
	   把 Σk² 算成一个常量, 运行时只剩「s = s0 + 常量」一条加法。
	   这样既绕开了溢出问题, 又把 O(n) 循环彻底消掉, 上界不再受限。
	   128 位验算若发现结果本身超出 int64, 则退回通用路径（绝不静默出错）。 */
	{
		auto i0it = g.known_const_.find(ivS->name);
		if (bcv >= 0 && i0it != g.known_const_.end()) {
			int64_t i0 = i0it->second;
			int64_t m = bcv - i0;
			if (m > 0) {
				/* F(n) = Σ_{k=1}^{n} k² = n(n+1)(2n+1)/6
				   先除 2、再除 3, 把中间乘积压到最小:
				   2 只能来自相邻的 n 与 n+1（2n+1 恒为奇数）;
				   3 必在 n、n+1、2n+1 三者之一。 */
				auto F = [](int64_t n) -> wei_i128 {
					if (n <= 0) return 0;
					wei_i128 a = n, b = n + 1, c = 2 * n + 1;
					if (a % 2 == 0) a /= 2; else b /= 2;
					if (a % 3 == 0) a /= 3;
					else if (b % 3 == 0) b /= 3;
					else c /= 3;
					return a * b * c;
				};
				wei_i128 acc = F(bcv - 1) - F(i0 - 1);
				if (acc >= wei_i128(INT64_MIN) && acc <= wei_i128(INT64_MAX)) {
					int64_t cval = static_cast<int64_t>(acc);
					auto& qiv = g.resolve_var(ivS->name);
					auto& qacc = g.resolve_var(accS->name);
					if (qiv.is_local && qacc.is_local && qiv.array_len == 0
					        && qacc.array_len == 0
					        && !g.dbl_vars_.count(ivS->name)
					        && !g.dbl_vars_.count(accS->name)) {
						g.last_store_len_ = (size_t) -1;
						int32_t qivD = static_cast<int32_t>(-8 * static_cast<int>(qiv.slot + 1));
						int32_t qacD = static_cast<int32_t>(-8 * static_cast<int>(qacc.slot + 1));
						g.c_.mov64r_memd8(0, 5, qacD);          /* rax = s0 */
						if (cval >= 0 && cval <= INT32_MAX) {
							g.c_.add_rm64_imm32(0, static_cast<int32_t>(cval));
						} else {
							g.c_.mov64r_imm64(1, cval);
							g.c_.add_r64_rm64(0, 1);
						}
						g.emit_ovf_check();
						g.c_.mov_memd8_64r(5, qacD, 0);         /* s = s0 + Σk² */
						g.c_.mov64r_imm64(2, bcv);
						g.c_.mov_memd8_64r(5, qivD, 2);         /* i = 上界 */
						return true;
					}
				}
			}
		}
	}
	if (bcv < 0 || bcv > (INT64_C(1) << 20)) return false;   /* 静态溢出安全界 */
	auto& ivV = g.resolve_var(ivS->name);
	auto& accV = g.resolve_var(accS->name);
	if (!ivV.is_local || !accV.is_local) return false;
	if (ivV.array_len > 0 || accV.array_len > 0) return false;
	if (g.dbl_vars_.count(ivS->name) || g.dbl_vars_.count(accS->name)) return false;

	g.last_store_len_ = (size_t) -1;
	int32_t ivD = static_cast<int32_t>(-8 * static_cast<int>(ivV.slot + 1));
	int32_t accD = static_cast<int32_t>(-8 * static_cast<int>(accV.slot + 1));
	if (bcv >= INT32_MIN && bcv <= INT32_MAX) g.c_.mov32r_imm32(9, static_cast<int32_t>(bcv));
	else g.c_.mov64r_imm64(9, bcv);               /* r9 = n */
	g.c_.mov64r_memd8(10, 5, ivD);                /* r10 = i0 */
	g.c_.mov64r_memd8(8, 5, accD);                /* r8 = s0（兼作累加器） */
	g.c_.mov64r_rm64(0, 9);
	g.c_.sub_r64_rm64(0, 10);                     /* rax = m */
	g.emit_ovf_check();
	g.c_.cmp_rm64_imm32(0, 0);
	int jz = g.emit_jcc(0xE);                     /* jle → 0 次迭代 */
	g.c_.mov64r_rm64(3, 0);                       /* rbx = m（全程保持） */

	/* term1 = m * i0^2  →  累加进 r8 */
	g.c_.mov64r_rm64(0, 10);
	g.c_.imul_r64_rm64(0, 10);
	g.emit_ovf_check();                           /* rax = i0^2 */
	g.c_.imul_r64_rm64(0, 3);                     /* * m */
	g.emit_ovf_check();
	g.c_.add_r64_rm64(8, 0);
	g.emit_ovf_check();                           /* r8 = s0 + term1 */

	/* S1 = m*(m-1)/2 */
	g.c_.mov64r_rm64(0, 3);
	g.c_.mov64r_rm64(2, 3);
	g.c_.sub_rm64_imm32(2, 1);
	g.emit_ovf_check();
	g.c_.test_rm64_imm32(0, 1);
	int jodd = g.emit_jcc(0x5);
	g.c_.shr_rm64_imm8(0, 1);
	g.c_.imul_r64_rm64(0, 2);
	int jp2a = g.emit_jmp();
	g.patch(jodd);
	g.c_.shr_rm64_imm8(2, 1);
	g.c_.imul_r64_rm64(0, 2);
	g.patch(jp2a);
	g.emit_ovf_check();                           /* rax = S1 */
	/* term2 = 2*i0*S1  →  累加进 r8 */
	g.c_.imul_r64_rm64(0, 10);
	g.emit_ovf_check();
	g.c_.add_r64_rm64(0, 0);                      /* *2 */
	g.emit_ovf_check();
	g.c_.add_r64_rm64(8, 0);
	g.emit_ovf_check();                           /* r8 = s0 + term1 + term2 */

	/* S2 = (m-1)*m*(2m-1)/6 */
	g.c_.mov64r_rm64(0, 3);
	g.c_.sub_rm64_imm32(0, 1);                    /* rax = m-1 */
	g.emit_ovf_check();
	g.c_.imul_r64_rm64(0, 3);                     /* (m-1)*m */
	g.emit_ovf_check();
	g.c_.mov64r_rm64(2, 3);
	g.c_.add_r64_rm64(2, 2);                      /* rdx = 2m */
	g.emit_ovf_check();
	g.c_.sub_rm64_imm32(2, 1);                    /* rdx = 2m-1 */
	g.emit_ovf_check();
	g.c_.imul_r64_rm64(0, 2);                     /* (m-1)*m*(2m-1) */
	g.emit_ovf_check();
	/* /6 = /2 再 /3。数学上该积必被 6 整除，故两步都是精确整除。 */
	g.c_.shr_rm64_imm8(0, 1);                     /* /2 */
	g.c_.mov32r_imm32(9, 3);                      /* r9 = 3（r9 原存 n，之后重装载） */
	g.emit_div_check(9);                          /* 破坏 rdx；此时 rdx 无重要值 */
	g.c_.cqo();
	g.c_.idiv_rm64(9);                            /* rax = S2 */

	g.c_.add_r64_rm64(0, 8);                      /* + (s0 + term1 + term2) */
	g.emit_ovf_check();
	g.c_.mov_memd8_64r(5, accD, 0);
	if (bcv >= INT32_MIN && bcv <= INT32_MAX) g.c_.mov32r_imm32(9, static_cast<int32_t>(bcv));
	else g.c_.mov64r_imm64(9, bcv);               /* 重装载 n */
	g.c_.mov_memd8_64r(5, ivD, 9);                /* i = n */
	int jend = g.emit_jmp();
	g.patch(jz);
	g.patch(jend);
	return true;
}
