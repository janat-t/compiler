/* XCC (x86): Experimental C-subset Compiler.
   Copyright (c) 2002-2019, gondow@cs.titech.ac.jp, All rights reserved.
   $Id: codegen.c,v 1.8 2019/05/28 02:42:04 gondow Exp gondow $ */
/* ---------------------------------------------------------------------- */
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>

#include "xcc.h"
#include "misc.h"
#include "AST.h"
#include "type.h"
#include "symbol.h"
#include "codegen.h"

#define  LABEL_PREFIX  "L.XCC"
#if 0
// Mac の正式なセクション名はこちら
#define  TEXT_SECTION   ".section __TEXT,__text,regular,pure_instructions"
#define  DATA_SECTION   ".section __DATA,__data"
#define  RDATA_SECTION  ".section __TEXT,__cstring,cstring_literals"
#else
// でも，Linux と共通化したいのでこちらを使う
#define  TEXT_SECTION   ".text"
#define  DATA_SECTION   ".data"
#define  RDATA_SECTION  ".text"
// RDATA_SECTION は Mac と Linux でセクション名が違うので .text で逃げる
#endif
static char *func_name;
static int   total_local_size;

static void emit_code (struct AST *ast, char *fmt, ...);
static void codegen_begin_block (struct AST *ast);
static void codegen_end_block (void);
static void codegen_begin_function (struct AST *ast);
static void codegen_end_function (void);
static int  is_library_func (char *libname);

static void codegen_exp_id (struct AST *ast);
static void codegen_exp_funcall (struct AST *ast);
static void codegen_exp (struct AST *ast);
static void codegen_stmt (struct AST *ast_stmt);
static void codegen_block (struct AST *ast_block);
static void codegen_func (struct AST *ast);
static void codegen_dec (struct AST *ast);
char* create_ctrl_label(void);

/* ---------------------------------------------------------------------- */
static char *library_funcs [] = {
    // 必要に応じて呼び出したいライブラリ関数を追加する
    "printf", "malloc", "random", "srandom", "atol", NULL
};

/* ---------------------------------------------------------------------- */

    static void
emit_code (struct AST *ast, char *fmt, ...)
{
    va_list  argp;
    va_start (argp, fmt);
    vfprintf (xcc_out, fmt, argp);
    va_end   (argp);

    /* the argument 'ast' can be used for debug purpose */
}

    static void
codegen_begin_block (struct AST *ast)
{
    assert (!strcmp (ast->ast_type, "AST_compound_statement"));
    sym_table.local [++sym_table.local_index] = ast->u.local;
}

    static void
codegen_end_block (void)
{
    sym_table.local_index--;
}

    static void
codegen_begin_function (struct AST *ast)
{
    assert(!strcmp (ast->ast_type, "AST_function_definition"));
    sym_table.local_index = -1;
    sym_table.global = ast->u.func.global;
    sym_table.arg    = ast->u.func.arg;
    sym_table.label  = ast->u.func.label;
    sym_table.string = ast->u.func.string;
}

    static void
codegen_end_function (void)
{
    /* do nothing */
}

    static int
is_library_func (char *libname)
{
    for (int i = 0; library_funcs [i] != NULL; i++) {
        if (!strcmp (libname, library_funcs [i]))
            return 1;
    }
    return 0;
}

// ここから上は（関数プロトタイプ宣言の追加等以外は）修正や拡張は不要のはず
/* ---------------------------------------------------------------------- */
// ここから下は好きに修正や拡張をしても構わない

int label_cnt = 0;

char* create_ctrl_label(void) {
    ++label_cnt;
    char* lab = (char*)malloc(sizeof(char)*64);
    sprintf(lab, "LBB0_%d", label_cnt);
    return lab;
}

    static void
codegen_exp_id (struct AST *ast)
{
    int offset;
    char *reg = "%rax";
    struct Symbol *sym = sym_lookup (ast->child [0]->u.id);
    assert (sym != NULL);

    switch (sym->name_space) {
        case NS_LOCAL:
        case NS_ARG:
            if (sym->name_space == NS_LOCAL) {
                offset = - (sym->offset + 8);
            } else if (sym->name_space == NS_ARG) {
                if (sym->offset <= 40) { // 1st-6th arguments
                    offset =  - (sym->offset + 8);
                    switch (sym->offset) {
                        case  0: reg = "%rdi"; break;
                        case  8: reg = "%rsi"; break;
                        case 16: reg = "%rdx"; break;
                        case 24: reg = "%rcx"; break;
                        case 32: reg = "%r8";  break;
                        case 40: reg = "%r9";  break;
                        default: assert (0);   break;
                    }
                } else { // 7th and subsequent arguments
                    offset =  sym->offset - 48 + 16;
                }
            } else {
                assert (0);
            }

            if (!((sym->name_space == NS_ARG) && (sym->offset <= 40))) { // other than 1st-6th arguments
                // char型，int型には非対応
                emit_code (ast, "\tmovq    %d(%%rbp), %%rax \t# %s, %d\n",
                        offset, sym->name, sym->offset);
            }
            emit_code (ast, "\tpushq   %s\n", reg);
            break;
        case NS_GLOBAL:
            // char型，int型には非対応
            if (sym->type->kind == TYPE_KIND_FUNCTION) {
                if (is_library_func (sym->name)) {
                    emit_code (ast, "\tmovq    _%s@GOTPCREL(%%rip), %%rax\n", sym->name);
                } else {
                    emit_code (ast, "\tleaq    _%s(%%rip), %%rax\n", sym->name);
                }
                emit_code (ast, "\tpushq   %%rax\n");
            } else {
                emit_code (ast, "\tpushq   _%s(%%rip)\n", sym->name);
            }
            break;
        case NS_LABEL: /* falling through */
        default: assert (0); break;
    }
}

    static void
codegen_exp_funcall (struct AST *ast_func)
{
    struct AST *ast, *ast_exp;
    int args_size = 0, narg = 0;
    char *regs[] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};

    assert (!strcmp (ast_func->ast_type, "AST_expression_funcall1")
            || !strcmp (ast_func->ast_type, "AST_expression_funcall2"));

    // count "narg" first
    if (!strcmp (ast_func->ast_type, "AST_expression_funcall1")) {
        for (ast = ast_func->child [1], narg = 1; ; ast = ast->child [0], narg++) {
            if (!strcmp (ast->ast_type, "AST_argument_expression_list_single"))
                break;
        }
    }

    // save all caller-save registers to the stack
    emit_code (ast_func, "# save caller-save registers\n");
    for (int i = 1; i <= 6; i++) {
        emit_code (ast_func, "\tpushq   %s\n", regs [i - 1]);
    }

    /* push arguments in reverse order (funcall2 has no arguments) */
    if (!strcmp (ast_func->ast_type, "AST_expression_funcall1")) {
        /* for Mac 16-bytes alignment */
        if (narg >= 7 && narg % 2 != 0) {
            emit_code (ast_func, "\tsubq    $8, %%rsp\n");
        }

        for (ast = ast_func->child [1]; ; ast = ast->child [0]) {
            if (!strcmp (ast->ast_type,
                        "AST_argument_expression_list_single")) {
                ast_exp = ast->child [0];
            } else if (!strcmp (ast->ast_type,
                        "AST_argument_expression_list_pair")) {
                ast_exp = ast->child [1];
            } else {
                assert (0);
            }
            args_size += ROUNDUP_LONG (ast_exp->type->size);
            codegen_exp (ast_exp);
            if (!strcmp (ast->ast_type,
                        "AST_argument_expression_list_single"))
                break;
        }
    }

    codegen_exp (ast_func->child [0]);
    emit_code (ast_func, "\tpopq    %%rax\n");

    // move 1st-6th args to registers
    for (int i = 1; i <= narg && i <= 6; i++) {
        char *regs[] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
        emit_code (ast_func, "\tpopq    %s\n", regs [i - 1]);
    }
    emit_code (ast_func, "\tcall    *%%rax\n");
    if (narg >= 7) {
        emit_code (ast_func, "\taddq    $%d, %%rsp \t# pop args\n",
                ROUNDUP_STACK(args_size) - 48);
    }
    // restore caller-save registers from the stack
    emit_code (ast_func, "# restore caller-save registers\n");
    for (int i = 6; i >= 1; i--) {
        emit_code (ast_func, "\tpopq   %s\n", regs [i - 1]);
    }

    emit_code (ast_func, "\tpushq   %%rax\n");
}

    static void
codegen_exp (struct AST *ast)
{
    char *L1 = (char*)malloc(sizeof (char)*64);
    char *L2 = (char*)malloc(sizeof (char)*64);
    if (   !strcmp (ast->ast_type, "AST_expression_int")
            || !strcmp (ast->ast_type, "AST_expression_char")
            || !strcmp (ast->ast_type, "AST_expression_long")) {
        emit_code (ast, "\tpushq   $%d\n", ast->u.long_val);
    } else if (!strcmp (ast->ast_type, "AST_expression_string")) {
        struct String *string = string_lookup (ast->u.id);
        assert (string != NULL);
        emit_code (ast, "\tleaq    %s.%s(%%rip), %%rax \t# \"%s\"\n",
                LABEL_PREFIX, string->label, string->data);
        emit_code (ast, "\tpushq   %%rax\n");
    } else if (!strcmp (ast->ast_type, "AST_expression_id")) {
        codegen_exp_id (ast);
    } else if (   !strcmp (ast->ast_type, "AST_expression_funcall1")
            || !strcmp (ast->ast_type, "AST_expression_funcall2")) {
        codegen_exp_funcall (ast);
        // 他の expression の場合のコードをここに追加する
    } else if (!strcmp (ast->ast_type, "AST_expression_assign")) {
        codegen_exp (ast->child [1]);
        if(!strcmp(ast->child[0]->ast_type,"AST_expression_unary")) {
            codegen_exp(ast->child[0]->child[1]);
            emit_code(ast, "\tpopq %%rax\n");
        } else if(!strcmp(ast->child[0]->ast_type,"AST_expression_id")){
            struct Symbol *sym = sym_lookup (ast->child[0]->child[0]->u.id);
            if(sym == NULL) assert(0);

            int offset = - (sym->offset + 8);
            switch (sym->name_space) {
                case NS_GLOBAL:
                    emit_code (ast, "\tleaq _%s(%%rip), %%rax\n", sym->name);
                    break;
                case NS_LOCAL:
                    emit_code (ast, "\tleaq %d(%%rbp), %%rax \t# %s, %d\n", offset, sym->name, sym->offset);
                    break;
                default:
                    break;
            }
        }
        emit_code (ast, "\tpopq %%r10\n");
        emit_code (ast, "\tmovq %%r10, 0(%%rax)\n");
        emit_code (ast, "\tpushq %%r10\n");
    } else if (!strcmp (ast->ast_type, "AST_expression_lor")){
        codegen_exp(ast->child[0]);
        L1 = create_ctrl_label();
        L2 = create_ctrl_label();
        emit_code (ast, "\tpopq %%rax\n");
        emit_code (ast, "\tcmpq $1, %%rax\n");
        emit_code (ast, "\tje %s\n", L1);
        codegen_exp(ast->child[1]);
        emit_code (ast, "\tpopq %%rax\n");
        emit_code (ast, "\tcmpq $1, %%rax\n");
        emit_code (ast, "\tje %s\n", L1);
        emit_code (ast, "\tmovq $0, %%rax\n");
        emit_code (ast, "\tjmp %s\n", L2);
        emit_code (ast, "%s:\n", L1);
        emit_code (ast, "\tmovq $1, %%rax\n");
        emit_code (ast, "%s:\n", L2);
        emit_code (ast, "\tpushq %%rax\n");
    } else if (!strcmp (ast->ast_type, "AST_expression_land")){
        codegen_exp(ast->child[0]);
        L1 = create_ctrl_label();
        L2 = create_ctrl_label();
        emit_code (ast, "\tpopq %%rax\n");
        emit_code (ast, "\tcmpq $0, %%rax\n");
        emit_code (ast, "\tje %s\n", L1);
        codegen_exp(ast->child[1]);
        emit_code (ast, "\tpopq %%rax\n");
        emit_code (ast, "\tcmpq $0, %%rax\n");
        emit_code (ast, "\tje %s\n", L1);
        emit_code (ast, "\tmovq $1, %%rax\n");
        emit_code (ast, "\tjmp %s\n", L2);
        emit_code (ast, "%s:\n", L1);
        emit_code (ast, "\tmovq $0, %%rax\n");
        emit_code (ast, "%s:\n", L2);
        emit_code (ast, "\tpushq %%rax\n");
    } else if (!strcmp (ast->ast_type, "AST_expression_eq")){
        codegen_exp (ast->child [0]);
        codegen_exp (ast->child [1]);
        emit_code (ast, "\tpopq %%rax\n");
        emit_code (ast, "\tpopq %%r10\n");
        emit_code (ast, "\tcmpq %%r10, %%rax\n");
        emit_code (ast, "\tsete %%al\n");
        emit_code (ast, "\tmovzbq %%al, %%rax\n");
        emit_code (ast, "\tpushq %%rax\n");
    } else if (!strcmp (ast->ast_type, "AST_expression_less")){
        codegen_exp (ast->child [0]);
        codegen_exp (ast->child [1]);
        emit_code (ast, "\tpopq %%rax\n");
        emit_code (ast, "\tpopq %%r10\n");
        emit_code (ast, "\tcmpq %%rax, %%r10\n");
        emit_code (ast, "\tsetl %%al\n");
        emit_code (ast, "\tmovzbq %%al, %%rax\n");
        emit_code (ast, "\tpushq %%rax\n");
    } else if (!strcmp (ast->ast_type, "AST_expression_add")){
        codegen_exp (ast->child [0]);
        codegen_exp (ast->child [1]);
        if (ast->child[0]->type->kind == TYPE_KIND_POINTER) {
            emit_code (ast, "\tpopq %%rax\n");
            emit_code (ast, "\timul $%d, %%rax\n", ast->child[0]->type->size);
            emit_code (ast, "\tpushq %%rax\n");
        }
        emit_code (ast, "\tpopq %%rax\n");
        emit_code (ast, "\tpopq %%r10\n");
        emit_code (ast, "\taddq %%r10, %%rax\n");
        emit_code (ast, "\tpushq %%rax\n");
    } else if (!strcmp (ast->ast_type, "AST_expression_sub")){
        codegen_exp (ast->child [0]);
        codegen_exp (ast->child [1]);
        emit_code (ast, "\tpopq %%r10\n");
        emit_code (ast, "\tpopq %%rax\n");
        emit_code (ast, "\tsubq %%r10, %%rax\n");
        emit_code (ast, "\tpushq %%rax\n");
    } else if (!strcmp (ast->ast_type, "AST_expression_mul")){
        codegen_exp (ast->child [0]);
        codegen_exp (ast->child [1]);
        emit_code (ast, "\tpopq %%rax\n");
        emit_code (ast, "\tpopq %%r10\n");
        emit_code (ast, "\timulq %%r10, %%rax\n");
        emit_code (ast, "\tpushq %%rax\n");
    } else if (!strcmp (ast->ast_type, "AST_expression_div")){
        codegen_exp (ast->child [0]);
        codegen_exp (ast->child [1]);
        emit_code (ast, "\tpopq %%r10\n");
        emit_code (ast, "\tpopq %%rax\n");
        emit_code (ast, "\tidivq %%r10, %%rax\n");
        emit_code (ast, "\tpushq %%rax\n");
    } else if (!strcmp (ast->ast_type, "AST_expression_paren")){
        codegen_exp(ast->child[0]);
    } else if (!strcmp (ast->ast_type, "AST_expression_opt_single")){
        codegen_exp(ast->child[0]);
    } else if (!strcmp (ast->ast_type, "AST_expression_unary")){
        if (!strcmp (ast->child[0]->ast_type, "AST_unary_operator_address")) {
            struct Symbol *sym = sym_lookup (ast->child[1]->child[0]->u.id);
            if (sym == NULL) assert(0);
            int offset = -(sym->offset + 8);
            switch (sym->name_space) {
                case NS_GLOBAL:
                    emit_code (ast, "\tleaq _%s(%%rip), %%rax\n", sym->name);
                    break;
                case NS_LOCAL:
                    emit_code (ast, "\tleaq %d(%%rbp), %%rax \t# %s, %d\n", offset, sym->name, sym->offset);
                    break;
                default:
                    break;
            }
            emit_code (ast, "\tpushq %%rax\n");
        } else if (!strcmp (ast->child[0]->ast_type, "AST_unary_operator_deref")) {
            codegen_exp (ast->child[1]);
            emit_code (ast, "\tpopq %%rax\n");
            emit_code (ast, "\tpushq (%%rax)\n");
        } else {
            assert (0);
        }
    } else {
        assert (0);
    }
}

    static void
codegen_stmt (struct AST *ast_stmt)
{
    char *L1 = (char*)malloc(sizeof(char)*64);
    char *L2 = (char*)malloc(sizeof(char)*64);
    if (!strcmp (ast_stmt->ast_type, "AST_statement_exp")) {
        if (!strcmp (ast_stmt->child [0]->ast_type, "AST_expression_opt_single")) {
            codegen_exp (ast_stmt->child [0]->child [0]);
            emit_code (ast_stmt, "\taddq    $8, %%rsp\n");
        } else if (!strcmp (ast_stmt->child [0]->ast_type, "AST_expression_opt_null")) {
            /* nothing to do */
        } else {
            assert (0);
        }
    } else if (!strcmp (ast_stmt->ast_type, "AST_statement_comp")) {
        codegen_block (ast_stmt->child [0]);
    } else if (!strcmp (ast_stmt->ast_type, "AST_statement_if")) {
        codegen_exp (ast_stmt->child [0]);
        L1 = create_ctrl_label ();
        emit_code (ast_stmt, "\tpopq %%rax\n");
        emit_code (ast_stmt, "\tcmpq $0, %%rax\n");
        emit_code (ast_stmt, "\tje %s\n", L1);
        codegen_stmt (ast_stmt->child [1]);
        emit_code (ast_stmt, "%s:\n", L1);
    } else if (!strcmp (ast_stmt->ast_type, "AST_statement_ifelse")) {
        codegen_exp (ast_stmt->child [0]);
        L1 = create_ctrl_label ();
        emit_code (ast_stmt, "\tpopq %%rax\n");
        emit_code (ast_stmt, "\tcmpq $0, %%rax\n");
        emit_code (ast_stmt, "\tje %s\n", L1);
        codegen_stmt (ast_stmt->child [1]);
        L2 = create_ctrl_label ();
        emit_code (ast_stmt, "\tjmp %s\n", L2);
        emit_code (ast_stmt, "%s:\n", L1);
        codegen_stmt (ast_stmt->child [2]);
        emit_code (ast_stmt, "%s:\n", L2);
    } else if (!strcmp (ast_stmt->ast_type, "AST_statement_while")) {
        L1 = create_ctrl_label ();
        emit_code (ast_stmt, "%s:\n", L1);
        codegen_exp (ast_stmt->child [0]);
        L2 = create_ctrl_label ();
        emit_code (ast_stmt, "\tpopq %%rax\n");
        emit_code (ast_stmt, "\tcmpq $0, %%rax\n");
        emit_code (ast_stmt, "\tje %s\n", L2);
        codegen_stmt (ast_stmt->child [1]);
        emit_code (ast_stmt, "\tjmp %s\n", L1);
        emit_code (ast_stmt, "%s:\n", L2);
    } else if (!strcmp (ast_stmt->ast_type, "AST_statement_goto")) {
        struct Symbol *sym = sym_lookup_label (ast_stmt->child[0]->u.id);
        if (sym == NULL) assert (0);
        emit_code (ast_stmt, "\tjmp G.%s\n", ast_stmt->child[0]->u.id);
    } else if (!strcmp (ast_stmt->ast_type, "AST_statement_label")) {
        emit_code (ast_stmt, "G.%s:\n", ast_stmt->child[0]->u.id);
        codegen_stmt (ast_stmt->child[1]);
    } else if (!strcmp (ast_stmt->ast_type, "AST_statement_return")) {
        codegen_exp (ast_stmt->child [0]);
        emit_code (ast_stmt, "\tpopq %%rax\n");
        emit_code (ast_stmt, "\tpopq %%rbp\n");
        emit_code (ast_stmt, "\tretq\n");
    } else {
        assert (0);
    }
}

    static void
codegen_block (struct AST *ast_block)
{
    struct AST *ast, *ast_stmt_list;
    assert (!strcmp (ast_block->ast_type, "AST_compound_statement"));
    codegen_begin_block (ast_block);

    ast_stmt_list = ast_block->child [1];
    ast = search_AST_bottom (ast_stmt_list, "AST_statement_list_single", NULL);
    while (1) {
        if (!strcmp (ast->ast_type, "AST_statement_list_single"))
            codegen_stmt (ast->child [0]);
        else if (!strcmp (ast->ast_type, "AST_statement_list_pair"))
            codegen_stmt (ast->child [1]);
        else
            assert (0);
        if (ast == ast_stmt_list)
            break;
        ast = ast->parent;
    }
    codegen_end_block ();
}

    static void
codegen_dec (struct AST *ast)
{
    assert (!strcmp (ast->ast_type, "AST_declaration"));
    if (ast->type->size <= 0)
        return;

    emit_code (ast, "\t.globl  _%s\n", ast->type->id);
    emit_code (ast, "\t%s\n", DATA_SECTION);
    // char型，int型には非対応
    if (ast->type->size == 8) {
        emit_code (ast, "\t.p2align  3\n");
    }
    emit_code (ast, "_%s:\n", ast->type->id);
    emit_code (ast, "\t.skip   %d\n\n", ast->type->size);
}

    static void
codegen_func (struct AST *ast)
{
    struct String *string, *head;

    assert (!strcmp (ast->ast_type, "AST_function_definition"));

    codegen_begin_function (ast);

    /* string literals */
    head = sym_table.string;
    if (head != NULL) {
        emit_code (ast, "\t%s\n", RDATA_SECTION);
        for (string = head; string != NULL; string = string->next) {
            emit_code (ast, "%s.%s:\n", LABEL_PREFIX, string->label);
            emit_code (ast, "\t.ascii  \"%s\\0\"\n", string->data);
        }
    }

    func_name = ast->type->id;
    total_local_size = ast->u.func.total_local_size;

    emit_code (ast, "\t%s\n", TEXT_SECTION);
    emit_code (ast, "\t.globl  _%s\n", func_name);
    emit_code (ast, "\t.p2align 4, 0x90\n");
    emit_code (ast, "_%s:\n", func_name);
    emit_code (ast, "\tpushq   %%rbp\n");
    emit_code (ast, "\tmovq    %%rsp, %%rbp\n");

    /* allocate space for local variables */
    emit_code (ast, "\tsubq    $%d, %%rsp\n", total_local_size);

    /* function body */
    codegen_block (ast->child [2]);

    /* function epilogue */
    emit_code (ast, "%s.RE.%s:\n", LABEL_PREFIX, func_name);
    emit_code (ast, "\tmovq    %%rbp, %%rsp\n");
    emit_code (ast, "\tpopq    %%rbp\n");
    emit_code (ast, "\tretq\n");

    codegen_end_function ();
}
/* ---------------------------------------------------------------------- */
    void
codegen (void)
{
    struct AST *ast, *ast_ext;
    ast = search_AST_bottom (ast_root, "AST_translation_unit_single", NULL);

    while (1) {
        if (!strcmp (ast->ast_type, "AST_translation_unit_single"))
            ast_ext = ast->child [0];
        else if (!strcmp (ast->ast_type, "AST_translation_unit_pair"))
            ast_ext = ast->child [1];
        else
            assert (0);

        if (!strcmp (ast_ext->ast_type, "AST_external_declaration_func"))
            codegen_func (ast_ext->child [0]);
        else if (!strcmp (ast_ext->ast_type, "AST_external_declaration_dec"))
            codegen_dec (ast_ext->child [0]);
        else
            assert (0);

        if (ast == ast_root)
            break;
        ast = ast->parent;
    }
}
/* ---------------------------------------------------------------------- */
