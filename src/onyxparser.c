#include "onyxlex.h"
#include "onyxmsgs.h"
#include "onyxparser.h"
#include "onyxutils.h"

// NOTE: The one weird define you need to know before read the code below
#define make_node(nclass, kind) onyx_ast_node_new(parser->allocator, sizeof(nclass), kind)

static AstNode error_node = { Ast_Kind_Error, 0, NULL, NULL };

AstBasicType basic_type_void   = { { Ast_Kind_Basic_Type, 0, "void"   }, &basic_types[Basic_Kind_Void]  };
AstBasicType basic_type_bool   = { { Ast_Kind_Basic_Type, 0, "bool"   }, &basic_types[Basic_Kind_Bool]  };
AstBasicType basic_type_i8     = { { Ast_Kind_Basic_Type, 0, "i8"     }, &basic_types[Basic_Kind_I8]    };
AstBasicType basic_type_u8     = { { Ast_Kind_Basic_Type, 0, "u8"     }, &basic_types[Basic_Kind_U8]    };
AstBasicType basic_type_i16    = { { Ast_Kind_Basic_Type, 0, "i16"    }, &basic_types[Basic_Kind_I16]   };
AstBasicType basic_type_u16    = { { Ast_Kind_Basic_Type, 0, "u16"    }, &basic_types[Basic_Kind_U16]   };
AstBasicType basic_type_i32    = { { Ast_Kind_Basic_Type, 0, "i32"    }, &basic_types[Basic_Kind_I32]   };
AstBasicType basic_type_u32    = { { Ast_Kind_Basic_Type, 0, "u32"    }, &basic_types[Basic_Kind_U32]   };
AstBasicType basic_type_i64    = { { Ast_Kind_Basic_Type, 0, "i64"    }, &basic_types[Basic_Kind_I64]   };
AstBasicType basic_type_u64    = { { Ast_Kind_Basic_Type, 0, "u64"    }, &basic_types[Basic_Kind_U64]   };
AstBasicType basic_type_f32    = { { Ast_Kind_Basic_Type, 0, "f32"    }, &basic_types[Basic_Kind_F32]   };
AstBasicType basic_type_f64    = { { Ast_Kind_Basic_Type, 0, "f64"    }, &basic_types[Basic_Kind_F64]   };
AstBasicType basic_type_rawptr = { { Ast_Kind_Basic_Type, 0, "rawptr" }, &basic_types[Basic_Kind_Rawptr] };

// NOTE: Forward declarations
static void consume_token(OnyxParser* parser);
static void unconsume_token(OnyxParser* parser);
static b32 is_terminating_token(TokenType token_type);
static OnyxToken* expect_token(OnyxParser* parser, TokenType token_type);

static AstNumLit*   parse_numeric_literal(OnyxParser* parser);
static AstTyped*    parse_factor(OnyxParser* parser);
static AstTyped*    parse_expression(OnyxParser* parser);
static AstIf*       parse_if_stmt(OnyxParser* parser);
static AstWhile*    parse_while_stmt(OnyxParser* parser);
static b32          parse_symbol_statement(OnyxParser* parser, AstNode** ret);
static AstReturn*   parse_return_statement(OnyxParser* parser);
static AstBlock*    parse_block(OnyxParser* parser);
static AstNode*     parse_statement(OnyxParser* parser);
static AstType*     parse_type(OnyxParser* parser);
static AstLocal*    parse_function_params(OnyxParser* parser);
static AstFunction* parse_function_definition(OnyxParser* parser);
static AstTyped*    parse_global_declaration(OnyxParser* parser);
static AstTyped*    parse_top_level_expression(OnyxParser* parser);
static AstNode*     parse_top_level_statement(OnyxParser* parser);

static void consume_token(OnyxParser* parser) {
    parser->prev = parser->curr;
    parser->curr++;
    while (parser->curr->type == Token_Type_Comment) parser->curr++;
}

static void unconsume_token(OnyxParser* parser) {
    // TODO: This is probably wrong
    while (parser->prev->type == Token_Type_Comment) parser->prev--;
    parser->curr = parser->prev;
    parser->prev--;
}

static b32 is_terminating_token(TokenType token_type) {
    return (token_type == ';')
        || (token_type == '}')
        || (token_type == '{')
        || (token_type == Token_Type_End_Stream);
}

static void find_token(OnyxParser* parser, TokenType token_type) {
    while (parser->curr->type != token_type && !is_terminating_token(parser->curr->type)) {
        consume_token(parser);
    }
}

// Advances to next token no matter what
static OnyxToken* expect_token(OnyxParser* parser, TokenType token_type) {
    OnyxToken* token = parser->curr;
    consume_token(parser);

    if (token->type != token_type) {
        onyx_message_add(parser->msgs,
                         ONYX_MESSAGE_TYPE_EXPECTED_TOKEN,
                         token->pos,
                         token_name(token_type), token_name(token->type));
        return NULL;
    }

    return token;
}

static AstNumLit* parse_numeric_literal(OnyxParser* parser) {
    AstNumLit* lit_node = make_node(AstNumLit, Ast_Kind_Literal);
    lit_node->base.token = expect_token(parser, Token_Type_Literal_Numeric);
    lit_node->base.flags |= Ast_Flag_Comptime;
    lit_node->value.l = 0ll;

    AstType* type;
    token_toggle_end(lit_node->base.token);
    char* tok = lit_node->base.token->text;

    // NOTE: charset_contains() behaves more like string_contains()
    // so I'm using it in this case
    if (charset_contains(tok, '.')) {
        if (tok[lit_node->base.token->length - 1] == 'f') {
            type = (AstType *) &basic_type_f32;
            lit_node->value.f = strtof(tok, NULL);
        } else {
            type = (AstType *) &basic_type_f64;
            lit_node->value.d = strtod(tok, NULL);
        }
    } else {
        i64 value = strtoll(tok, NULL, 0);
        if (bh_abs(value) < ((u64) 1 << 32)) {
            type = (AstType *) &basic_type_i32;
        } else {
            type = (AstType *) &basic_type_i64;
        }

        lit_node->value.l = value;
    }

    lit_node->base.type_node = type;
    token_toggle_end(lit_node->base.token);
    return lit_node;
}

// ( <expr> )
// - <factor>
// ! <factor>
// proc ...
// <symbol> ( '(' <exprlist> ')' )?
// <numlit>
// 'true'
// 'false'
// All of these could be followed by a cast
static AstTyped* parse_factor(OnyxParser* parser) {
    AstTyped* retval = NULL;

    switch ((u16) parser->curr->type) {
        case '(':
            {
                consume_token(parser);
                AstTyped* expr = parse_expression(parser);
                expect_token(parser, ')');
                retval = expr;
                break;
            }

        case '-':
            {
                consume_token(parser);
                AstTyped* factor = parse_factor(parser);

                AstUnaryOp* negate_node = make_node(AstUnaryOp, Ast_Kind_Unary_Op);
                negate_node->operation = Unary_Op_Negate;
                negate_node->expr = factor;

                if ((factor->flags & Ast_Flag_Comptime) != 0) {
                    negate_node->base.flags |= Ast_Flag_Comptime;
                }

                retval = (AstTyped *) negate_node;
                break;
            }

        case '!':
            {
                AstUnaryOp* not_node = make_node(AstUnaryOp, Ast_Kind_Unary_Op);
                not_node->operation = Unary_Op_Not;
                not_node->base.token = expect_token(parser, '!');
                not_node->expr = parse_factor(parser);

                if ((not_node->expr->flags & Ast_Flag_Comptime) != 0) {
                    not_node->base.flags |= Ast_Flag_Comptime;
                }

                retval = (AstTyped *) not_node;
                break;
            }

        case Token_Type_Symbol:
            {
                OnyxToken* sym_token = expect_token(parser, Token_Type_Symbol);
                AstTyped* sym_node = make_node(AstTyped, Ast_Kind_Symbol);
                sym_node->token = sym_token;

                if (parser->curr->type != '(') {
                    retval = sym_node;
                    break;
                }

                // NOTE: Function call
                AstCall* call_node = make_node(AstCall, Ast_Kind_Call);
                call_node->base.token = expect_token(parser, '(');
                call_node->callee = (AstNode *) sym_node;

                AstArgument** prev = &call_node->arguments;
                AstArgument* curr = NULL;
                while (parser->curr->type != ')') {
                    curr = make_node(AstArgument, Ast_Kind_Argument);
                    curr->base.token = parser->curr;
                    curr->value = parse_expression(parser);

                    if (curr != NULL && curr->base.kind != Ast_Kind_Error) {
                        *prev = curr;
                        prev = (AstArgument **) &curr->base.next;
                    }

                    if (parser->curr->type == ')')
                        break;

                    if (parser->curr->type != ',') {
                        onyx_message_add(parser->msgs,
                                ONYX_MESSAGE_TYPE_EXPECTED_TOKEN,
                                parser->curr->pos,
                                token_name(','),
                                token_name(parser->curr->type));
                        return (AstTyped *) &error_node;
                    }

                    consume_token(parser);
                }
                consume_token(parser);

                retval = (AstTyped *) call_node;
                break;
            }

        case Token_Type_Literal_Numeric:
            retval = (AstTyped *) parse_numeric_literal(parser);
            break;

        case Token_Type_Literal_True:
            {
                AstNumLit* bool_node = make_node(AstNumLit, Ast_Kind_Literal);
                bool_node->base.type_node = (AstType *) &basic_type_bool;
                bool_node->base.token = expect_token(parser, Token_Type_Literal_True);
                bool_node->value.i = 1;
                retval = (AstTyped *) bool_node;
                break;
            }

        case Token_Type_Literal_False:
            {
                AstNumLit* bool_node = make_node(AstNumLit, Ast_Kind_Literal);
                bool_node->base.type_node = (AstType *) &basic_type_bool;
                bool_node->base.token = expect_token(parser, Token_Type_Literal_False);
                bool_node->value.i = 0;
                retval = (AstTyped *) bool_node;
                break;
            }

        default:
            onyx_message_add(parser->msgs,
                    ONYX_MESSAGE_TYPE_UNEXPECTED_TOKEN,
                    parser->curr->pos,
                    token_name(parser->curr->type));
            return NULL;
    }

    while (parser->curr->type == Token_Type_Keyword_Cast) {
        consume_token(parser);

        AstUnaryOp* cast_node = make_node(AstUnaryOp, Ast_Kind_Unary_Op);
        cast_node->base.type_node = parse_type(parser);
        cast_node->operation = Unary_Op_Cast;
        cast_node->expr = retval;
        retval = (AstTyped *) cast_node;
    }

    return retval;
}

static inline i32 get_precedence(BinaryOp kind) {
    switch (kind) {
        case Binary_Op_Equal: return 3;
        case Binary_Op_Not_Equal: return 3;

        case Binary_Op_Less_Equal: return 4;
        case Binary_Op_Less: return 4;
        case Binary_Op_Greater_Equal: return 4;
        case Binary_Op_Greater: return 4;

        case Binary_Op_Add: return 5;
        case Binary_Op_Minus: return 5;

        case Binary_Op_Multiply: return 6;
        case Binary_Op_Divide: return 6;

        case Binary_Op_Modulus: return 7;
        default: return -1;
    }
}

// <factor> + <factor>
// <factor> - <factor>
// <factor> * <factor>
// <factor> / <factor>
// <factor> % <factor>
// <factor> == <factor>
// <factor> != <factor>
// <factor> <= <factor>
// <factor> >= <factor>
// <factor> < <factor>
// <factor> > <factor>
// With expected precedence rules
static AstTyped* parse_expression(OnyxParser* parser) {
    bh_arr(AstBinaryOp*) tree_stack = NULL;
    bh_arr_new(global_scratch_allocator, tree_stack, 4);
    bh_arr_set_length(tree_stack, 0);

    AstTyped* left = parse_factor(parser);
    AstTyped* right;
    AstTyped* root = left;

    BinaryOp bin_op_kind;
    OnyxToken* bin_op_tok;

    while (1) {
        bin_op_kind = -1;
        switch ((u16) parser->curr->type) {
            case Token_Type_Equal_Equal:    bin_op_kind = Binary_Op_Equal; break;
            case Token_Type_Not_Equal:      bin_op_kind = Binary_Op_Not_Equal; break;
            case Token_Type_Less_Equal:     bin_op_kind = Binary_Op_Less_Equal; break;
            case Token_Type_Greater_Equal:  bin_op_kind = Binary_Op_Greater_Equal; break;
            case '<':                       bin_op_kind = Binary_Op_Less; break;
            case '>':                       bin_op_kind = Binary_Op_Greater; break;

            case '+':                       bin_op_kind = Binary_Op_Add; break;
            case '-':                       bin_op_kind = Binary_Op_Minus; break;
            case '*':                       bin_op_kind = Binary_Op_Multiply; break;
            case '/':                       bin_op_kind = Binary_Op_Divide; break;
            case '%':                       bin_op_kind = Binary_Op_Modulus; break;
            default: goto expression_done;
        }

        if (bin_op_kind != -1) {
            bin_op_tok = parser->curr;
            consume_token(parser);

            AstBinaryOp* bin_op = make_node(AstBinaryOp, Ast_Kind_Binary_Op);
            bin_op->operation = bin_op_kind;
            bin_op->base.token = bin_op_tok;

            while ( !bh_arr_is_empty(tree_stack) &&
                    get_precedence(bh_arr_last(tree_stack)->operation) >= get_precedence(bin_op_kind))
                bh_arr_pop(tree_stack);

            if (bh_arr_is_empty(tree_stack)) {
                // NOTE: new is now the root node
                bin_op->left = root;
                root = (AstTyped *) bin_op;
            } else {
                bin_op->left = bh_arr_last(tree_stack)->right;
                bh_arr_last(tree_stack)->right = (AstTyped *) bin_op;
            }

            bh_arr_push(tree_stack, bin_op);

            right = parse_factor(parser);
            bin_op->right = right;

            if ((left->flags & Ast_Flag_Comptime) != 0 && (right->flags & Ast_Flag_Comptime) != 0) {
                bin_op->base.flags |= Ast_Flag_Comptime;
            }
        }
    }

expression_done:
    return root;
}

// 'if' <expr> <block> ('elseif' <cond> <block>)* ('else' <block>)?
static AstIf* parse_if_stmt(OnyxParser* parser) {
    expect_token(parser, Token_Type_Keyword_If);

    AstTyped* cond = parse_expression(parser);
    AstBlock* true_block = parse_block(parser);

    AstIf* if_node = make_node(AstIf, Ast_Kind_If);
    AstIf* root_if = if_node;

    if_node->cond = cond;
    if (true_block != NULL)
        if_node->true_block.as_block = true_block;

    while (parser->curr->type == Token_Type_Keyword_Elseif) {
        consume_token(parser);
        AstIf* elseif_node = make_node(AstIf, Ast_Kind_If);

        cond = parse_expression(parser);
        true_block = parse_block(parser);

        elseif_node->cond = cond;
        if (true_block != NULL)
            elseif_node->true_block.as_block = true_block;

        if_node->false_block.as_if = elseif_node;
        if_node = elseif_node;
    }

    if (parser->curr->type == Token_Type_Keyword_Else) {
        consume_token(parser);

        AstBlock* false_block = parse_block(parser);
        if (false_block != NULL)
            if_node->false_block.as_block = false_block;
    }

    return root_if;
}

// 'while' <expr> <block>
static AstWhile* parse_while_stmt(OnyxParser* parser) {
    OnyxToken* while_token = expect_token(parser, Token_Type_Keyword_While);

    AstTyped* cond = parse_expression(parser);
    AstBlock* body = parse_block(parser);

    AstWhile* while_node = make_node(AstWhile, Ast_Kind_While);
    while_node->base.token = while_token;
    while_node->cond = cond;
    while_node->body = body;

    return while_node;
}

// Returns 1 if the symbol was consumed. Returns 0 otherwise
// ret is set to the statement to insert
// <symbol> : <type> = <expr>
// <symbol> : <type> : <expr>
// <symbol> := <expr>
// <symbol> :: <expr>
// <symbol> = <expr>
// <symbol> += <expr>
// <symbol> -= <expr>
// <symbol> *= <expr>
// <symbol> /= <expr>
// <symbol> %= <expr>
static b32 parse_symbol_statement(OnyxParser* parser, AstNode** ret) {
    if (parser->curr->type != Token_Type_Symbol) return 0;
    OnyxToken* symbol = expect_token(parser, Token_Type_Symbol);

    switch ((u16) parser->curr->type) {
        // NOTE: Declaration
        case ':':
            {
                consume_token(parser);
                AstType* type_node = NULL;

                // NOTE: var: type
                if (parser->curr->type != ':'
                        && parser->curr->type != '=') {
                    type_node = parse_type(parser);
                }

                AstLocal* local = make_node(AstLocal, Ast_Kind_Local);
                local->base.token = symbol;
                local->base.type_node = type_node;
                local->base.flags |= Ast_Flag_Lval; // NOTE: DELETE
                *ret = (AstNode *) local;

                if (parser->curr->type == '=' || parser->curr->type == ':') {
                    if (parser->curr->type == ':') {
                        local->base.flags |= Ast_Flag_Const;
                    }

                    AstAssign* assignment = make_node(AstAssign, Ast_Kind_Assignment);
                    local->base.next = (AstNode *) assignment;
                    assignment->base.token = parser->curr;
                    consume_token(parser);

                    AstTyped* expr = parse_expression(parser);
                    if (expr == NULL) {
                        token_toggle_end(parser->curr);
                        onyx_message_add(parser->msgs,
                                ONYX_MESSAGE_TYPE_EXPECTED_EXPRESSION,
                                assignment->base.token->pos,
                                parser->curr->text);
                        token_toggle_end(parser->curr);
                        return 1;
                    }
                    assignment->expr = expr;

                    AstNode* left_symbol = make_node(AstNode, Ast_Kind_Symbol);
                    left_symbol->token = symbol;
                    assignment->lval = (AstTyped *) left_symbol;
                }
                return 1;
            }

            // NOTE: Assignment
        case '=':
            {
                AstAssign* assignment = make_node(AstAssign, Ast_Kind_Assignment);
                assignment->base.token = parser->curr;
                consume_token(parser);

                AstNode* lval = make_node(AstNode, Ast_Kind_Symbol);
                lval->token = symbol;

                AstTyped* rval = parse_expression(parser);
                assignment->expr = rval;
                assignment->lval = (AstTyped *) lval;
                *ret = (AstNode *) assignment;
                return 1;
            }

        case Token_Type_Plus_Equal:
        case Token_Type_Minus_Equal:
        case Token_Type_Star_Equal:
        case Token_Type_Fslash_Equal:
        case Token_Type_Percent_Equal:
            {
                BinaryOp bin_op;
                if      (parser->curr->type == Token_Type_Plus_Equal)    bin_op = Binary_Op_Add;
                else if (parser->curr->type == Token_Type_Minus_Equal)   bin_op = Binary_Op_Minus;
                else if (parser->curr->type == Token_Type_Star_Equal)    bin_op = Binary_Op_Multiply;
                else if (parser->curr->type == Token_Type_Fslash_Equal)  bin_op = Binary_Op_Divide;
                else if (parser->curr->type == Token_Type_Percent_Equal) bin_op = Binary_Op_Modulus;

                AstBinaryOp* bin_op_node = make_node(AstBinaryOp, Ast_Kind_Binary_Op);
                bin_op_node->operation = bin_op;
                bin_op_node->base.token = parser->curr;

                consume_token(parser);
                AstTyped* expr = parse_expression(parser);

                AstNode* bin_op_left = make_node(AstNode, Ast_Kind_Symbol);
                bin_op_left->token = symbol;
                bin_op_node->left = (AstTyped *) bin_op_left;
                bin_op_node->right = expr;

                AstAssign* assign_node = make_node(AstAssign, Ast_Kind_Assignment);
                assign_node->base.token = bin_op_node->base.token;

                // TODO: Maybe I don't need to make another lval node?
                AstNode* lval = make_node(AstNode, Ast_Kind_Symbol);
                lval->token = symbol;
                assign_node->lval = (AstTyped *) lval;
                assign_node->expr = (AstTyped *) bin_op_node;

                *ret = (AstNode *) assign_node;

                return 1;
            }

        default:
            unconsume_token(parser);
    }

    return 0;
}

// 'return' <expr>?
static AstReturn* parse_return_statement(OnyxParser* parser) {
    AstReturn* return_node = make_node(AstReturn, Ast_Kind_Return);
    return_node->base.token = expect_token(parser, Token_Type_Keyword_Return);

    AstTyped* expr = NULL;

    if (parser->curr->type != ';') {
        expr = parse_expression(parser);

        if (expr == NULL || expr == (AstTyped *) &error_node) {
            return (AstReturn *) &error_node;
        } else {
            return_node->expr = expr;
        }
    }

    return return_node;
}

// <return> ;
// <block>
// <symbol_statement> ;
// <expr> ;
// <if>
// <while>
// 'break' ;
// 'continue' ;
static AstNode* parse_statement(OnyxParser* parser) {
    b32 needs_semicolon = 1;
    AstNode* retval = NULL;

    switch ((u16) parser->curr->type) {
        case Token_Type_Keyword_Return:
            retval = (AstNode *) parse_return_statement(parser);
            break;

        case '{':
            needs_semicolon = 0;
            retval = (AstNode *) parse_block(parser);
            break;

        case Token_Type_Symbol:
            if (parse_symbol_statement(parser, &retval)) break;
            // fallthrough

        case '(':
        case '+':
        case '-':
        case '!':
        case Token_Type_Literal_Numeric:
        case Token_Type_Literal_String:
            retval = (AstNode *) parse_expression(parser);
            break;

        case Token_Type_Keyword_If:
            needs_semicolon = 0;
            retval = (AstNode *) parse_if_stmt(parser);
            break;

        case Token_Type_Keyword_While:
            needs_semicolon = 0;
            retval = (AstNode *) parse_while_stmt(parser);
            break;

        case Token_Type_Keyword_Break:
            retval = make_node(AstNode, Ast_Kind_Break);
            retval->token = expect_token(parser, Token_Type_Keyword_Break);
            break;

        case Token_Type_Keyword_Continue:
            retval = make_node(AstNode, Ast_Kind_Break);
            retval->token = expect_token(parser, Token_Type_Keyword_Continue);
            break;

        default:
            break;
    }

    if (needs_semicolon) {
        if (parser->curr->type != ';') {
            onyx_message_add(parser->msgs,
                ONYX_MESSAGE_TYPE_EXPECTED_TOKEN,
                parser->curr->pos,
                token_name(';'),
                token_name(parser->curr->type));

            find_token(parser, ';');
        }
        consume_token(parser);
    }

    return retval;
}

// '---'
// '{' <stmtlist> '}'
static AstBlock* parse_block(OnyxParser* parser) {
    AstBlock* block = make_node(AstBlock, Ast_Kind_Block);
    AstLocalGroup* lg = make_node(AstLocalGroup, Ast_Kind_Local_Group);
    block->locals = lg;

    // NOTE: --- is for an empty block
    if (parser->curr->type == Token_Type_Empty_Block) {
        expect_token(parser, Token_Type_Empty_Block);
        return block;
    }

    expect_token(parser, '{');

    AstNode** next = &block->body;
    AstNode* stmt = NULL;
    while (parser->curr->type != '}') {
        stmt = parse_statement(parser);

        if (stmt != NULL && stmt->kind != Ast_Kind_Error) {
            *next = stmt;

            while (stmt->next != NULL) stmt = stmt->next;
            next = &stmt->next;
        }
    }

    expect_token(parser, '}');

    return block;
}

// <symbol>
// '^' <type>
static AstType* parse_type(OnyxParser* parser) {
    AstType* root = NULL;
    AstType** next_insertion = &root;

    while (1) {
        if (parser->curr->type == '^') {
            consume_token(parser);
            AstPointerType* new = make_node(AstPointerType, Ast_Kind_Pointer_Type);
            new->base.flags |= Basic_Flag_Pointer;
            *next_insertion = (AstType *) new;
            next_insertion = &new->elem;
        }

        else if (parser->curr->type == Token_Type_Symbol) {
            AstNode* symbol_node = make_node(AstNode, Ast_Kind_Symbol);
            symbol_node->token = expect_token(parser, Token_Type_Symbol);
            *next_insertion = (AstType *) symbol_node;
            next_insertion = NULL;
        }

        else {
            token_toggle_end(parser->curr);
            onyx_message_add(parser->msgs,
                    ONYX_MESSAGE_TYPE_UNEXPECTED_TOKEN,
                    parser->curr->pos,
                    parser->curr->text);
            token_toggle_end(parser->curr);

            consume_token(parser);
            break;
        }

        if (next_insertion == NULL) break;
    }

    return root;
}

// e
// '(' (<symbol>: <type>,?)* ')'
static AstLocal* parse_function_params(OnyxParser* parser) {
    if (parser->curr->type != '(')
        return NULL;

    expect_token(parser, '(');

    if (parser->curr->type == ')') {
        consume_token(parser);
        return NULL;
    }

    AstLocal* first_param = NULL;
    AstLocal* curr_param = NULL;
    AstLocal* trailer = NULL;

    OnyxToken* symbol;
    while (parser->curr->type != ')') {
        if (parser->curr->type == ',') consume_token(parser);

        symbol = expect_token(parser, Token_Type_Symbol);
        expect_token(parser, ':');

        curr_param = make_node(AstLocal, Ast_Kind_Param);
        curr_param->base.token = symbol;
        curr_param->base.flags |= Ast_Flag_Const;
        curr_param->base.type_node = parse_type(parser);

        if (first_param == NULL) first_param = curr_param;

        curr_param->base.next = NULL;
        if (trailer) trailer->base.next = (AstNode *) curr_param;

        trailer = curr_param;
    }

    consume_token(parser); // Skip the )
    return first_param;
}

// e
// '#' <symbol>
static b32 parse_possible_directive(OnyxParser* parser, const char* dir) {
    if (parser->curr->type != '#') return 0;

    expect_token(parser, '#');
    OnyxToken* sym = expect_token(parser, Token_Type_Symbol);

    b32 match = (strlen(dir) == sym->length) && (strncmp(dir, sym->text, sym->length) == 0);
    if (!match) {
        unconsume_token(parser);
        unconsume_token(parser);
    }
    return match;
}

// 'proc' <directive>* <func_params> ('->' <type>)? <block>
static AstFunction* parse_function_definition(OnyxParser* parser) {

    AstFunction* func_def = make_node(AstFunction, Ast_Kind_Function);
    func_def->base.token = expect_token(parser, Token_Type_Keyword_Proc);

    while (parser->curr->type == '#') {
        if (parse_possible_directive(parser, "intrinsic")) {
            func_def->base.flags |= Ast_Flag_Intrinsic;

            if (parser->curr->type == Token_Type_Literal_String) {
                OnyxToken* str_token = expect_token(parser, Token_Type_Literal_String);
                func_def->intrinsic_name = str_token;
            }
        }

        else if (parse_possible_directive(parser, "inline")) {
            func_def->base.flags |= Ast_Flag_Inline;
        }

        else if (parse_possible_directive(parser, "foreign")) {
            func_def->foreign_module = expect_token(parser, Token_Type_Literal_String);
            func_def->foreign_name   = expect_token(parser, Token_Type_Literal_String);

            func_def->base.flags |= Ast_Flag_Foreign;
        }

        else if (parse_possible_directive(parser, "export")) {
            func_def->base.flags |= Ast_Flag_Exported;

            if (parser->curr->type == Token_Type_Literal_String) {
                OnyxToken* str_token = expect_token(parser, Token_Type_Literal_String);
                func_def->exported_name = str_token;
            }
        }

        else {
            OnyxToken* directive_token = expect_token(parser, '#');
            OnyxToken* symbol_token = expect_token(parser, Token_Type_Symbol);

            onyx_message_add(parser->msgs,
                    ONYX_MESSAGE_TYPE_UNKNOWN_DIRECTIVE,
                    directive_token->pos,
                    symbol_token->text, symbol_token->length);
        }
    }

    AstLocal* params = parse_function_params(parser);
    func_def->params = params;

    AstType* return_type = (AstType *) &basic_type_void;
    if (parser->curr->type == Token_Type_Right_Arrow) {
        expect_token(parser, Token_Type_Right_Arrow);

        return_type = parse_type(parser);
    }

    u64 param_count = 0;
    for (AstLocal* param = params;
            param != NULL;
            param = (AstLocal *) param->base.next)
        param_count++;

    AstFunctionType* type_node = bh_alloc(parser->allocator, sizeof(AstFunctionType) + param_count * sizeof(AstType *));
    type_node->base.kind = Ast_Kind_Function_Type;
    type_node->param_count = param_count;
    type_node->return_type = return_type;

    u32 i = 0;
    for (AstLocal* param = params;
            param != NULL;
            param = (AstLocal *) param->base.next) {
        type_node->params[i] = param->base.type_node;
        i++;
    }

    func_def->base.type_node = (AstType *) type_node;

    func_def->body = parse_block(parser);

    return func_def;
}

// 'global' <type>
static AstTyped* parse_global_declaration(OnyxParser* parser) {
    AstGlobal* global_node = make_node(AstGlobal, Ast_Kind_Global);
    global_node->base.token = expect_token(parser, Token_Type_Keyword_Global);

    while (parser->curr->type == '#') {
        if (parse_possible_directive(parser, "foreign")) {
            global_node->foreign_module = expect_token(parser, Token_Type_Literal_String);
            global_node->foreign_name   = expect_token(parser, Token_Type_Literal_String);

            global_node->base.flags |= Ast_Flag_Foreign;
        }

        else if (parse_possible_directive(parser, "export")) {
            global_node->base.flags |= Ast_Flag_Exported;

            if (parser->curr->type == Token_Type_Literal_String) {
                OnyxToken* str_token = expect_token(parser, Token_Type_Literal_String);
                global_node->exported_name = str_token;
            }
        }

        else {
            OnyxToken* directive_token = expect_token(parser, '#');
            OnyxToken* symbol_token = expect_token(parser, Token_Type_Symbol);

            onyx_message_add(parser->msgs,
                    ONYX_MESSAGE_TYPE_UNKNOWN_DIRECTIVE,
                    directive_token->pos,
                    symbol_token->text, symbol_token->length);
        }
    }

    global_node->base.type_node = parse_type(parser);
    global_node->base.flags |= Ast_Flag_Lval;


    bh_arr_push(parser->results.nodes_to_process, (AstNode *) global_node);

    return (AstTyped *) global_node;
}

static AstTyped* parse_top_level_expression(OnyxParser* parser) {
    if (parser->curr->type == Token_Type_Keyword_Proc) {
        AstFunction* func_node = parse_function_definition(parser);

        bh_arr_push(parser->results.nodes_to_process, (AstNode *) func_node);

        return (AstTyped *) func_node;
    }
    else if (parser->curr->type == Token_Type_Keyword_Global) {
        return parse_global_declaration(parser);
    }
    else {
        return parse_expression(parser);
    }
}

// 'use' <string>
// <symbol> :: <expr>
static AstNode* parse_top_level_statement(OnyxParser* parser) {
    switch (parser->curr->type) {
        case Token_Type_Keyword_Use:
            {
                AstUse* use_node = make_node(AstUse, Ast_Kind_Use);
                use_node->base.token = expect_token(parser, Token_Type_Keyword_Use);
                use_node->filename = expect_token(parser, Token_Type_Literal_String);

                return (AstNode *) use_node;
            }

        case Token_Type_Symbol:
            {
                OnyxToken* symbol = parser->curr;
                consume_token(parser);

                expect_token(parser, ':');
                expect_token(parser, ':');

                AstTyped* node = parse_top_level_expression(parser);

                if (node->kind == Ast_Kind_Function) {
                    AstFunction* func = (AstFunction *) node;

                    if (func->exported_name == NULL)
                        func->exported_name = symbol;

                } else if (node->kind == Ast_Kind_Global) {
                    AstGlobal* global = (AstGlobal *) node;

                    if (global->exported_name == NULL)
                        global->exported_name = symbol;

                } else {
                    // HACK
                    bh_arr_push(parser->results.nodes_to_process, (AstNode *) node);
                }

                AstBinding* binding = make_node(AstBinding, Ast_Kind_Binding);
                binding->base.token = symbol;
                binding->node = (AstNode *) node;

                return (AstNode *) binding;
            }

        default: break;
    }

    consume_token(parser);
    return NULL;
}





// NOTE: This returns a void* so I don't need to cast it everytime I use it
void* onyx_ast_node_new(bh_allocator alloc, i32 size, AstKind kind) {
    void* node = bh_alloc(alloc, size);

    memset(node, 0, size);
    *(AstKind *) node = kind;

    return node;
}

OnyxParser onyx_parser_create(bh_allocator alloc, OnyxTokenizer *tokenizer, OnyxMessages* msgs) {
    OnyxParser parser;

    parser.allocator = alloc;
    parser.tokenizer = tokenizer;
    parser.curr = tokenizer->tokens;
    parser.prev = NULL;
    parser.msgs = msgs;

    parser.results = (ParseResults) {
        .allocator = global_heap_allocator,

        .uses = NULL,
        .bindings = NULL,
        .nodes_to_process = NULL,
    };

    bh_arr_new(parser.results.allocator, parser.results.uses, 4);
    bh_arr_new(parser.results.allocator, parser.results.bindings, 4);
    bh_arr_new(parser.results.allocator, parser.results.nodes_to_process, 4);

    return parser;
}

void onyx_parser_free(OnyxParser* parser) {
}

ParseResults onyx_parse(OnyxParser *parser) {
    while (parser->curr->type != Token_Type_End_Stream) {
        AstNode* curr_stmt = parse_top_level_statement(parser);

        if (curr_stmt != NULL && curr_stmt != &error_node) {
            while (curr_stmt != NULL) {

                switch (curr_stmt->kind) {
                    case Ast_Kind_Use:     bh_arr_push(parser->results.uses, (AstUse *) curr_stmt); break;
                    case Ast_Kind_Binding: bh_arr_push(parser->results.bindings, (AstBinding *) curr_stmt); break;
                    default: assert(("Invalid top level node", 0));
                }

                curr_stmt = curr_stmt->next;
            }
        }
    }

    return parser->results;
}
