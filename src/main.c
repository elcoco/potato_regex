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
    struct TokenList tokens_infix = re_tokenlist_init();
    struct TokenList tokens_infix_parsed = re_tokenlist_init();
    struct TokenList tokens_postfix = re_tokenlist_init();
    //struct ReToken *tokens_infix[MAX_REGEX]; // regex chars with added concat symbols
    //struct ReToken **tokens_postfix;
    
    if (tokenize(expr, &tokens_infix) == NULL)
        return 1;

    DEBUG("INFIX:   "); re_tokenlist_debug(&tokens_infix);

    if (re_parse_cclass(&tokens_infix, &tokens_infix_parsed) == NULL)
        return 1;

    DEBUG("CCLASS:  "); re_tokenlist_debug(&tokens_infix_parsed);

    if (re2post(&tokens_infix_parsed, &tokens_postfix) == NULL)
        return 1;

    DEBUG("POSTFIX: "); re_tokenlist_debug(&tokens_postfix);

    struct NFA nfa = nfa_init();
    nfa_compile(&nfa, &tokens_postfix);
    re_state_debug(nfa.start, 0);


    // TODO when looking for matches, search cclass linked list
    
    if (re_match(&nfa, input, result, MAX_STR_RESULT))
        DEBUG("RESULT: %s\n", result);
    return 1;
}
