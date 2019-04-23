/*
 * lexer.c
 * Copyright (C) 2019 Marc Kirchner
 *
 * Distributed under terms of the MIT license.
 */

#include "lexer.h"

#include <stdlib.h>
#include <string.h>

static char* symbol_chars = "!*+-0123456789<=>?@"
                            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                            "abcdefghijklmnopqrstuvwxyz";

lexer_t* lexer_new(FILE* fp)
{
    lexer_t* lexer = (lexer_t*) malloc(sizeof(lexer_t));
    *lexer = (lexer_t) {
        .fp=fp,
        .state=LEXER_STATE_ZERO,
        .line_no=1,
        .char_no=0
    };
    return lexer;
}

void lexer_delete(lexer_t* l)
{
    free(l);
}

void lexer_delete_token(lexer_token_t* t)
{
    free(t->value);
    free(t);
}

static lexer_token_t* lexer_make_token(token_type_t token_type, char* buf)
{
    /* FIXME: check malloc return values for NULL */
    lexer_token_t* tok = (lexer_token_t*) malloc(sizeof(lexer_token_t));
    tok->type = token_type;
    switch(token_type) {
    case INT:
        tok->value = (int*) malloc(sizeof(int));
        *((int*)tok->value) = atoi(buf);
    case FLOAT:
        tok->value = (double*) malloc(sizeof(double));
        *((double*)tok->value) = atof(buf);
    case STRING:
    case ERROR:
    case SYMBOL:
    case LPAREN:
    case RPAREN:
    case QUOTE:
        tok->value = (char*) malloc(strlen(buf)*sizeof(char));
        strcpy((char*) tok->value, buf);
    }
    return tok;
}

lexer_token_t* lexer_get_token(lexer_t* l)
{
    char buf[1024] = {0};
    size_t bufpos = 0;
    int c;
    char* pos;
    while ((c = fgetc(l->fp)) != EOF) {
        switch (l->state) {
        case LEXER_STATE_ZERO:
            switch(c) {
            case '(':
                buf[bufpos++] = c;
                return lexer_make_token(LPAREN, buf);
                break;
            case ')':
                buf[bufpos++] = c;
                return lexer_make_token(RPAREN, buf);
                break;
            case '\'':
                buf[bufpos++] = c;
                return lexer_make_token(QUOTE, buf);
                break;
            /* start a string */
            case '\"':
                /* don't put c in the buffer */
                l->state = LEXER_STATE_STRING;
                break;
            /* start a number */
            case '0' ... '9':
                buf[bufpos++] = c;
                l->state = LEXER_STATE_NUMBER;
                break;
            /* start a symbol */
            case 'a' ... 'z':
            case 'A' ... 'Z':
                buf[bufpos++] = c;
                l->state = LEXER_STATE_SYMBOL;
                break;
            /* eat whitespace */
            case ' ':
            case '\r':
            case '\t':
                break;
            case '\n':
                l->line_no++;
                break;
            /* error */
            default:
                buf[bufpos++] = c;
                return lexer_make_token(ERROR, buf);
            }
            break;
        case LEXER_STATE_STRING:
            if (c != '\"') {
                buf[bufpos++] = c;
                if (c == '\n') l->line_no++;
            } else {
                /* don't put c in the buffer */
                l->state = LEXER_STATE_ZERO;
                return lexer_make_token(STRING, buf);
            }
            break;

        case LEXER_STATE_NUMBER:
            switch(c) {
            case '(':
            case ')':
                ungetc(c, l->fp);
                l->state = LEXER_STATE_ZERO;
                return lexer_make_token(INT, buf);
            case '\n':
                ungetc(c, l->fp);
            case '\t':
            case '\r':
            case ' ':
                l->state = LEXER_STATE_ZERO;
                return lexer_make_token(INT, buf);
            case '.':
                buf[bufpos++] = c;
                l->state = LEXER_STATE_FLOAT;
                break;
            case '0' ... '9':
                buf[bufpos++] = c;
                break;
            default:
                /* error */
                buf[bufpos++] = c;
                return lexer_make_token(ERROR, buf);
            }
            break;
        case LEXER_STATE_FLOAT:
            switch(c) {
            case '(':
            case ')':
                ungetc(c, l->fp);
                l->state = LEXER_STATE_ZERO;
                return lexer_make_token(FLOAT, buf);
            case '\n':
                ungetc(c, l->fp);
            case '\t':
            case '\r':
            case ' ':
                l->state = LEXER_STATE_ZERO;
                return lexer_make_token(FLOAT, buf);
            case '0' ... '9':
                buf[bufpos++] = c;
                break;
            default:
                /* error */
                l->state = LEXER_STATE_ZERO;
                return lexer_make_token(ERROR, buf);
            }
            break;
        case LEXER_STATE_SYMBOL:
            pos = strchr(symbol_chars, c);
            if (pos != NULL) {
                buf[bufpos++] = c;
            } else {
                ungetc(c, l->fp);
                l->state = LEXER_STATE_ZERO;
                return lexer_make_token(SYMBOL, buf);
            }
            break;
        default:
            buf[bufpos++] = c;
            return lexer_make_token(ERROR, buf);
        }
    }
    if (l->state != LEXER_STATE_ZERO) {
        return lexer_make_token(ERROR, buf);
    } else {
        return NULL;
    }
}

