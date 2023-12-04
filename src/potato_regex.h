#ifndef POTATO_REGEX_H
#define POTATO_REGEX_H


#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>


/* Read stuff:
 * OG Ken Thompson: https://dl.acm.org/doi/10.1145/363347.363387
 * Russ Cox: https://swtch.com/~rsc/regexp/regexp1.html
 *
 * Reverse Polish Notation:
 * https://gist.github.com/gmenard/6161825
 * https://gist.github.com/DmitrySoshnikov/1239804/ba3f22f72d7ea00c3a662b900ded98d344d46752
 * https://www.youtube.com/watch?v=QzVVjboyb0s
 */

extern int do_debug;
extern int do_info;
extern int do_error;

#define DEBUG(M, ...) if(do_debug){fprintf(stdout, "[DEBUG] " M, ##__VA_ARGS__);}
#define INFO(M, ...) if(do_info){fprintf(stdout, M, ##__VA_ARGS__);}
#define ERROR(M, ...) if(do_error){fprintf(stderr, "[ERROR] (%s:%d) " M, __FILE__, __LINE__, ##__VA_ARGS__);}

#define MAX_TOKEN_POOL  128
#define MAX_STR_RESULT  128
#define MAX_STATE_POOL  1024
#define MAX_OUT_LIST_POOL  1024
#define MAX_GROUP_STACK 256
#define MAX_STATE_OUT   1024
#define MAX_CCLASS   32

#define PRRESET   "\x1B[0m"
#define PRRED     "\x1B[31m"
#define PRGREEN   "\x1B[32m"
#define PRYELLOW  "\x1B[33m"
#define PRBLUE    "\x1B[34m"
#define PRMAGENTA "\x1B[35m"
#define PRCYAN    "\x1B[36m"
#define PRWHITE   "\x1B[37m"

#define MAX_REGEX 256

#define CONCAT_SYM '&'
#define RE_SPACE_CHARS           " \t"
#define RE_LINE_BREAK_CHARS           "\n\r"


/* Enum is ordered in order of precedence, do not change order!!!
 * Higher number is higher precedence so we can easily compare.
 * Precedence HIGH->LOW: (|&?*+
 * */
enum ReTokenType {
    RE_TOK_TYPE_UNDEFINED = 0,

    // QUANTIFIERS (in order of precedence) don't change this!!!!!
    RE_TOK_TYPE_PLUS,       //  +   GREEDY     match preceding 1 or more times
    RE_TOK_TYPE_STAR,       //  *   GREEDY     match preceding 0 or more times
    RE_TOK_TYPE_QUESTION,   //  ?   NON GREEDY match preceding 1 time            when combined with another quantifier it makes it non greedy
    RE_TOK_TYPE_CONCAT,          // explicit concat symbol
    RE_TOK_TYPE_PIPE,       //  |   OR
    //////////////////////////
                         //
    RE_TOK_TYPE_CCLASS, // [
    RE_TOK_TYPE_CCLASS_NEGATED, // [
    RE_TOK_TYPE_RANGE_START,  // {n}  NON GREEDY match preceding n times
    RE_TOK_TYPE_RANGE_END,    // {n}  NON GREEDY match preceding n times
    RE_TOK_TYPE_GROUP_START,  // (
    RE_TOK_TYPE_GROUP_END,    // )
    RE_TOK_TYPE_CCLASS_START, // [
    RE_TOK_TYPE_CCLASS_END,   // ]
                         
    // OPERATORS
    RE_TOK_TYPE_CARET,        // ^  can be NEGATE|BEGIN
    RE_TOK_TYPE_NEGATE,       // ^
    RE_TOK_TYPE_BEGIN,        // ^

    RE_TOK_TYPE_END,          // $

    RE_TOK_TYPE_BACKSLASH,    // \ backreference, not going to implement
    RE_TOK_TYPE_DOT,          // .    any char except ' '
                          
    RE_TOK_TYPE_CHAR,             // literal char
                         
    RE_TOK_TYPE_DIGIT,            // \d   [0-9]
    RE_TOK_TYPE_NON_DIGIT,        // \D   [^0-9]
    RE_TOK_TYPE_ALPHA_NUM,        // \w   [a-bA-B0-9]
    RE_TOK_TYPE_NON_ALPHA_NUM,    // \W   [^a-bA-B0-9]
    RE_TOK_TYPE_SPACE,            // \s   ' ', \n, \t, \r
    RE_TOK_TYPE_NON_SPACE,        // \S   ^' '
                                  //
    RE_TOK_TYPE_HYPHEN,           // -   (divides a range: [a-z]

    RE_TOK_TYPE_RANGE,              // not a meta char, but represents a range
};

static const char *token_type_table[] = {
    "RE_TOK_TYPE_UNDEFINED",
    "RE_TOK_TYPE_PLUS",       //  +   GREEDY     match preceding 1 or more times
    "RE_TOK_TYPE_STAR",       //  *   GREEDY     match preceding 0 or more times
    "RE_TOK_TYPE_QUESTION",   //  ?   NON GREEDY match preceding 1 time            when combined with another quantifier it makes it non greedy
    "RE_TOK_TYPE_CONCAT",          // explicit concat symbol
    "RE_TOK_TYPE_PIPE",       //  |   OR
    "RE_TOK_TYPE_CCLASS",
    "RE_TOK_TYPE_CCLASS_NEGATED",
    "RE_TOK_TYPE_RANGE_START",  // {n}  NON GREEDY match preceding n times
    "RE_TOK_TYPE_RANGE_END",    // {n}  NON GREEDY match preceding n times
    "RE_TOK_TYPE_GROUP_START",  // (
    "RE_TOK_TYPE_GROUP_END",    // )
    "RE_TOK_TYPE_CCLASS_START", // [
    "RE_TOK_TYPE_CCLASS_END",   // ]
    "RE_TOK_TYPE_CARET",        // ^  can be NEGATE|BEGIN
    "RE_TOK_TYPE_NEGATE",       // ^
    "RE_TOK_TYPE_BEGIN",        // ^
    "RE_TOK_TYPE_END",          // $
    "RE_TOK_TYPE_BACKSLASH",    // \ backreference, not going to implement
    "RE_TOK_TYPE_DOT",          // .    any char except ' '
    "RE_TOK_TYPE_CHAR",             // literal char
    "RE_TOK_TYPE_DIGIT",            // \d   [0-9]
    "RE_TOK_TYPE_NON_DIGIT",        // \D   [^0-9]
    "RE_TOK_TYPE_ALPHA_NUM",        // \w   [a-bA-B0-9]
    "RE_TOK_TYPE_NON_ALPHA_NUM",    // \W   [^a-bA-B0-9]
    "RE_TOK_TYPE_SPACE",            // \s   ' ', \n, \t, \r
    "RE_TOK_TYPE_NON_SPACE",        // \S   ^' '
    "RE_TOK_TYPE_HYPHEN",           // -   (divides a range: [a-z]
    "RE_TOK_TYPE_RANGE"              // not a meta char, but represents a range
};

/* Regex expression is broken up into tokens.
 * The two chars represent things like ranges.
 * In case of character, the c1 is empty.
 * In case of operator, both are empty. */
struct ReToken {
    enum ReTokenType type;
    char c0;
    char c1;

    // Is used in case of a character class. All the chars are stored here
    struct ReToken *next;
};

enum StateType {
    STATE_TYPE_NONE,   // this is a state that is a char or an operator
    STATE_TYPE_MATCH,   // no output
    STATE_TYPE_SPLIT,   // two outputs to next states
};

struct State {
    struct ReToken *t;

    enum StateType type;          // indicate split or match state

    // struct Token *token;
    struct State *out;
    struct State *out1;
    unsigned char is_alloc;       // check if state is allocated
};

/* Holds links to endpoints of state chains that are part of a Group */
struct OutList {
    struct State **s;
    struct OutList *next;
};

/* Holds State chains */
struct Group {
    struct State *start;
    struct OutList *out;
    char is_alloc;
};

struct NFA {
    struct State spool[MAX_STATE_POOL];

    // The first node in the NFA
    struct State *start;
};

struct MatchList {
    struct State *states[256];
    int n;
};

int re_match(struct NFA *nfa, const char *str, char *buf, size_t bufsiz);
struct State* nfa_compile(struct NFA *nfa, struct ReToken *tokens);
void re_state_debug(struct State *s, int level);
struct ReToken* tokenize(const char *expr, struct ReToken *buf, size_t size);
struct NFA nfa_init();
struct ReToken* re2post(struct ReToken *tokens, size_t size);
struct ReToken* re_rewrite_range(struct ReToken *tokens, size_t size);
void re_debug_reg(struct ReToken *tokens);
struct ReToken* re_parse_cclass(struct ReToken *tokens, size_t size);


#endif
