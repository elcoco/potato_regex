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
    char result[RE_MAX_STR_RESULT] = "";

    DEBUG("Parsing: %s\n", expr);

    struct Regex re;
    if (re_init(&re, expr) == NULL) {
        ERROR("Failed init\n");
        return 1;
    }

    struct ReMatch m = re_match(&re, input, result, RE_MAX_STR_RESULT);
    if (m.state >= 0) {
        re_match_debug(&m);
    }
    return 1;
}
