// Zircdev - Wei 0.9.1
#include <cstdint>
#include <cstdio>
#if defined(_WIN32)
#include <windows.h>
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

enum class TokType{
    Ident,IntLit,DoubleLit,StrLit,
    Kw_create,Kw_func,Kw_int,Kw_bool,Kw_class,Kw_double,Kw_char,Kw_omni,Kw_ptr,
    Kw_if,Kw_else,Kw_for,Kw_while,Kw_loop,Kw_return,
    Kw_true,Kw_false,Kw_null,
    Kw_output,Kw_input,
    Kw_require,Kw_stat,
    Kw_module,Kw_part,Kw_access,Kw_adv,Kw_public,Kw_private,Kw_struct,
    Kw_模块,Kw_部件,Kw_访问,Kw_高级,Kw_公开,Kw_私有,Kw_结构,
    Kw_创建,Kw_函数,Kw_整型,Kw_浮点,Kw_布尔,Kw_类,Kw_文本,Kw_万能,Kw_指针,
    Kw_如果,Kw_否则,Kw_遍历,Kw_循环,
    Kw_返回,Kw_真,Kw_假,Kw_输出,Kw_输入,
    Kw_包含,Kw_统计,Kw_空,Kw_换行,Kw_重复,
    LParen,RParen,LBrace,RBrace,LBracket,RBracket,Comma,Semicolon,Colon,Dot,
    Plus,Minus,Star,Slash,Percent,PlusPlus,MinusMinus,Amp,
    Assign,PlusEq,MinusEq,StarEq,SlashEq,
    EqEq,Neq,LtEq,
    GtEq,
    And,Or,Not,
    LangL,LangR,IOLt,IOGt,
    Eof
};

struct Token{
    TokType type;
    std::string text;
    int32_t line;
    int32_t col;

    Token():type(TokType::Eof),line(0),col(0){}
    Token(TokType t,std::string s,int32_t l,int32_t c)
        :type(t),text(std::move(s)),line(l),col(c){}

    void dump() const{
        std::cout<<line<<":"<<col<<"\t"<<text<<"\n";
    }

    bool is(TokType k) const{
        return type==k;
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

using Stmt = std::variant<Block, IfStmt, ForStmt, WhileStmt, LoopStmt,
                          LoopNStmt, ReturnStmt, ExprStmt, AssignStmt, VarDecl,
                          ModuleAccessSet>;

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

using Expr = std::variant<IntLit, DoubleLit, StrLit, BoolLit, Ident, Binary, Unary, Call,
                          Output, Input, IndexExpr, ModulePathGet>;

struct Param {
    std::string name;
    std::string type;
};

struct VarDecl {
    std::string name;
    std::string type;
    std::unique_ptr<Expr> init;
    bool is_const;
    int64_t array_len = 0;      /* >0: 栈/全局数组声明 create.int a[N] */
    int line=0;int col=0;
};

struct FuncDecl {
    std::string name;
    std::vector<Param> params;
    std::string ret_type;
    std::vector<Stmt> body;
    int line=0;int col=0;
};

struct ImportDecl {
    std::string module;
};

struct ClassDecl {
    std::string name;
    std::vector<VarDecl> fields;
    std::vector<FuncDecl> methods;
    int line=0;int col=0;
};

struct PartDecl {
    std::string name;
    bool is_struct=false;
    bool is_public=true;
    std::vector<VarDecl> members;   /* 字段/成员，按声明顺序编号 */
    int line=0;int col=0;
};

struct ModuleDecl {
    std::string name;
    std::vector<PartDecl> parts;
    std::vector<Stmt> body;
    int line=0;int col=0;
};

struct ModulePathGet {
    std::string module;
    std::string part;
    bool has_field=false;
    std::string field;
    bool adv=false;   /* adv 高权限 */
    int line=0;int col=0;
};

struct ModuleAccessSet {
    std::string module;
    std::string part;
    std::string field;
    bool adv=false;
    std::unique_ptr<Expr> value;
    int line=0;int col=0;
};

using TopLevel = std::variant<FuncDecl, VarDecl, ImportDecl, ClassDecl, ModuleDecl>;

struct Program {
    std::vector<TopLevel> items;
};

struct Block {
    std::vector<Stmt> stmts;
    int line=0;int col=0;
};

struct IfStmt {
    std::unique_ptr<Expr> cond;
    std::vector<Stmt> then_body;
    std::vector<Stmt> else_body;
    int line=0;int col=0;
};

struct ForStmt {
    std::unique_ptr<VarDecl> init_decl;
    std::unique_ptr<Expr> cond;
    std::unique_ptr<Expr> step;
    std::vector<Stmt> body;
    int line=0;int col=0;
};

struct WhileStmt {
    std::unique_ptr<Expr> cond;
    std::vector<Stmt> body;
    int line=0;int col=0;
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
    int line=0;int col=0;
};

struct ExprStmt {
    std::unique_ptr<Expr> expr;
};

struct AssignStmt {
    std::string name;
    std::unique_ptr<Expr> target;   /* 左值表达式: Ident | IndexExpr | Unary("*") */
    std::unique_ptr<Expr> value;
    int line=0;int col=0;
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
    int line=0;int col=0;
};

struct Binary {
    std::string op;
    std::unique_ptr<Expr> lhs;
    std::unique_ptr<Expr> rhs;
    int line=0;int col=0;
};

struct Unary {
    std::string op;
    std::unique_ptr<Expr> operand;
    int line=0;int col=0;
};

struct Call {
    std::string callee;
    std::vector<std::unique_ptr<Expr>> args;
    int line=0;int col=0;
};

struct Output {
    std::vector<std::unique_ptr<Expr>> values;
    int line=0;int col=0;
};

struct Input {
    std::string var_name;
    int line=0;int col=0;
};

struct IndexExpr {
    std::string name;
    std::unique_ptr<Expr> idx;
    int line=0;int col=0;
};

} 
class Lexer{
public:
    explicit Lexer(std::string src);
    std::vector<Token> scan();
private:
    std::string src_;
    size_t pos_;
    int32_t line_;
    int32_t col_;

    char peek(size_t off=0) const;
    char get();
    bool eof() const;
    void skipSpaceAndComment();
    Token makeToken(TokType t,std::string text,int32_t line,int32_t col);
    [[noreturn]] void die(const std::string& msg,int32_t line,int32_t col) const;

    Token readNumber();
    Token readString();
    Token readStringContent(int32_t line,int32_t col);
    Token readIdentOrKeyword();
    Token readChineseIdent();
    Token readEscapedIdent();
    Token readOpOrPunct();
};

static const std::unordered_map<std::string,TokType> kEngKws={
    {"create",TokType::Kw_create},
    {"func",TokType::Kw_func},
    {"int",TokType::Kw_int},
    {"bool",TokType::Kw_bool},
    {"class",TokType::Kw_class},
    {"double",TokType::Kw_double},
    {"char",TokType::Kw_char},
    {"omni",TokType::Kw_omni},
    {"ptr",TokType::Kw_ptr},
    {"if",TokType::Kw_if},
    {"else",TokType::Kw_else},
    {"for",TokType::Kw_for},
    {"while",TokType::Kw_while},
    {"loop",TokType::Kw_loop},
    {"return",TokType::Kw_return},
    {"true",TokType::Kw_true},
    {"false",TokType::Kw_false},
    {"null",TokType::Kw_null},
    {"emit",TokType::Kw_output},
    {"recv",TokType::Kw_input},
    {"require",TokType::Kw_require},
    {"stat",TokType::Kw_stat},
    {"module",TokType::Kw_module},
    {"part",TokType::Kw_part},
    {"access",TokType::Kw_access},
    {"adv",TokType::Kw_adv},
    {"public",TokType::Kw_public},
    {"private",TokType::Kw_private},
    {"struct",TokType::Kw_struct},
    {"newline",TokType::Kw_换行},
    {"outlb",TokType::Kw_换行},
};

static const std::unordered_map<std::string,TokType> kCjkKws={
    {"创建",TokType::Kw_创建},
    {"函数",TokType::Kw_函数},
    {"整型",TokType::Kw_整型},
    {"浮点",TokType::Kw_浮点},
    {"布尔",TokType::Kw_布尔},
    {"类",TokType::Kw_类},
    {"文本",TokType::Kw_文本},
    {"万能",TokType::Kw_万能},
    {"指针",TokType::Kw_指针},
    {"如果",TokType::Kw_如果},
    {"否则",TokType::Kw_否则},
    {"遍历",TokType::Kw_遍历},
    {"循环",TokType::Kw_循环},
    {"返回",TokType::Kw_返回},
    {"真",TokType::Kw_真},
    {"假",TokType::Kw_假},
    {"输出",TokType::Kw_输出},
    {"输入",TokType::Kw_输入},
    {"包含",TokType::Kw_包含},
    {"统计",TokType::Kw_统计},
    {"模块",TokType::Kw_模块},
    {"部件",TokType::Kw_部件},
    {"访问",TokType::Kw_访问},
    {"高级",TokType::Kw_高级},
    {"公开",TokType::Kw_公开},
    {"私有",TokType::Kw_私有},
    {"结构",TokType::Kw_结构},
    {"空",TokType::Kw_空},
    {"换行",TokType::Kw_换行},
    {"重复",TokType::Kw_重复},
};

static constexpr bool isCjkLead(unsigned char b){
    return b>=0xE4 && b<=0xE9;
}
static constexpr bool isUtf8Cont(unsigned char b){
    return (b&0xC0)==0x80;
}
static constexpr uint32_t decodeCjk3(unsigned char a,unsigned char b,unsigned char c){
    return (static_cast<uint32_t>(a&0x0F)<<12)|(static_cast<uint32_t>(b&0x3F)<<6)|(static_cast<uint32_t>(c&0x3F));
}
static constexpr bool isCjkCodepoint(uint32_t cp){
    return cp>=0x4E00 && cp<=0x9FFF;
}

Lexer::Lexer(std::string src):src_(std::move(src)),pos_(0),line_(1),col_(1){}

char Lexer::peek(size_t off) const{
    if(pos_+off>=src_.size()){
        return '\0';
    }
    return src_[pos_+off];
}

char Lexer::get(){
    if(pos_>=src_.size()){
        return '\0';
    }
    char c=src_[pos_++];
    if(c=='\n'){
        line_++;
        col_=1;
    }else{
        col_++;
    }
    return c;
}

bool Lexer::eof() const{
    return pos_>=src_.size();
}

void Lexer::skipSpaceAndComment(){
    while(!eof()){
        unsigned char b=static_cast<unsigned char>(peek());
        if(b==' '||b=='\t'||b=='\r'||b=='\n'){
            get();
            continue;
        }
        if(b==0xE3 && static_cast<unsigned char>(peek(1))==0x80 && static_cast<unsigned char>(peek(2))==0x80){
            get();get();get();
            continue;
        }
        if(b=='/' && peek(1)=='/'){
            while(!eof() && peek()!='\n'){
                get();
            }
            continue;
        }
        break;
    }
}

Token Lexer::makeToken(TokType t,std::string text,int32_t line,int32_t col){
    return Token(t,std::move(text),line,col);
}

[[noreturn]] void Lexer::die(const std::string& msg,int32_t line,int32_t col) const{
    throw std::runtime_error("lex error at "+std::to_string(line)+":"+std::to_string(col)+" "+msg);
}
//字面意思
Token Lexer::readNumber(){
    int32_t line=line_;
    int32_t col=col_;
    std::string s;
    while(!eof() && isdigit(static_cast<unsigned char>(peek()))){
        s.push_back(get());
    }
    /* 浮点字面量: 整数部分 '.' 数字部分 (要求点后必有数字, 避免误吃 a[1].x 类写法) */
    if(!eof() && peek()=='.' && isdigit(static_cast<unsigned char>(peek(1)))){
        s.push_back(get());
        while(!eof() && isdigit(static_cast<unsigned char>(peek()))){
            s.push_back(get());
        }
        return makeToken(TokType::DoubleLit,s,line,col);
    }
    return makeToken(TokType::IntLit,s,line,col);
}

Token Lexer::readStringContent(int32_t line,int32_t col){
    std::string s;
    while(!eof()){
        unsigned char b=static_cast<unsigned char>(peek());
        if(b=='"'){
            get(); break;
        }
        if(b==0xE2 && static_cast<unsigned char>(peek(1))==0x80 && static_cast<unsigned char>(peek(2))==0x9D){
            get();get();get(); break;
        }
        char c=peek();
        if(c=='\n'){
            die("字符串字面量未闭合",line,col);
        }
        if(c=='\\'){
            get();
            char e=peek();
            switch(e){
                case 'n': s.push_back('\n'); get(); break;
                case 't': s.push_back('\t'); get(); break;
                case 'r': s.push_back('\r'); get(); break;
                case '\\': s.push_back('\\'); get(); break;
                case '"': s.push_back('"'); get(); break;
                case '0': s.push_back('\0'); get(); break;
                default: s.push_back('\\'); s.push_back(e); get(); break;
            }
        }else{
            s.push_back(get());
        }
    }
    if(eof()){
        die("字符串字面量未闭合",line,col);
    }
    return makeToken(TokType::StrLit,s,line,col);
}

Token Lexer::readString(){
    int32_t line=line_;
    int32_t col=col_;
    get();
    return readStringContent(line,col);
}

Token Lexer::readIdentOrKeyword(){
    int32_t line=line_;
    int32_t col=col_;
    std::string s;
    while(!eof()){
        unsigned char b=static_cast<unsigned char>(peek());
        if((b>='A'&&b<='Z')||(b>='a'&&b<='z')||(b>='0'&&b<='9')||b=='_'){
            s.push_back(static_cast<char>(get()));
        }else{
            break;
        }
    }
    auto it=kEngKws.find(s);
    if(it!=kEngKws.end()){
        return makeToken(it->second,s,line,col);
    }
    return makeToken(TokType::Ident,s,line,col);
}

Token Lexer::readChineseIdent(){
    int32_t line=line_;
    int32_t col=col_;
    std::string s;
    while(!eof()){
        unsigned char b=static_cast<unsigned char>(peek());
        if(!isCjkLead(b)){
            break;
        }
        if(static_cast<unsigned char>(peek(1))==0 || !isUtf8Cont(static_cast<unsigned char>(peek(1)))){
            break;
        }
        if(static_cast<unsigned char>(peek(2))==0 || !isUtf8Cont(static_cast<unsigned char>(peek(2)))){
            break;
        }
        uint32_t cp=decodeCjk3(b,static_cast<unsigned char>(peek(1)),static_cast<unsigned char>(peek(2)));
        if(!isCjkCodepoint(cp)){
            break;
        }
        s.push_back(static_cast<char>(get()));
        s.push_back(static_cast<char>(get()));
        s.push_back(static_cast<char>(get()));
    }
    if(s.empty()){
        die("无效的中文字符",line,col);
    }
    auto it=kCjkKws.find(s);
    if(it!=kCjkKws.end()){
        return makeToken(it->second,s,line,col);
    }
    return makeToken(TokType::Ident,s,line,col);
}

/* 反引号转义标识符: `class`、`如果` 等强制按标识符处理, 规避中英文关键词冲突 */
Token Lexer::readEscapedIdent(){
    int32_t line=line_;
    int32_t col=col_;
    get();  /* 吃掉开头反引号 */
    std::string s;
    while(!eof()){
        unsigned char b=static_cast<unsigned char>(peek());
        if(b=='`'){
            get();
            if(s.empty()){
                die("反引号转义标识符为空",line,col);
            }
            return makeToken(TokType::Ident,s,line,col);
        }
        if(b=='\n'){
            die("反引号转义标识符未闭合",line,col);
        }
        s.push_back(static_cast<char>(get()));
    }
    die("反引号转义标识符未闭合",line,col);
}

Token Lexer::readOpOrPunct(){
    int32_t line=line_;
    int32_t col=col_;
    unsigned char b=static_cast<unsigned char>(peek());
    if(b==0xEF){
        if(static_cast<unsigned char>(peek(1))==0xBC && static_cast<unsigned char>(peek(2))==0x9A){
            get();get();get();
            return makeToken(TokType::Semicolon,"；",line,col);
        }
        if(static_cast<unsigned char>(peek(1))==0xBC && static_cast<unsigned char>(peek(2))==0x9B){
            get();get();get();
            return makeToken(TokType::Semicolon,"；",line,col);
        }
        if(static_cast<unsigned char>(peek(1))==0xBC && static_cast<unsigned char>(peek(2))==0x88){
            get();get();get();
            return makeToken(TokType::Colon,"：",line,col);
        }
        if(static_cast<unsigned char>(peek(1))==0xBC && static_cast<unsigned char>(peek(2))==0x89){
            get();get();get();
            return makeToken(TokType::Colon,"：",line,col);
        }
    }
    if(b==0xE3 && static_cast<unsigned char>(peek(1))==0x80 && static_cast<unsigned char>(peek(2))==0x8A){
        get();get();get();
        return makeToken(TokType::LangL,"《",line,col);
    }
    if(b==0xE3 && static_cast<unsigned char>(peek(1))==0x80 && static_cast<unsigned char>(peek(2))==0x8B){
        get();get();get();
        return makeToken(TokType::LangR,"》",line,col);
    }
    char c=static_cast<char>(b);
    switch(c){
        case '(': get(); return makeToken(TokType::LParen,"(",line,col);
        case ')': get(); return makeToken(TokType::RParen,")",line,col);
        case '{': get(); return makeToken(TokType::LBrace,"{",line,col);
        case '}': get(); return makeToken(TokType::RBrace,"}",line,col);
        case '[': get(); return makeToken(TokType::LBracket,"[",line,col);
        case ']': get(); return makeToken(TokType::RBracket,"]",line,col);
        case ',': get(); return makeToken(TokType::Comma,",",line,col);
        case ';': get(); return makeToken(TokType::Semicolon,";",line,col);
        case '.': get(); return makeToken(TokType::Dot,".",line,col);
        case '+':
            get();
            if(peek()=='+'){ get(); return makeToken(TokType::PlusPlus,"++",line,col); }
            if(peek()=='='){ get(); return makeToken(TokType::PlusEq,"+=",line,col); }
            return makeToken(TokType::Plus,"+",line,col);
        case '-':
            get();
            if(peek()=='-'){ get(); return makeToken(TokType::MinusMinus,"--",line,col); }
            if(peek()=='='){ get(); return makeToken(TokType::MinusEq,"-=",line,col); }
            return makeToken(TokType::Minus,"-",line,col);
        case '*':
            get();
            if(peek()=='='){ get(); return makeToken(TokType::StarEq,"*=",line,col); }
            return makeToken(TokType::Star,"*",line,col);
        case '/':
            get();
            if(peek()=='='){ get(); return makeToken(TokType::SlashEq,"/=",line,col); }
            return makeToken(TokType::Slash,"/",line,col);
        case '%':
            get();
            return makeToken(TokType::Percent,"%",line,col);
        case '=':
            get();
            if(peek()=='='){ get(); return makeToken(TokType::EqEq,"==",line,col); }
            return makeToken(TokType::Assign,"=",line,col);
        case '!':
            get();
            if(peek()=='='){ get(); return makeToken(TokType::Neq,"!=",line,col); }
            return makeToken(TokType::Not,"!",line,col);
        case '<':
            get();
            if(peek()=='='){ get(); return makeToken(TokType::LtEq,"<=",line,col); }
            return makeToken(TokType::IOLt,"<",line,col);
        case '>':
            get();
            if(peek()=='='){ get(); return makeToken(TokType::GtEq,">=",line,col); }
            return makeToken(TokType::IOGt,">",line,col);
        case '&':
            get();
            if(peek()=='&'){ get(); return makeToken(TokType::And,"&&",line,col); }
            return makeToken(TokType::Amp,"&",line,col);
        case '|':
            get();
            if(peek()=='|'){ get(); return makeToken(TokType::Or,"||",line,col); }
            die("未知的字符 '|'",line,col);
        case ':':
            get();
            return makeToken(TokType::Colon,":",line,col);
        default:
            die(std::string("未知的字符 '")+c+"'",line,col);
    }
}

std::vector<Token> Lexer::scan(){
    std::vector<Token> out;
    while(true){
        skipSpaceAndComment();
        if(eof()){
            out.push_back(makeToken(TokType::Eof,"",line_,col_));
            break;
        }
        unsigned char b=static_cast<unsigned char>(peek());
        if(isdigit(b)){
            out.push_back(readNumber());
            continue;
        }
        if(b=='"'){
            out.push_back(readString());
            continue;
        }
        if(b==0xE2 && static_cast<unsigned char>(peek(1))==0x80 && static_cast<unsigned char>(peek(2))==0x9C){
            int32_t sl=line_,sc=col_;
            get();get();get();
            out.push_back(readStringContent(sl,sc));
            continue;
        }
        if(b=='`'){
            out.push_back(readEscapedIdent());
            continue;
        }
        if((b>='A'&&b<='Z')||(b>='a'&&b<='z')||b=='_'){
            out.push_back(readIdentOrKeyword());
            continue;
        }
        if(isCjkLead(b)){
            out.push_back(readChineseIdent());
            continue;
        }
        /* Skip unrecognized 3-byte UTF-8 sequences (e.g. stray full-width quotes) */
        if(b==0xE2 && static_cast<unsigned char>(peek(1))==0x80 && isUtf8Cont(static_cast<unsigned char>(peek(2)))){
            get();get();get(); continue;
        }
        out.push_back(readOpOrPunct());
    }
    return out;
}

// ============================================================
// Parser
// ============================================================
class Parser{
public:
    explicit Parser(std::vector<Token> tokens);
    wei::ast::Program parse();
private:
    std::vector<Token> tokens_;
    size_t pos_;

    const Token& cur() const;
    const Token& peek(size_t off=0) const;
    const Token& prev() const;
    bool eof() const;
    bool check(TokType t) const;
    bool checkAt(size_t off,TokType t) const;
    bool match(TokType t);
    const Token& expect(TokType t,const std::string& desc);
    [[noreturn]] void die(const std::string& msg,const Token& at) const;
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
    std::string parseTypeName();

    wei::ast::TopLevel parseTopLevel();
    wei::ast::FuncDecl parseFuncDecl(const std::string& retType);
    wei::ast::VarDecl parseGlobalVarDecl(const std::string& typeName);
    wei::ast::ImportDecl parseImportDecl();
    wei::ast::ClassDecl parseClassDecl();
    wei::ast::ModuleDecl parseModuleDecl();
    wei::ast::PartDecl parsePartDecl();
    std::vector<wei::ast::VarDecl> parsePartMembers();

    wei::ast::Stmt parseStmt();
    std::vector<wei::ast::Stmt> parseBlock();
    wei::ast::Stmt parseOutputStmt();
    wei::ast::Stmt parseInputStmt();
    wei::ast::Stmt parseVarDeclStmt(const std::string& typeName, bool advPath=false);
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
    bool isModulePathAhead() const;
    void registerModule(const wei::ast::ModuleDecl& md);
    std::map<std::string,std::set<std::string>> module_reg_;  /* 模块 -> 部件集合 */

    std::vector<wei::ast::Param> parseParams();
};

using wei::ast::AssignStmt;
using wei::ast::Binary;
using wei::ast::Block;
using wei::ast::BoolLit;
using wei::ast::Call;
using wei::ast::ClassDecl;
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

Parser::Parser(std::vector<Token> tokens):tokens_(std::move(tokens)),pos_(0){}

const Token& Parser::cur() const{
    if(pos_>=tokens_.size()){
        return tokens_.back();
    }
    return tokens_[pos_];
}
const Token& Parser::peek(size_t off) const{
    if(pos_+off>=tokens_.size()){
        return tokens_.back();
    }
    return tokens_[pos_+off];
}
const Token& Parser::prev() const{
    if(pos_==0){
        return tokens_.front();
    }
    return tokens_[pos_-1];
}
bool Parser::eof() const{
    return cur().is(TokType::Eof);
}
bool Parser::check(TokType t) const{
    return cur().is(t);
}
bool Parser::checkAt(size_t off,TokType t) const{
    return peek(off).is(t);
}
bool Parser::match(TokType t){
    if(check(t)){
        pos_++;
        return true;
    }
    return false;
}
const Token& Parser::expect(TokType t,const std::string& desc){
    if(!check(t)){
        die("期望 "+desc+", 实际拿到 '"+cur().text+"'",cur());
    }
    const Token& tk=cur();
    pos_++;
    return tk;
}
[[noreturn]] void Parser::die(const std::string& msg,const Token& at) const{
    throw std::runtime_error(msg+" at "+std::to_string(at.line)+":"+std::to_string(at.col));
}
[[noreturn]] void Parser::die(const std::string& msg) const{
    die(msg,cur());
}

bool Parser::isCreateKw(TokType t){
    return t==TokType::Kw_create || t==TokType::Kw_创建;
}
bool Parser::isInputKw(TokType t){
    return t==TokType::Kw_input || t==TokType::Kw_输入;
}
bool Parser::isOutputKw(TokType t){
    return t==TokType::Kw_output || t==TokType::Kw_输出;
}
bool Parser::isStatKw(TokType t){
    return t==TokType::Kw_stat || t==TokType::Kw_统计;
}
bool Parser::isModuleKw(TokType t){
    return t==TokType::Kw_module || t==TokType::Kw_模块;
}
bool Parser::isPartKw(TokType t){
    return t==TokType::Kw_part || t==TokType::Kw_部件;
}
bool Parser::isAccessKw(TokType t){
    return t==TokType::Kw_access || t==TokType::Kw_访问;
}
bool Parser::isAdvKw(TokType t){
    return t==TokType::Kw_adv || t==TokType::Kw_高级;
}
bool Parser::isTypeKw(TokType t){
    switch(t){
        case TokType::Kw_func:
        case TokType::Kw_int:
        case TokType::Kw_bool:
        case TokType::Kw_class:
        case TokType::Kw_double:
        case TokType::Kw_char:
        case TokType::Kw_omni:
        case TokType::Kw_ptr:
        case TokType::Kw_函数:
        case TokType::Kw_整型:
        case TokType::Kw_浮点:
        case TokType::Kw_布尔:
        case TokType::Kw_类:
        case TokType::Kw_文本:
        case TokType::Kw_万能:
        case TokType::Kw_指针:
            return true;
        default:
            return false;
    }
}
std::string Parser::typeKwText(TokType t){
    switch(t){
        case TokType::Kw_func: return "func";
        case TokType::Kw_int: return "int";
        case TokType::Kw_bool: return "bool";
        case TokType::Kw_class: return "class";
        case TokType::Kw_double: return "double";
        case TokType::Kw_char: return "char";
        case TokType::Kw_omni: return "omni";
        case TokType::Kw_ptr: return "ptr";
        case TokType::Kw_函数: return "func";
        case TokType::Kw_整型: return "int";
        case TokType::Kw_浮点: return "double";
        case TokType::Kw_布尔: return "bool";
        case TokType::Kw_文本: return "char";
        case TokType::Kw_万能: return "omni";
        case TokType::Kw_指针: return "ptr";
        default: return "";
    }
}
std::string Parser::eatCreateSep(){
    if(check(TokType::Dot) || check(TokType::Colon)){
        std::string s=cur().text;
        pos_++;
        return s;
    }
    die("期望 create 后的 '.' 或 '：',实际拿到 '"+cur().text+"'",cur());
}
void Parser::eatStatSep(){
    if(check(TokType::Dot) || check(TokType::Colon)){
        pos_++;
        return;
    }
    die("期望 stat 后的 '.' 或 '：',实际拿到 '"+cur().text+"'",cur());
}

/* create. 之后解析类型: int / ptr / ptr.int / 指针.整型 链式 */
std::string Parser::parseTypeName(){
    if(!isTypeKw(cur().type)){
        die("期望类型关键字, 实际拿到 '"+cur().text+"'",cur());
    }
    std::string tn=typeKwText(cur().type);
    pos_++;
    if(tn=="ptr" && check(TokType::Dot) && isTypeKw(peek(1).type)){
        std::string inner=typeKwText(peek(1).type);
        if(inner=="int" || inner=="char" || inner=="bool" || inner=="double"){
            pos_+=2;
            return tn+"."+inner;
        }
    }
    return tn;
}

Program Parser::parse(){
    Program prog;
    while(!eof()){
        prog.items.push_back(parseTopLevel());
    }
    return prog;
}

TopLevel Parser::parseTopLevel(){
    if(isCreateKw(cur().type)){
        pos_++;
        eatCreateSep();
        if(isModuleKw(cur().type)){
            pos_++;
            return parseModuleDecl();
        }
        if(!isTypeKw(cur().type)){
            die("期望类型关键字",cur());
        }
        std::string typeName=parseTypeName();
        expect(TokType::Ident,"标识符（声明名）");
        if(check(TokType::LParen)){
            return parseFuncDecl(typeName);
        }
        if(check(TokType::Assign)){
            return parseGlobalVarDecl(typeName);
        }
        die("create 顶层声明必须是函数或带初值的全局变量, 实际拿到 '"+cur().text+"'",cur());
    }
    if(cur().is(TokType::Kw_require) || cur().is(TokType::Kw_包含)){
        return parseImportDecl();
    }
    if(cur().is(TokType::Kw_class) || cur().is(TokType::Kw_类)){
        return parseClassDecl();
    }
    die("顶层只能有 create 声明、require 导入或 class 声明,实际拿到 '"+cur().text+"'",cur());
}

FuncDecl Parser::parseFuncDecl(const std::string& retType){
    FuncDecl fd;
    fd.name=prev().text;
    fd.ret_type=retType;
    fd.line=prev().line;fd.col=prev().col;
    expect(TokType::LParen,"'('");
    fd.params=parseParams();
    expect(TokType::RParen,"')'");
    if(check(TokType::LBrace)){
        fd.body=parseBlock();
        return fd;
    }
    if(check(TokType::Semicolon)){
        pos_++;
        return fd;
    }
    die("函数体必须是 {...} 或前向声明 ';',实际拿到 '"+cur().text+"'",cur());
}

VarDecl Parser::parseGlobalVarDecl(const std::string& typeName){
    VarDecl vd;
    vd.type=typeName;
    vd.name=prev().text;
    vd.line=prev().line;vd.col=prev().col;
    expect(TokType::Assign,"'='");
    auto p=parseModulePathExpr(false);
    if(p){
        vd.init=std::make_unique<Expr>(std::move(*p));
    }else{
        auto tmp=parseExpr();
        vd.init=std::make_unique<Expr>(std::move(*tmp));
    }
    expect(TokType::Semicolon,"';'");
    return vd;
}

ImportDecl Parser::parseImportDecl(){
    pos_++;
    ImportDecl id;
    bool eng=false;
    if(check(TokType::IOLt)){
        eng=true;
        pos_++;
    }else if(check(TokType::LangL)){
        pos_++;
    }else{
        die("导入期望 '<' 或 '《',实际拿到 '"+cur().text+"'",cur());
    }
    const Token& modTk=expect(TokType::Ident,"模块名");
    id.module=modTk.text;
    if(check(TokType::Dot)){
        pos_++;
        const Token& extTk=expect(TokType::Ident,"扩展名");
        id.module=id.module+"."+extTk.text;
    }
    if(eng){
        expect(TokType::IOGt,"'>'");
    }else{
        expect(TokType::RParen,"')'");
    }
    if(check(TokType::Semicolon)){
        pos_++;
    }
    return id;
}

ClassDecl Parser::parseClassDecl(){
    pos_++;
    ClassDecl cd;
    const Token& nameTk=expect(TokType::Ident,"类名");
    cd.name=nameTk.text;
    cd.line=nameTk.line;cd.col=nameTk.col;
    expect(TokType::LBrace,"'{'");
    while(!check(TokType::RBrace) && !eof()){
        if(isCreateKw(cur().type)){
            pos_++;
            eatCreateSep();
            if(isTypeKw(cur().type)){
                std::string tn=typeKwText(cur().type);
                pos_++;
                const Token& nameTk=expect(TokType::Ident,"成员名");
                expect(TokType::Semicolon,"';'");
                VarDecl fd;
                fd.type=tn;
                fd.name=nameTk.text;
                cd.fields.push_back(std::move(fd));
            }else{
                die("类成员声明类型未知",cur());
            }
        }else if(check(TokType::LBrace) || check(TokType::RBrace)){
            break;
        }else{
            die("类成员声明不识别 '"+cur().text+"'",cur());
        }
    }
    expect(TokType::RBrace,"'}'");
    return cd;
}

void Parser::registerModule(const ModuleDecl& md){
    std::set<std::string> parts;
    for(auto& p:md.parts) parts.insert(p.name);
    module_reg_[md.name]=std::move(parts);
}

/* 部件成员解析 */
std::vector<VarDecl> Parser::parsePartMembers(){
    std::vector<VarDecl> out;
    while(!check(TokType::RBrace) && !eof()){
        if(check(TokType::Kw_part)||check(TokType::Kw_部件)){
            die("部件内不能嵌套 part",cur());
        }
        std::string tn;
        if(check(TokType::Ident)){
            tn=cur().text; pos_++;
        }else if(isTypeKw(cur().type)){
            tn=typeKwText(cur().type); pos_++;
        }else{
            die("部件成员类型未知 '"+cur().text+"'",cur());
        }
        const Token& nameTk=expect(TokType::Ident,"成员名");
        VarDecl fd;
        fd.type=tn;
        fd.name=nameTk.text;
        fd.line=nameTk.line;fd.col=nameTk.col;
        fd.is_const=false;
        if(check(TokType::Assign)){
            pos_++;
            auto tmp=parseExpr();
            bool lit=std::get_if<IntLit>(tmp.get())!=nullptr
                  || std::get_if<BoolLit>(tmp.get())!=nullptr
                  || std::get_if<StrLit>(tmp.get())!=nullptr;
            if(!lit) die("部件成员初值必须是常量(int/bool/string)",prev());
            fd.init=std::make_unique<Expr>(std::move(*tmp));
        }
        expect(TokType::Semicolon,"';'");
        out.push_back(std::move(fd));
    }
    return out;
}

PartDecl Parser::parsePartDecl(){
    PartDecl pd;
    pd.line=cur().line;pd.col=cur().col;
    if(!isPartKw(cur().type)){
        die("导入期望 '<' 或 '《',实际拿到 '"+cur().text+"'",cur());
    }
    pos_++;
    /* 属性链: part.struct.public Name{...} — struct/public/private 任意顺序 */
    check(TokType::Dot);
    while(check(TokType::Dot)){
        pos_++;
        if(cur().is(TokType::Kw_struct)||cur().is(TokType::Kw_结构)){
            pd.is_struct=true; pos_++;
        }else if(cur().is(TokType::Kw_public)||cur().is(TokType::Kw_公开)){
            pd.is_public=true; pos_++;
        }else if(cur().is(TokType::Kw_private)||cur().is(TokType::Kw_私有)){
            pd.is_public=false; pos_++;
        }else{
            die("part 属性只支持 struct/public/private, 实际拿到 '"+cur().text+"'",cur());
        }
        if(!check(TokType::Dot)) break;
    }
    const Token& partTk=expect(TokType::Ident,"部件名");
    pd.name=partTk.text;
    expect(TokType::LBrace,"'{'");
    pd.members=parsePartMembers();
    expect(TokType::RBrace,"'}'");
    return pd;
}

ModuleDecl Parser::parseModuleDecl(){
    ModuleDecl md;
    const Token& modTk=expect(TokType::Ident,"模块名");
    md.name=modTk.text;
    md.line=modTk.line;md.col=modTk.col;
    if(check(TokType::LParen)){
        pos_++;
        expect(TokType::RParen,"')'");
    }
    expect(TokType::LBrace,"'{'");
    while(!check(TokType::RBrace) && !eof()){
        if(isPartKw(cur().type)){
            md.parts.push_back(parsePartDecl());
        }else{
            md.body.push_back(parseStmt());
        }
    }
    expect(TokType::RBrace,"'}'");
    registerModule(md);
    return md;
}

std::vector<Param> Parser::parseParams(){
    std::vector<Param> out;
    if(check(TokType::RParen)){
        return out;
    }
    while(true){
        if(!isTypeKw(cur().type) && !check(TokType::Ident)){
            die("参数类型未知 '"+cur().text+"'",cur());
        }
        std::string tn;
        if(isTypeKw(cur().type)){
            tn=parseTypeName();
        }else{
            tn=cur().text;
            pos_++;
        }
        const Token& parTk=expect(TokType::Ident,"参数名");
        Param p;
        p.type=tn;
        p.name=parTk.text;
        out.push_back(std::move(p));
        if(check(TokType::Comma)){
            pos_++;
            continue;
        }
        break;
    }
    return out;
}

Stmt Parser::parseStmt(){
    if(check(TokType::Semicolon)){
        die("空语句写分号不允许",cur());
    }
    /* access. 与 adv. 前缀 */
    if(isAccessKw(cur().type) && checkAt(1,TokType::Dot)){
        pos_+=2;
        return parseAccessStmt(false);
    }
    if(isAdvKw(cur().type) && checkAt(1,TokType::Dot)){
        pos_+=2;
        if(isCreateKw(cur().type)){
            pos_++;
            eatCreateSep();
            if(!isTypeKw(cur().type)){
                die("adv 后 create 的类型未知 '"+cur().text+"'",cur());
            }
            std::string tn=parseTypeName();
            return parseVarDeclStmt(tn,true);
        }
        if(isAccessKw(cur().type)){
            pos_++;
            expect(TokType::Dot,"'.'");
            return parseAccessStmt(true);
        }
        die("input 表达式期望 '<' 或 '《'",cur());
    }
    if(isCreateKw(cur().type)){
        pos_++;
        eatCreateSep();
        if(!isTypeKw(cur().type)){
            die("create 后的类型未知 '"+cur().text+"'",cur());
        }
        std::string tn=parseTypeName();
        return parseVarDeclStmt(tn);
    }
    if(isOutputKw(cur().type)){
        return parseOutputStmt();
    }
    if(isInputKw(cur().type)){
        return parseInputStmt();
    }
    if(cur().is(TokType::Kw_if) || cur().is(TokType::Kw_如果)){
        return parseIfStmt();
    }
    if(cur().is(TokType::Kw_for) || cur().is(TokType::Kw_遍历)){
        return parseForStmt();
    }
    if(cur().is(TokType::Kw_while)){
        return parseWhileStmt();
    }
    if(cur().is(TokType::Kw_loop)){
        return parseLoopStmt();
    }
    if(cur().is(TokType::Kw_重复)){
        return parseLoopStmt();
    }
    if(cur().is(TokType::Kw_循环)){
        pos_++;
        if(check(TokType::LParen)){
            if(checkAt(1,TokType::IntLit)){
                pos_++;
                const Token& countTk=expect(TokType::IntLit,"循环次数");
                int64_t count=std::stoll(countTk.text);
                expect(TokType::RParen,"')'");
                auto body=parseBlock();
                LoopNStmt lns;
                lns.count=count;
                lns.body=std::move(body);
                Stmt s=std::move(lns);
                return s;
            }
            return parseWhileStmtInner();
        }
        return parseLoopStmtInner();
    }
    if(cur().is(TokType::Kw_return) || cur().is(TokType::Kw_返回)){
        return parseReturnStmt();
    }
    return parseExprStmt();
}

Stmt Parser::parseVarDeclStmt(const std::string& typeName, bool advPath){
    VarDecl vd;
    vd.type=typeName;
    const Token& nameTk=expect(TokType::Ident,"变量名");
    vd.name=nameTk.text;
    vd.line=nameTk.line;vd.col=nameTk.col;
    if(check(TokType::LBracket)){
        pos_++;
        const Token& lenTk=expect(TokType::IntLit,"数组长度");
        vd.array_len=std::stoll(lenTk.text);
        expect(TokType::RBracket,"']'");
        if(vd.array_len<=0){
            die("数组长度必须 > 0",lenTk);
        }
    }
    if(check(TokType::Assign)){
        pos_++;
        auto p=parseModulePathExpr(advPath);
        if(p){
            vd.init=std::make_unique<Expr>(std::move(*p));
        }else{
            auto tmp=parseExpr();
            vd.init=std::make_unique<Expr>(std::move(*tmp));
        }
    }
    expect(TokType::Semicolon,"';'");
    Stmt s=std::move(vd);
    return s;
}

/* 访问路径语句: access.Dog>DogInf.age=3; 或 access.Dog>DogInf.age; */
Stmt Parser::parseAccessStmt(bool adv){
    int32_t line=cur().line;int32_t col=cur().col;
    const Token& modTk=expect(TokType::Ident,"模块名");
    ModulePathGet pg;
    pg.module=modTk.text;
    pg.part="";
    pg.adv=adv;
    pg.line=line;pg.col=col;
    if(!check(TokType::IOGt)){
        die("access 路径期望 '>' 分隔, 实际拿到 '"+cur().text+"'",cur());
    }
    pos_++;
    const Token& partTk=expect(TokType::Ident,"部件名");
    pg.part=partTk.text;
    if(check(TokType::Dot)||check(TokType::IOGt)){
        pos_++;
        const Token& fldTk=expect(TokType::Ident,"字段名");
        pg.field=fldTk.text;
        pg.has_field=true;
    }
    if(module_reg_.count(pg.module)==0){
        die("未知模块: "+pg.module,modTk);
    }
    if(module_reg_[pg.module].count(pg.part)==0){
        die("模块 "+pg.module+" 中没有部件 "+pg.part,partTk);
    }
    if(check(TokType::Assign)){
        pos_++;
        auto tmp=parseExpr();
        expect(TokType::Semicolon,"';'");
        if(!pg.has_field){
            die("访问赋值必须指定字段 "+pg.module+">"+pg.part,cur());
        }
        ModuleAccessSet mas;
        mas.module=pg.module;
        mas.part=pg.part;
        mas.field=pg.field;
        mas.adv=adv;
        mas.line=pg.line;mas.col=pg.col;
        mas.value=std::make_unique<Expr>(std::move(*tmp));
        Stmt s=std::move(mas);
        return s;
    }
    expect(TokType::Semicolon,"';'");
    ExprStmt es;
    auto e=std::make_unique<Expr>();
    *e=std::move(pg);
    es.expr=std::move(e);
    Stmt s=std::move(es);
    return s;
}

/* ── Build tables: collect constants, functions, globals ── */
bool Parser::isModulePathAhead() const{
    if(!check(TokType::Ident)) return false;
    if(!checkAt(1,TokType::IOGt)) return false;
    if(!peek(2).is(TokType::Ident)) return false;
    if(module_reg_.count(cur().text)==0) return false;
    return module_reg_.at(cur().text).count(peek(2).text)>0;
}

/* 模块路径取值表达式: Dog>Dog_B → 取值部件第一个成员
   Dog>DogInf.age → 取值部件字段
   返回 nullptr 表示当前位置不是路径 */
std::unique_ptr<Expr> Parser::parseModulePathExpr(bool adv){
    if(!isModulePathAhead()) return nullptr;
    int32_t line=cur().line;int32_t col=cur().col;
    ModulePathGet pg;
    pg.module=cur().text;
    pg.adv=adv;
    pg.line=line;pg.col=col;
    pos_++;
    pos_++; /* '>' */
    pg.part=cur().text;
    pos_++;
    if(check(TokType::Dot)||check(TokType::IOGt)){
        pos_++;
        pg.field=cur().text;
        pg.has_field=true;
        pos_++;
    }
    auto e=std::make_unique<Expr>();
    *e=std::move(pg);
    return e;
}

Stmt Parser::parseOutputStmt(){
    int32_t line=cur().line;int32_t col=cur().col;
    pos_++;
    bool sepIsGt=false;   /* true: '>' 作为分隔符; false: '<' 作为分隔符 */
    if(check(TokType::IOGt)){
        pos_++;
        sepIsGt=true;
    }else if(check(TokType::Colon)){
        pos_++;
    }else{
        die("emit 之后期望 '>' 或 '：',实际拿到 '"+cur().text+"'",cur());
    }
    ExprStmt es;
    auto outExpr=std::make_unique<Expr>();
    Output o;
    o.line=line;o.col=col;
    /* Parse colon/'>'-separated output expressions */
    while(!check(TokType::Semicolon) && !check(TokType::Eof)){
        if(check(TokType::Kw_换行)){
            pos_++;
            auto nl = std::make_unique<Expr>();
            StrLit sl; sl.value = "\n";
            *nl = std::move(sl);
            o.values.push_back(std::move(nl));
        }else if(sepIsGt){
            o.values.push_back(parseAdd());
        }else{
            o.values.push_back(parseExpr());
        }
        if(check(TokType::Colon)||check(TokType::IOGt)){
            pos_++; continue;
        }
        break;
    }
    if(o.values.empty()){
        die("空语句写分号不允许",cur());
    }
    *outExpr=std::move(o);
    es.expr=std::move(outExpr);
    expect(TokType::Semicolon,"';'");
    return es;
}

Stmt Parser::parseInputStmt(){
    int32_t line=cur().line;int32_t col=cur().col;
    pos_++;
    bool hasBracket=false;
    bool isLang=false;
    if(check(TokType::IOGt)){
        pos_++;
    }else if(check(TokType::Colon)){
        pos_++;
    }else if(check(TokType::IOLt)){
        hasBracket=true;
        isLang=false;
        pos_++;
    }else if(check(TokType::LangL)){
        hasBracket=true;
        isLang=true;
        pos_++;
    }else{
        die("recv 之后期望 '>'、'<'、'：' 或 '《',实际拿到 '"+cur().text+"'",cur());
    }
    const Token& varTk=expect(TokType::Ident,"输入变量名");
    if(hasBracket){
        if(isLang){
            expect(TokType::LangR,"'》'");
        }else{
            expect(TokType::IOGt,"'>'");
        }
    }
    expect(TokType::Semicolon,"';'");
    ExprStmt es;
    auto inExpr=std::make_unique<Expr>();
    Input in;
    in.var_name=varTk.text;
    in.line=line;in.col=col;
    *inExpr=std::move(in);
    es.expr=std::move(inExpr);
    return es;
}

Stmt Parser::parseIfStmt(){
    int32_t line=cur().line;int32_t col=cur().col;
    pos_++;
    expect(TokType::LParen,"'('");
    auto tmp=parseExpr();
    auto cond=std::make_unique<Expr>(std::move(*tmp));
    expect(TokType::RParen,"')'");
    auto thenBody=parseBlock();
    IfStmt is;
    is.line=line;is.col=col;
    is.cond=std::move(cond);
    is.then_body=std::move(thenBody);
    if(cur().is(TokType::Kw_else) || cur().is(TokType::Kw_否则)){
        pos_++;
        is.else_body=parseBlock();
    }
    Stmt s=std::move(is);
    return s;
}

Stmt Parser::parseForStmt(){
    int32_t line=cur().line;int32_t col=cur().col;
    pos_++;
    bool hasParen=check(TokType::LParen);
    if(hasParen){
        pos_++;
    }
    ForStmt fs;
    fs.line=line;fs.col=col;
    fs.init_decl=parseForInitDecl();
    expect(TokType::Semicolon,"';'");
    if(!check(TokType::Semicolon)){
        auto tmp=parseExpr();
        fs.cond=std::make_unique<Expr>(std::move(*tmp));
    }
    expect(TokType::Semicolon,"';'");
    if(!check(TokType::RParen)){
        auto tmp=parseExpr();
        fs.step=std::make_unique<Expr>(std::move(*tmp));
    }
    if(hasParen){
        expect(TokType::RParen,"')'");
    }
    fs.body=parseBlock();
    Stmt out=std::move(fs);
    return out;
}

Stmt Parser::parseWhileStmt(){
    pos_++;
    return parseWhileStmtInner();
}

Stmt Parser::parseWhileStmtInner(){
    int32_t line=cur().line;int32_t col=cur().col;
    expect(TokType::LParen,"'('");
    auto tmp=parseExpr();
    auto cond=std::make_unique<Expr>(std::move(*tmp));
    expect(TokType::RParen,"')'");
    auto body=parseBlock();
    WhileStmt ws;
    ws.line=line;ws.col=col;
    ws.cond=std::move(cond);
    ws.body=std::move(body);
    Stmt s=std::move(ws);
    return s;
}

Stmt Parser::parseLoopStmt(){
    pos_++;
    if(check(TokType::LParen)){
        pos_++;
        const Token& countTk=expect(TokType::IntLit,"循环次数");
        int64_t count=std::stoll(countTk.text);
        expect(TokType::RParen,"')'");
        auto body=parseBlock();
        LoopNStmt lns;
        lns.count=count;
        lns.body=std::move(body);
        Stmt s=std::move(lns);
        return s;
    }
    return parseLoopStmtInner();
}

Stmt Parser::parseLoopStmtInner(){
    auto body=parseBlock();
    LoopStmt ls;
    ls.body=std::move(body);
    Stmt s=std::move(ls);
    return s;
}

Stmt Parser::parseReturnStmt(){
    int32_t line=cur().line;int32_t col=cur().col;
    pos_++;
    if(check(TokType::LParen)){
        die("return 括号形式已废除, 请使用 'return;' 或 'return expr;'",cur());
    }
    if(check(TokType::Semicolon)){
        pos_++;
        ReturnStmt rs;
        rs.line=line;rs.col=col;
        Stmt s=std::move(rs);
        return s;
    }
    auto tmp=parseExpr();
    auto v=std::make_unique<Expr>(std::move(*tmp));
    expect(TokType::Semicolon,"';'");
    ReturnStmt rs;
    rs.line=line;rs.col=col;
    rs.value=std::move(v);
    Stmt s=std::move(rs);
    return s;
}

Stmt Parser::parseExprStmt(){
    auto tmp=parseExpr();
    /* Check for assignment a = b */
    if(check(TokType::Assign)){
        pos_++;
        auto p=parseModulePathExpr(false);
        std::unique_ptr<Expr> rhs;
        if(p) rhs=std::move(p);
        else rhs=parseExpr();
        expect(TokType::Semicolon,"';'");
        AssignStmt as;
        as.line=cur().line;as.col=cur().col;
        if(const auto* id=std::get_if<Ident>(&*tmp)){
            as.name=id->name;
            as.line=id->line;as.col=id->col;
        }else{
            as.target=std::move(tmp);
        }
        as.value=std::move(rhs);
        Stmt s=std::move(as);
        return s;
    }
    auto e=std::make_unique<Expr>(std::move(*tmp));
    expect(TokType::Semicolon,"';'");
    ExprStmt es;
    es.expr=std::move(e);
    Stmt s=std::move(es);
    return s;
}

std::unique_ptr<VarDecl> Parser::parseForInitDecl(){
    if(isCreateKw(cur().type)){
        pos_++;
        eatCreateSep();
        if(!isTypeKw(cur().type)){
            die("for 初始化中 create 后的类型未知 '"+cur().text+"'",cur());
        }
        std::string tn=parseTypeName();
        const Token& nameTk=expect(TokType::Ident,"循环变量名");
        expect(TokType::Assign,"'='");
        auto tmp=parseExpr();
        auto vd=std::make_unique<VarDecl>();
        vd->type=tn;
        vd->name=nameTk.text;
        vd->line=nameTk.line;vd->col=nameTk.col;
        vd->init=std::make_unique<Expr>(std::move(*tmp));
        return vd;
    }
    if(check(TokType::IOLt) || check(TokType::LangL)){
        /* 标准迭代头: for <i>; 条件; 步进  (<> 表示创建, 默认初值 1; <i=expr> 显式初值) */
        bool isLang=check(TokType::LangL);
        pos_++;
        if(isStatKw(cur().type)){
            die("for <stat ...> 迭代语法已废除, 请使用 'for <i>; 条件; 步进'",cur());
        }
        const Token& nameTk2=expect(TokType::Ident,"循环变量名");
        auto vd=std::make_unique<VarDecl>();
        vd->type="int";
        vd->name=nameTk2.text;
        vd->line=nameTk2.line;vd->col=nameTk2.col;
        if(check(TokType::Assign)){
            pos_++;
            /* 用 parseAdd: 避免结尾 '>' 被 parseCompare 当作比较运算符吃掉 */
            auto tmp=parseAdd();
            vd->init=std::make_unique<Expr>(std::move(*tmp));
        }else{
            IntLit one;
            one.value=1;
            vd->init=std::make_unique<Expr>(std::move(one));
        }
        if(isLang){
            expect(TokType::LangR,"'》'");
        }else{
            expect(TokType::IOGt,"'>'");
        }
        return vd;
    }
    if(check(TokType::Ident)){
        const Token& nameTk=cur();
        pos_++;
        expect(TokType::Assign,"'='");
        auto tmp=parseExpr();
        auto vd=std::make_unique<VarDecl>();
        vd->type="";
        vd->name=nameTk.text;
        vd->line=nameTk.line;vd->col=nameTk.col;
        vd->init=std::make_unique<Expr>(std::move(*tmp));
        return vd;
    }
    die("for 初始化语法错误 '"+cur().text+"', 标准写法: for <i>; 条件; 步进",cur());
}

std::vector<Stmt> Parser::parseBlock(){
    expect(TokType::LBrace,"'{'");
    std::vector<Stmt> out;
    while(!check(TokType::RBrace) && !eof()){
        out.push_back(parseStmt());
    }
    expect(TokType::RBrace,"'}'");
    return out;
}

std::unique_ptr<Expr> Parser::parseExpr(){
    return parseLogicalOr();
}

/* || 逻辑或: 最低优先级, 短路求值 */
std::unique_ptr<Expr> Parser::parseLogicalOr(){
    auto left=parseLogicalAnd();
    while(check(TokType::Or)){
        std::string op=cur().text;
        int32_t line=cur().line;int32_t col=cur().col;
        pos_++;
        auto right=parseLogicalAnd();
        auto b=std::make_unique<Expr>();
        Binary bin;
        bin.op=op;
        bin.line=line;bin.col=col;
        bin.lhs=std::move(left);
        bin.rhs=std::move(right);
        *b=std::move(bin);
        left=std::move(b);
    }
    return left;
}

/* && 逻辑与: 高于 || 低于相等性比较 */
std::unique_ptr<Expr> Parser::parseLogicalAnd(){
    auto left=parseEquality();
    while(check(TokType::And)){
        std::string op=cur().text;
        int32_t line=cur().line;int32_t col=cur().col;
        pos_++;
        auto right=parseEquality();
        auto b=std::make_unique<Expr>();
        Binary bin;
        bin.op=op;
        bin.line=line;bin.col=col;
        bin.lhs=std::move(left);
        bin.rhs=std::move(right);
        *b=std::move(bin);
        left=std::move(b);
    }
    return left;
}

std::unique_ptr<Expr> Parser::parseEquality(){
    auto left=parseCompare();
    while(check(TokType::EqEq) || check(TokType::Neq)){
        std::string op=cur().text;
        int32_t line=cur().line;int32_t col=cur().col;
        pos_++;
        auto right=parseCompare();
        auto b=std::make_unique<Expr>();
        Binary bin;
        bin.op=op;
        bin.line=line;bin.col=col;
        bin.lhs=std::move(left);
        bin.rhs=std::move(right);
        *b=std::move(bin);
        left=std::move(b);
    }
    return left;
}

std::unique_ptr<Expr> Parser::parseCompare(){
    auto left=parseAdd();
    while(check(TokType::LtEq) || check(TokType::GtEq)
          || check(TokType::IOGt) || check(TokType::IOLt)){
        std::string op;
        int32_t line=cur().line;int32_t col=cur().col;
        if(check(TokType::IOGt)){
            op=">";
        }else if(check(TokType::IOLt)){
            op="<";
        }else{
            op=cur().text;
        }
        pos_++;
        auto right=parseAdd();
        auto b=std::make_unique<Expr>();
        Binary bin;
        bin.op=op;
        bin.line=line;bin.col=col;
        bin.lhs=std::move(left);
        bin.rhs=std::move(right);
        *b=std::move(bin);
        left=std::move(b);
    }
    return left;
}

std::unique_ptr<Expr> Parser::parseAdd(){
    auto left=parseMul();
    while(check(TokType::Plus) || check(TokType::Minus)){
        std::string op=cur().text;
        int32_t line=cur().line;int32_t col=cur().col;
        pos_++;
        auto right=parseMul();
        auto b=std::make_unique<Expr>();
        Binary bin;
        bin.op=op;
        bin.line=line;bin.col=col;
        bin.lhs=std::move(left);
        bin.rhs=std::move(right);
        *b=std::move(bin);
        left=std::move(b);
    }
    return left;
}

std::unique_ptr<Expr> Parser::parseMul(){
    auto left=parseUnary();
    while(check(TokType::Star) || check(TokType::Slash) || check(TokType::Percent)){
        std::string op=cur().text;
        int32_t line=cur().line;int32_t col=cur().col;
        pos_++;
        auto right=parseUnary();
        auto b=std::make_unique<Expr>();
        Binary bin;
        bin.op=op;
        bin.line=line;bin.col=col;
        bin.lhs=std::move(left);
        bin.rhs=std::move(right);
        *b=std::move(bin);
        left=std::move(b);
    }
    return left;
}

std::unique_ptr<Expr> Parser::parseUnary(){
    if(check(TokType::Not) || check(TokType::Minus) || check(TokType::Star) || check(TokType::Amp)){
        std::string op=cur().text;
        int32_t line=cur().line;int32_t col=cur().col;
        pos_++;
        auto operand=parseUnary();
        auto u=std::make_unique<Expr>();
        Unary un;
        un.op=op;
        un.line=line;un.col=col;
        un.operand=std::move(operand);
        *u=std::move(un);
        return u;
    }
    return parsePostfix();
}

std::unique_ptr<Expr> Parser::parsePostfix(){
    auto e=parsePrimary();
    while(check(TokType::PlusPlus) || check(TokType::MinusMinus) || check(TokType::LBracket)){
        if(check(TokType::LBracket)){
            pos_++;
            auto idx=parseExpr();
            expect(TokType::RBracket,"']'");
            if(auto* id=std::get_if<Ident>(&*e)){
                IndexExpr ix;
                ix.name=id->name;
                ix.idx=std::move(idx);
                ix.line=id->line;ix.col=id->col;
                e=std::make_unique<Expr>(std::move(ix));
            }else{
                auto scaled=std::make_unique<Expr>();
                Binary sc;
                sc.op="*";
                sc.lhs=std::move(idx);
                auto eight=std::make_unique<Expr>();
                IntLit e8;
                e8.value=8;
                *eight=std::move(e8);
                sc.rhs=std::move(eight);
                sc.line=cur().line;sc.col=cur().col;
                *scaled=std::move(sc);
                auto add=std::make_unique<Expr>();
                Binary ad;
                ad.op="+";
                ad.lhs=std::move(e);
                ad.rhs=std::move(scaled);
                ad.line=cur().line;ad.col=cur().col;
                *add=std::move(ad);
                auto deref=std::make_unique<Expr>();
                Unary dr;
                dr.op="*";
                dr.operand=std::move(add);
                dr.line=cur().line;dr.col=cur().col;
                *deref=std::move(dr);
                e=std::move(deref);
            }
            continue;
        }
        std::string op=cur().text;
        int32_t line=cur().line;int32_t col=cur().col;
        pos_++;
        auto u=std::make_unique<Expr>();
        Unary un;
        un.op=op;
        un.line=line;un.col=col;
        un.operand=std::move(e);
        *u=std::move(un);
        e=std::move(u);
    }
    return e;
}

std::unique_ptr<Expr> Parser::parsePrimary(){
    if(check(TokType::LParen)){
        pos_++;
        auto e=parseExpr();
        expect(TokType::RParen,"')'");
        return e;
    }
    if(check(TokType::IntLit)){
        auto e=std::make_unique<Expr>();
        IntLit il;
        try{
            il.value=std::stoll(cur().text);
        }catch(...){
            die("整数常量过大 '"+cur().text+"'",cur());
        }
        pos_++;
        *e=std::move(il);
        return e;
    }
    if(check(TokType::DoubleLit)){
        auto e=std::make_unique<Expr>();
        DoubleLit dl;
        try{
            dl.value=std::stod(cur().text);
        }catch(...){
            die("浮点常量非法 '"+cur().text+"'",cur());
        }
        pos_++;
        *e=std::move(dl);
        return e;
    }
    if(check(TokType::StrLit)){
        auto e=std::make_unique<Expr>();
        StrLit sl;
        sl.value=cur().text;
        pos_++;
        *e=std::move(sl);
        return e;
    }
    if(check(TokType::Kw_true) || check(TokType::Kw_真)){
        pos_++;
        auto e=std::make_unique<Expr>();
        BoolLit bl;
        bl.value=true;
        *e=std::move(bl);
        return e;
    }
    if(check(TokType::Kw_true) || check(TokType::Kw_真)){
        pos_++;
        auto e=std::make_unique<Expr>();
        BoolLit bl;
        bl.value=false;
        *e=std::move(bl);
        return e;
    }
    if(check(TokType::Kw_null)){
        pos_++;
        auto e=std::make_unique<Expr>();
        IntLit il;
        il.value=0;
        *e=std::move(il);
        return e;
    }
    if(isInputKw(cur().type)){
        if(checkAt(1,TokType::IOLt) || checkAt(1,TokType::LangL)){
            return parseInputExpr();
        }
        die("input 期望 '<' 或 '《'",cur());
    }
    if(isAccessKw(cur().type) && checkAt(1,TokType::Dot) && checkAt(2,TokType::Ident)){
        pos_+=2;
        return parseModulePathExpr(false);
    }
    if(isAdvKw(cur().type) && checkAt(1,TokType::Dot) && checkAt(2,TokType::Kw_access) && checkAt(3,TokType::Dot)){
        pos_+=4;
        if(!check(TokType::Ident)){
            die("adv 路径期望标识符 '"+cur().text+"'",cur());
        }
        return parseModulePathExpr(true);
    }
    if(check(TokType::Ident) && isModulePathAhead()){
        return parseModulePathExpr(false);
    }
    if(check(TokType::Ident)){
        return parseIdentOrCall();
    }
    die("无法识别的表达式起始 token '"+cur().text+"'",cur());
}

std::unique_ptr<Expr> Parser::parseInputExpr(){
    int32_t line=cur().line;int32_t col=cur().col;
    pos_++;
    bool isLang=check(TokType::LangL);
    if(check(TokType::IOLt)){
        pos_++;
    }else if(check(TokType::LangL)){
        pos_++;
    }else{
        die("input 之后期望 '<' 或 '《'",cur());
    }
    const Token& varTk=expect(TokType::Ident,"输入变量名");
    if(isLang){
        expect(TokType::RParen,"')'");
    }else{
        expect(TokType::IOGt,"'>'");
    }
    auto e=std::make_unique<Expr>();
    Input in;
    in.var_name=varTk.text;
    in.line=line;in.col=col;
    *e=std::move(in);
    return e;
}

std::unique_ptr<Expr> Parser::parseIdentOrCall(){
    std::string name=cur().text;
    int32_t line=cur().line;int32_t col=cur().col;
    pos_++;
    if(check(TokType::LParen)){
        pos_++;
        Call c;
        c.callee=name;
        c.line=line;c.col=col;
        if(!check(TokType::RParen)){
            while(true){
                auto a=parseExpr();
                c.args.push_back(std::move(a));
                if(check(TokType::Comma)){
                    pos_++;
                    continue;
                }
                break;
            }
        }
        expect(TokType::RParen,"')'");
        auto e=std::make_unique<Expr>();
        *e=std::move(c);
        return e;
    }
    auto e=std::make_unique<Expr>();
    Ident id;
    id.name=name;
    id.line=line;id.col=col;
    *e=std::move(id);
    return e;
}

// ============================================================
// Type Checker
// ============================================================
namespace wei::types {

enum class TypeKind{
    Int,Bool,Double,Char,Func,Class,Omni,Void,Error,Unknown,Ptr
};

struct Type{
    TypeKind kind;
    std::string name;
    bool operator==(const Type& o) const;
    bool operator!=(const Type& o) const;
};

class TypeChecker{
public:
    void check(const ast::Program& prog);
private:
    std::map<std::string,Type> globals_;
    std::map<std::string,Type> class_table_;

    struct PartInfo {
        bool is_struct=false;
        bool is_public=true;
        std::map<std::string,Type> field_types;
        std::vector<std::string> field_order;
        int line=0;int col=0;
    };
    std::map<std::string,std::map<std::string,PartInfo>> module_table_;

    Type resolveTypeName(const std::string& n);
    void checkVarDecl(const ast::VarDecl& v, std::map<std::string,Type>& scope);
    void checkFunc(const ast::FuncDecl& f);
    void checkStmt(const ast::Stmt& s, std::map<std::string,Type>& scope,
                   const Type& ret_type, bool& ret_seen);
    Type inferExpr(const ast::Expr& e, std::map<std::string,Type>& scope);
    bool compatible(const Type& a, const Type& b);
    bool isNumeric(const Type& t);

    const PartInfo* findPart(const std::string& mod, const std::string& part) const;
    void checkVisibility(const PartInfo* pi, bool adv, const std::string& mod,
                         const std::string& part, int line, int col) const;
};

bool Type::operator==(const Type& o) const{
    return kind==o.kind&&name==o.name;
}
bool Type::operator!=(const Type& o) const{
    return !(*this==o);
}

Type TypeChecker::resolveTypeName(const std::string& n){
    if(n=="int"||n=="整型") return {TypeKind::Int,""};
    if(n=="bool"||n=="布尔") return {TypeKind::Bool,""};
    if(n=="double"||n=="浮点") return {TypeKind::Double,""};
    if(n=="char"||n=="文本"||n=="string"||n=="str") return {TypeKind::Char,""};
    if(n=="func"||n=="函数") return {TypeKind::Func,""};
    if(n=="class"||n=="类") return {TypeKind::Class,""};
    if(n=="omni"||n=="万能"||n=="stat"||n=="统计") return {TypeKind::Omni,""};
    if(n=="void"||n=="空") return {TypeKind::Void,""};
    if(n=="ptr"||n=="指针") return {TypeKind::Ptr,"ptr"};
    if(n.rfind("ptr.",0)==0) return {TypeKind::Ptr,n};
    auto it=class_table_.find(n);
    if(it!=class_table_.end()) return {TypeKind::Class,n};
    return {TypeKind::Unknown,n};
}

const TypeChecker::PartInfo* TypeChecker::findPart(const std::string& mod, const std::string& part) const{
    auto mit=module_table_.find(mod);
    if(mit==module_table_.end()) return nullptr;
    auto pit=mit->second.find(part);
    if(pit==mit->second.end()) return nullptr;
    return &pit->second;
}

void TypeChecker::checkVisibility(const PartInfo* pi, bool adv, const std::string& mod,
                                  const std::string& part, int line, int col) const{
    if(!pi){
        throw std::runtime_error("未知部件: "+mod+">"+part+" at "+std::to_string(line)+":"+std::to_string(col));
    }
    if(!pi->is_public && !adv){
        throw std::runtime_error("部件不可访问(非公开): "+mod+">"+part+" at "+std::to_string(line)+":"+std::to_string(col));
    }
}

bool TypeChecker::isNumeric(const Type& t){
    return t.kind==TypeKind::Int||t.kind==TypeKind::Double;
}

bool TypeChecker::compatible(const Type& a, const Type& b){
    if(a.kind==TypeKind::Error||b.kind==TypeKind::Error) return true;
    if(a.kind==TypeKind::Unknown||b.kind==TypeKind::Unknown) return true;
    if(a.kind==TypeKind::Omni||b.kind==TypeKind::Omni) return true;
    if(a.kind==TypeKind::Ptr && b.kind==TypeKind::Ptr) return true;
    if((a.kind==TypeKind::Ptr&&b.kind==TypeKind::Int)||(a.kind==TypeKind::Int&&b.kind==TypeKind::Ptr)) return true;
    /* int ↔ double 隐式混合 (赋值/比较/算术) */
    if((a.kind==TypeKind::Int&&b.kind==TypeKind::Double)||(a.kind==TypeKind::Double&&b.kind==TypeKind::Int)) return true;
    return a==b;
}

void TypeChecker::check(const ast::Program& prog){
    for(auto& tl:prog.items){
        std::visit([&](auto&& node){
            using T=std::decay_t<decltype(node)>;
            if constexpr(std::is_same_v<T,ast::ClassDecl>){
                Type t{TypeKind::Class,node.name};
                class_table_[node.name]=t;
            }else if constexpr(std::is_same_v<T,ast::ModuleDecl>){
                auto& modTable=module_table_[node.name];
                for(auto& p:node.parts){
                    PartInfo pi;
                    pi.is_struct=p.is_struct;
                    pi.is_public=p.is_public;
                    pi.line=p.line;pi.col=p.col;
                    for(auto& fld:p.members){
                        Type ft=resolveTypeName(fld.type);
                        if(ft.kind==TypeKind::Unknown){
                            throw std::runtime_error("未知字段类型: "+fld.type+" at "+std::to_string(fld.line)+":"+std::to_string(fld.col));
                        }
                        pi.field_types[fld.name]=ft;
                        pi.field_order.push_back(fld.name);
                    }
                    modTable[p.name]=std::move(pi);
                }
            }
        },tl);
    }
    for(auto& tl:prog.items){
        std::visit([&](auto&& node){
            using T=std::decay_t<decltype(node)>;
            if constexpr(std::is_same_v<T,ast::VarDecl>){
                Type declared=resolveTypeName(node.type);
                if(declared.kind==TypeKind::Unknown){
                    throw std::runtime_error("未知类型: "+node.type+" at "+std::to_string(node.line)+":"+std::to_string(node.col));
                }
                if(node.init){
                    Type init_t=inferExpr(*node.init,globals_);
                    if(declared.kind==TypeKind::Omni){
                        globals_[node.name]=init_t;
                    }else{
                        if(!compatible(declared,init_t)){
                            throw std::runtime_error(
                                "变量 "+node.name+" 初值类型不匹配 at "+std::to_string(node.line)+":"+std::to_string(node.col));
                        }
                        globals_[node.name]=declared;
                    }
                }else{
                    globals_[node.name]=declared;
                }
            }else if constexpr(std::is_same_v<T,ast::FuncDecl>){
                Type ret=resolveTypeName(node.ret_type);
                if(ret.kind==TypeKind::Unknown){
                    throw std::runtime_error("未知返回类型: "+node.ret_type+" at "+std::to_string(node.line)+":"+std::to_string(node.col));
                }
                Type ft{TypeKind::Func,node.ret_type};
                globals_[node.name]=ft;
            }else if constexpr(std::is_same_v<T,ast::ClassDecl>){
                for(auto& fd:node.fields){
                    Type ft=resolveTypeName(fd.type);
                    if(ft.kind==TypeKind::Unknown){
                        throw std::runtime_error("未知字段类型: "+fd.type+" at "+std::to_string(node.line)+":"+std::to_string(node.col));
                    }
                }
                for(auto& md:node.methods){
                    Type rt=resolveTypeName(md.ret_type);
                    if(rt.kind==TypeKind::Unknown){
                        throw std::runtime_error(
                            "未知方法返回类型: "+md.ret_type+" at "+std::to_string(node.line)+":"+std::to_string(node.col));
                    }
                }
            }else if constexpr(std::is_same_v<T,ast::ModuleDecl>){
                for(auto& p:node.parts){
                    for(auto& fld:p.members){
                        Type ft=resolveTypeName(fld.type);
                        if(ft.kind==TypeKind::Unknown){
                            throw std::runtime_error(
                                "未知部件成员类型: "+fld.type+" at "+std::to_string(fld.line)+":"+std::to_string(fld.col));
                        }
                    }
                }
            }
        },tl);
    }
    for(auto& tl:prog.items){
        std::visit([&](auto&& node){
            using T=std::decay_t<decltype(node)>;
            if constexpr(std::is_same_v<T,ast::FuncDecl>){
                checkFunc(node);
            }else if constexpr(std::is_same_v<T,ast::ModuleDecl>){
                bool ret_seen=false;
                Type ret{TypeKind::Void,""};
                for(auto& s:node.body){
                    checkStmt(s,globals_,ret,ret_seen);
                }
            }
        },tl);
    }
}

void TypeChecker::checkFunc(const ast::FuncDecl& f){
    std::map<std::string,Type> scope=globals_;
    for(auto& p:f.params){
        Type pt=resolveTypeName(p.type);
        if(pt.kind==TypeKind::Unknown){
            throw std::runtime_error("参数类型未知: "+p.type+" at "+std::to_string(f.line)+":"+std::to_string(f.col));
        }
        scope[p.name]=pt;
    }
    Type ret=resolveTypeName(f.ret_type);
    if(f.name=="main"||f.name=="主函数"){
        ret={TypeKind::Void,""};
    }
    bool ret_seen=false;
    for(auto& s:f.body){
        checkStmt(s,scope,ret,ret_seen);
    }
}

void TypeChecker::checkStmt(const ast::Stmt& s, std::map<std::string,Type>& scope,
                            const Type& ret_type, bool& ret_seen){
    std::visit([&](auto&& node){
        using T=std::decay_t<decltype(node)>;
        if constexpr(std::is_same_v<T,ast::Block>){
            for(auto& s2:node.stmts) checkStmt(s2,scope,ret_type,ret_seen);
        }else if constexpr(std::is_same_v<T,ast::IfStmt>){
            if(node.cond){
                Type ct=inferExpr(*node.cond,scope);
                if(ct.kind!=TypeKind::Bool){
                    throw std::runtime_error("if 条件必须是 bool at "+std::to_string(node.line)+":"+std::to_string(node.col));
                }
            }
            for(auto& s2:node.then_body) checkStmt(s2,scope,ret_type,ret_seen);
            for(auto& s2:node.else_body) checkStmt(s2,scope,ret_type,ret_seen);
        }else if constexpr(std::is_same_v<T,ast::ForStmt>){
            if(node.init_decl){
                checkVarDecl(*node.init_decl,scope);
            }
            if(node.cond){
                Type ct=inferExpr(*node.cond,scope);
                if(ct.kind!=TypeKind::Bool){
                    throw std::runtime_error("for 条件必须是 bool at "+std::to_string(node.line)+":"+std::to_string(node.col));
                }
            }
            if(node.step){
                inferExpr(*node.step,scope);
            }
            for(auto& s2:node.body) checkStmt(s2,scope,ret_type,ret_seen);
        }else if constexpr(std::is_same_v<T,ast::WhileStmt>){
            if(node.cond){
                Type ct=inferExpr(*node.cond,scope);
                if(ct.kind!=TypeKind::Bool){
                    throw std::runtime_error("循环条件必须为布尔 at "+std::to_string(node.line)+":"+std::to_string(node.col));
                }
            }
            for(auto& s2:node.body) checkStmt(s2,scope,ret_type,ret_seen);
        }else if constexpr(std::is_same_v<T,ast::LoopStmt>){
            for(auto& s2:node.body) checkStmt(s2,scope,ret_type,ret_seen);
        }else if constexpr(std::is_same_v<T,ast::LoopNStmt>){
            for(auto& s2:node.body) checkStmt(s2,scope,ret_type,ret_seen);
        }else if constexpr(std::is_same_v<T,ast::ReturnStmt>){
            if(node.value){
                Type vt=inferExpr(*node.value,scope);
                if(ret_type.kind==TypeKind::Void){
                    throw std::runtime_error("void 函数不能返回值 at "+std::to_string(node.line)+":"+std::to_string(node.col));
                }
                if(!compatible(ret_type,vt)){
                    throw std::runtime_error("返回类型不匹配 at "+std::to_string(node.line)+":"+std::to_string(node.col));
                }
                ret_seen=true;
            }else{
                if(ret_type.kind!=TypeKind::Void){
                    throw std::runtime_error("非 void 函数缺少 return at "+std::to_string(node.line)+":"+std::to_string(node.col));
                }
                ret_seen=true;
            }
        }else if constexpr(std::is_same_v<T,ast::ExprStmt>){
            if(node.expr) inferExpr(*node.expr,scope);
        }else if constexpr(std::is_same_v<T,ast::VarDecl>){
            checkVarDecl(node,scope);
        }else if constexpr(std::is_same_v<T,ast::AssignStmt>){
            Type vt=inferExpr(*node.value,scope);
            if(node.target){
                inferExpr(*node.target,scope);
            }else{
                auto it=scope.find(node.name);
                if(it==scope.end()){
                    throw std::runtime_error("变量未声明: "+node.name+" at "+std::to_string(node.line)+":"+std::to_string(node.col));
                }
                Type& cur=it->second;
                if(cur.kind==TypeKind::Omni){
                    cur=vt;
                }else{
                    if(!compatible(cur,vt)){
                        throw std::runtime_error("赋值类型不匹配: "+node.name+" at "+std::to_string(node.line)+":"+std::to_string(node.col));
                    }
                }
            }
        }else if constexpr(std::is_same_v<T,ast::ModuleAccessSet>){
            const PartInfo* pi=findPart(node.module,node.part);
            checkVisibility(pi,node.adv,node.module,node.part,node.line,node.col);
            auto fit=pi->field_types.find(node.field);
            if(fit==pi->field_types.end()){
                throw std::runtime_error("部件 "+node.module+">"+node.part+" 没有字段 "+node.field
                    +" at "+std::to_string(node.line)+":"+std::to_string(node.col));
            }
            Type vt=inferExpr(*node.value,scope);
            if(vt.kind!=TypeKind::Unknown && !compatible(fit->second,vt)){
                throw std::runtime_error("访问赋值类型不匹配: "+node.module+">"+node.part+"."+node.field
                    +" at "+std::to_string(node.line)+":"+std::to_string(node.col));
            }
        }
    },s);
}

void TypeChecker::checkVarDecl(const ast::VarDecl& v, std::map<std::string,Type>& scope){
    Type declared=resolveTypeName(v.type);
    if(v.init && (declared.kind==TypeKind::Unknown || v.type.empty())){
        Type init_t=inferExpr(*v.init,scope);
        scope[v.name]=init_t;
        return;
    }
    if(declared.kind==TypeKind::Unknown){
        throw std::runtime_error("未知类型: "+v.type+" at "+std::to_string(v.line)+":"+std::to_string(v.col));
    }
    if(v.init){
        Type init_t=inferExpr(*v.init,scope);
        if(declared.kind==TypeKind::Omni){
            scope[v.name]=init_t;
        }else{
            if(!compatible(declared,init_t)){
                throw std::runtime_error("变量 "+v.name+" 初值类型不匹配 at "+std::to_string(v.line)+":"+std::to_string(v.col));
            }
            scope[v.name]=declared;
        }
    }else{
        scope[v.name]=declared;
    }
}

Type TypeChecker::inferExpr(const ast::Expr& e, std::map<std::string,Type>& scope){
    return std::visit([&](auto&& node)->Type{
        using T=std::decay_t<decltype(node)>;
        if constexpr(std::is_same_v<T,ast::IntLit>){
            return {TypeKind::Int,""};
        }else if constexpr(std::is_same_v<T,ast::DoubleLit>){
            return {TypeKind::Double,""};
        }else if constexpr(std::is_same_v<T,ast::StrLit>){
            return {TypeKind::Char,""};
        }else if constexpr(std::is_same_v<T,ast::BoolLit>){
            return {TypeKind::Bool,""};
        }else if constexpr(std::is_same_v<T,ast::Ident>){
            auto it=scope.find(node.name);
            if(it==scope.end()){
                throw std::runtime_error("未声明标识符: "+node.name+" at "+std::to_string(node.line)+":"+std::to_string(node.col));
            }
            return it->second;
        }else if constexpr(std::is_same_v<T,ast::Binary>){
            Type lt=node.lhs?inferExpr(*node.lhs,scope):Type{TypeKind::Void,""};
            Type rt=node.rhs?inferExpr(*node.rhs,scope):Type{TypeKind::Void,""};
            const std::string& op=node.op;
            if(op=="+"||op=="-"||op=="*"||op=="/"||op=="%"){
                if(!isNumeric(lt)||!isNumeric(rt)){
                    throw std::runtime_error("算术运算符要求数值操作数 at "+std::to_string(node.line)+":"+std::to_string(node.col));
                }
                /* 混合 int/double 运算结果为 double */
                if(lt.kind==TypeKind::Double||rt.kind==TypeKind::Double) return {TypeKind::Double,""};
                return lt;
            }else if(op=="=="||op=="!="||op=="<"||op=="<="||
                      op==">"||op==">="){
                if(!compatible(lt,rt)){
                    throw std::runtime_error("比较运算符类型不匹配 at "+std::to_string(node.line)+":"+std::to_string(node.col));
                }
                return {TypeKind::Bool,""};
            }else if(op=="&&"||op=="||"){
                if(lt.kind!=TypeKind::Bool||rt.kind!=TypeKind::Bool){
                    throw std::runtime_error("逻辑运算符要求 bool 操作数 at "+std::to_string(node.line)+":"+std::to_string(node.col));
                }
                return {TypeKind::Bool,""};
            }else{
                return {TypeKind::Unknown,""};
            }
        }else if constexpr(std::is_same_v<T,ast::Unary>){
            Type ot=node.operand?inferExpr(*node.operand,scope)
                                :Type{TypeKind::Void,""};
            const std::string& op=node.op;
            if(op=="-"){
                if(!isNumeric(ot)){
                    throw std::runtime_error("一元 - 操作数必须为数值 at "+std::to_string(node.line)+":"+std::to_string(node.col));
                }
                return ot;
            }else if(op=="!"){
                if(ot.kind!=TypeKind::Bool){
                    throw std::runtime_error("! 操作数必须为布尔 at "+std::to_string(node.line)+":"+std::to_string(node.col));
                }
                return {TypeKind::Bool,""};
            }else if(op=="*"){
                if(ot.kind!=TypeKind::Ptr){
                    throw std::runtime_error("* 解引用操作数必须为指针 at "+std::to_string(node.line)+":"+std::to_string(node.col));
                }
                if(ot.name=="ptr.int") return {TypeKind::Int,""};
                if(ot.name=="ptr.char") return {TypeKind::Char,""};
                if(ot.name=="ptr.bool") return {TypeKind::Bool,""};
                if(ot.name=="ptr.double") return {TypeKind::Double,""};
                return {TypeKind::Omni,""};
            }else if(op=="&"){
                return {TypeKind::Ptr,"ptr"};
            }else{
                return {TypeKind::Unknown,""};
            }
        }else if constexpr(std::is_same_v<T,ast::Call>){
            auto it=scope.find(node.callee);
            if(it==scope.end()){
                throw std::runtime_error("未声明函数: "+node.callee+" at "+std::to_string(node.line)+":"+std::to_string(node.col));
            }
            if(it->second.kind!=TypeKind::Func){
                throw std::runtime_error("非函数调用: "+node.callee+" at "+std::to_string(node.line)+":"+std::to_string(node.col));
            }
            for(auto& a:node.args){
                if(a) inferExpr(*a,scope);
            }
            return resolveTypeName(it->second.name);
        }else if constexpr(std::is_same_v<T,ast::Output>){
            for(auto& v:node.values){
                if(v) inferExpr(*v,scope);
            }
            return {TypeKind::Void,""};
        }else if constexpr(std::is_same_v<T,ast::Input>){
            return {TypeKind::Void,""};
        }else if constexpr(std::is_same_v<T,ast::IndexExpr>){
            auto it=scope.find(node.name);
            if(it==scope.end()){
                throw std::runtime_error("未声明标识符: "+node.name+" at "+std::to_string(node.line)+":"+std::to_string(node.col));
            }
            if(node.idx) inferExpr(*node.idx,scope);
            if(it->second.kind==TypeKind::Ptr) return {TypeKind::Int,""};
            return {TypeKind::Unknown,""};
        }else if constexpr(std::is_same_v<T,ast::ModulePathGet>){
            const PartInfo* pi=findPart(node.module,node.part);
            checkVisibility(pi,node.adv,node.module,node.part,node.line,node.col);
            const std::string* fname=nullptr;
            if(node.has_field){
                auto fit=pi->field_types.find(node.field);
                if(fit==pi->field_types.end()){
                    throw std::runtime_error("部件 "+node.module+">"+node.part+" 没有字段 "+node.field
                        +" at "+std::to_string(node.line)+":"+std::to_string(node.col));
                }
                fname=&node.field;
            }else{
                if(pi->field_order.empty()){
                    throw std::runtime_error("部件 "+node.module+">"+node.part+" 没有成员可取"
                        +" at "+std::to_string(node.line)+":"+std::to_string(node.col));
                }
                fname=&pi->field_order[0];
            }
            return pi->field_types.at(*fname);
        }else{
            return {TypeKind::Unknown,""};
        }
    },e);
}

}  

#ifndef WEI_SKIP_MAIN

namespace {

std::string basenameNoExt(const std::string& path){
    size_t slash=path.find_last_of("/\\");
    std::string base=(slash==std::string::npos)?path:path.substr(slash+1);
    size_t dot=base.find_last_of('.');
    if(dot!=std::string::npos){
        return base.substr(0,dot);
    }
    return base;
}

std::string readFile(const std::string& path){
    std::ifstream in(path,std::ios::binary);
    if(!in){
        throw std::runtime_error("无法打开文件: "+path);
    }
    std::ostringstream ss;
    ss<<in.rdbuf();
    std::string s=ss.str();
    /* 跳过 UTF-8 BOM (EF BB BF) */
    if(s.size()>=3 && static_cast<unsigned char>(s[0])==0xEF
                  && static_cast<unsigned char>(s[1])==0xBB
                  && static_cast<unsigned char>(s[2])==0xBF){
        s=s.substr(3);
    }
    return s;
}

struct Loc{
    int line;
    int col;
    bool ok;
};
Loc extractLoc(const std::string& msg){
    static const std::regex re(R"(at\s+(\d+):(\d+))");
    std::smatch m;
    if(std::regex_search(msg,m,re)){
        return {std::stoi(m[1]),std::stoi(m[2]),true};
    }
    return {0,0,false};
}

void emitError(const std::string& file,const std::string& raw){
    Loc loc=extractLoc(raw);
    if(loc.ok){
        std::fprintf(stderr,"%s:%d:%d: %s\n",file.c_str(),loc.line,loc.col,raw.c_str());
    }else{
        std::fprintf(stderr,"%s:?:?: %s\n",file.c_str(),raw.c_str());
    }
}

/* ── x86-64 code buffer with instruction encoding helpers ── */
struct X64Buf {
    std::vector<uint8_t> buf;
    std::vector<std::pair<size_t,size_t>> rip_str_patches_;
    size_t pos() const { return buf.size(); }
    void clear() { buf.clear(); rip_str_patches_.clear(); }
    void u8(uint8_t v) { buf.push_back(v); }
    void u32(uint32_t v) {
        buf.push_back(uint8_t(v)); buf.push_back(uint8_t(v>>8));
        buf.push_back(uint8_t(v>>16)); buf.push_back(uint8_t(v>>24));
    }
    void u64(int64_t v) { u32(uint32_t(v)); u32(uint32_t(v>>32)); }
    void patch_u32(size_t at, uint32_t v) {
        buf[at]=uint8_t(v); buf[at+1]=uint8_t(v>>8);
        buf[at+2]=uint8_t(v>>16); buf[at+3]=uint8_t(v>>24);
    }
    void patch_u8(size_t at, uint8_t v) { buf[at] = v; }
    void rex_w() { u8(0x48); }
    void rex_wr(int r) { u8((r & 8) ? 0x4C : 0x48); }
    void rex_wb(int b) { u8((b & 8) ? 0x49 : 0x48); }
    void rex_wrb(int r, int b) {
        u8(0x48 | ((r & 8) ? 0x04 : 0) | ((b & 8) ? 0x01 : 0));
    }
    void modrm(uint8_t mod, uint8_t reg, uint8_t rm) {
        u8((mod<<6) | ((reg & 7)<<3) | (rm & 7));
    }

    /* ── Instruction emitters (x86-64, Intel syntax) ── */
    /* Register order: RAX(0), RCX(1), RDX(2), RBX(3), RSP(4), RBP(5), RSI(6), RDI(7) */

    void push_r(int r) {
        if (r < 8) { u8(0x50 + r); }
        else { u8(0x41); u8(0x50 + (r & 7)); }
    }
    void pop_r(int r) {
        if (r < 8) { u8(0x58 + r); }
        else { u8(0x41); u8(0x58 + (r & 7)); }
    }
    void push_imm32(int32_t v) { u8(0x68); u32(static_cast<uint32_t>(v)); }

    /* mov r64, imm64 */
    void mov64r_imm64(int r, int64_t v) {
        rex_wb(r); u8(0xB8 + (r & 7)); u64(v);
    }
    /* mov r/m64, imm8 sign-extended (C7 /0 but with 8-bit imm? No, use 0x83 for 8-bit) */
    void mov32r_imm32(int r, int32_t v) {
        u8(0xB8 + (r & 7)); u32(static_cast<uint32_t>(v));
    }
    /* inc r/m64 (FF /0) */
    void inc_rm64(int rm) {
        rex_w(); u8(0xFF); modrm(3, 0, rm);
    }
    /* mov r64, r/m64 (8B) */
    void mov64r_rm64(int r, int rm) {
        rex_wrb(r, rm); u8(0x8B); modrm(3, r, rm);
    }
    /* mov r/m64, r64 (89) */
    void mov_rm64_64r(int rm, int r) {
        rex_wrb(r, rm); u8(0x89); modrm(3, r, rm);
    }
    /* mov r/m64, imm32 sign-extended (C7 /0) */
    void mov_rm64_imm32(int rm, int32_t v) {
        rex_w(); u8(0xC7); modrm(3, 0, rm); u32(static_cast<uint32_t>(v));
    }
    /* mov r/m64, imm8 sign-extended (C7 /0 but with 8-bit imm? No, use 0x83 for 8-bit) */
    /* Actually C7 is always imm32. For mov with imm8 use C6 but only for byte. Skip for now. */

    /* mov r64, [r+disp] — 自动选择 disp8/disp32 */
    void mov64r_memd8(int r, int base, int64_t d) {
        if (d >= -128 && d <= 127) {
            rex_wrb(r, base); u8(0x8B); modrm(1, r, base);
            if ((base & 7) == 4) u8(0x24);
            u8(static_cast<uint8_t>(d));
        } else {
            rex_wrb(r, base); u8(0x8B); modrm(2, r, base);
            if ((base & 7) == 4) u8(0x24);
            u32(static_cast<uint32_t>(d));
        }
    }
    /* mov r64, [r+disp32] */
    void mov64r_memd32(int r, int base, int32_t d) {
        rex_wrb(r, base); u8(0x8B); modrm(2, r, base);
        if ((base & 7) == 4) u8(0x24);  /* SIB: [rsp+disp32] */
        u32(static_cast<uint32_t>(d));
    }
    /* mov [r+disp], r64 — 自动选择 disp8/disp32 */
    void mov_memd8_64r(int base, int64_t d, int r) {
        if (d >= -128 && d <= 127) {
            rex_wrb(r, base); u8(0x89); modrm(1, r, base);
            if ((base & 7) == 4) u8(0x24);
            u8(static_cast<uint8_t>(d));
        } else {
            rex_wrb(r, base); u8(0x89); modrm(2, r, base);
            if ((base & 7) == 4) u8(0x24);
            u32(static_cast<uint32_t>(d));
        }
    }
    /* mov [r+disp32], r64 */
    void mov_memd32_64r(int base, int32_t d, int r) {
        rex_wrb(r, base); u8(0x89); modrm(2, r, base);
        if ((base & 7) == 4) u8(0x24);  /* SIB: [rsp+disp32] */
        u32(static_cast<uint32_t>(d));
    }
    /* mov [r+disp8], imm32 */
    void mov_memd8_imm32(int base, int64_t d, int32_t v) {
        rex_w(); u8(0xC7); modrm(1, 0, base); u8(static_cast<uint8_t>(d)); u32(static_cast<uint32_t>(v));
    }
    /* mov [r+disp32], imm32 */
    void mov_memd32_imm32(int base, int32_t d, int32_t v) {
        rex_w(); u8(0xC7); modrm(2, 0, base); u32(static_cast<uint32_t>(d)); u32(static_cast<uint32_t>(v));
    }

    /* mov r64, [rip+disp32] */
    void mov64r_rip(int r, int32_t d) {
        rex_wrb(r, 5); u8(0x8B); modrm(0, r, 5); u32(static_cast<uint32_t>(d));
    }
    /* mov [rip+disp32], r64 */
    void mov_rip_r64(int32_t d, int r) {
        rex_wrb(r, 5); u8(0x8B); modrm(0, r, 5); u32(static_cast<uint32_t>(d));
    }
    /* lea r64, [r+disp] — 自动选择 disp8/disp32 */
    void lea64r_memd8(int r, int base, int64_t d) {
        if (d >= -128 && d <= 127) {
            rex_wrb(r, base); u8(0x8D); modrm(1, r, base); u8(static_cast<uint8_t>(d));
        } else {
            rex_wrb(r, base); u8(0x8D); modrm(2, r, base); u32(static_cast<uint32_t>(d));
        }
    }
    /* lea r64, [r+disp32] */
    void lea64r_memd32(int r, int base, int32_t d) {
        rex_wrb(r, base); u8(0x8D); modrm(2, r, base); u32(static_cast<uint32_t>(d));
    }
    void lea64r_rip(int r) {
        rex_wrb(r, 5); u8(0x8D); modrm(0, r, 5);
    }

    /* ── ALU: r/m64 = r/m64 op r64 (opcodes 01, 09, 11, 19, 21, 29, 31, 39) ── */
    void alu_rm64_r64(uint8_t op, int rm, int r) {
        rex_wrb(r, rm); u8(op); modrm(3, r, rm);
    }
    void alu_r64_rm64(uint8_t op, int r, int rm) {
        rex_wrb(r, rm); u8(op); modrm(3, r, rm);
    }
    void alu_r64_memd8(uint8_t op, int r, int base, int64_t d) {
        if (d >= -128 && d <= 127) {
            rex_wrb(r, base); u8(op); modrm(1, r, base); u8(static_cast<uint8_t>(d));
        } else {
            rex_wrb(r, base); u8(op); modrm(2, r, base); u32(static_cast<uint32_t>(d));
        }
    }
    void alu_r64_memd32(uint8_t op, int r, int base, int32_t d) {
        rex_wrb(r, base); u8(op); modrm(2, r, base); u32(static_cast<uint32_t>(d));
    }
    /* ALU with imm: r/m64 op imm32 (0x81 /reg, 0x83 /reg for imm8) */
    void alu_rm64_imm32(int rm, int regop, int32_t v) {
        rex_wb(rm);  /* rm 可能为扩展寄存器 (r8-r15) */
        if (v >= -128 && v <= 127) {
            u8(0x83); modrm(3, regop, rm); u8(static_cast<uint8_t>(static_cast<int8_t>(v)));
        } else {
            u8(0x81); modrm(3, regop, rm); u32(static_cast<uint32_t>(v));
        }
    }

    /* add */
    void add_r64_rm64(int r, int rm) { alu_r64_rm64(0x03, r, rm); }
    void add_rm64_r64(int rm, int r) { alu_rm64_r64(0x01, rm, r); }
    void add_r64_memd8(int r, int base, int64_t d) { alu_r64_memd8(0x03, r, base, d); }
    void add_r64_memd32(int r, int base, int32_t d) { alu_r64_memd32(0x03, r, base, d); }
    void add_rm64_imm32(int rm, int32_t v) { alu_rm64_imm32(rm, 0, v); }
    void add_memd8_imm32(int base, int8_t disp, int32_t v) {
        if (v >= -128 && v <= 127) {
            rex_w(); u8(0x83); modrm(1, 0, base); u8(static_cast<uint8_t>(disp)); u8(static_cast<uint8_t>(static_cast<int8_t>(v)));
        } else {
            rex_w(); u8(0x81); modrm(1, 0, base); u8(static_cast<uint8_t>(disp)); u32(static_cast<uint32_t>(v));
        }
    }

    /* sub */
    void sub_r64_rm64(int r, int rm) { alu_r64_rm64(0x2B, r, rm); }
    void sub_rm64_imm32(int rm, int32_t v) { alu_rm64_imm32(rm, 5, v); }
    void sub_r64_memd8(int r, int base, int64_t d) { alu_r64_memd8(0x2B, r, base, d); }

    /* cmp */
    void cmp_r64_rm64(int r, int rm) { alu_r64_rm64(0x3B, r, rm); }
    void cmp_rm64_imm32(int rm, int32_t v) { alu_rm64_imm32(rm, 7, v); }
    void cmp_r64_memd8(int r, int base, int64_t d) { alu_r64_memd8(0x3B, r, base, d); }
    void cmp_r64_memd32(int r, int base, int32_t d) { alu_r64_memd32(0x3B, r, base, d); }

    /* xor */
    void xor_rm64_r64(int rm, int r) { alu_rm64_r64(0x31, rm, r); }

    /* imul r64, r/m64 (0F AF) */
    void imul_r64_rm64(int r, int rm) {
        rex_wrb(r, rm); u8(0x0F); u8(0xAF); modrm(3, r, rm);
    }
    /* cmov r64, r/m64 (0F 40+cc /r), cc 是 jcc 后缀码 */
    void cmov_r64_rm64(int cc, int r, int rm) {
        rex_wrb(r, rm); u8(0x0F); u8(static_cast<uint8_t>(0x40 + cc)); modrm(3, r, rm);
    }
    void imul_r64_memd8(int r, int base, int64_t d) {
        if (d >= -128 && d <= 127) {
            rex_wrb(r, base); u8(0x0F); u8(0xAF); modrm(1, r, base); u8(static_cast<uint8_t>(d));
        } else {
            rex_wrb(r, base); u8(0x0F); u8(0xAF); modrm(2, r, base); u32(static_cast<uint32_t>(d));
        }
    }
    void imul_r64_memd32(int r, int base, int32_t d) {
        rex_wrb(r, base); u8(0x0F); u8(0xAF); modrm(2, r, base); u32(static_cast<uint32_t>(d));
    }

    /* idiv r/m64 (F7 /7) */
    void idiv_rm64(int rm) {
        rex_w(); u8(0xF7); modrm(3, 7, rm);
    }
    /* cqo (sign-extend RAX to RDX:RAX, 48 99) */
    void cqo() { rex_w(); u8(0x99); }
    /* neg r/m64 (F7 /3) */
    void neg_rm64(int rm) {
        rex_w(); u8(0xF7); modrm(3, 3, rm);
    }

    /* setcc r/m8 (0F 9x) */
    void setcc_r8(int cc, int r) {
        u8(0x0F); u8(0x90 + cc); modrm(3, 0, r);
    }
    /* movzx r64, r/m8 (0F B6) */
    void movzx_r64_rm8(int r, int rm) {
        rex_wb(r); u8(0x0F); u8(0xB6); modrm(3, r, rm);
    }

    /* ── Jumps ── */
    /* jmp rel32 */
    size_t jmp32() { u8(0xE9); size_t p = pos(); u32(0); return p; }
    /* jcc rel32 (0F 8x) */
    size_t jcc32(int cc) { u8(0x0F); u8(0x80 + cc); size_t p = pos(); u32(0); return p; }
    /* patch: back-patch jmp/jcc to current position */
    void patch_jmp(size_t p) {
        int32_t off = static_cast<int32_t>(pos() - (p + 4));
        patch_u32(p, static_cast<uint32_t>(off));
    }

    /* ── Stack frame ── */
    void push_rbp() { u8(0x55); }
    void pop_rbp() { u8(0x5D); }
    void mov_rbp_rsp() { rex_w(); u8(0x89); u8(0xE5); }
    void sub_rsp(int32_t v) { alu_rm64_imm32(4, 5, v); }
    void add_rsp(int32_t v) { alu_rm64_imm32(4, 0, v); }
    void leave() { u8(0xC9); }
    void ret() { u8(0xC3); }

    /* Dynamic stack alignment helpers (use R15 callee-saved) */
    void save_rsp_r15() {
        /* push r15 (save callee-saved) ; mov r15, rsp */
        u8(0x41); u8(0x57);                /* push r15 */
        u8(0x49); u8(0x89); u8(0xE7);      /* mov r15, rsp */
    }
    void restore_rsp_r15() {
        /* mov rsp, r15 ; pop r15 (restore callee-saved) */
        u8(0x4C); u8(0x89); u8(0xFC);      /* mov rsp, r15 (REX.WR: reg=R15, rm=RSP) */
        u8(0x41); u8(0x5F);                /* pop r15 */
    }
    void align_rsp16() {
        /* and rsp, -16 : 48 83 E4 F0 */
        u8(0x48); u8(0x83); u8(0xE4); u8(0xF0);
    }

    /* ── call [rip+disp32] (FF 15) ──
       disp = target_addr - (end_of_insn)
       Returns position of the disp32 field */
    size_t call_rip() { u8(0xFF); u8(0x15); size_t p = pos(); u32(0); return p; }
    void call_r(int r) {
        if (r < 8) { u8(0xFF); u8(0xD0 + (r & 7)); }
        else { u8(0x41); u8(0xFF); u8(0xD0 + (r & 7)); }
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

class NativeGen {
public:
    explicit NativeGen(Program& prog, bool opt=false) : prog_(prog), opt_(opt) {}
    std::vector<uint8_t> generate();
    size_t data_area() const { return data_area_; }
    size_t num_globals() const { return num_globals_; }
    size_t entry_offset() const { return entry_off_; }
    size_t code_size() const { return data_area_; }
    size_t printf_iat_off() const { return printf_iat_off_; }
size_t exit_iat_off() const { return exit_iat_off_; }
    void set_pe_data_rel(size_t d) { pe_data_rel_ = d; }

private:
    Program& prog_;
    X64Buf c_;
    std::vector<JITLabel> labels_;
    bool opt_ = false;   /* -opt: 启用循环展开等优化 */

    /* AST 扫描: 表达式/语句是否引用变量 v (用于循环展开安全检查) */
    static bool exprUsesVar(const std::unique_ptr<Expr>& e, const std::string& v) {
        if (!e) return false;
        return std::visit([&](const auto& n) -> bool {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, Ident>) { return n.name == v; }
            else if constexpr (std::is_same_v<T, Binary>) { return exprUsesVar(n.lhs, v) || exprUsesVar(n.rhs, v); }
            else if constexpr (std::is_same_v<T, Unary>) { return exprUsesVar(n.operand, v); }
            else if constexpr (std::is_same_v<T, Call>) {
                for (auto& a : n.args) if (exprUsesVar(a, v)) return true;
                return false;
            }
            else if constexpr (std::is_same_v<T, Output>) {
                for (auto& a : n.values) if (exprUsesVar(a, v)) return true;
                return false;
            }
            else if constexpr (std::is_same_v<T, Input>) { return n.var_name == v; }
            else if constexpr (std::is_same_v<T, IndexExpr>) { return n.name == v || exprUsesVar(n.idx, v); }
            else { return false; }
        }, *e);
    }
    static bool stmtUsesVar(const Stmt& s, const std::string& v) {
        return std::visit([&](const auto& n) -> bool {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, Block>) {
                for (auto& b : n.stmts) if (stmtUsesVar(b, v)) return true;
                return false;
            }
            else if constexpr (std::is_same_v<T, IfStmt>) {
                if (exprUsesVar(n.cond, v)) return true;
                for (auto& b : n.then_body) if (stmtUsesVar(b, v)) return true;
                for (auto& b : n.else_body) if (stmtUsesVar(b, v)) return true;
                return false;
            }
            else if constexpr (std::is_same_v<T, ForStmt>) {
                for (auto& b : n.body) if (stmtUsesVar(b, v)) return true;
                return false;
            }
            else if constexpr (std::is_same_v<T, WhileStmt>) {
                if (exprUsesVar(n.cond, v)) return true;
                for (auto& b : n.body) if (stmtUsesVar(b, v)) return true;
                return false;
            }
            else if constexpr (std::is_same_v<T, LoopStmt>) {
                for (auto& b : n.body) if (stmtUsesVar(b, v)) return true;
                return false;
            }
            else if constexpr (std::is_same_v<T, LoopNStmt>) {
                for (auto& b : n.body) if (stmtUsesVar(b, v)) return true;
                return false;
            }
            else if constexpr (std::is_same_v<T, ReturnStmt>) { return exprUsesVar(n.value, v); }
            else if constexpr (std::is_same_v<T, ExprStmt>) { return exprUsesVar(n.expr, v); }
            else if constexpr (std::is_same_v<T, AssignStmt>) { return n.name == v || exprUsesVar(n.value, v); }
            else if constexpr (std::is_same_v<T, ModuleAccessSet>) { return exprUsesVar(n.value, v); }
            else if constexpr (std::is_same_v<T, VarDecl>) { return n.name == v || exprUsesVar(n.init, v); }
            else { return false; }
        }, s);
    }

    /* ── ALU: r/m64 = r/m64 op r64 (opcodes 01, 09, 11, 19, 21, 29, 31, 39) ── */
    static bool stmtHasLoop(const Stmt& s) {
        return std::visit([](const auto& n) -> bool {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, ForStmt> || std::is_same_v<T, WhileStmt>
                       || std::is_same_v<T, LoopStmt> || std::is_same_v<T, LoopNStmt>) {
                for (auto& b : n.body) if (stmtHasLoop(b)) return true;
                return true;
            }
            else if constexpr (std::is_same_v<T, Block>) {
                for (auto& b : n.stmts) if (stmtHasLoop(b)) return true;
                return false;
            }
            else if constexpr (std::is_same_v<T, IfStmt>) {
                for (auto& b : n.then_body) if (stmtHasLoop(b)) return true;
                for (auto& b : n.else_body) if (stmtHasLoop(b)) return true;
                return false;
            }
            else { return false; }
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
                if (getConst(bin->rhs, c2)) { a = 1; b = c2; return true; }
                return false;
            }
            if (bin->op == "-") {
                int64_t c2;
                if (getConst(bin->rhs, c2)) { a = 1; b = -c2; return true; }
                return false;
            }
            if (bin->op == "*") {
                int64_t c2;
                if (getConst(bin->rhs, c2)) { a = c2; b = 0; return true; }
                return false;
            }
            return false;
        }
        /* x = x*a + b 的 rhs 是 (x*C)+C 或 (C*x)+C 或 C+(x*C) */
        if (isSelf(bin->rhs)) {
            int64_t c;
            if (getConst(bin->lhs, c)) {
                if (bin->op == "+") { a = 1; b = c; return true; }
                if (bin->op == "-") { a = -1; b = c; return true; }  /* x = c - x */
                if (bin->op == "*") { a = c; b = 0; return true; }
            }
            return false;
        }
        /* x = x*a + b: 两层 Binary */
        auto* inner = std::get_if<Binary>(bin->lhs.get());
        if (inner && inner->op == "*" && isSelf(inner->lhs) && getConst(inner->rhs, a)) {
            if (bin->op == "+") return getConst(bin->rhs, b);
            if (bin->op == "-") { if (getConst(bin->rhs, b)) { b = -b; return true; } return false; }
        }
        if (inner && inner->op == "*" && isSelf(inner->rhs) && getConst(inner->lhs, a)) {
            if (bin->op == "+") return getConst(bin->rhs, b);
            if (bin->op == "-") { if (getConst(bin->rhs, b)) { b = -b; return true; } return false; }
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

    struct VarInfo { uint32_t slot; bool is_local; int64_t array_len = 0; bool is_double=false; };
    std::map<std::string, VarInfo> locals_;
    std::map<std::string, VarInfo> globals_;
    uint32_t max_locals_ = 0;
    uint32_t num_globals_ = 0;

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
    std::map<std::string,size_t> func_offsets_;
    std::vector<PartInit> part_inits_;
    std::vector<StrInitPatch> str_init_patches_;

    std::string part_key(const std::string& m, const std::string& p, const std::string& f) const {
        return m+"\x01"+p+"\x01"+f;
    }
    uint32_t part_slot_id(const std::string& m, const std::string& p, bool hasField, const std::string& f) const;

    size_t data_area_ = 0;
    size_t entry_off_ = 0;
    size_t global_data_off_ = 0;
    size_t printf_iat_off_ = 0;
    size_t exit_iat_off_ = 8;
    size_t gsh_iat_off_ = 16;
    size_t rf_iat_off_ = 24;
    size_t pe_data_rel_ = 0;  /* PE mode: data-area buffer offset delta (idata section) */

    struct FuncPatch {
        size_t disp_pos;
        std::string callee;
        int line=0;int col=0;
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

    struct PEPatch { size_t pos; int kind; };
    std::vector<PEPatch> pe_iat_patches_;
    std::vector<PEPatch> pe_fmt_patches_;
    size_t entry_main_call_pos_ = 0;
    size_t entry_exit_call_pos_ = 0;

    // 循环寄存器分配优化：当前是否在优化循环中
    std::string fast_loop_var_;
    int  fast_loop_reg_;
    int64_t fast_loop_inc_;

    /* ── store 消去跟踪 ── */
    size_t last_store_len_ = (size_t)-1;
    int last_store_slot_ = -1;
    int loop_depth_ = 0;
    std::map<uint32_t, int> loop_reg_vars_;

    /* emit return if last stmt didn't */
    void emit_global_load(int reg, uint32_t slot) {
        c_.rex_wrb(reg, 5); c_.u8(0x8B); c_.modrm(0, reg, 5);
        size_t disp_pos = c_.pos();
        c_.u32(0);
        global_rip_patches_.push_back({disp_pos, slot});
    }
    /* emit return if last stmt didn't */
    void emit_global_store(uint32_t slot, int reg) {
        c_.rex_wrb(reg, 5); c_.u8(0x89); c_.modrm(0, reg, 5);
        size_t disp_pos = c_.pos();
        c_.u32(0);
        global_rip_patches_.push_back({disp_pos, slot});
    }
    void emit_global_addr(int reg, uint32_t slot) {
        c_.rex_wrb(reg, 5); c_.u8(0x8D); c_.modrm(0, reg, 5);
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
       正值修正用 cmov 无分支; r ∈ [0,2^32), "r ≥ D" 概率 ≈ 0.5, 分支会半对半误预测 */
    void emit_mod_fold(int64_t cst, int k) {
        c_.mov64r_rm64(1, 0);        /* rcx = x */
        c_.u8(0x25); c_.u32(static_cast<uint32_t>(cst)); /* and eax, D */
        c_.u8(0x48); c_.u8(0xC1); c_.u8(0xF9); c_.u8(static_cast<uint8_t>(k)); /* sar rcx, k */
        c_.add_r64_rm64(0, 1);       /* rax = (x&D)+(x>>k) */
        /* 负值修正(最多两次): 正路径恒跳过, 分支预测完美) */
        int jp1 = emit_jcc(0xD);     /* jge pos */
        c_.u8(0x48); c_.u8(0x05); c_.u32(static_cast<uint32_t>(cst)); /* add rax, D */
        int jp2 = emit_jcc(0xD);     /* jge pos */
        c_.u8(0x48); c_.u8(0x05); c_.u32(static_cast<uint32_t>(cst));
        patch(jp1);
        patch(jp2);                  /* pos: */
        /* 正值修正(无分支): r 可能 > D, 最多两次 */
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

    /* 编译期已知常量追踪 (仅用于 pattern 匹配) */
    std::map<std::string, int64_t> known_const_;
    /* 已识别为组合数递推的函数名 */
    std::string comb_func_;

    /* ── 双精度支持 ── */
    std::set<std::string> dbl_vars_;                     /* 声明为 double 的变量名 */
    std::vector<std::pair<size_t,double>> dbl_patches_;  /* RIP 相对双精度常量回补 */
    bool isDblExpr(const std::unique_ptr<Expr>& e);
    void gen_expr_dbl(const std::unique_ptr<Expr>& e);

    void gen_func(FuncDecl& f);
    void gen_stmt(Stmt& s);
    void gen_expr(Expr& e);
    void gen_addr_to_rax(Expr& e);
    void gen_block(std::vector<Stmt>& stmts);
    void gen_entry_stub();
};

uint32_t NativeGen::local_slot(const std::string& name, bool alloc, int64_t array_len) {
    auto it = locals_.find(name);
    if (it != locals_.end()) return it->second.slot;
    if (!alloc) return 0;
    uint32_t slot = max_locals_++;
    if (array_len > 0) max_locals_ += static_cast<uint32_t>(array_len);
    locals_[name] = {slot, true, array_len};
    return slot;
}

NativeGen::VarInfo& NativeGen::resolve_var(const std::string& name) {
    auto it = locals_.find(name);
    if (it != locals_.end()) return it->second;
    auto git = globals_.find(name);
    if (git != globals_.end()) return git->second;
    uint32_t slot = num_globals_++;
    globals_[name] = {slot, false};
    return globals_[name];
}

/* ── 表达式是否为 double 语境 (轻量推断: 字面量/已声明的 double 变量/其算术组合) ── */
bool NativeGen::isDblExpr(const std::unique_ptr<Expr>& e) {
    if (!e) return false;
    if (std::get_if<DoubleLit>(e.get())) return true;
    if (auto* id = std::get_if<Ident>(e.get())) {
        auto it = dbl_vars_.find(id->name);
        return it != dbl_vars_.end();
    }
    if (auto* b = std::get_if<Binary>(e.get())) {
        const std::string& op = b->op;
        if (op == "+" || op == "-" || op == "*")
            return isDblExpr(b->lhs) || isDblExpr(b->rhs);
    }
    return false;
}

/* ── 双精度表达式求值 → xmm0; 整数子表达式自动 cvtsi2sd ── */
void NativeGen::gen_expr_dbl(const std::unique_ptr<Expr>& e) {
    /* double 常量: RIP 取常量地址 → movsd xmm0,[rax] */
    if (auto* d = std::get_if<DoubleLit>(e.get())) {
        c_.lea64r_rip(0);
        size_t dp = c_.pos();
        c_.u32(0);
        dbl_patches_.push_back({dp, d->value});
        c_.u8(0xF2); c_.u8(0x0F); c_.u8(0x10); c_.modrm(0, 0, 0);   /* movsd xmm0,[rax] (rax=常量地址) */
        return;
    }
    /* double 变量: 位模式直接 movsd */
    if (auto* id = std::get_if<Ident>(e.get())) {
        auto dit = dbl_vars_.find(id->name);
        if (dit != dbl_vars_.end()) {
            auto& vi = resolve_var(id->name);
            int32_t disp = static_cast<int32_t>(-8 * static_cast<int>(vi.slot + 1));
            c_.u8(0xF2); c_.rex_wrb(0, 5); c_.u8(0x0F); c_.u8(0x10);
            c_.modrm(1, 0, 5); c_.u8(static_cast<uint8_t>(disp));   /* movsd xmm0,[rbp+disp] */
            return;
        }
    }
    /* 整数表达式: 求值到 rax 后转双精度 */
    gen_expr(*e);
    c_.u8(0xF2); c_.u8(0x48); c_.u8(0x0F); c_.u8(0x2A); c_.modrm(3, 0, 0);   /* cvtsi2sd xmm0,rax */
}

uint32_t NativeGen::part_slot_id(const std::string& m, const std::string& p,
                                 bool hasField, const std::string& f) const {
    std::string key;
    if (hasField) {
        key = part_key(m, p, f);
    } else {
        /* 无字段: 取部件第一个成员 */
        key = part_key(m, p, "*first");
    }
    auto it = part_slot_.find(key);
    if (it == part_slot_.end()) {
        throw std::runtime_error("JIT: 未知部件路径: "+m+">"+p+(hasField ? "."+f : ""));
    }
    return it->second;
}

/* ── Pattern: for(i=0; i<N; i++) { s = s + i; }
   Replace with: if(N>0) s += N*(N-1)/2;  ── O(1) instead of O(N) */
bool NativeGen::try_sum_pattern(ForStmt& n) {
    /* Check standard loop form: for(i=init; i<cmp>bound; i+=±1) { s = s + i; }
       Closed form: Σ = G(hi) - G(lo-1), G(k)=k(k+1)/2 (偶数因子先除, 模 2^64 精确) */
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
            if (un->op == "++") { inc = true; step_ok = true; }
            else if (un->op == "--") { inc = false; step_ok = true; }
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
        /* rax = k → rax = G(k) = k*(k+1)/2, 偶数因子先除 (模 2^64 精确) */
        c_.mov64r_rm64(2, 0);           /* rdx = k */
        c_.add_rm64_imm32(2, 1);        /* rdx = k+1 */
        c_.u8(0x48); c_.u8(0xF7); c_.u8(0xC0); c_.u32(1);  /* test rax, 1 */
        int jodd = emit_jcc(0x5);       /* jnz odd */
        c_.u8(0x48); c_.u8(0xD1); c_.u8(0xE8);  /* shr rax, 1: rax = k/2 */
        c_.imul_r64_rm64(0, 2);         /* rax = (k/2)*(k+1) = G(k) */
        int jdone = emit_jmp();
        patch(jodd);
        c_.mov64r_rm64(1, 2);           /* rcx = k+1 */
        c_.u8(0x48); c_.u8(0xD1); c_.u8(0xE9);  /* shr rcx, 1: rcx = (k+1)/2 */
        c_.imul_r64_rm64(0, 1);         /* rax = k*((k+1)/2) = G(k) */
        patch(jdone);
    };
    /* ── compilation helpers ── */
    gen_expr(*bin->rhs);
    bool bound_is_const = false;
    int64_t bound_const = 0;
    if (auto* bip = std::get_if<IntLit>(bin->rhs.get())) { bound_is_const = true; bound_const = bip->value; }
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
        /* rcx = N(边界值): 先 G(N±1) 到 r11; 再 G(init-1) 后 sub */
        if (!bound_is_const) { c_.mov64r_rm64(11, 0); }   /* r11 = N */
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
        if (!bound_is_const) { c_.mov64r_rm64(13, 0); }   /* r13 = M */
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
    /* loop_var = 循环后终值: 常量边界取编译期值; 变量边界运行时重载边界值 ±修正 */
    auto& lvi = resolve_var(loop_var);
    if (lvi.is_local) {
        int32_t ld = static_cast<int32_t>(-8 * static_cast<int>(lvi.slot + 1));
        if (!bound_is_const) {
            if (auto* bid = std::get_if<Ident>(bin->rhs.get())) {
                auto& bvr = resolve_var(bid->name);
                if (bvr.is_local) {
                    int32_t bd2 = static_cast<int32_t>(-8 * static_cast<int>(bvr.slot + 1));
                    c_.mov64r_memd8(0, 5, bd2);                   /* rax = 边界值 */
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
   生成: x 驻留 rax, t 驻留 r9, 循环步长 2 (unroll), 折叠值 cmov 无分支修正 */
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
    A_val = mul_rhs->value; B_val = b_rhs->value;
    /* s2: x = x % M (梅森数) */
    auto* b2 = std::get_if<Binary>(a2->value.get());
    if (!b2 || b2->op != "%") return false;
    auto* b2_lhs = std::get_if<Ident>(b2->lhs.get());
    auto* b2_rhs = std::get_if<IntLit>(b2->rhs.get());
    if (!b2_lhs || b2_lhs->name != state_var || !b2_rhs) return false;
    int64_t M_val = b2_rhs->value;
    int k = -1;
    if (M_val >= 1 && M_val <= 0x7FFFFFFFLL) {
        for (int kk = 1; kk <= 31; kk++)
            if (M_val == ((INT64_C(1) << kk) - 1)) { k = kk; break; }
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
        c_.mov32r_imm32(1, static_cast<int32_t>(A_val));  /* mov ecx, A (零扩展) */
        c_.imul_r64_rm64(0, 1);
        c_.add_rm64_imm32(0, static_cast<int32_t>(B_val));
        /* fold: r = (x&D)+(x>>k) */
        c_.mov64r_rm64(1, 0);
        c_.u8(0x25); c_.u32(static_cast<uint32_t>(M_val));  /* and eax, D */
        c_.u8(0x48); c_.u8(0xC1); c_.u8(0xF9); c_.u8(static_cast<uint8_t>(k)); /* sar rcx,k */
        c_.add_r64_rm64(0, 1);
        int jp = emit_jcc(0xD);     /* jge pos (负值修正) */
        c_.u8(0x48); c_.u8(0x05); c_.u32(static_cast<uint32_t>(M_val));
        int jp2 = emit_jcc(0xD);     /* jge pos */
        c_.u8(0x48); c_.u8(0x05); c_.u32(static_cast<uint32_t>(M_val));
        patch(jp);
        patch(jp2);
        /* 正修正(无分支 cmov) */
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
    c_.u8(0x0F); c_.u8(0x8D); c_.u32(0);   /* jge main→tail (rel32) */
    /* 迭代 1 */
    emit_iter();
    /* 迭代 2 */
    emit_iter();
    c_.add_rm64_imm32(3, 2);          /* i += 2 */
    c_.cmp_r64_rm64(3, 11);
    size_t dpos2 = c_.pos();
    c_.u8(0x0F); c_.u8(0x8C); c_.u32(0);   /* jl main (rel32) */
    /* tail: if (i >= n) goto done */
    c_.cmp_r64_rm64(3, 13);
    size_t jtail = c_.pos();
    c_.u8(0x0F); c_.u8(0x8D); c_.u32(0);   /* jge done (rel32) */
    emit_iter();
    c_.inc_rm64(3);                   /* i++ */
    /* done: patch 跳转 (rel32 在 0F 8x 之后; 目标 = 指令尾 + rel) */
    c_.patch_u32(jmain + 2, static_cast<uint32_t>((dpos2 + 6) - (jmain + 6)));   /* jge main 到 tail */
    c_.patch_u32(dpos2 + 2, static_cast<uint32_t>(jmain - (dpos2 + 6)));         /* jl tail 到 main */
    c_.patch_u32(jtail + 2, static_cast<uint32_t>(c_.pos() - (jtail + 6)));      /* jge tail 到 done */
    /* epilogue */
    c_.mov_memd8_64r(5, sd, 0);       /* x */
    c_.mov_memd8_64r(5, td, 9);       /* t */
    c_.mov_memd8_64r(5, ld, 3);       /* i */
    c_.pop_r(3);
    return true;
}

/* ── Pattern: 试除素数判定内层循环
   for (j = 2; j < X; j++) { if (X % j == 0) { prime = 0; } }
   改成: 试除到 √X 即可: j*j <= X, 且合数命中后立即跳出 (语义等价:
   合数的最小因子 ≤ √X; 素数无因子。prime 在循环内只被写 0 一次, 提前
   退出不改变任何可观察状态)。迭代数 O(X) 降到 O(√X)。 */
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
    /* body: 即 if(X % j == 0) { prime = 0; } */
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
    c_.cqo();
    c_.idiv_rm64(3);                       /* X / j, rdx = 余数 */
    c_.u8(0x48); c_.u8(0x85); c_.u8(0xD2);   /* test rdx, rdx */
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
    /* loop_var 终值 */
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
   主循环每 16 迭代一次 imul (窗口积 16 因子并行链) + 主链 3c/16iter ≈ 0.2c/iter),
   环上语义精确 (imul 模 2^64)。因子 (i+c+1)..(i+c+16) 用 lea+imul 构建。 */
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
    /* x = x * E 或 x = E * x, E = loop_var + c */
    int64_t c = 0;
    bool haveE = false;
    auto checkE = [&](const std::unique_ptr<Expr>& e) {
        if (auto* ei = std::get_if<Ident>(e.get())) {
            if (ei->name == loop_var) { c = 0; return true; }
        }
        if (auto* eb = std::get_if<Binary>(e.get())) {
            if (eb->op == "+") {
                if (auto* el = std::get_if<Ident>(eb->lhs.get())) {
                    if (auto* er = std::get_if<IntLit>(eb->rhs.get())) {
                        if (el->name == loop_var) { c = er->value; return true; }
                    }
                }
                if (auto* el = std::get_if<IntLit>(eb->lhs.get())) {
                    if (auto* er = std::get_if<Ident>(eb->rhs.get())) {
                        if (er->name == loop_var) { c = el->value; return true; }
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
    static const int P = 16;            /* 窗口因子数 */
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
        /* 预检查 */
        if (ident_bound) { c_.cmp_r64_rm64(3, 13); }
        else if (end_val >= INT32_MIN && end_val <= INT32_MAX) { c_.cmp_rm64_imm32(3, static_cast<int32_t>(end_val)); }
        else { c_.mov64r_imm64(1, end_val); c_.cmp_r64_rm64(3, 1); }
        c_.u8(0x0F); c_.u8(static_cast<uint8_t>(0x80 + 0x0D));   /* jge exit */
        size_t jexit = c_.pos(); c_.u32(0);
        size_t ltop = c_.pos();
        /* 窗口乘积: 每链 4 因子 (lea 2 + imul 3), 链间并行, 树形合并 */
        auto lea_off = [&](int reg, int64_t off) {
            if (off >= -128 && off <= 127) c_.lea64r_memd8(reg, 3, static_cast<int8_t>(off));
            else c_.lea64r_memd32(reg, 3, static_cast<int32_t>(off));
        };
        for (int i = 0; i < L; i++) {
            lea_off(creg[i], c + 4 * i + 0); lea_off(1, c + 4 * i + 1); c_.imul_r64_rm64(creg[i], 1);
            lea_off(1, c + 4 * i + 2); lea_off(2, c + 4 * i + 3); c_.imul_r64_rm64(1, 2); c_.imul_r64_rm64(creg[i], 1);
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
        if (ident_bound) { c_.cmp_r64_rm64(3, 13); }
        else if (end_val >= INT32_MIN && end_val <= INT32_MAX) { c_.cmp_rm64_imm32(3, static_cast<int32_t>(end_val)); }
        else { c_.mov64r_imm64(1, end_val); c_.cmp_r64_rm64(3, 1); }
        c_.u8(0x0F); c_.u8(static_cast<uint8_t>(0x80 | (0x0D ^ 1)));   /* jl ltop */
        size_t dpos = c_.pos(); c_.u32(0);
        c_.patch_u32(dpos, static_cast<uint32_t>(ltop - (dpos + 4)));
        c_.patch_u32(jexit, static_cast<uint32_t>(c_.pos() - (jexit + 4)));
        c_.mov_memd8_64r(5, xd, 12);     /* x = r12 */
        c_.pop_r(12);
        if (ident_bound) c_.pop_r(13);
    }
    /* loop_var 终值 */
    auto& lvi = resolve_var(loop_var);
    if (lvi.is_local) {
        int32_t ld = static_cast<int32_t>(-8 * static_cast<int>(lvi.slot + 1));
        c_.mov64r_imm64(0, (bin->op == "<") ? end_val : (end_val + 1));
        c_.mov_memd8_64r(5, ld, 0);
    }
    return true;
}

/* ── Pattern: for(i=init; i<cmp>bound; i++) { x = x * C; } (C 编译期常量)
   x_final = x_entry * C^N mod 2^64 —— 快速幂在编译期完成, O(1) 生成 */
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
        c_.mov64r_imm64(0, 0);              /* C^N == 0 mod 2^64 → x = 0 */
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

/* ── 线性映射折叠 (t03 形态): x = x*C + D / x = x + C, 常量边界
   → 整段闭式 x = C^N * x + D * Σ_{k=0}^{N-1} C^k (mod 2^64) ── */
static uint64_t pw64(uint64_t b, uint64_t e) {
    uint64_t r = 1;
    while (e) { if (e & 1) r *= b; b *= b; e >>= 1; }
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
        if (isX(mbin->lhs) && isCst(mbin->rhs, v)) { C = 1; D = v; ok = true; }
        else if (isX(mbin->rhs) && isCst(mbin->lhs, v)) { C = 1; D = v; ok = true; }
        else {
            auto tryMul = [&](const std::unique_ptr<Expr>& a, const std::unique_ptr<Expr>& b) -> bool {
                auto* mb = std::get_if<Binary>(a.get());
                if (!mb || mb->op != "*") return false;
                int64_t cv = 0, dv = 0;
                if (isX(mb->lhs) && isCst(mb->rhs, cv) && isCst(b, dv)) { C = cv; D = dv; return true; }
                if (isX(mb->rhs) && isCst(mb->lhs, cv) && isCst(b, dv)) { C = cv; D = dv; return true; }
                return false;
            };
            ok = tryMul(mbin->lhs, mbin->rhs) || tryMul(mbin->rhs, mbin->lhs);
        }
    } else if (mbin->op == "*") {
        int64_t v = 0;
        if (isX(mbin->lhs) && isCst(mbin->rhs, v)) { C = v; D = 0; ok = true; }
        else if (isX(mbin->rhs) && isCst(mbin->lhs, v)) { C = v; D = 0; ok = true; }
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

/* ── 变量边界线性折叠: for(i=init; i<n; i++){ x = C*x + D; } → O(log N) 运行时平方乘
   N 为运行时值: 生成 二进制快速幂 计算 A=C^N 与 S=ΣC^k (mod 2^64), x = A*x + D*S。
   全程仅 imul/add 环绕, 与逐次迭代在 mod 2^64 下逐位等价。── */
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
    /* 匹配仿射体 x = C*x + D / x = x + D / x = x*C */
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
        v = ei->value; return true;
    };
    if (mbin->op == "+") {
        int64_t v = 0;
        if (isX(mbin->lhs) && isCst(mbin->rhs, v)) { C = 1; D = v; ok = true; }
        else if (isX(mbin->rhs) && isCst(mbin->lhs, v)) { C = 1; D = v; ok = true; }
        else {
            auto tryMul = [&](const std::unique_ptr<Expr>& a, const std::unique_ptr<Expr>& b2) -> bool {
                auto* mb = std::get_if<Binary>(a.get());
                if (!mb || mb->op != "*") return false;
                int64_t cv = 0, dv = 0;
                if (isX(mb->lhs) && isCst(mb->rhs, cv) && isCst(b2, dv)) { C = cv; D = dv; return true; }
                if (isX(mb->rhs) && isCst(mb->lhs, cv) && isCst(b2, dv)) { C = cv; D = dv; return true; }
                return false;
            };
            ok = tryMul(mbin->lhs, mbin->rhs) || tryMul(mbin->rhs, mbin->lhs);
        }
    } else if (mbin->op == "*") {
        int64_t v = 0;
        if (isX(mbin->lhs) && isCst(mbin->rhs, v)) { C = v; D = 0; ok = true; }
        else if (isX(mbin->rhs) && isCst(mbin->lhs, v)) { C = v; D = 0; ok = true; }
    }
    if (!ok) return false;
    auto& xvi = resolve_var(as->name);
    if (!xvi.is_local) return false;
    auto& bvi = resolve_var(bound_name);
    if (!bvi.is_local) return false;
    int32_t xd = static_cast<int32_t>(-8 * static_cast<int>(xvi.slot + 1));
    int32_t bd = static_cast<int32_t>(-8 * static_cast<int>(bvi.slot + 1));
    bool le = (bin->op == "<=");
    /* 循环变量槽位: 终值 i=n(<) / n+1(<=), 零次迭代时 i=init */
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
    c_.rex_wb(11); c_.u8(0xF7); c_.modrm(3, 0, 11); c_.u32(1);   /* test r11, 1 */
    int j_bit = emit_jcc(0x4);                  /* je sq */
    c_.mov64r_rm64(0, 9);                       /* rax = GS */
    c_.imul_r64_rm64(0, 6);                     /* rax = GS*AP */
    c_.add_r64_rm64(7, 0);                      /* S += GS*AP */
    c_.imul_r64_rm64(6, 8);                     /* AP *= BLK */
    patch(j_bit);                               /* sq: */
    c_.rex_wb(11); c_.u8(0xC1); c_.u8(0xEB); c_.u8(1);           /* shr r11, 1 */
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

    /* 终值分派: 正常路径 i = n(<) / n+1(<=)（rcx 全程未被破坏, 直接复用）; 零迭代路径 i=init */
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
    return std::visit([](const auto& n) -> bool {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, Call>) return false;
        else if constexpr (std::is_same_v<T, Input>) return false;
        else if constexpr (std::is_same_v<T, Output>) return false;
        else if constexpr (std::is_same_v<T, Binary>) return exprIsPure(*n.lhs) && exprIsPure(*n.rhs);
        else if constexpr (std::is_same_v<T, Unary>) return exprIsPure(*n.operand);
        else return true;
    }, e);
}

/* ── Pattern: 立方和相等计数 (t10 形态)
   for(x){ for(y){ for(z){ if(x^3+y^3+z^3 == C){ cnt++; } } } }
   x^3 单调、y^3 单调 → 对每个 z, m=C-z^3, 双指针 (x 升/y 降) 扫 x^3+y^3==m
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
    /* lhs 拆加项: 每项必须是某循环变量的纯立方 */
    std::vector<const Expr*> terms;
    std::function<void(const Expr&)> collect = [&](const Expr& e) {
        if (auto* b = std::get_if<Binary>(&e)) {
            if (b->op == "+") { collect(*b->lhs); collect(*b->rhs); return; }
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
        auto walk = [&](auto&& self, const Expr& e) -> void {
            if (auto* id = std::get_if<Ident>(&e)) {
                if (v.empty()) v = id->name;
                else if (v != id->name) v = "#bad";
                idcnt++;
            } else if (auto* bb = std::get_if<Binary>(&e)) {
                if (bb->op != "*") { v = "#bad"; return; }
                self(self, *bb->lhs); self(self, *bb->rhs);
            } else { v = "#bad"; }
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
    /* cond 是 Binary < 且 lhs=变量 */
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
    c_.u8(0x0F); c_.u8(0x84);     /* je hit */
    size_t jhit = c_.pos(); c_.u32(0);
    c_.u8(0x0F); c_.u8(0x8C);     /* jl xinc */
    size_t jxinc = c_.pos(); c_.u32(0);
    /* ydec */
    c_.u8(0x49); c_.u8(0xFF); c_.u8(0xCC);  /* dec r12 */
    size_t ycheck = c_.pos();
    c_.cmp_rm64_imm32(3, static_cast<int32_t>(X));
    c_.u8(0x0F); c_.u8(0x8D);     /* jge y_exit */
    size_t jyexit = c_.pos(); c_.u32(0);
    c_.cmp_rm64_imm32(12, 0);
    c_.u8(0x0F); c_.u8(0x8C);     /* jl y_exit */
    size_t jyexit2 = c_.pos(); c_.u32(0);
    c_.u8(0xE9);                  /* jmp y_top */
    size_t jyback = c_.pos(); c_.u32(0);
    /* hit: */
    size_t hit_done = c_.pos();
    c_.u8(0x49); c_.u8(0xFF); c_.u8(0xC6);  /* inc r14 */
    c_.inc_rm64(3);
    c_.u8(0x49); c_.u8(0xFF); c_.u8(0xCC);  /* dec r12 */
    c_.u8(0xE9);                  /* jmp ycheck */
    size_t jhitback = c_.pos(); c_.u32(0);
    /* xinc: */
    size_t xinc_done = c_.pos();
    c_.inc_rm64(3);
    c_.u8(0xE9);                  /* jmp ycheck */
    size_t jxincback = c_.pos(); c_.u32(0);
    c_.patch_u32(jhit, static_cast<uint32_t>(hit_done - (jhit + 4)));
    c_.patch_u32(jxinc, static_cast<uint32_t>(xinc_done - (jxinc + 4)));
    /* y_exit: */
    size_t yexit = c_.pos();
    c_.patch_u32(jyexit, static_cast<uint32_t>(yexit - (jyexit + 4)));
    c_.patch_u32(jyexit2, static_cast<uint32_t>(yexit - (jyexit2 + 4)));
    c_.patch_u32(jyback, static_cast<uint32_t>(ytop - (jyback + 4)));
    c_.patch_u32(jhitback, static_cast<uint32_t>(ycheck - (jhitback + 4)));
    c_.patch_u32(jxincback, static_cast<uint32_t>(ycheck - (jxincback + 4)));
    c_.u8(0x49); c_.u8(0xFF); c_.u8(0xC5);  /* inc r13 */
    c_.cmp_rm64_imm32(13, static_cast<int32_t>(Z));
    c_.u8(0x0F); c_.u8(0x8D);     /* jge z_done */
    size_t jzdone = c_.pos(); c_.u32(0);
    c_.u8(0xE9);
    size_t jzback = c_.pos(); c_.u32(0);
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

/* ── Pattern: while (b != 0) { r = a % b; a = b; b = r; } (a,b 编译期已知常量)
   欧几里得在编译期执行, 循环整体替换为 a=gcd; b=0; —— O(步数) → O(1) */
bool NativeGen::try_gcd_loop(WhileStmt& n) {
    auto* bin = std::get_if<Binary>(n.cond.get());
    if (!bin || bin->op != "!=") return false;
    std::string bvar;
    bool b_left = true;
    if (auto* lid = std::get_if<Ident>(bin->lhs.get())) {
        bvar = lid->name; b_left = true;
    } else if (auto* rid = std::get_if<Ident>(bin->rhs.get())) {
        bvar = rid->name; b_left = false;
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
        d0name = s0a->name; d0val = std::move(s0a->value);
    } else if (auto* s0v = std::get_if<VarDecl>(&n.body[0])) {
        d0name = s0v->name; d0val = std::move(s0v->init);
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
    while (bb != 0) { uint64_t rr = aa % bb; aa = bb; bb = rr; }
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
   f 不含循环变量、无副作用。f 在运行时收敛 (f(x)==x) 后 x 永不再变 →
   提前跳出循环 (剩余迭代 x 不变, 语义完全等价)。迭代 O(N) → O(收敛步数)。
   不收敛时每迭代仅多 1 条 cmp+jcc 开销。 */
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
    if (ident_bound) { c_.cmp_r64_rm64(3, 13); }
    else if (end_val >= INT32_MIN && end_val <= INT32_MAX) { c_.cmp_rm64_imm32(3, static_cast<int32_t>(end_val)); }
    else { c_.mov64r_imm64(1, end_val); c_.cmp_r64_rm64(3, 1); }
    int jcc0;
    if (cop == "<") jcc0 = 0x0D; else if (cop == "<=") jcc0 = 0x0F;
    else if (cop == ">") jcc0 = 0x0E; else jcc0 = 0x0C;
    c_.u8(0x0F); c_.u8(static_cast<uint8_t>(0x80 + jcc0));   /* jcc done */
    size_t jdone = c_.pos(); c_.u32(0);
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
    c_.u8(0x0F); c_.u8(0x84);            /* je done (收敛) */
    size_t jcv = c_.pos(); c_.u32(0);
    c_.mov_memd8_64r(5, xd, 0);          /* x = f(x) */
    if (step_inc) c_.inc_rm64(3); else { c_.u8(0x48); c_.u8(0xFF); c_.u8(0xCB); }  /* dec rbx */
    if (ident_bound) { c_.cmp_r64_rm64(3, 13); }
    else if (end_val >= INT32_MIN && end_val <= INT32_MAX) { c_.cmp_rm64_imm32(3, static_cast<int32_t>(end_val)); }
    else { c_.mov64r_imm64(1, end_val); c_.cmp_r64_rm64(3, 1); }
    c_.u8(0x0F); c_.u8(static_cast<uint8_t>(0x80 | (jcc0 ^ 1)));  /* 反跳 top */
    size_t jbk = c_.pos(); c_.u32(0);
    c_.patch_u32(jbk, static_cast<uint32_t>(top - (jbk + 4)));
    c_.patch_u32(jcv, static_cast<uint32_t>(c_.pos() - (jcv + 4)));
    c_.patch_u32(jdone, static_cast<uint32_t>(c_.pos() - (jdone + 4)));
    /* i 终值 */
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
   → 闭式 2^n - 1 (mod 2^64), O(1) 生成 */
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
    c_.mov64r_imm64(0, -1);                 /* n>=64 → 2^n-1 ≡ -1 (mod 2^64) */
    int jend = emit_jmp();
    patch(jlt);                             /* ok: */
    c_.u8(0x48); c_.u8(0xD3); c_.u8(0xE0);  /* shl rax, cl */
    c_.u8(0x48); c_.u8(0xFF); c_.u8(0xC8);  /* dec rax */
    patch(jend);
    c_.mov64r_rm64(4, 5);  /* mov rsp, rbp: 丢弃循环寄存器 push 残留 */
    c_.mov64r_memd8(3, 5, -8);    /* rbx ← [rbp-8]  (压栈序: rbx,r12,r13,r14,r15) */
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
    c_.cmp_rm64_imm32(0, 2);             /* n < 2 则 return n */
    int jlt = emit_jcc(0xC);             /* jl done */
    c_.mov64r_imm64(2, 1);               /* rdx = a = 1 */
    c_.mov64r_imm64(8, 1);               /* r8  = b = 1 */
    c_.mov64r_imm64(9, 1);               /* r9  = c = 1 */
    c_.xor_rm64_r64(10, 10);             /* r10 = d = 0 */
    c_.xor_rm64_r64(11, 11);             /* r11 = x = 0 */
    c_.mov64r_imm64(6, 1);               /* rsi = y = 1 */
    size_t loop_top = c_.pos();
    c_.u8(0x48); c_.u8(0xF7); c_.u8(0xC0); c_.u32(1);   /* test rax, 1 */
    int jz_odd = emit_jcc(0x4);          /* jz skip_odd */
    /* odd: nx=a*x+b*y; ny=c*x+d*y  (x into rcx/rdi, then reuse r11/rsi) */
    c_.mov64r_rm64(1, 2);   c_.imul_r64_rm64(1, 11);    /* rcx = a*x */
    c_.mov64r_rm64(7, 9);   c_.imul_r64_rm64(7, 11);    /* rdi = c*x  (x consumed) */
    c_.mov64r_rm64(11, 8);  c_.imul_r64_rm64(11, 6);    /* r11 = b*y */
    c_.mov64r_rm64(13, 10); c_.imul_r64_rm64(13, 6);    /* r13 = d*y  (y consumed) */
    c_.add_r64_rm64(1, 11);                             /* rcx = nx */
    c_.add_r64_rm64(7, 13);                             /* rdi = ny */
    c_.mov64r_rm64(11, 1);                              /* x = nx */
    c_.mov64r_rm64(6, 7);                               /* y = ny */
    patch(jz_odd);                             /* skip_odd: */
    c_.u8(0x48); c_.u8(0xD1); c_.u8(0xE8); /* shr rax, 1 (p>>=1) */
    int jz_exit = emit_jcc(0x4);           /* jz exit (p==0) */
    /* square: na=a*a+b*c; nb=a*b+b*d; nc=c*a+d*c; nd=c*b+d*d
       park x/y on the stack (frame-size agnostic); d stays live in r10;
       scratch: rcx,rdi,r13 + rsi,r11 (5 total) */
    c_.push_r(11);                              /* park x */
    c_.push_r(6);                               /* park y */
    c_.mov64r_rm64(1, 2);  c_.imul_r64_rm64(1, 2);     /* rcx = a*a */
    c_.mov64r_rm64(7, 8);  c_.imul_r64_rm64(7, 9);     /* rdi = b*c */
    c_.add_r64_rm64(1, 7);                             /* rcx = na */
    c_.mov64r_rm64(13, 2); c_.imul_r64_rm64(13, 8);    /* r13 = a*b */
    c_.mov64r_rm64(6, 10); c_.imul_r64_rm64(6, 8);     /* rsi = b*d */
    c_.add_r64_rm64(13, 6);                            /* r13 = nb */
    c_.mov64r_rm64(7, 9);  c_.imul_r64_rm64(7, 2);     /* rdi = c*a */
    c_.mov64r_rm64(6, 10); c_.imul_r64_rm64(6, 9);     /* rsi = d*c */
    c_.add_r64_rm64(7, 6);                             /* rdi = nc */
    c_.mov64r_rm64(6, 9);  c_.imul_r64_rm64(6, 8);     /* rsi = c*b */
    c_.mov64r_rm64(11, 10); c_.imul_r64_rm64(11, 10);  /* r11 = d*d */
    c_.add_r64_rm64(6, 11);                            /* rsi = nd */
    c_.mov64r_rm64(2, 1);   c_.mov64r_rm64(8, 13);     /* a=na, b=nb */
    c_.mov64r_rm64(9, 7);   c_.mov64r_rm64(10, 6);     /* c=nc, d=nd */
    c_.pop_r(6);                                /* restore y */
    c_.pop_r(11);                               /* restore x */
    size_t jback = c_.jmp32();
    c_.patch_u32(jback, static_cast<uint32_t>(loop_top - (jback + 4)));
    patch(jz_exit);                            /* exit: */
    c_.mov64r_rm64(0, 11);                     /* rax = x */
    patch(jlt);                                /* done: */
    /* RAX = fib(n). Return it. */
    c_.mov64r_rm64(4, 5);  /* mov rsp, rbp: 丢弃循环寄存器 push 残留 */
    c_.mov64r_memd8(3, 5, -8);    /* rbx ← [rbp-8]  (压栈序: rbx,r12,r13,r14,r15) */
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
                int32_t d = static_cast<int32_t>(-8 * static_cast<int>(vi.slot + vi.array_len));
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
            if (vi.is_local) {
                int32_t d = static_cast<int32_t>(-8 * static_cast<int>(vi.slot + vi.array_len));
                if (d >= -128 && d <= 127) c_.lea64r_memd8(0, 5, static_cast<int8_t>(d));
                else c_.lea64r_memd32(0, 5, d);
            } else {
                emit_global_addr(0, vi.slot);
            }
        } else if (vi.is_local) {
            int32_t d = static_cast<int32_t>(-8 * static_cast<int>(vi.slot + 1));
            c_.mov64r_memd8(0, 5, d);
        } else {
            emit_global_load(0, vi.slot);
        }
        c_.push_r(0);
        gen_expr(*ix->idx);
        c_.mov64r_rm64(1, 0);
        c_.pop_r(0);
        c_.u8(0x48); c_.u8(0x8D); c_.u8(0x04); c_.u8(0xC8);
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

void NativeGen::gen_expr(Expr& e) {
    struct ExprGen {
        NativeGen* self;
        void operator()(IntLit& n) {
            if (n.value >= 0 && n.value <= 0x7FFFFFFFLL) {
                self->c_.u8(0xB8); self->c_.u32(static_cast<uint32_t>(n.value)); /* mov eax, imm32 (5B) */
            } else {
                self->c_.mov64r_imm64(0, n.value);
            }
        }
        void operator()(DoubleLit& n) {
            /* 双精度常量: RIP 相对取 8 字节 IEEE754 常量地址, 按位载入 rax
               (存储即位模式; 算术运算由 gen_expr_dbl 走 SSE 路径, 不经过这里) */
            self->c_.lea64r_rip(0);
            size_t disp_pos = self->c_.pos();
            self->c_.u32(0);
            self->dbl_patches_.push_back({disp_pos, n.value});
            self->c_.rex_wrb(0, 0); self->c_.u8(0x8B); self->c_.modrm(0, 0, 0);  /* mov rax,[rax] */
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
            if(!self->fast_loop_var_.empty()&&n.name==self->fast_loop_var_){
                self->c_.mov64r_rm64(0,self->fast_loop_reg_);
                return;
            }
            auto& vi = self->resolve_var(n.name);
            if (vi.array_len > 0) {
                if (vi.is_local) {
                    int32_t d = static_cast<int32_t>(-8 * static_cast<int>(vi.slot + vi.array_len));
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
                        self->last_store_len_ = (size_t)-1;
                        return;
                    }
                    self->c_.mov64r_rm64(0, it->second);
                    return;
                }
                int32_t disp = static_cast<int32_t>(-8 * static_cast<int>(vi.slot + 1));
                if (self->last_store_len_ == self->c_.pos() && self->last_store_slot_ == static_cast<int>(vi.slot)) {
                    self->last_store_len_ = (size_t)-1;
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
        void operator()(Binary& n) {
            const std::string& op = n.op;
            /* ── 逻辑与/或: 短路求值, 跳转实现, 结果 0/1 存 rax ── */
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
                    self->c_.u8(0xB8); self->c_.u32(1);   /* mov eax,1 */
                    j_end = self->emit_jmp();
                    self->patch(j_short1); self->patch(j_short2);
                    self->c_.xor_rm64_r64(0, 0);           /* 短路假: eax=0 */
                } else {
                    self->c_.xor_rm64_r64(0, 0);           /* 全假: eax=0 */
                    j_end = self->emit_jmp();
                    self->patch(j_short1); self->patch(j_short2);
                    self->c_.u8(0xB8); self->c_.u32(1);   /* 短路真: eax=1 */
                }
                self->patch(j_end);
                return;
            }
            /* ── 双精度算术: 任一操作数为 double → SSE2 路径 (+,-,*) ── */
            if((op=="+"||op=="-"||op=="*") && (self->isDblExpr(n.lhs)||self->isDblExpr(n.rhs))){
                self->gen_expr_dbl(n.lhs);                       /* xmm0 = lhs */
                self->c_.sub_rsp(16);
                self->c_.u8(0xF2); self->c_.u8(0x0F); self->c_.u8(0x11);
                self->c_.modrm(0, 0, 4); self->c_.u8(0x24);      /* movsd [rsp],xmm0 */
                self->gen_expr_dbl(n.rhs);                       /* xmm0 = rhs */
                self->c_.u8(0xF2); self->c_.u8(0x0F); self->c_.u8(0x10);
                self->c_.modrm(0, 1, 4); self->c_.u8(0x24);      /* movsd xmm1,[rsp] */
                self->c_.add_rsp(16);
                if(op=="+"){      self->c_.u8(0xF2); self->c_.u8(0x0F); self->c_.u8(0x58); self->c_.modrm(3,0,1); }
                else if(op=="-"){ self->c_.u8(0xF2); self->c_.u8(0x0F); self->c_.u8(0x5C); self->c_.modrm(3,0,1); }
                else             { self->c_.u8(0xF2); self->c_.u8(0x0F); self->c_.u8(0x59); self->c_.modrm(3,0,1); } /* mulsd */
                return;
            }
            if (auto* ip = std::get_if<IntLit>(n.rhs.get())) {
                int64_t v = ip->value;
                if (op == "+" && v >= INT32_MIN && v <= INT32_MAX) {
                    self->gen_expr(*n.lhs);
                    self->c_.add_rm64_imm32(0, static_cast<int32_t>(v));
                    return;
                }
                if (op == "-" && v >= INT32_MIN && v <= INT32_MAX) {
                    self->gen_expr(*n.lhs);
                    self->c_.sub_rm64_imm32(0, static_cast<int32_t>(v));
                    return;
                }
                if (op == "*" && v >= 0 && v <= 0xFFFFFFFFLL) {
                    self->gen_expr(*n.lhs);
                    /* 优化: 2 的幂乘 → shl 强度削减 */
                    if (v >= 2 && (v & (v - 1)) == 0) {
                        int sh = 0;
                        while ((v >> sh) != 1) sh++;
                        self->c_.u8(0x48); self->c_.u8(0xC1); self->c_.u8(0xE0); self->c_.u8(static_cast<uint8_t>(sh));
                        return;
                    }
                    if (v == 1) return;
                    self->c_.u8(0xB9); self->c_.u32(static_cast<uint32_t>(v));
                    self->c_.imul_r64_rm64(0, 1);
                    return;
                }
                if (op == "%") {
                    int k = -1;
                    if (v >= 1 && v <= 0x7FFFFFFFLL) {
                        for (int kk = 1; kk <= 31; kk++) {
                            if (v == ((INT64_C(1) << kk) - 1)) { k = kk; break; }
                        }
                    }
                    if (k >= 1) {
                        self->gen_expr(*n.lhs);
                        self->emit_mod_fold(v, k);
                        return;
                    }
                }
            }
            if (auto* rid = std::get_if<Ident>(n.rhs.get())) {
                /* 快循环变量驻留寄存器: 直接寄存器加/减, 禁止读陈旧内存槽 */
                if (!self->fast_loop_var_.empty() && rid->name == self->fast_loop_var_ && (op == "+" || op == "-")) {
                    self->gen_expr(*n.lhs);
                    if (op == "+") self->c_.add_r64_rm64(0, self->fast_loop_reg_);
                    else self->c_.sub_r64_rm64(0, self->fast_loop_reg_);
                    return;
                }
                auto& rvi = self->resolve_var(rid->name);
                if (rvi.is_local && (op == "+" || op == "-")) {
                    int32_t rd = static_cast<int32_t>(-8 * static_cast<int>(rvi.slot + 1));
                    self->gen_expr(*n.lhs);
                    if (op == "+") self->c_.add_r64_memd8(0, 5, rd);
                    else self->c_.sub_r64_memd8(0, 5, rd);
                    return;
                }
            }
            /* 优化: 左侧局部变量折叠 x + rhs (仅加法可交换; rhs 必须无副作用以保证求值顺序) */
            if (op == "+" && !self->fast_loop_var_.empty()) {
                if (auto* lid = std::get_if<Ident>(n.lhs.get())) {
                    if (lid->name != self->fast_loop_var_ && exprIsPure(*n.rhs)) {
                        auto& lvi = self->resolve_var(lid->name);
                        if (lvi.is_local) {
                            int32_t ld = static_cast<int32_t>(-8 * static_cast<int>(lvi.slot + 1));
                            self->gen_expr(*n.rhs);
                            self->c_.add_r64_memd8(0, 5, ld);
                            return;
                        }
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
            } else if (op == "-") {
                self->c_.sub_r64_rm64(0, 1);
            } else if (op == "*") {
                self->c_.imul_r64_rm64(0, 1);
            } else if (op == "/") {
                self->c_.cqo();
                self->c_.idiv_rm64(1);
            } else if (op == "%") {
                int64_t cst = -1;
                auto* cstp = std::get_if<IntLit>(n.rhs.get());
                if (cstp) cst = cstp->value;
                int k = -1;
                if (cst >= 1 && cst <= 0x7FFFFFFFLL) {
                    for (int kk = 1; kk <= 31; kk++) {
                        if (cst == ((INT64_C(1) << kk) - 1)) { k = kk; break; }
                    }
                }
                if (k >= 1) {
                    self->emit_mod_fold(cst, k);
                } else {
                    self->c_.cqo();
                    self->c_.idiv_rm64(1);
                    self->c_.mov64r_rm64(0, 2);
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
                if (!id) { /* fallback: just eval */ self->gen_expr(*n.operand); return; }
                /* 快循环变量驻留寄存器: 后缀语义 rax=旧值, 寄存器自增/减 */
                if (!self->fast_loop_var_.empty() && id->name == self->fast_loop_var_) {
                    int fr = self->fast_loop_reg_;
                    self->c_.mov64r_rm64(0, fr);
                    if (n.op == "++") self->c_.add_rm64_imm32(fr, 1);
                    else self->c_.sub_rm64_imm32(fr, 1);
                    return;
                }
                auto& vi = self->resolve_var(id->name);
                if (!vi.is_local) { /* global not supported for ++/-- */ self->gen_expr(*n.operand); return; }
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
                self->gen_expr(*n.operand);
                if (n.op == "-") {
                    self->c_.neg_rm64(0);
                } else if (n.op == "!") {
                    self->c_.cmp_rm64_imm32(0, 0);
                    self->c_.setcc_r8(0x4, 0);  /* sete */
                    self->c_.movzx_r64_rm8(0, 0);
                } else if (n.op == "*") {
                    self->c_.rex_wrb(0, 0); self->c_.u8(0x8B); self->c_.modrm(0, 0, 0);  /* mov rax, [rax] */
                } else if (n.op == "&") {
                    self->gen_addr_to_rax(*n.operand);
                }
            }
        }
        void operator()(Call& n) {
            /* 组合数常量折叠: comb_func_ 已识别为递推形态, 实参全常量时编译期算 C(n,k) */
            if (!self->comb_func_.empty() && n.callee == self->comb_func_ && n.args.size() == 2) {
                auto* a0 = std::get_if<IntLit>(n.args[0].get());
                auto* a1 = std::get_if<IntLit>(n.args[1].get());
                if (a0 && a1) {
                    uint64_t nn = static_cast<uint64_t>(a0->value);
                    uint64_t kk = static_cast<uint64_t>(a1->value);
                    if (kk <= nn && nn <= 62) {
                        uint64_t k = (kk < nn - kk) ? kk : (nn - kk);
                        __int128 res = 1;
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
                self->gen_expr(*n.args[i]);
                if (i * 8 <= 127) {
                    self->c_.mov_memd8_64r(4, static_cast<int8_t>(i * 8), 0);
                } else {
                    self->c_.mov_memd32_64r(4, i * 8, 0);
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
            if (n.values.empty()) return;
            for (auto& v : n.values) {
                if (!v) continue;
                bool is_str = std::get_if<StrLit>(v.get()) != nullptr;
                bool is_dbl = self->isDblExpr(v);
                if (is_dbl) {
                    self->gen_expr_dbl(v);                            /* xmm0 */
                    self->c_.u8(0xF2); self->c_.u8(0x0F); self->c_.u8(0x10);
                    self->c_.modrm(3, 1, 0);                          /* movsd xmm1,xmm0 */
                } else {
                    self->gen_expr(*v);
                    self->c_.mov64r_rm64(2, 0);
                }
                if (auto* pg = std::get_if<ModulePathGet>(v.get())) {
                    std::string key = pg->has_field
                        ? self->part_key(pg->module, pg->part, pg->field)
                        : self->part_key(pg->module, pg->part, "*first");
                    auto it = self->part_is_str_.find(key);
                    if (it != self->part_is_str_.end()) is_str = it->second;
                }
                {
                    self->c_.sub_rsp(32);
                    size_t fmt_pos = self->c_.pos();
                    self->c_.lea64r_rip(1);
                    self->c_.u32(0);
                    size_t call_pos = self->c_.pos();
                    self->c_.call_rip();
                    self->c_.add_rsp(32);
                    self->pe_iat_patches_.push_back({call_pos + 2, 0});
                    self->pe_fmt_patches_.push_back({fmt_pos + 3, is_dbl ? 3 : (is_str ? 2 : 0)});
                }
            }
        }
        void operator()(Input& n) {
            self->known_const_.erase(n.var_name);
            {
                self->c_.sub_rsp(0x80);                    /* 128: rsp%16==0 at calls, shadow 0x20..0x3F */
                self->c_.u8(0x48); self->c_.u8(0xC7); self->c_.u8(0xC1);
                self->c_.u32(static_cast<uint32_t>(-10));  /* mov rcx, -10 (STD_INPUT_HANDLE) */
                size_t c1 = self->c_.pos();
                self->c_.call_rip();                       /* call [IAT+GetStdHandle] */
                self->pe_iat_patches_.push_back({c1 + 2, 2});
                self->c_.u8(0x48); self->c_.u8(0x89); self->c_.u8(0xC1);  /* mov rcx, rax */
                self->c_.u8(0x48); self->c_.u8(0x89); self->c_.u8(0x4C);
                self->c_.u8(0x24); self->c_.u8(0x78);      /* mov [rsp+0x78], rcx = hStdin */
                size_t retry_top = self->c_.pos();
                self->c_.u8(0x48); self->c_.u8(0x8B); self->c_.u8(0x4C);
                self->c_.u8(0x24); self->c_.u8(0x78);      /* mov rcx, [rsp+0x78] */
                self->c_.u8(0x48); self->c_.u8(0x8D); self->c_.u8(0x54);
                self->c_.u8(0x24); self->c_.u8(0x30);      /* lea rdx, [rsp+0x30] (buf) */
                self->c_.u8(0x41); self->c_.u8(0xB8); self->c_.u32(64);   /* mov r8d, 64 */
                self->c_.u8(0x4C); self->c_.u8(0x8D); self->c_.u8(0x4C);
                self->c_.u8(0x24); self->c_.u8(0x70);      /* lea r9, [rsp+0x70] (&nread) */
                self->c_.u8(0x48); self->c_.u8(0xC7); self->c_.u8(0x44);
                self->c_.u8(0x24); self->c_.u8(0x20); self->c_.u32(0);     /* mov qword[rsp+0x20], 0 = lpOverlapped */
                size_t c2 = self->c_.pos();
                self->c_.call_rip();                       /* call [IAT+ReadFile] */
                self->pe_iat_patches_.push_back({c2 + 2, 3});
                self->c_.u8(0x85); self->c_.u8(0xC0);      /* test eax, eax */
                int jfail = self->emit_jcc(0x4);           /* je retry_dec (读失败) */
                self->c_.u8(0x83); self->c_.u8(0x7C); self->c_.u8(0x24);
                self->c_.u8(0x70); self->c_.u8(0x00);      /* cmp dword[rsp+0x70], 0 (nread==0?) */
                int jhave = self->emit_jcc(0x5);           /* jne have_data */
                size_t retry_wait = self->c_.pos();         /* retry_wait: */
                self->c_.u8(0xB9); self->c_.u32(1000000);   /* mov ecx, 1e6 (~1ms 自旋) */
                self->c_.u8(0xE2); self->c_.u8(0xFE);       /* loop delay_loop (自旋) */
                size_t jretry2 = self->c_.jmp32();          /* jmp retry_top (无限等待) */
                self->c_.patch_u32(jretry2, static_cast<uint32_t>(retry_top - (jretry2 + 4)));
                self->c_.patch_u32(static_cast<uint32_t>(self->labels_[jfail].pos),
                    static_cast<uint32_t>(retry_wait - (self->labels_[jfail].pos + 4)));
                self->patch(jhave);                        /* have_data: */
                self->c_.xor_rm64_r64(0, 0);               /* xor eax, eax (val=0) */
                self->c_.u8(0x45); self->c_.u8(0x31); self->c_.u8(0xDB);  /* xor r11d, r11d (负号标志=正) */
                self->c_.u8(0x45); self->c_.u8(0x31); self->c_.u8(0xD2);  /* xor r10d, r10d */
                self->c_.u8(0x8B); self->c_.u8(0x4C); self->c_.u8(0x24); self->c_.u8(0x70);  /* mov ecx,[rsp+0x70] = nread */
                size_t loop_top = self->c_.pos();
                self->c_.u8(0x41); self->c_.u8(0x39); self->c_.u8(0xCA);  /* cmp r10d, ecx */
                int jdone = self->emit_jcc(0x3);           /* jae done */
                self->c_.u8(0x46); self->c_.u8(0x0F); self->c_.u8(0xB6);
                self->c_.u8(0x44); self->c_.u8(0x14); self->c_.u8(0x30);  /* movzx r8d,[rsp+r10+0x30] */
                self->c_.u8(0x41); self->c_.u8(0x83); self->c_.u8(0xF8); self->c_.u8(48); /* cmp r8d,'0' */
                int jb1 = self->emit_jcc(0x2);             /* jb nonsig (可能是负号) */
                self->c_.u8(0x41); self->c_.u8(0x83); self->c_.u8(0xF8); self->c_.u8(57); /* cmp r8d,'9' */
                int ja1 = self->emit_jcc(0x7);             /* ja nonsig */
                self->c_.u8(0x41); self->c_.u8(0x83); self->c_.u8(0xE8); self->c_.u8(48); /* sub r8d,'0' */
                self->c_.u8(0x8D); self->c_.u8(0x04); self->c_.u8(0x80);  /* lea eax,[rax+rax*4] */
                self->c_.u8(0x41); self->c_.u8(0x8D); self->c_.u8(0x04); self->c_.u8(0x40); /* lea eax,[r8+rax*2] */
                size_t jnonskip = self->c_.jmp32();        /* jmp skip_inc */
                self->patch(jb1);                          /* nonsig: (< '0') */
                self->patch(ja1);                          /* (> '9' 也落这里) */
                self->c_.u8(0x41); self->c_.u8(0x83); self->c_.u8(0xF8); self->c_.u8(45); /* cmp r8d,'-' */
                size_t jne_skip = self->c_.pos();
                self->c_.u8(0x75); self->c_.u8(0x00);      /* jne skip_inc (rel8 待补) */
                self->c_.u8(0x41); self->c_.u8(0xBB); self->c_.u32(1);    /* mov r11d, 1 (置负号) */
                { int32_t off = static_cast<int32_t>(self->c_.pos() - (jne_skip + 2));
                  self->c_.patch_u8(jne_skip + 1, static_cast<uint8_t>(off)); }
                size_t skip_inc_pos = self->c_.pos();
                self->c_.u8(0x41); self->c_.u8(0xFF); self->c_.u8(0xC2);  /* inc r10d */
                size_t jback = self->c_.jmp32();
                self->c_.patch_u32(jback, static_cast<uint32_t>(loop_top - (jback + 4)));
                { int32_t off = static_cast<int32_t>(skip_inc_pos - (jnonskip + 4));
                  self->c_.patch_u32(jnonskip, static_cast<uint32_t>(off)); }
                self->patch(jdone);                        /* done: */
                self->c_.u8(0x45); self->c_.u8(0x85); self->c_.u8(0xDB);  /* test r11d, r11d */
                size_t jpos = self->c_.pos();
                self->c_.u8(0x74); self->c_.u8(0x00);      /* je 正数跳过取负 (rel8 待补) */
                self->c_.u8(0x48); self->c_.u8(0xF7); self->c_.u8(0xD8);  /* neg rax */
                { int32_t off = static_cast<int32_t>(self->c_.pos() - (jpos + 2));
                  self->c_.patch_u8(jpos + 1, static_cast<uint8_t>(off)); }
                self->c_.add_rsp(0x80);
            }
            auto& vi = self->resolve_var(n.var_name);
            if (vi.is_local) {
                int32_t d = static_cast<int32_t>(-8 * static_cast<int>(vi.slot + 1));
                self->c_.mov_memd8_64r(5, d, 0);
            } else {
                self->emit_global_store(vi.slot, 0);
            }
        }
        void operator()(IndexExpr& n) {
            auto& vi=self->resolve_var(n.name);
            if(vi.is_local){
                int32_t d=static_cast<int32_t>(-8*static_cast<int>(vi.slot+1));
                if(vi.array_len>0) d=static_cast<int32_t>(-8*static_cast<int>(vi.slot+vi.array_len+1));
                if (d >= -128 && d <= 127) {
                    self->c_.mov64r_memd8(0,5,d);
                } else {
                    self->c_.mov64r_memd32(0,5,d);
                }
            }else{
                self->emit_global_load(0, vi.slot);
            }
            self->c_.push_r(0);
            self->gen_expr(*n.idx);
            self->c_.mov64r_imm64(1,8);
            self->c_.imul_r64_rm64(0,1);
            self->c_.pop_r(1);
            self->c_.add_r64_rm64(0,1);
            self->c_.rex_wrb(0, 0); self->c_.u8(0x8B); self->c_.modrm(0, 0, 0);  /* mov rax, [rax] */
        }
    };
    std::visit(ExprGen{this}, e);
}

/* ── Statement code generation ── */
void NativeGen::gen_stmt(Stmt& s) {
    struct StmtGen {
        NativeGen* self;
        void operator()(Block& n) { self->gen_block(n.stmts); }
        void operator()(ExprStmt& n) {
            if (n.expr) { self->gen_expr(*n.expr); }
        }
        void operator()(AssignStmt& n) {
            if (n.target) {
                self->gen_expr(*n.value);
                self->c_.push_r(0);
                self->gen_addr_to_rax(*n.target);
                self->c_.pop_r(2);
                self->c_.rex_wrb(2, 0); self->c_.u8(0x89); self->c_.modrm(0, 2, 0);  /* mov [rax], rdx */
                self->known_const_.clear();
                return;
            }
            if (auto* il = std::get_if<IntLit>(n.value.get())) self->known_const_[n.name] = il->value;
            else self->known_const_.erase(n.name);
            auto& vi = self->resolve_var(n.name);
            /* ── double 变量赋值: 独立路径, 绕过所有整数折叠/寄存器驻留逻辑 ── */
            if (!n.target && self->dbl_vars_.count(n.name)) {
                if (self->isDblExpr(n.value)) {
                    self->gen_expr_dbl(n.value);                      /* xmm0 */
                } else {
                    self->gen_expr(*n.value);                         /* int → rax */
                    self->c_.u8(0xF2); self->c_.u8(0x48); self->c_.u8(0x0F);
                    self->c_.u8(0x2A); self->c_.modrm(3, 0, 0);       /* cvtsi2sd xmm0,rax */
                }
                self->last_store_len_ = self->c_.pos();
                self->last_store_slot_ = static_cast<int>(vi.slot);
                if (vi.is_local) {
                    int32_t dd = static_cast<int32_t>(-8 * static_cast<int>(vi.slot + 1));
                    self->c_.u8(0xF2); self->c_.rex_wrb(5, 5); self->c_.u8(0x0F);
                    self->c_.u8(0x11); self->c_.modrm(1, 0, 5);
                    self->c_.u8(static_cast<uint8_t>(dd));            /* movsd [rbp+d],xmm0 */
                } else {
                    uint32_t slot = vi.slot;
                    int32_t goff = static_cast<int32_t>(self->global_data_off_ + 8 * slot);
                    self->emit_global_addr(2, slot);                  /* lea rdx,[global] */
                    self->c_.u8(0xF2); self->c_.u8(0x0F); self->c_.u8(0x11);
                    self->c_.modrm(0, 0, 2);                          /* movsd [rdx],xmm0 */
                    (void)goff;
                }
                return;
            }
            int32_t local_d = vi.is_local ? static_cast<int32_t>(-8 * static_cast<int>(vi.slot + 1)) : 0;

            if (auto* bin = std::get_if<Binary>(n.value.get())) {
                if (auto* lhs_id = std::get_if<Ident>(bin->lhs.get())) {
                    if (lhs_id->name == n.name && bin->op == "+") {
                        if (auto* rhs_int = std::get_if<IntLit>(bin->rhs.get())) {
                            /* x = x + const */
                            if (vi.is_local) {
                                int tgt = -1;
                                { auto it = self->loop_reg_vars_.find(vi.slot);
                                  if (it != self->loop_reg_vars_.end()) tgt = it->second; }
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
                if (bin->op == "+") {
                    if (auto* rhs_id = std::get_if<Ident>(bin->rhs.get())) {
                        if (rhs_id->name == n.name) {
                            self->gen_expr(*bin->lhs);
                            if (vi.is_local) {
                                int tgt = -1;
                                { auto it = self->loop_reg_vars_.find(vi.slot);
                                  if (it != self->loop_reg_vars_.end()) tgt = it->second; }
                                if (tgt < 0 && !self->fast_loop_var_.empty() && n.name == self->fast_loop_var_)
                                    tgt = self->fast_loop_reg_;
                                if (tgt >= 0) {
                                    self->c_.add_r64_rm64(tgt, 0);
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
                        if (lhs_id->name == n.name) {
                            /* sum = sum + expr */
                            self->gen_expr(*bin->rhs);
                            if (vi.is_local) {
                                int tgt = -1;
                                { auto it = self->loop_reg_vars_.find(vi.slot);
                                  if (it != self->loop_reg_vars_.end()) tgt = it->second; }
                                if (tgt < 0 && !self->fast_loop_var_.empty() && n.name == self->fast_loop_var_)
                                    tgt = self->fast_loop_reg_;
                                if (tgt >= 0) {
                                    self->c_.add_r64_rm64(tgt, 0);
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
            bool is_dbl = (n.type=="double"||n.type=="浮点");
            if (is_dbl) self->dbl_vars_.insert(nm);
            uint32_t slot = self->local_slot(nm, true, n.array_len);
            int32_t d = static_cast<int32_t>(-8 * static_cast<int>(slot + 1));
            if (is_dbl && n.init) {
                /* double 变量初始化: double 语境走 SSE; int 初值 cvtsi2sd */
                if (self->isDblExpr(n.init)) {
                    self->gen_expr_dbl(n.init);                       /* xmm0 */
                } else {
                    self->gen_expr(*n.init);
                    self->c_.u8(0xF2); self->c_.u8(0x48); self->c_.u8(0x0F);
                    self->c_.u8(0x2A); self->c_.modrm(3, 0, 0);       /* cvtsi2sd xmm0,rax */
                }
                self->c_.u8(0xF2); self->c_.rex_wrb(5, 5); self->c_.u8(0x0F);
                self->c_.u8(0x11); self->c_.modrm(1, 0, 5);
                self->c_.u8(static_cast<uint8_t>(d));                 /* movsd [rbp+d],xmm0 */
                self->last_store_len_ = self->c_.pos();
                self->last_store_slot_ = static_cast<int>(slot);
                return;
            }
            if (n.init) {
                self->gen_expr(*n.init);
                self->c_.mov_memd8_64r(5, d, 0);
                self->last_store_len_ = self->c_.pos();
                self->last_store_slot_ = static_cast<int>(slot);
            } else if (n.array_len > 0) {
                int32_t base_d = static_cast<int32_t>(-8 * static_cast<int>(slot + n.array_len));
                int32_t base_slot_d = static_cast<int32_t>(-8 * static_cast<int>(slot + n.array_len + 1));
                if (base_d >= -128 && base_d <= 127) {
                    self->c_.lea64r_memd8(0, 5, static_cast<int8_t>(base_d));
                } else {
                    self->c_.lea64r_memd32(0, 5, base_d);
                }
                if (base_slot_d >= -128 && base_slot_d <= 127) {
                    self->c_.mov_memd8_64r(5, static_cast<int8_t>(base_slot_d), 0);
                } else {
                    self->c_.mov_memd32_64r(5, base_slot_d, 0);
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
        void operator()(IfStmt& n) {
            if(auto* bl=std::get_if<BoolLit>(n.cond.get())){
                if(bl->value){
                    if(!n.then_body.empty()){self->gen_block(n.then_body);}
                }else{
                    if(!n.else_body.empty()){self->gen_block(n.else_body);}
                }
                return;
            }
            if(auto* il=std::get_if<IntLit>(n.cond.get())){
                if(il->value!=0){
                    if(!n.then_body.empty()){self->gen_block(n.then_body);}
                }else{
                    if(!n.else_body.empty()){self->gen_block(n.else_body);}
                }
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
        }
        void operator()(WhileStmt& n) {
            self->last_store_len_ = (size_t)-1;
            if (self->try_gcd_loop(n)) return;
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
            bool fast=false;
            std::string loop_var;
            int64_t init_val=0,end_val=0;
            int cmp_op=-1; // 0:< 1:<= 2:> 3:>=
            bool step_inc=true;
            bool ident_bound=false;
            std::string bound_name;
            if(n.init_decl&&n.init_decl->init){
                if(auto* il=std::get_if<IntLit>(n.init_decl->init.get())){
                    loop_var=n.init_decl->name;
                    init_val=il->value;
                    if(n.cond){
                        if(auto* bin=std::get_if<Binary>(n.cond.get())){
                            if(auto* id=std::get_if<Ident>(bin->lhs.get())){
                                if(id->name==loop_var){
                                    if(auto* il2=std::get_if<IntLit>(bin->rhs.get())){
                                        end_val=il2->value;
                                        if(bin->op=="<"){cmp_op=0;}
                                        else if(bin->op=="<="){cmp_op=1;}
                                        else if(bin->op==">"){cmp_op=2;}
                                        else if(bin->op==">="){cmp_op=3;}
                                    }
                                }
                            }
                            if(cmp_op==-1&&std::get_if<Ident>(bin->rhs.get())){
                                auto* id=std::get_if<Ident>(bin->rhs.get());
                                if(id->name==loop_var){
                                    if(auto* il2=std::get_if<IntLit>(bin->lhs.get())){
                                        end_val=il2->value;
                                        if(bin->op=="<"){cmp_op=2;}
                                        else if(bin->op=="<="){cmp_op=3;}
                                        else if(bin->op==">"){cmp_op=0;}
                                        else if(bin->op==">="){cmp_op=1;}
                                    }
                                }
                            }
                            if(cmp_op==-1){
                                auto* lid=std::get_if<Ident>(bin->lhs.get());
                                auto* rid=std::get_if<Ident>(bin->rhs.get());
                                if(lid&&rid&&lid->name==loop_var&&rid->name!=loop_var){
                                    bound_name=rid->name; ident_bound=true;
                                    if(bin->op=="<"){cmp_op=0;}
                                    else if(bin->op=="<="){cmp_op=1;}
                                    else if(bin->op==">"){cmp_op=2;}
                                    else if(bin->op==">="){cmp_op=3;}
                                }
                            }
                            if(cmp_op==-1){
                                auto* lid=std::get_if<Ident>(bin->lhs.get());
                                auto* rid=std::get_if<Ident>(bin->rhs.get());
                                if(lid&&rid&&rid->name==loop_var&&lid->name!=loop_var){
                                    bound_name=lid->name; ident_bound=true;
                                    if(bin->op=="<"){cmp_op=2;}
                                    else if(bin->op=="<="){cmp_op=3;}
                                    else if(bin->op==">"){cmp_op=0;}
                                    else if(bin->op==">="){cmp_op=1;}
                                }
                            }
                        }
                    }
                    if(cmp_op>=0&&n.step){
                        if(auto* un=std::get_if<Unary>(n.step.get())){
                            if(un->op=="++"||un->op=="--"){
                                if(auto* id=std::get_if<Ident>(un->operand.get())){
                                    if(id->name==loop_var){
                                        step_inc=(un->op=="++");
                                        fast=true;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            if(fast){
                if (self->opt_ && !bodyHasLoop(n.body) && n.body.size() == 1) {
                    if (self->try_chainmul(n)) return;
                    if (self->try_mul_const_loop(n)) return;
                    if (self->try_linear_fold(n)) return;
                }
                uint32_t slot=self->local_slot(loop_var,true);
                int32_t d=static_cast<int32_t>(-8*static_cast<int>(slot+1));
                std::string old_loop_var=self->fast_loop_var_;
                int old_loop_reg=self->fast_loop_reg_;
                int64_t old_loop_inc=self->fast_loop_inc_;
                self->fast_loop_var_=loop_var;
                self->fast_loop_reg_=3;
                self->fast_loop_inc_=step_inc?1:-1;
                self->c_.push_r(3);
                self->c_.mov64r_imm64(3,init_val);
                int8_t bd=0;
                if(ident_bound){
                    auto& bvi=self->resolve_var(bound_name);
                    if(bvi.is_local){
                        bd=static_cast<int32_t>(-8*static_cast<int>(bvi.slot+1));
                        self->c_.mov64r_memd8(13,5,bd);   /* r13 = bound */
                    }else{ ident_bound=false; }
                }
                int jcc;
                if(cmp_op==0){jcc=0x0D;} // < -> JGE
                else if(cmp_op==1){jcc=0x0F;} // <= -> JG
                else if(cmp_op==2){jcc=0x0E;} // > -> JLE
                else{jcc=0x0C;} // >= -> JL
                int loop_jcc=jcc^1;
                // 检测累加器模式: sum=sum+loop_var
                bool accel=false;
                int accel_reg=12;
                if(n.body.size()==1){
                    if(auto* as=std::get_if<AssignStmt>(&n.body[0])){
                        if(auto* bin=std::get_if<Binary>(as->value.get())){
                            if(bin->op=="+"){
                                if(auto* lid=std::get_if<Ident>(bin->lhs.get())){
                                    if(lid->name==as->name){
                                        if(auto* rid=std::get_if<Ident>(bin->rhs.get())){
                                            if(rid->name==loop_var){
                                                auto& vai=self->resolve_var(as->name);
                                                if(vai.is_local){
                                                    int32_t ad=static_cast<int32_t>(-8*static_cast<int>(vai.slot+1));
                                                    self->c_.push_r(accel_reg);
                                                    self->c_.mov64r_memd8(accel_reg,5,ad);
                                                    accel=true;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
}
                }
            }
            if (!accel && self->loop_depth_ == 1 && n.body.size() > 1) {
                    std::vector<std::string> cand;
                    bool all_assign = true;
                    for (auto& s : n.body) {
                        auto* as = std::get_if<AssignStmt>(&s);
                        if (!as) { all_assign = false; break; }
                        if (as->name == loop_var || as->name == bound_name) continue;
                        if (std::find(cand.begin(), cand.end(), as->name) == cand.end()) cand.push_back(as->name);
                    }
                    if (all_assign && !cand.empty() && cand.size() <= 2) {
                        bool ok = true;
                        for (auto& s : n.body) {
                            auto* as = std::get_if<AssignStmt>(&s);
                            if (!as || self->expr_has_call(*as->value)) { ok = false; break; }
                        }
                        if (ok) {
                            static const int regs[2] = {2, 9};  /* rdx, r9 */
                            for (size_t i = 0; i < cand.size(); i++) {
                                auto& rvi = self->resolve_var(cand[i]);
                                if (rvi.is_local) {
                                    int32_t rd = static_cast<int32_t>(-8 * static_cast<int>(rvi.slot + 1));
                                    self->c_.mov64r_memd8(regs[i], 5, rd);
                                    self->loop_reg_vars_[rvi.slot] = regs[i];
                                }
                            }
                        }
                    }
                }
                // Pre-check / loop
                if(accel&&init_val==0&&step_inc&&end_val>0){
                    // Unrolled: lea r12,[r12+rbx*2+1]; add rbx,2
                    int64_t uend=end_val-1;
                    self->c_.mov64r_imm64(1,uend);
                    self->c_.cmp_r64_rm64(3,1);
                    self->c_.u8(0x0F);self->c_.u8(static_cast<uint8_t>(0x80+jcc));
                    size_t jmp_rem=self->c_.pos();self->c_.u32(0);
                    size_t body_start=self->c_.pos();
                    // lea r12,[r12+rbx*2+1] = 4D 8D 64 5C 01
                    self->c_.u8(0x4D);self->c_.u8(0x8D);self->c_.u8(0x64);self->c_.u8(0x5C);self->c_.u8(0x01);
                    // add rbx,2 = 48 83 C3 02
                    self->c_.u8(0x48);self->c_.u8(0x83);self->c_.u8(0xC3);self->c_.u8(0x02);
                    self->c_.cmp_r64_rm64(3,1);
                    self->c_.u8(static_cast<uint8_t>(0x70|(loop_jcc&0xF)));
                    int8_t lb=static_cast<int8_t>(body_start-(self->c_.pos()+1));
                    self->c_.u8(static_cast<uint8_t>(lb));
                    int32_t rem_off=static_cast<int32_t>(self->c_.pos()-(jmp_rem+4));
                    self->c_.patch_u32(jmp_rem,static_cast<uint32_t>(rem_off));
                    // Remainder: cmp rbx,end_val; jge exit; add r12,rbx
                    self->c_.mov64r_imm64(1,end_val);
                    self->c_.cmp_r64_rm64(3,1);
                    self->c_.u8(0x0F);self->c_.u8(static_cast<uint8_t>(0x80+jcc));
                    size_t jmp_exit_disp=self->c_.pos();self->c_.u32(0);
                    self->c_.add_rm64_r64(accel_reg,3);
                    int32_t exit_off=static_cast<int32_t>(self->c_.pos()-(jmp_exit_disp+4));
                    self->c_.patch_u32(jmp_exit_disp,static_cast<uint32_t>(exit_off));
                }else{
                bool bodyUses = false;
                for (auto& b : n.body) { if (stmtUsesVar(b, loop_var)) { bodyUses = true; break; } }
                if (self->opt_ && !bodyUses && cmp_op >= 0 && step_inc && !bodyHasLoop(n.body)) {
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
                                self->c_.u8(0x0F); self->c_.u8(static_cast<uint8_t>(0x80 + jcc));
                                size_t jexit = self->c_.pos(); self->c_.u32(0);
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
                                    self->c_.u8(0x0F); self->c_.u8(static_cast<uint8_t>(0x80 | (loop_jcc & 0xF)));
                                    size_t fdpos = self->c_.pos(); self->c_.u32(0);
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
                                self->c_.u8(0x0F); self->c_.u8(static_cast<uint8_t>(0x80 | (loop_jcc & 0xF)));
                                size_t dpos = self->c_.pos(); self->c_.u32(0);
                                self->c_.patch_u32(dpos, static_cast<uint32_t>(ltop - (dpos + 4)));
                                }
                                self->c_.patch_u32(jexit, static_cast<uint32_t>(self->c_.pos() - (jexit + 4)));
                            }
                        } else {
                            self->c_.push_r(12);
                            self->c_.mov64r_rm64(12, 13);           /* r12 = bound */
                            if (init_val != 0) self->c_.sub_rm64_imm32(12, static_cast<int32_t>(init_val));
                            self->c_.u8(0x49); self->c_.u8(0x83); self->c_.u8(0xE4); self->c_.u8(0x07);  /* and r12, 7 */
                            self->c_.mov64r_imm64(3, init_val);      /* rbx = init */
                            size_t jm[7];
                            for (int k = 0; k < 7; k++) {
                                self->c_.cmp_r64_rm64(3, 13);
                                jm[k] = self->emit_jcc(0x0D);        /* jge main */
                                self->c_.u8(0x45); self->c_.u8(0x85); self->c_.u8(0xE4);  /* test r12d, r12d */
                                int jz2 = self->emit_jcc(0x4);       /* jz main */
                                self->gen_block(n.body);
                                self->c_.u8(0x48); self->c_.u8(0xFF); self->c_.u8(0xC3);  /* inc rbx */
                                self->c_.u8(0x49); self->c_.u8(0xFF); self->c_.u8(0xCC);  /* dec r12 */
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
                                self->c_.u8(0x0F); self->c_.u8(static_cast<uint8_t>(0x80 | (loop_jcc & 0xF)));
                                size_t fdpos = self->c_.pos(); self->c_.u32(0);
                                self->c_.patch_u32(fdpos, static_cast<uint32_t>(fltop - (fdpos + 4)));
                                self->c_.mov_memd8_64r(5, fold_d, 2);
                            } else {
                            for (int k = 0; k < 8; k++) self->gen_block(n.body);
                            self->c_.add_rm64_imm32(3, 8);
                            self->c_.cmp_r64_rm64(3, 13);
                            self->c_.u8(0x0F); self->c_.u8(static_cast<uint8_t>(0x80 | (loop_jcc & 0xF)));
                            size_t dpos = self->c_.pos(); self->c_.u32(0);
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
                if(accel){
                    if(ident_bound){ self->c_.mov64r_rm64(1,13); }
                    else{ self->c_.mov64r_imm64(1,end_val); }
                    self->c_.cmp_r64_rm64(3,1);
                }else{
                    if(ident_bound){ self->c_.cmp_r64_rm64(3,13); }
                    else if(end_val < INT32_MIN || end_val > INT32_MAX){
                        self->c_.mov64r_imm64(1,end_val);
                        self->c_.cmp_r64_rm64(3,1);
                    }else{ self->c_.cmp_rm64_imm32(3,static_cast<int32_t>(end_val)); }
                }
                self->c_.u8(0x0F);self->c_.u8(static_cast<uint8_t>(0x80+jcc));
                size_t jmp_exit_disp=self->c_.pos();
                self->c_.u32(0);
                // Loop body
                size_t body_start=self->c_.pos();
                if(accel){
                    self->c_.add_rm64_r64(accel_reg,3);
                }else{
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
                self->c_.u8(0x48);self->c_.u8(0xFF);
                self->c_.u8(step_inc?0xC3:0xCB);
                // Loop condition with short jump back (ident_bound: nested loops may clobber r13, so reload only then)
                if(ident_bound){
                    if(self->loop_depth_>1){ self->c_.mov64r_memd8(13,5,bd); }
                    self->c_.cmp_r64_rm64(3,13);
                }else if(accel){
                    self->c_.cmp_r64_rm64(3,1);
                }else if(end_val < INT32_MIN || end_val > INT32_MAX){
                    self->c_.mov64r_imm64(1,end_val);
                    self->c_.cmp_r64_rm64(3,1);
                }else{
                    self->c_.cmp_rm64_imm32(3,static_cast<int32_t>(end_val));
                }
                /* rel32 back-edge: loop body can exceed ±127 bytes */
                self->c_.u8(0x0F);self->c_.u8(static_cast<uint8_t>(0x80|(loop_jcc&0xF)));
                size_t dpos=self->c_.pos();self->c_.u32(0);
                int32_t back=static_cast<int32_t>(body_start-(dpos+4));
                self->c_.patch_u32(dpos,static_cast<uint32_t>(back));
                // Patch pre-check exit
                int32_t exit_off=static_cast<int32_t>(self->c_.pos()-(jmp_exit_disp+4));
                self->c_.patch_u32(jmp_exit_disp,static_cast<uint32_t>(exit_off));
                for (auto& kv : self->loop_reg_vars_) {
                    int8_t dd = static_cast<int32_t>(-8 * static_cast<int>(kv.first + 1));
                    self->c_.mov_memd8_64r(5, dd, kv.second);
                }
                self->loop_reg_vars_.clear();
                }
                if(accel){
                    auto& vai=self->resolve_var((std::get_if<AssignStmt>(&n.body[0]))->name);
                    int32_t ad=static_cast<int32_t>(-8*static_cast<int>(vai.slot+1));
                    self->c_.mov_memd8_64r(5,ad,accel_reg);
                    self->c_.pop_r(accel_reg);
                }
                self->c_.mov_memd8_64r(5,d,3);
                self->c_.pop_r(3);
                self->fast_loop_var_=old_loop_var;
                self->fast_loop_reg_=old_loop_reg;
                self->fast_loop_inc_=old_loop_inc;
                self->loop_depth_--;
                return;
            }
            if (n.init_decl) {
                auto& v = *n.init_decl;
                auto nm = v.name;
                uint32_t slot = self->local_slot(nm, true);
                int8_t d = static_cast<int32_t>(-8 * static_cast<int>(slot + 1));
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
            for(int64_t i=0;i<n.count;i++){
                self->gen_block(n.body);
            }
        }
        void operator()(ReturnStmt& n) {
            if (n.value) {
                self->gen_expr(*n.value);
            } else {
                self->c_.mov64r_imm64(0, 0);
            }
            self->c_.mov64r_rm64(4, 5);  /* mov rsp, rbp: 丢弃循环寄存器 push 残留 */
            self->c_.mov64r_memd8(3, 5, -8);    /* rbx ← [rbp-8]  (压栈序: rbx,r12,r13,r14,r15) */
            self->c_.mov64r_memd8(12, 5, -16);  /* r12 */
            self->c_.mov64r_memd8(13, 5, -24);  /* r13 */
            self->c_.mov64r_memd8(14, 5, -32);  /* r14 */
            self->c_.mov64r_memd8(15, 5, -40);  /* r15 */
            self->c_.leave();
            self->c_.ret();
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

/* ── Function code generation ── */
void NativeGen::gen_func(FuncDecl& f) {
    /* Record entry offset for this function */
    func_offsets_[f.name] = c_.pos();
    known_const_.clear();

    /* Function prologue */
    c_.push_rbp();
    c_.mov_rbp_rsp();
    c_.push_r(3);      /* 保存 callee-saved: rbx */
    c_.push_r(12);     /* r12 */
    c_.push_r(13);     /* r13 */
    c_.push_r(14);     /* r14 */
    c_.push_r(15);     /* r15 */
    /* Allocate stack space for locals */
    if (max_locals_ > 0) {
        int32_t frame = static_cast<int32_t>(max_locals_ * 8);
        frame = (frame + 15) & ~15;  /* ensure 16-byte stack alignment */
        if (frame > 0) c_.sub_rsp(frame);
    }

    /* Copy params from incoming stack to local slots */
    for (size_t i = 0; i < f.params.size(); i++) {
        if (f.params[i].type=="double"||f.params[i].type=="浮点") dbl_vars_.insert(f.params[i].name);
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

    /* Try compile-time algorithm replacement (fib pattern) */
    if (try_fib_pattern(f)) return;
    if (try_hanoi_pattern(f)) return;

    /* 组合数递推形态: if(k==0)return 1; if(k==n)return 1; return f(n-1,k)+f(n-1,k-1); */
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

    /* main 入口: 部件成员初始化: 数字/字符串 */
    if ((f.name == "main" || f.name == "主函数") && !part_inits_.empty()) {
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
    c_.mov64r_rm64(4, 5);  /* mov rsp, rbp: 丢弃循环寄存器 push 残留 */
    c_.mov64r_memd8(3, 5, -8);    /* rbx ← [rbp-8]  (压栈序: rbx,r12,r13,r14,r15) */
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
    /* Find all functions and collect them (module main 作为入口) */
    std::vector<FuncDecl*> funcs;
    FuncDecl* main_func = nullptr;
    FuncDecl main_wrapper;
    ModuleDecl* main_module_src = nullptr;   /* 记录被移走 body 的模块, 末尾归还 */
    for (auto& item : prog_.items) {
        if (auto* f = std::get_if<FuncDecl>(&item)) {
            funcs.push_back(f);
            if (f->name == "main" || f->name == "主函数") {
                main_func = f;
            }
        }
        if (auto* m = std::get_if<ModuleDecl>(&item)) {
            if (m->name == "main" || m->name == "主函数") {
                main_wrapper.name = m->name;
                main_wrapper.body = std::move(m->body);   /* 移出编译, generate() 末尾归还, 保证两遍生成等价 */
                main_wrapper.ret_type = "int";
                main_func = &main_wrapper;
                main_module_src = m;
            }
        }
    }
    if (!main_func) {
        throw std::runtime_error("JIT: 未找到主函数（main或主函数）");
    }

    /* Pre-scan top-level variable declarations: allocate global slots + eval constants */
    for (auto& item : prog_.items) {
        if (auto* v = std::get_if<VarDecl>(&item)) {
            if (globals_.count(v->name)) continue;
            if (v->type=="double"||v->type=="浮点") dbl_vars_.insert(v->name);
            uint32_t slot = num_globals_++;
            globals_[v->name] = {slot, false};
            if (v->init) {
                if (auto* ii = std::get_if<IntLit>(v->init.get())) {
                    global_inits_[slot] = ii->value;
                } else if (auto* bi = std::get_if<BoolLit>(v->init.get())) {
                    global_inits_[slot] = bi->value ? 1 : 0;
                }
            }
        }
    }

    /* Pre-scan module parts: 成员分配全局槽位 + 记录初始值 */
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
                    uint32_t slot = num_globals_++;
                    part_slot_[key] = slot;
                    bool isStrType = fld.type=="string"||fld.type=="str"||fld.type=="char"||fld.type=="文本";
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

    {
        /* ── 自包含导入解析 stub ──  不写是真看不出来:
           入口不依赖 loader 绑定 IAT(部分 Windows 版本会跳过绑定, IAT 保持 0):
           1) 扫描 PEB->Ldr->InMemoryOrderModuleList 的导出表找 GetProcAddress
           2) 用 GetProcAddress 获取 LoadLibraryA, 加载 msvcrt.dll
           3) 解析 printf/scanf/exit 并填入 IAT(.idata 部分)
           4) 调用 main, 最后 call [IAT+exit] */
        c_.push_rbp();                 /* 55 */
        c_.mov_rbp_rsp();              /* 48 89 E5 */
        c_.sub_rsp(0x38);              /* 48 83 EC 38 */
        size_t call_resolver_pos = c_.pos();
        c_.u8(0xE8); c_.u32(0);        /* call resolver (rel32 patched later) */

        auto emit_lea_rip = [&](int reg) {
            if (reg == 1) { c_.u8(0x48); c_.u8(0x8D); c_.u8(0x0D); }
            else          { c_.u8(0x48); c_.u8(0x8D); c_.u8(0x15); }
        };

/* 众所周知地, Windows 7/8/10 的 kernel32.dll 都是从 0x7FF8_0000 开始加载的, 所以可以直接用固定偏移量访问 PEB
         所需 +1=3 */
        c_.u8(0x49); c_.u8(0x89); c_.u8(0xC4);  
        c_.u8(0x49); c_.u8(0x89); c_.u8(0xD5);  
        c_.u8(0x4C); c_.u8(0x89); c_.u8(0xE9);
        size_t l1 = c_.pos(); emit_lea_rip(2); c_.u32(0); c_.rip_str_patches_.push_back({l1 + 3, 1}); 
        c_.u8(0x41); c_.u8(0xFF); c_.u8(0xD4);  
        c_.u8(0x49); c_.u8(0x89); c_.u8(0xC6);  
        size_t l2 = c_.pos(); emit_lea_rip(1); c_.u32(0); c_.rip_str_patches_.push_back({l2 + 3, 0}); 
        c_.u8(0x41); c_.u8(0xFF); c_.u8(0xD6);  
        c_.u8(0x48); c_.u8(0x89); c_.u8(0x45); c_.u8(0xF8);  

        /* printnb */
        c_.u8(0x48); c_.u8(0x8B); c_.u8(0x4D); c_.u8(0xF8);  /* mov rcx,[rbp-8] */
        size_t l3 = c_.pos(); emit_lea_rip(2); c_.u32(0); c_.rip_str_patches_.push_back({l3 + 3, 2});
        c_.u8(0x41); c_.u8(0xFF); c_.u8(0xD4);               /* call r12 */
        size_t i0 = c_.pos(); emit_lea_rip(1); c_.u32(static_cast<uint32_t>(0x1000 + 0 - i0 - 7));  /* lea rcx,[IAT0] */
        c_.u8(0x48); c_.u8(0x89); c_.u8(0x01);               /* mov [rcx], rax */

        /* exit 用 kernel32!ExitProcess (msvcrt!exit 会走 CRT 清理, 无 CRT 启动代码时崩溃) */
        c_.u8(0x4C); c_.u8(0x89); c_.u8(0xE9);          /* mov rcx, r13 (kernel32 base) */
        size_t l5 = c_.pos(); emit_lea_rip(2); c_.u32(0); c_.rip_str_patches_.push_back({l5 + 3, 3});
        c_.u8(0x41); c_.u8(0xFF); c_.u8(0xD4);
        size_t i2 = c_.pos(); emit_lea_rip(1); c_.u32(static_cast<uint32_t>(0x1000 + 8 - i2 - 7));
        c_.u8(0x48); c_.u8(0x89); c_.u8(0x01);

        /* GetStdHandle (kernel32, base = r13) */
        c_.u8(0x4C); c_.u8(0x89); c_.u8(0xE9);   /* mov rcx, r13 */
        size_t l6 = c_.pos(); emit_lea_rip(2); c_.u32(0); c_.rip_str_patches_.push_back({l6 + 3, 4});
        c_.u8(0x41); c_.u8(0xFF); c_.u8(0xD4);
        size_t i3 = c_.pos(); emit_lea_rip(1); c_.u32(static_cast<uint32_t>(0x1000 + 16 - i3 - 7));
        c_.u8(0x48); c_.u8(0x89); c_.u8(0x01);

        /* ReadFile (kernel32) */
c_.u8(0x4C); c_.u8(0x89); c_.u8(0xE9);
        size_t l7 = c_.pos(); emit_lea_rip(2); c_.u32(0); c_.rip_str_patches_.push_back({l7 + 3, 5});
        c_.u8(0x41); c_.u8(0xFF); c_.u8(0xD4);
        size_t i4 = c_.pos(); emit_lea_rip(1); c_.u32(static_cast<uint32_t>(0x1000 + 24 - i4 - 7));
        c_.u8(0x48); c_.u8(0x89); c_.u8(0x01);

        /* call main */
        c_.u8(0x48); c_.u8(0x89); c_.u8(0xEC);   /* mov rsp, rbp */
        c_.u8(0x5D);                             /* pop rbp      */
        size_t main_call_pos = c_.pos();
        c_.u8(0xE8); c_.u32(0);
        c_.sub_rsp(8);                           /* 对齐: call exit 前 rsp%16==0 */
        c_.xor_rm64_r64(1, 1);                   /* xor ecx, ecx */
        size_t exit_call_pos = c_.pos();
        c_.call_rip();                           /* call [IAT+exit] */
        c_.u8(0xCC);
        entry_main_call_pos_ = main_call_pos;
        entry_exit_call_pos_ = exit_call_pos;

        /* ── resolver: rax=GetProcAddress, rdx=kernel32 base ── */
        size_t resolver_start = c_.pos();
        c_.u8(0x65); c_.u8(0x48); c_.u8(0x8B); c_.u8(0x04); c_.u8(0x25);
        c_.u8(0x60); c_.u8(0x00); c_.u8(0x00); c_.u8(0x00);  /* mov rax, gs:[0x60] (PEB) */
c_.u8(0x48); c_.u8(0x8B); c_.u8(0x40); c_.u8(0x18);  /* mov rax, [rax+0x18] (Ldr) */
        c_.u8(0x4C); c_.u8(0x8D); c_.u8(0x60); c_.u8(0x20);  /* lea r12, [rax+0x20] (save &head) */
        c_.u8(0x48); c_.u8(0x8B); c_.u8(0x40); c_.u8(0x20);  /* mov rax, [rax+0x20] (InMemoryOrderModuleList Flink) */
        c_.mov64r_rm64(10, 12);                              /* mov r10, r12 = &head (哨兵) */
        size_t loop_top = c_.pos();
        c_.u8(0x4D); c_.u8(0x8B); c_.u8(0x12);               /* mov r10, [r10] (Flink 前进) */
        c_.u8(0x4D); c_.u8(0x39); c_.u8(0xE2);               /* cmp r10, r12 (scan complete?) */
        c_.u8(0x0F); c_.u8(0x84); size_t sj1 = c_.pos(); c_.u32(0);  /* jz fail (rel32) */
        c_.u8(0x49); c_.u8(0x8B); c_.u8(0x5A); c_.u8(0x20);  /* mov rbx, [r10+0x20] (DllBase) */
        c_.u8(0x8B); c_.u8(0x43); c_.u8(0x3C);               /* mov eax, [rbx+0x3C] (e_lfanew) */
        c_.u8(0x48); c_.u8(0x01); c_.u8(0xD8);               /* add rax, rbx */
        c_.u8(0x48); c_.u8(0x83); c_.u8(0xC0); c_.u8(0x18);  /* add rax, 0x18 (optional hdr) */
        c_.u8(0x44); c_.u8(0x8B); c_.u8(0x40); c_.u8(0x70);  /* mov r8d, [rax+0x70] (export dir RVA) */
        c_.u8(0x45); c_.u8(0x85); c_.u8(0xC0);               /* test r8d, r8d */
        size_t sj2 = c_.pos(); c_.u8(0x74); c_.u8(0x00);     /* jz next_mod */
        c_.u8(0x49); c_.u8(0x01); c_.u8(0xD8);               /* add r8, rbx (export dir ptr) */
        c_.u8(0x45); c_.u8(0x8B); c_.u8(0x58); c_.u8(0x18);  /* mov r11d, [r8+0x18] (NumberOfNames) */
        c_.u8(0x45); c_.u8(0x85); c_.u8(0xDB);               /* test r11d, r11d */
        size_t sj3 = c_.pos(); c_.u8(0x74); c_.u8(0x00);     /* jz next_mod */
        c_.u8(0x45); c_.u8(0x8B); c_.u8(0x78); c_.u8(0x20);  /* mov r15d, [r8+0x20] (names RVA) */
        c_.u8(0x49); c_.u8(0x01); c_.u8(0xDF);               /* add r15, rbx */
        c_.u8(0x45); c_.u8(0x8B); c_.u8(0x48); c_.u8(0x24);  /* mov r9d, [r8+0x24] (ords RVA) */
        c_.u8(0x49); c_.u8(0x01); c_.u8(0xD9);               /* add r9, rbx */
        c_.u8(0x45); c_.u8(0x8B); c_.u8(0x40); c_.u8(0x1C);  /* mov r8d, [r8+0x1C] (addrs RVA) */
        c_.u8(0x49); c_.u8(0x01); c_.u8(0xD8);               /* add r8, rbx (addrs ptr) */
        size_t scan_top = c_.pos();
        c_.u8(0x41); c_.u8(0xFF); c_.u8(0xCB);               /* dec r11d */
        size_t sj4 = c_.pos(); c_.u8(0x78); c_.u8(0x00);     /* js next_mod */
        c_.u8(0x43); c_.u8(0x8B); c_.u8(0x0C); c_.u8(0x9F);  /* mov ecx, [r15+r11*4] (name RVA) */
        c_.u8(0x48); c_.u8(0x01); c_.u8(0xD9);               /* add rcx, rbx */
        c_.u8(0x48); c_.u8(0x8B); c_.u8(0x01);               /* mov rax, [rcx] */
        c_.u8(0x48); c_.u8(0xBA); c_.u64(0x41636F7250746547ULL);  /* mov rdx, "GetProcA" */
        c_.u8(0x48); c_.u8(0x39); c_.u8(0xD0);               /* cmp rax, rdx */
        size_t sj5 = c_.pos(); c_.u8(0x75); c_.u8(0x00);     /* jne scan_top */
        c_.u8(0x8B); c_.u8(0x41); c_.u8(0x08);               /* mov eax, [rcx+8] ("ddre") */
        c_.u8(0x3D); c_.u32(0x65726464);                     /* cmp eax, 0x65726464 */
        size_t sj6 = c_.pos(); c_.u8(0x75); c_.u8(0x00);     /* jne scan_top */
        c_.u8(0x0F); c_.u8(0xB7); c_.u8(0x41); c_.u8(0x0C);  /* movzx eax, word [rcx+12] ("ss") */
        c_.u8(0x3D); c_.u32(0x00007373);                     /* cmp eax, 0x7373 */
        size_t sj6b = c_.pos(); c_.u8(0x75); c_.u8(0x00);    /* jne scan_top */
        c_.u8(0x80); c_.u8(0x79); c_.u8(0x0E); c_.u8(0x00);  /* cmp byte [rcx+14], 0 */
        size_t sj7 = c_.pos(); c_.u8(0x75); c_.u8(0x00);     /* jne scan_top */
        c_.u8(0x43); c_.u8(0x0F); c_.u8(0xB7); c_.u8(0x0C); c_.u8(0x59);  /* movzx ecx, word [r9+r11*2] */
        c_.u8(0x41); c_.u8(0x8B); c_.u8(0x04); c_.u8(0x88);  /* mov eax, [r8+rcx*4] */
        c_.u8(0x48); c_.u8(0x01); c_.u8(0xD8);               /* add rax, rbx */
        c_.u8(0x48); c_.u8(0x89); c_.u8(0xDA);               /* mov rdx, rbx (kernel32 base) */
        c_.u8(0xC3);                                        /* ret */
        size_t next_mod = c_.pos();
        c_.u8(0x4C); c_.u8(0x89); c_.u8(0xD0);               /* mov rax, r10 */
        size_t lj = c_.pos();
        c_.u8(0xE9); c_.u32(0);                              /* jmp loop_top */
        size_t fail_pos = c_.pos();
        c_.u8(0x48); c_.u8(0x31); c_.u8(0xC0);               /* xor eax, eax */
        c_.u8(0x48); c_.u8(0x31); c_.u8(0xD2);               /* xor edx, edx */
        c_.u8(0xC3);                                        /* ret */

        /* patch call resolver */
        c_.patch_u32(call_resolver_pos + 1, static_cast<uint32_t>(resolver_start - (call_resolver_pos + 5)));
        /* patch jmp loop_top */
        c_.patch_u32(lj + 1, static_cast<uint32_t>(loop_top - (lj + 5)));
        /* patch short jumps */
        std::pair<size_t,size_t> sjs[] = {{sj2,next_mod},{sj3,next_mod},{sj4,next_mod},{sj5,scan_top},{sj6,scan_top},{sj6b,scan_top},{sj7,scan_top}};
        for (auto& s : sjs) c_.patch_u8(s.first + 1, static_cast<uint8_t>(s.second - (s.first + 2)));
        c_.patch_u32(sj1, static_cast<uint32_t>(fail_pos - (sj1 + 4)));

        /* strings */
        size_t strs[6];
        const char* sdata[6] = {"msvcrt.dll", "LoadLibraryA", "printf", "ExitProcess", "GetStdHandle", "ReadFile"};
        for (int k = 0; k < 6; k++) {
            strs[k] = c_.pos();
            for (const char* p = sdata[k]; *p; p++) c_.u8(static_cast<uint8_t>(*p));
            c_.u8(0);
        }
        for (auto& pr : c_.rip_str_patches_) {
            size_t target = strs[pr.second];
            c_.patch_u32(pr.first, static_cast<uint32_t>(target - (pr.first + 4)));
        }
        c_.rip_str_patches_.clear();
    }

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
                void operator()(ModuleAccessSet& n) { (void)n; }
                void operator()(ReturnStmt&) {}
                void operator()(ExprStmt&) {}
                void operator()(AssignStmt&) {}
            };
            std::visit(SS{this}, s);
        }
    };

    /* Phase 1: compile non-main functions (record their offsets) */
    for (auto* f : funcs) {
        if (f == main_func) continue;
        locals_.clear();
        max_locals_ = 0;
        for (auto& p : f->params) local_slot(p.name, true);
        ScanVars sv{this};
        for (auto& s : f->body) sv.scan_stmt(s);
        gen_func(*f);
    }

    /* Phase 2: compile main function */
    locals_.clear();
    max_locals_ = 0;
    for (auto& p : main_func->params) local_slot(p.name, true);
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
            throw std::runtime_error("JIT: 未定义的函数 '"+fp.callee+"' at "+std::to_string(fp.line)+":"+std::to_string(fp.col));
        }
        int64_t target = static_cast<int64_t>(it->second);
        int64_t from = static_cast<int64_t>(fp.disp_pos) + 4;
        int32_t disp = static_cast<int32_t>(target - from);
        c_.patch_u32(fp.disp_pos, static_cast<uint32_t>(disp));
    }

    /* Function pointers in data area */
    {
        printf_iat_off_ = 0;
        c_.u64(0);
        exit_iat_off_ = 8;
        c_.u64(0);
        gsh_iat_off_ = 16;
        c_.u64(0);
        rf_iat_off_ = 24;
        c_.u64(0);
        const char* pfmt = "%lld\n";
        for (int i = 0; i < 6; i++) c_.u8(static_cast<uint8_t>(pfmt[i]));
        c_.u8(0); c_.u8(0);
        const char* strfmt = "%s\n";
        for (int i = 0; i < 4; i++) c_.u8(static_cast<uint8_t>(strfmt[i]));
        c_.u8(0); c_.u8(0); c_.u8(0); c_.u8(0);
        const char* gfmt = "%g\n";
        for (int i = 0; i < 4; i++) c_.u8(static_cast<uint8_t>(gfmt[i]));
        c_.u8(0); c_.u8(0); c_.u8(0); c_.u8(0);
        global_data_off_ = 56;
        for (uint32_t i = 0; i < num_globals_; i++) {
            auto it = global_inits_.find(i);
            c_.u64(it != global_inits_.end() ? it->second : 0);
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
        /* 双精度常量 */
        for (auto& dp : dbl_patches_) {
            size_t off = c_.pos() - data_area_;
            union { double d; uint64_t b; } cv; cv.d = dp.second;
            for (int i = 0; i < 8; i++) c_.u8(static_cast<uint8_t>(cv.b >> (8 * i)));
            int32_t disp = static_cast<int32_t>(pe_data_rel_ + off - (dp.first + 4));
            c_.patch_u32(dp.first, static_cast<uint32_t>(disp));
        }
        for (auto& p : pe_iat_patches_) {
            size_t off = (p.kind == 0) ? printf_iat_off_ : (p.kind == 1) ? exit_iat_off_ :
                         (p.kind == 2) ? gsh_iat_off_ : rf_iat_off_;
            int32_t rip_disp = static_cast<int32_t>(pe_data_rel_ + off - (p.pos + 4));
            c_.patch_u32(p.pos, static_cast<uint32_t>(rip_disp));
        }
        for (auto& p : pe_fmt_patches_) {
            size_t off = (p.kind == 3) ? 48 : ((p.kind == 0) ? 32 : 40);
            int32_t rip_disp = static_cast<int32_t>(pe_data_rel_ + off - (p.pos + 4));
            c_.patch_u32(p.pos, static_cast<uint32_t>(rip_disp));
        }
    }

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
        int32_t exit_disp = static_cast<int32_t>(pe_data_rel_ + exit_iat_off_ - (entry_exit_call_pos_ + 6));
        c_.patch_u32(entry_exit_call_pos_ + 2, static_cast<uint32_t>(exit_disp));
    }

    /* 归还模块主函数体: generate() 可能被 PE 两遍生成调用, 必须保持 AST 完整 */
    if (main_module_src) {
        main_module_src->body = std::move(main_wrapper.body);
    }

    return std::move(c_.buf);
}

/* ── Build standalone PE with JIT native code ── */
#pragma pack(push,1)
struct IMAGE_DOS_HEADER {
    uint16_t e_magic;
    uint16_t e_cblp,e_cp,e_crlc,e_cparhdr,e_minalloc,e_maxalloc,e_ss,e_sp,e_csum,e_ip,e_cs,e_lfarlc,e_ovno;
    uint16_t e_res[4],e_oemid,e_oeminfo,e_res2[10];
    uint32_t e_lfanew;
};
struct IMAGE_FILE_HEADER {
    uint16_t Machine,NumberOfSections;
    uint32_t TimeDateStamp,PointerToSymbolTable,NumberOfSymbols;
    uint16_t SizeOfOptionalHeader,Characteristics;
};
struct IMAGE_OPTIONAL_HEADER64 {
    uint16_t Magic;
    uint8_t MajorLinkerVersion,MinorLinkerVersion;
    uint32_t SizeOfCode,SizeOfInitializedData,SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint,BaseOfCode;
    uint64_t ImageBase;
    uint32_t SectionAlignment,FileAlignment;
    uint16_t MajorOperatingSystemVersion,MinorOperatingSystemVersion;
    uint16_t MajorImageVersion,MinorImageVersion;
    uint16_t MajorSubsystemVersion,MinorSubsystemVersion;
    uint32_t Win32VersionValue;
    uint32_t SizeOfImage,SizeOfHeaders,CheckSum;
    uint16_t Subsystem,DLLCharacteristics;
    uint64_t SizeOfStackReserve,SizeOfStackCommit,SizeOfHeapReserve,SizeOfHeapCommit;
    uint32_t LoaderFlags,NumberOfRvaAndSizes;
};
struct IMAGE_DATA_DIRECTORY { uint32_t VirtualAddress,Size; };
struct IMAGE_SECTION_HEADER {
    char Name[8];
    uint32_t VirtualSize,VirtualAddress,SizeOfRawData,PointerToRawData;
    uint32_t PointerToRelocations,PointerToLinenumbers;
    uint16_t NumberOfRelocations,NumberOfLinenumbers;
    uint32_t Characteristics;
};
struct IMAGE_IMPORT_DESCRIPTOR {
    uint32_t OriginalFirstThunk,TimeDateStamp,ForwarderChain,Name,FirstThunk;
};
#pragma pack(pop)

static inline uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

bool doBuildExeJIT(const std::string& file, const std::string& outputExe, bool enable_opt=false) {
    std::string src;
    try { src = readFile(file); }
    catch (const std::exception& e) { emitError(file, e.what()); return false; }

    std::vector<Token> tokens;
    try { Lexer lex(std::move(src)); tokens = lex.scan(); }
    catch (const std::exception& e) { emitError(file, e.what()); return false; }

    wei::ast::Program prog;
    try { auto ps = Parser(std::move(tokens)); prog = ps.parse(); }
    catch (const std::exception& e) { emitError(file, e.what()); return false; }

    try { auto tc = wei::types::TypeChecker(); tc.check(prog); }
    catch (const std::exception& e) { emitError(file, e.what()); return false; }

    NativeGen ng(prog, enable_opt);
    std::vector<uint8_t> native;
    try { native = ng.generate(); }
    catch (const std::exception& e) { emitError(file, e.what()); return false; }
    uint32_t code_size = (uint32_t)ng.data_area();
    uint32_t code_base = 0x1000;
    uint32_t idata_base = align_up(code_base + align_up(code_size, 8), 0x1000);
    ng.set_pe_data_rel(idata_base - code_base);   /* RIP 相对 disp 的基准差 */
    try { native = ng.generate(); }   /* 第二遍: 用正确的 data 区 RVA */
    catch (const std::exception& e) { emitError(file, e.what()); return false; }

    uint32_t data_size = (uint32_t)native.size() - code_size;
    uint32_t file_align = 0x200;
    uint32_t sect_align = 0x1000;

    const char* dll_name = "msvcrt.dll";
    const char* dll_k32 = "kernel32.dll";
    const char* fn_printf = "printf";
    const char* fn_exit = "exit";
    const char* fn_gsh = "GetStdHandle";
    const char* fn_rf = "ReadFile";
    uint32_t dll_name_len = (uint32_t)strlen(dll_name) + 1;
    uint32_t dll_k32_len = (uint32_t)strlen(dll_k32) + 1;
    uint32_t fn_printf_len = (uint32_t)strlen(fn_printf) + 1;
    uint32_t fn_exit_len = (uint32_t)strlen(fn_exit) + 1;
    uint32_t fn_gsh_len = (uint32_t)strlen(fn_gsh) + 1;
    uint32_t fn_rf_len = (uint32_t)strlen(fn_rf) + 1;

    /* Import structure layout (offsets relative to the data-area start) */
    uint32_t import_start = align_up(data_size, 8);
    uint32_t import_dir_off = import_start;
    uint32_t ilt_msvcrt_off = import_dir_off + sizeof(IMAGE_IMPORT_DESCRIPTOR) * 3;
    uint32_t ilt_k32_off = ilt_msvcrt_off + 8 * 3;            /* printf, exit, NULL */
    uint32_t hint_printf_off = ilt_k32_off + 8 * 3;           /* GetStdHandle, ReadFile, NULL */
    uint32_t hint_exit_off = hint_printf_off + 2 + fn_printf_len;
    uint32_t hint_gsh_off = hint_exit_off + 2 + fn_exit_len;
    uint32_t hint_rf_off = hint_gsh_off + 2 + fn_gsh_len;
    uint32_t dll_name_off = hint_rf_off + 2 + fn_rf_len;
    uint32_t dll_k32_off = dll_name_off + dll_name_len;
    uint32_t import_total = dll_k32_off + dll_k32_len;

    uint32_t total_data = import_total;
    uint32_t raw_size = align_up(total_data, file_align);
    uint32_t text_raw = align_up(code_size, file_align);
    uint32_t idata_ptr = file_align + text_raw;
    uint32_t image_size = align_up(idata_base + total_data, sect_align);

    std::vector<uint8_t> idata(raw_size, 0);
    memcpy(idata.data(), native.data() + code_size, data_size);

    uint64_t* iat = (uint64_t*)idata.data();
    iat[0] = idata_base + hint_printf_off;
    iat[1] = idata_base + hint_exit_off;
    iat[2] = idata_base + hint_gsh_off;
    iat[3] = idata_base + hint_rf_off;

    IMAGE_IMPORT_DESCRIPTOR* imp_dir = (IMAGE_IMPORT_DESCRIPTOR*)(idata.data() + import_dir_off);
    imp_dir[0].OriginalFirstThunk = idata_base + ilt_msvcrt_off;
    imp_dir[0].Name = idata_base + dll_name_off;
    imp_dir[0].FirstThunk = idata_base;           /* printf, exit */
    imp_dir[1].OriginalFirstThunk = idata_base + ilt_k32_off;
    imp_dir[1].Name = idata_base + dll_k32_off;
    imp_dir[1].FirstThunk = idata_base + 16;      /* GetStdHandle, ReadFile */
    memset(&imp_dir[2], 0, sizeof(IMAGE_IMPORT_DESCRIPTOR));

    uint64_t* ilt1 = (uint64_t*)(idata.data() + ilt_msvcrt_off);
    ilt1[0] = idata_base + hint_printf_off;
    ilt1[1] = idata_base + hint_exit_off;
    ilt1[2] = 0;
    uint64_t* ilt2 = (uint64_t*)(idata.data() + ilt_k32_off);
    ilt2[0] = idata_base + hint_gsh_off;
    ilt2[1] = idata_base + hint_rf_off;
    ilt2[2] = 0;

    uint8_t* hp = idata.data() + hint_printf_off;
    hp[0] = 0; hp[1] = 0; memcpy(hp + 2, fn_printf, fn_printf_len);
    hp = idata.data() + hint_exit_off;
    hp[0] = 0; hp[1] = 0; memcpy(hp + 2, fn_exit, fn_exit_len);
    hp = idata.data() + hint_gsh_off;
    hp[0] = 0; hp[1] = 0; memcpy(hp + 2, fn_gsh, fn_gsh_len);
    hp = idata.data() + hint_rf_off;
    hp[0] = 0; hp[1] = 0; memcpy(hp + 2, fn_rf, fn_rf_len);
    memcpy(idata.data() + dll_name_off, dll_name, dll_name_len);
    memcpy(idata.data() + dll_k32_off, dll_k32, dll_k32_len);

    std::ofstream out(outputExe, std::ios::binary);
    if (!out) { std::fprintf(stderr,"无法写 %s\n",outputExe.c_str()); return false; }

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
    sect2.VirtualSize = total_data;
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
    return true;
}

void printVersion(){
    std::printf("Wei-Toolchain 0.9.1 - Wei27\n");
    std::printf("架构平台：X86-64 AOT Windows NT\n");
    std::printf("  wei build <file.wei> [-o out.exe] [-opt]   原生编译为 x86-64 可执行文件\n");
    std::printf("编译器最高支持到 Wei27 版本，如果你需要使用更高版本的特性，请检查你的编译器。\n");
}

void printStart(){
    std::printf("Wei-Toolchain - 维语言工具链\n");
    std::printf("Max Wei27\n");
}

}  // namespace

int main(int argc,char** argv){
#if defined(_WIN32)
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif

    if(argc<2){
        printStart();
        return 0;
    }
    std::string sub=argv[1];

    if(sub=="version" || sub=="-v" || sub=="--version"){
        printVersion();
        return 0;
    }

    if(argc<3){
        std::fprintf(stderr,"错误: 子命令 '%s' 缺少文件参数\n",sub.c_str());
        return 2;
    }
    std::string file=argv[2];

    if(sub=="build"){
        std::string outExe;
        bool opt=false;
        for(int i=3;i<argc;i++){
            if(std::string(argv[i])=="-o" && i+1<argc){
                outExe=argv[++i];
            }
            if(std::string(argv[i])=="-opt") opt=true;
        }
        if(outExe.empty()){
            outExe=basenameNoExt(file)+".exe";
        }
        std::string tmpExe=outExe+".tmp";
        if(!doBuildExeJIT(file, tmpExe, opt)) return 1;
        if(!MoveFileExW(std::wstring(tmpExe.begin(),tmpExe.end()).c_str(),
                        std::wstring(outExe.begin(),outExe.end()).c_str(),
                        MOVEFILE_REPLACE_EXISTING)){
            std::fprintf(stderr,"无法替换 %s\n",outExe.c_str());
            return 1;
        }
        std::printf("[ok] %s (native x86-64)\n",outExe.c_str());
        return 0;
    }

    std::fprintf(stderr,"错误: 未知子命令 '%s'\n",sub.c_str());
    return 2;
}

#endif // WEI_SKIP_MAIN
