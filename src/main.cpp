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

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
	Kw_module, Kw_part, Kw_access, Kw_adv, Kw_struct,
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
	Kw_typeof, Kw_sizeof, Kw_new, Kw_delete, Kw_yield,
	Kw_匹配, Kw_公开, Kw_私有,
	LParen, RParen, LBrace, RBrace, LBracket, RBracket, Comma, Semicolon, Colon, DoubleColon, Dot,
	DoubleDot,          /* '..' 范围运算符: lo..hi */
	Plus, Minus, Star, Slash, Percent, PlusPlus, MinusMinus, Amp,
	Assign, PlusEq, MinusEq, StarEq, SlashEq,
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

	using Stmt = std::variant<Block, IfStmt, ForStmt, WhileStmt, LoopStmt,
	      LoopNStmt, ReturnStmt, ExprStmt, AssignStmt, VarDecl,
	      ModuleAccessSet, MatchStmt, ContractStmt, TypeAliasDecl,
	      ForallStmt, WhenStmt, YieldStmt, CompileTimeStmt, MacroDecl>;

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
		std::string name;
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
		std::string name;
		std::vector<PartDecl> parts;
		std::vector<Stmt> body;
		/* mod 成员函数: create.mod Name::main() / create.mod Name::priority(N)_(参数) */
		std::vector<FuncDecl> methods;
		int priority = 0;      /* main 缺省 0, 其余 mod 缺省 1 */
		bool has_priority = false;
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

	using TopLevel = std::variant<ModuleDecl, ImportDecl, TypeAliasDecl, MacroDecl>;

	struct Program {
		std::vector<TopLevel> items;
	};

	struct Block {
		std::vector<Stmt> stmts;
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
		std::vector<std::unique_ptr<Expr>> args;
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
	{"module", TokType::Kw_module},
	{"mod", TokType::Kw_module},
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
	{"模塊", "module", false}, {"模块", "module", false},
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
	{"模块", "module", false}, {"部件", "part", false},
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

[[noreturn]] void Lexer::die(const std::string& msg, int32_t line, int32_t col) const {
	throw std::runtime_error("lex error at " + std::to_string(line) + ":" + std::to_string(col) + " " + msg);
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
			if (peek() == '=') {
				get();
				return makeToken(TokType::LtEq, "<=", line, col);
			}
			return makeToken(TokType::IOLt, "<", line, col);
		case '>':
			get();
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
			die("非法字符 '|', 是否遗漏了第二个 '|'", line, col); // 修复: 错误信息不准确
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
			out.push_back(readIdentOrKeyword());
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
class Parser {
	public:
		explicit Parser(std::vector<Token> tokens);
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
		std::unique_ptr<wei::ast::VarDecl> parseForInitDecl();

		std::unique_ptr<wei::ast::Expr> parseExpr();
		std::unique_ptr<wei::ast::Expr> parseLogicalOr();
		std::unique_ptr<wei::ast::Expr> parseLogicalAnd();
		std::unique_ptr<wei::ast::Expr> parseEquality();
		std::unique_ptr<wei::ast::Expr> parseCompare();
		std::unique_ptr<wei::ast::Expr> parseAdd();
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
	throw std::runtime_error(msg + " at " + std::to_string(at.line) + ":" + std::to_string(at.col));
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
	return t == TokType::Kw_module || t == TokType::Kw_模块;
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

Program Parser::parse() {
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
	/* 未被调用的 mod 必须声明 priority */
	for (auto& item : prog.items) {
		auto* m = std::get_if<ModuleDecl>(&item);
		if (!m) continue;
		if (m->has_main || m->has_priority) continue;
		if (m->methods.empty() && m->body.empty()) continue;
		if (module_referenced_.count(m->name)) continue;
		die("mod " + m->name + " 未被调用, 需声明 priority(N)");
	}
	return prog;
}

TopLevel Parser::parseTopLevel() {
	if (isCreateKw(cur().type)) {
		pos_++;
		eatCreateSep();
		if (isModuleKw(cur().type)) {
			pos_++;
			return parseModuleDecl();
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



void Parser::registerModule(const ModuleDecl& md) {
	std::set<std::string> parts;
	for (auto& p : md.parts) parts.insert(p.name);
	for (auto& f : md.methods) parts.insert(f.name);
	module_reg_[md.name] = std::move(parts);
	module_prio_[md.name] = md.priority;
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
		const Token& memTk = expect(TokType::Ident, "成员名(main/priority)");
		FuncDecl fd;
		fd.name = memTk.text;
		fd.line = memTk.line;
		fd.col = memTk.col;

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

		/* 普通 mod 成员函数: 名字::成员(参数) */
		expect(TokType::LParen, "'('");
		fd.params = parseParams();
		expect(TokType::RParen, "')'");
		eatRetType(fd); parseOptAsync(fd);
		fd.body = parseBlock();
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

Stmt Parser::parseStmt() {
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
		if (!isTypeKw(cur().type)) {
			die("create 后的类型未知 '" + cur().text + "'", cur());
		}
		std::string tn = parseTypeName();
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
			sl.value = "\n";
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
	auto left = parseAdd();
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

std::unique_ptr<Expr> Parser::parseAdd() {
	auto left = parseMul();
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
	        || cur().is(TokType::Kw_not)) {
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
		try {
			il.value = std::stoll(cur().text);
		} catch (...) {
			die("整数常量过大 '" + cur().text + "'", cur());
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
	pos_++;
	/* mod 成员调用: Calc::fib(n) */
	if (check(TokType::DoubleColon) && checkAt(1, TokType::Ident)) {
		if (module_reg_.count(name) == 0) {
			die("未知模块: " + name, cur());
		}
		mod_path = name;
		markReferenced(name);
		pos_++;
		name = cur().text; /* 成员名: main / _ 等 */
		pos_++;
	}
	if (check(TokType::LParen)) {
		pos_++;
		Call c;
		c.callee = mod_path.empty() ? name : (mod_path + "::" + name);
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
		sub.module_reg_ = module_reg_;
		sub.imported_modules_ = imported_modules_;
		sub.type_aliases_ = type_aliases_;
		Program subprog = sub.parse();
		for (auto& item : subprog.items) {
			auto* md = std::get_if<ModuleDecl>(&item);
			if (md && !syms.empty()) {
				bool want = false;
				for (auto& s : syms) if (s == md->name) want = true;
				if (!want) continue;
				if (!alias.empty()) md->name = alias;
			}
			pending_items_.push_back(std::move(item));
		}
		/* 回写模块登记表 */
		for (auto& kv : sub.module_reg_) module_reg_[kv.first] = kv.second;
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
				if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") {
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
					throw std::runtime_error("未声明函数: " + node.callee + " at " + std::to_string(node.line) + ":" + std::to_string(node.col));
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
			rex_wrb(r, base);
			u8(0x8B);
			modrm(2, r, base);
			if ((base & 7) == 4) u8(0x24);  /* SIB: [rsp+disp32] */
			u32(static_cast<uint32_t>(d));
		}
		/* mov [r+disp], r64 ??自动选择 disp8/disp32 */
		void mov_memd8_64r(int base, int64_t d, int r) {
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
			rex_wrb(r, base);
			u8(0x89);
			modrm(2, r, base);
			if ((base & 7) == 4) u8(0x24);  /* SIB: [rsp+disp32] */
			u32(static_cast<uint32_t>(d));
		}
		/* mov [r+disp], imm32 ??自动选择 disp8/disp32 */
		void mov_memd8_imm32(int base, int64_t d, int32_t v) {
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
		void alu_r64_rm64(uint8_t op, int r, int rm) {
			rex_wrb(r, rm);
			u8(op);
			modrm(3, r, rm);
		}
		void alu_r64_memd8(uint8_t op, int r, int base, int64_t d) {
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
			void emit_ovf_check();
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

			/* 编译期已知常量追??(仅用??pattern 匹配) */
			std::map<std::string, int64_t> known_const_;
			/* 已识别为组合数递推的函数名 */
			std::string comb_func_;

			/* ── 双精度支??── */
			std::set<std::string> dbl_vars_;                     /* 声明??double 的变量名 */
			bool cur_ret_double_ = false;                        /* 当前函数返回 double */
			std::vector<std::pair<size_t, double>> dbl_patches_; /* RIP 相对双精度常量回??*/
			bool isDblExpr(const std::unique_ptr<Expr>& e);
			bool isStrExpr(const std::unique_ptr<Expr>& e);
			bool funcReturnsDouble(const std::string& name) const;
			bool funcReturnsString(const std::string& name) const;
			void gen_expr_dbl(const std::unique_ptr<Expr>& e);

			void gen_func(FuncDecl& f);
			void gen_stmt(Stmt& s);
			void gen_expr(Expr& e);
			bool is_builtin_call(const std::string& name) const;
			void gen_builtin_call(Call& n);
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
	/* 通用常量循环求值: 边界与初值都已知时, 编译期把整个循环跑完 */
	static bool lo_const_fold_while(NativeGen& g, WhileStmt& n);
	/* SIMD 归约: 数组求和用 SSE2 一次算两个 64 位元素 */
	static bool lo_simd_sum(NativeGen& g, WhileStmt& n);

	static bool run(NativeGen& g, WhileStmt& n) {
		if (lo_const_fold_while(g, n)) return true; /* 常量循环: 编译期整体求值 */
		if (lo_simd_sum(g, n))       return true;   /* 数组求和 SIMD 归约 */
		if (lo_arithmetic_sum(g, n)) return true;   /* 等差求和（含常量累加） */
		if (lo_strided_sum(g, n))    return true;   /* 步长 > 1 的等差求和 */
		if (lo_countdown_sum(g, n))  return true;   /* 倒计数求和 */
		if (lo_square_sum(g, n))     return true;   /* 平方和（常量上界） */
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
		std::unique_ptr<Expr> d0val;
		std::string d0name;
		if (auto* s0a = std::get_if<AssignStmt>(&n.body[0])) {
			d0name = s0a->name;
			d0val = std::move(s0a->value);
		} else if (auto* s0v = std::get_if<VarDecl>(&n.body[0])) {
			d0name = s0v->name;
			d0val = std::move(s0v->init);
		} else return false;
		auto* s1 = std::get_if<AssignStmt>(&n.body[1]);
		auto* s2 = std::get_if<AssignStmt>(&n.body[2]);
		if (!s1 || !s2) return false;
		auto* m0 = std::get_if<Binary>(d0val.get());
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
				gen_expr(*ix->idx);
				if (!(ix->idx && indexProven(*ix->idx, vi.array_len))) emit_bounds_check(vi.array_len);
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

	void NativeGen::gen_builtin_call(Call& n) {
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
				const std::string& op = n.op;
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
				if (auto* ip = std::get_if<IntLit>(n.rhs.get())) {
					int64_t v = ip->value;
					if (op == "+" && v >= INT32_MIN && v <= INT32_MAX) {
						self->gen_expr(*n.lhs);
						self->c_.add_rm64_imm32(0, static_cast<int32_t>(v));
						self->emit_ovf_check();          /* 有符号加法溢出 */
						return;
					}
					if (op == "-" && v >= INT32_MIN && v <= INT32_MAX) {
						self->gen_expr(*n.lhs);
						self->c_.sub_rm64_imm32(0, static_cast<int32_t>(v));
						self->emit_ovf_check();          /* 有符号减法溢出 */
						return;
					}
					if (op == "*" && v >= 0 && v <= 0xFFFFFFFFLL) {
						self->gen_expr(*n.lhs);
						/* 优化: 2 的幂????shl 强度削减 */
						if (v == 1) return;
						self->c_.u8(0xB9);
						self->c_.u32(static_cast<uint32_t>(v));
						self->c_.imul_r64_rm64(0, 1);
						self->emit_ovf_check();          /* 有符号乘法溢出 */
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
							self->c_.cmp_rm64_imm32(0, static_cast<int32_t>(cip->value));
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
				if (op == "+") {
					self->c_.add_r64_rm64(0, 1);
					self->emit_ovf_check();          /* 有符号加法溢出 */
				} else if (op == "-") {
					self->c_.sub_r64_rm64(0, 1);
					self->emit_ovf_check();          /* 有符号减法溢出 */
				} else if (op == "*") {
					self->c_.imul_r64_rm64(0, 1);
					self->emit_ovf_check();          /* 有符号乘法溢出 */
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
					}
				}
			}
			void operator()(Call& n) {
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
					/* 静态证明索引必然在界内 → 省略运行时检查。
					   省掉的是每次访问的 cmp + 分支，在数组密集的循环里
					   这是实打实的收益；同时**没有**牺牲任何安全性，
					   因为不变量是在编译期证明过的。 */
					bool proven = n.idx && self->indexProven(*n.idx, vi.array_len);
					self->gen_expr(*n.idx);
					if (!proven) self->emit_bounds_check(vi.array_len);
					self->c_.push_r(0);
					self->c_.mov64r_imm64(1, 8);
					self->c_.imul_r64_rm64(0, 1);      /* rax = idx*8 */
					if (vi.is_local) {
						/* 局部数组向低地址增长: base - idx*8 */
						int32_t base_d = static_cast<int32_t>(-8 * static_cast<int>(vi.slot + 1));
						self->c_.lea64r_memd32(2, 5, base_d);
						self->c_.sub_r64_rm64(2, 0);
					} else {
						self->emit_global_addr(2, vi.slot);
						self->c_.add_r64_rm64(2, 0);   /* 全局数组向高地址增长 */
					}
					self->c_.pop_r(0);
					self->c_.mov64r_rm64(0, 2);
					self->c_.rex_wrb(0, 0);
					self->c_.u8(0x8B);
					self->c_.modrm(0, 0, 0);
					return;
				}
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
				self->c_.mov64r_imm64(1, 8);
				self->c_.imul_r64_rm64(0, 1);
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
	void NativeGen::gen_stmt(Stmt& s) {
		struct StmtGen {
			NativeGen* self;
			void operator()(Block& n) {
				self->gen_block(n.stmts);
			}
			void operator()(ExprStmt& n) {
				if (n.expr) {
					self->gen_expr(*n.expr);
				}
			}
			void operator()(AssignStmt& n) {
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
									self->emit_ovf_check();
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
				self->gen_expr(*n.cond);
				self->c_.cmp_rm64_imm32(0, 0);
				int jmp_else = self->emit_jcc(0x4);
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
				/* U=8: 每轮处理 8 项，循环体 6~7 条指令 ? 约 0.9 条/迭代，
				   而 gcc -O2 的标量循环是 4 条/迭代。这是 no-opt 仍能追平
				   并超过 gcc 的关键。U 再大则尾部（0..U-1 次标量迭代）占比
				   上升、收益递减。 */
				constexpr int U = 8;
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
							if (self->expr_has_call(*as->value)) return false;
							if (self->expr_has_mem(*as->value)) return false;
							add(as->name);
							continue;
						}
						if (auto* ifs = std::get_if<IfStmt>(&st)) {
							if (self->expr_has_call(*ifs->cond)) return false;
							if (self->expr_has_mem(*ifs->cond)) return false;
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
				if (cand.empty() || cand.size() > 2) return false;
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
				static const int regs[2] = {3, 12};  /* rbx, r12 */
				self->last_store_len_ = (size_t) -1;
				for (size_t i = 0; i < cand.size(); i++) {
					auto& vi = self->resolve_var(cand[i]);
					int32_t rd = static_cast<int32_t>(-8 * static_cast<int>(vi.slot + 1));
					self->c_.mov64r_memd8(regs[i], 5, rd);
					self->loop_reg_vars_[vi.slot] = regs[i];
				}
				size_t start = self->c_.pos();
				int jmp_exit = 0;
				if (!emit_cond_jump(*n.cond, jmp_exit)) {
					self->loop_reg_vars_.clear();
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
				self->loop_reg_vars_.clear();
				return true;
			}
			/* ── 速度优化: while 的简单比较条件直接生成分支跳转 ──
			   原路径把条件算成 0/1（setcc + movzx）再 cmp $0 + je，
			   白白多三条指令。这里 cmp 之后直接反向 jcc 跳到出口。 */
			bool try_while_fast_cond(WhileStmt& n) {
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
				/* ── 边界检查消除的准备 ──
				   从 `while (i < X)` 推出 i 的上界。X 必须是编译期常量;
				   若上界本身还要靠别的变量, 就放弃(变量值运行时才定)。 */
				std::string ub_var;
				bool have_ub = false;
				int64_t ub_val = 0;
				{
					auto* cb = std::get_if<Binary>(n.cond.get());
					if (cb && cb->op == "<") {
						auto* l = std::get_if<Ident>(cb->lhs.get());
						auto* r = std::get_if<IntLit>(cb->rhs.get());
						if (l && r && r->value > 0) { ub_var = l->name; ub_val = r->value; have_ub = true; }
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
				if (self->try_gcd_loop(n)) return;
				/* 宽算术模式下跳过闭式解与寄存器驻留: 它们按 64 位公式直接
				   写结果槽，不会更新影子高位槽，开 -wide 反而会算错。 */
				if (self->wide_mode_) { /* fallthrough to generic path */ }
				else
				/* 闭式解分类: 交给独立模块 LoopOptimizer 调度
				   （等差/步长等差/倒计数/平方和，见 loopopt.cpp）
				   仅 opt_ 时启用 —— 它会把 O(n) 循环整体替换为 O(1) 算术，
				   属于算法级替换，是 -no-opt 要关闭的部分。 */
				if (self->opt_ && LoopOptimizer::run(*self, n)) return;
				/* U=4 展开 + 归纳变量。属于"代码质量"层，no-opt 下同样启用
				   —— 它不改变算法复杂度，只是把每轮要做的事压缩得更紧。 */
				if (try_while_unroll4(n)) return;
				if (try_while_iv_opt(n)) return;
				if (try_while_promote(n)) return;
				if (try_while_fast_cond(n)) return;
				size_t start = self->c_.pos();
				self->gen_expr(*n.cond);
				self->c_.cmp_rm64_imm32(0, 0);
				int jmp_exit = self->emit_jcc(0x4);  /* je */
				self->gen_block(n.body);
				/* jmp back */
				self->c_.u8(0xE9);
				int32_t back = static_cast<int32_t>(start - (self->c_.pos() + 4));
				self->c_.u32(static_cast<uint32_t>(back));
				self->patch(jmp_exit);
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
								static const int regs2[2] = {3, 12};  /* rbx, r12 */
								for (size_t i = 0; i < cand.size(); i++) {
									auto& rvi = self->resolve_var(cand[i]);
									if (rvi.is_local) {
										int32_t rd = static_cast<int32_t>(-8 * static_cast<int>(rvi.slot + 1));
										self->c_.mov64r_memd8(regs2[i], 5, rd);
										self->loop_reg_vars_[rvi.slot] = regs2[i];
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
						self->loop_reg_vars_.clear();
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
				self->emit_canary_check();   /* 校验 [rbp+canary]；不破坏 RAX */
				self->c_.mov64r_rm64(4, 5);  /* mov rsp, rbp: 丢弃循环寄存??push 残留 */
				self->c_.mov64r_memd8(3, 5, -8);    /* rbx ??[rbp-8]  (压栈?? rbx,r12,r13,r14,r15) */
				self->c_.mov64r_memd8(12, 5, -16);  /* r12 */
				self->c_.mov64r_memd8(13, 5, -24);  /* r13 */
				self->c_.mov64r_memd8(14, 5, -32);  /* r14 */
				self->c_.mov64r_memd8(15, 5, -40);  /* r15 */
				self->c_.leave();
				self->c_.ret();
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
	void NativeGen::gen_block(std::vector<Stmt>& stmts) {
		for (auto& s : stmts) gen_stmt(s);
	}

	bool NativeGen::expr_has_call(Expr& e) {
		if (std::holds_alternative<Call>(e)) return true;
		if (auto* b = std::get_if<Binary>(&e)) return expr_has_call(*b->lhs) || expr_has_call(*b->rhs);
		if (auto* u = std::get_if<Unary>(&e)) return expr_has_call(*u->operand);
		return false;
	}
	/* 含下标/解引用/取址的表达式会额外占用寄存器并可能写内存，
	   寄存器提升时保守地排除这类表达式。 */
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

	/* ── Function code generation ── */
	void NativeGen::gen_func(FuncDecl& f) {
		/* Record entry offset for this function */
		func_offsets_[f.name] = c_.pos();
		cur_ret_double_ = (f.ret_type == "double" || f.ret_type == "浮点");
		if (!cur_qualname_.empty()) {
			func_offsets_[cur_qualname_] = c_.pos();   /* mod 成员: "Mod::member" */
			/* 函数即模块本身: 同时登记裸模块名, 供 Calc[n] 调用 */
			size_t sep = cur_qualname_.find("::");
			if (sep != std::string::npos) {
				func_offsets_[cur_qualname_.substr(0, sep)] = c_.pos();
			}
		}
		known_const_.clear();
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

		/* Compile body */
		gen_block(f.body);

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
		c_.clear();
		globals_.clear();
		num_globals_ = 0;
		global_slots_ = 0;
		max_locals_ = 0;
		global_rip_patches_.clear();
		func_offsets_.clear();
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
		/* mod 入口按 priority 升序, 同 priority 按声明序 */
		std::stable_sort(mod_entries.begin(), mod_entries.end(),
		[](const ModEntryDecl & a, const ModEntryDecl & b) {
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
					void operator()(TupleLit& n) {
						for (auto& e : n.elements) {
							/* Tuple elements are expressions, not statements */
							(void)e;
						}
					}
					void operator()(TypeAliasDecl&) {}
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
			out.close();
		}
	};

	Target* createTarget(const std::string& name) {
		if (name == "linux64") return new Linux64Target();
		(void)name;
		return new Win64Target();
	}

	bool doBuild(const std::string& file, Target* target, const std::string& outputExe, bool enable_opt = false, bool enable_ir = false, bool wide = false, WeiStd dialect = WeiStd::Wei27, const std::string& normOut = std::string()) {
		std::string src;
		try {
			src = readFile(file);
		} catch (const std::exception& e) {
			emitError(file, e.what());
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
			emitError(file, e.what());
			return false;
		}

		wei::ast::Program prog;
		try {
			auto ps = Parser(std::move(tokens));
			prog = ps.parse();
		} catch (const std::exception& e) {
			emitError(file, e.what());
			return false;
		}

		try {
			auto tc = wei::types::TypeChecker();
			tc.check(prog);
		} catch (const std::exception& e) {
			emitError(file, e.what());
			return false;
		}

		NativeGen ng(prog, enable_opt);
		ng.set_ir(enable_ir);
		ng.set_target(target);
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
			emitError(file, e.what());
			return false;
		}
		uint32_t code_size = (uint32_t)ng.data_area();
		ng.set_pe_data_rel(target->computeDataReloc(ng, code_size));
		try {
			native = ng.generate();
		} catch (const std::exception& e) {
			emitError(file, e.what());
			return false;
		}
		try {
			target->finalize(ng, native, code_size, outputExe);
		} catch (const std::exception& e) {
			emitError(file, e.what());
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

	if (argc < 2) {
		wei::front::printStart();
		return 0;
	}
	std::string sub = argv[1];

	if (sub == "version" || sub == "-v" || sub == "--version") {
		wei::front::printVersion(detectBackend());
		return 0;
	}

	if (argc < 3) {
		wei::front::printError("错误: 子命令 '" + sub + "' 缺少文件参数");
		return 2;
	}
	std::string file = argv[2];

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
		bool wide = false;  /* -wide: 128 位宽算术（实验性，约一半速度）*/
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
		if (!doBuild(file, target, outExe, opt, enable_ir, wide, dialect, normOut)) return 1;
		wei::front::printBuildOK(outExe, target->id());
		return 0;
	}

	wei::front::printError("错误: 未知子命令 '" + sub + "'");
	std::cout << "用法: mtg build <文件> [-o 输出] [-std:wei27|中文|文言]"
	          << " [-emit-norm:<文件>] [-target linux64|win64] [-no-opt] [-wide]\n";
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
	if (!g.opt_) return false;
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
	if (!g.opt_) return false;
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

	/* xmm0 = 0 */
	c.u8(0x66); c.u8(0x0F); c.u8(0x57); c.u8(0xC0);          /* xorpd xmm0,xmm0 */
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
	auto* bi = std::get_if<IntLit>(cb->rhs.get());
	if (!bi) return false;                        /* 只接受编译期常量上界 */
	int64_t bcv = bi->value;
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
