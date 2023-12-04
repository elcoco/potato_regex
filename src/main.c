#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "potato_regex.h"


int main(int argc, char **argv)
{
    if (argc < 2) {
        ERROR("Missing expression\n");
        return 1;
    }

    if (argc < 3) {
        ERROR("Missing input string\n");
        return 1;
    }

    const char *expr = argv[1];
    const char *input = argv[2];
    char result[MAX_STR_RESULT] = "";

    DEBUG("Parsing: %s\n", expr);
    struct ReToken tokens_infix[MAX_REGEX]; // regex chars with added concat symbols
    struct ReToken *tokens_postfix;
    
    if (tokenize(expr, tokens_infix, MAX_REGEX) == NULL)
        return 1;

    DEBUG("TOKENS:  "); re_debug_reg(tokens_infix);
    //if (re_rewrite_range(tokens_infix, MAX_REGEX) == NULL)
    //    return 1;
    if (re_parse_cclass(tokens_infix, MAX_REGEX) == NULL)
        return 1;


    DEBUG("TOKENS POST CCLASS:  "); re_debug_reg(tokens_infix);
    
    tokens_postfix = re2post(tokens_infix, MAX_REGEX);

    DEBUG("TOKENS:  "); re_debug_reg(tokens_postfix);



    //DEBUG("RANGE:   "); debug_reg(tokens_infix);
    //

    // TODO: problem recides in explicit cat not placing cats correctly
    //if (re_to_explicit_cat(tokens_infix, MAX_REGEX) == NULL)
    //    return 1;

    //DEBUG("CAT:     "); debug_reg(tokens_infix);

    //infix_to_postfix(tokens_infix, tokens_postfix, MAX_REGEX);

    //DEBUG("POSTFIX: "); debug_reg(tokens_postfix);

    struct NFA nfa = nfa_init();
    //struct State *s = nfa_compile_rec(&nfa, tokens_postfix, NULL);
    nfa_compile(&nfa, tokens_postfix);
    re_state_debug(nfa.start, 0);
    //state_debug(s, 0);
    if (re_match(&nfa, input, result, MAX_STR_RESULT))
        DEBUG("RESULT: %s\n", result);
    return 1;
}
