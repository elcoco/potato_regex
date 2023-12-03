#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

int do_debug = 1;
int do_info = 1;
int do_error = 1;

/* Read stuff:
 * OG Ken Thompson: https://dl.acm.org/doi/10.1145/363347.363387
 * Russ Cox: https://swtch.com/~rsc/regexp/regexp1.html
 *
 * Reverse Polish Notation:
 * https://gist.github.com/gmenard/6161825
 * https://gist.github.com/DmitrySoshnikov/1239804/ba3f22f72d7ea00c3a662b900ded98d344d46752
 * https://www.youtube.com/watch?v=QzVVjboyb0s
 */

#define DEBUG(M, ...) if(do_debug){fprintf(stdout, "[DEBUG] " M, ##__VA_ARGS__);}
#define INFO(M, ...) if(do_info){fprintf(stdout, M, ##__VA_ARGS__);}
#define ERROR(M, ...) if(do_error){fprintf(stderr, "[ERROR] (%s:%d) " M, __FILE__, __LINE__, ##__VA_ARGS__);}


#define MAX_STR_RESULT  128
#define MAX_STATE_POOL  1024
#define MAX_OUT_LIST_POOL  1024
#define MAX_GROUP_STACK 256
#define MAX_STATE_OUT   1024
#define MAX_CCLASS   32

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

/* Regex expression is broken up into tokens.
 * The two chars represent things like ranges.
 * In case of character, the c1 is empty.
 * In case of operator, both are empty. */
struct ReToken {
    enum ReTokenType type;
    char c0;
    char c1;
    unsigned char negated;
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

static char* re_token_to_str(struct  ReToken *t, char *buf, size_t size);

struct State* nfa_state_init(struct NFA *nfa, struct ReToken *t, enum StateType type, struct State *s_out, struct State *s_out1)
{
    /* Find unused state in pool */
    struct State *s = nfa->spool;
    for (int i=0 ; i<MAX_STATE_POOL ; i++, s++) {
        if (!s->is_alloc) {
            s->is_alloc = 1;
            s->t = t;
            s->out = s_out;
            s->out1 = s_out1;
            s->type = type;
            return s;
        }
    }
    ERROR("Max states reached: %d\n", MAX_STATE_POOL);
    return NULL;
}

void state_debug(struct State *s, int level)
{
    const int spaces = 2;

    if (s == NULL) {
        //printf("-> NULL\n");
        return;
    }
    for (int i=0 ; i<level*spaces ; i++)
        printf(" ");

    switch (s->type) {
        case STATE_TYPE_MATCH:
            printf("MATCH!\n");
            return;
        case STATE_TYPE_SPLIT:
            printf("SPLIT: [%d] '%c'\n", s->t->type, s->t->c0);

            // don't follow start and plus because that would create endless loop
            if (s->t->type == RE_TOK_TYPE_PLUS || s->t->type == RE_TOK_TYPE_STAR) {
                for (int i=0 ; i<level*spaces ; i++)
                    printf(" ");
                printf("NOT FOLLOWING: %c\n", s->out->t->c0);
                state_debug(s->out1, level+1);
                return;
            }
            break;
        default:
            if (s->t->negated)
                printf("State: !");
            else
                printf("State: ");

            char buf[16] = "";
            printf("%s\n", re_token_to_str(s->t, buf, sizeof(buf)));
            break;

    }
    state_debug(s->out, level+1);
    state_debug(s->out1, level+1);
}

void stack_debug(struct Group *stack, size_t size)
{
    DEBUG("** STACK ***********************\n");
    for (int i=0 ; i<size ; i++) {
        if (stack[i].start == NULL)
            break;
        DEBUG("%d => %c\n", i, stack[i].start->t->c0);
    }
}

struct NFA nfa_init() {
    struct NFA nfa;
    memset(&nfa, 0, sizeof(struct NFA));
    return nfa;
}

struct OutList* ol_init(struct OutList *l, struct State **s)
{
    l->s = s;
    l->next = NULL;
    return l;
}

struct Group group_init(struct NFA *nfa, struct State *s_start, struct OutList *out)
{
    /*                     GROUP
     *            -------------------------
     *            |                       |
     *            |    ---------------    |    OUT LINKED LIST
     *    START   |    |         out |----|---->X
     *      X<----|    |    STATE    |    |
     *            |    |        out1 |->X |
     *            |    ---------------    |
     *            |                       |
     *            -------------------------
     */
    struct Group g;
    memset(&g, 0, sizeof(struct Group));
    g.start = s_start;

    // pointer to first item in linked list
    g.out = out;
    return g;
}

void group_patch_outlist(struct Group *g, struct State **s)
{
    /* Tie all State end pointers in outlist to s.
     * Effectively connecting endpoints in group to other group */
    struct OutList *lp = g->out;
    while (lp != NULL) {
        *(lp->s) = *s;
        lp = lp->next;
    }
}

struct OutList* outlist_join(struct OutList *l0, struct OutList *l1)
{
    /* Joint out of  to start of g1 */
    struct OutList *bak = l0;
    while (l0->next != NULL)
        l0 = l0->next;

    l0->next = l1;
    return bak;
}

struct State* nfa_compile(struct NFA *nfa, struct ReToken *tokens)
{
    /* Create NFA from pattern */

    // Groups are chained states
    // The group can be treated as a black box with one start point
    // and many out points.
    // This is how we can split states.
    // The out points need to be connected to a start point
    // of the next group
    // This will create a tree structure
    //
    // Groups are pushed onto the stack. We wait for a meta char
    // and then decide how the group should be treated
    // When done, the group is pushed back to the stack

    // A normal char pushes a group onto the stack
    // A meta char pops one or two groups from the stack
    struct Group stack[MAX_GROUP_STACK];
    struct OutList lpool[MAX_OUT_LIST_POOL];
    struct Group *stackp = stack;
    struct Group g, g0, g1; // the paths groups take from stack

    struct State *s;
    struct OutList *outlistp = lpool;
    struct OutList *l;

    #define PUSH(S) *stackp++ = S
    #define POP()   *--stackp
    #define GET_OL() outlistp++

    //DEBUG("Compiling pattern: '%s'\n", pattern);

    for (struct ReToken *t=tokens ; t->type != RE_TOK_TYPE_UNDEFINED ; t++) {

        //for (int i=0 ; i<stackp-stack ; i++) {
        //    DEBUG("GROUP: %d ******************************\n", i);
        //    state_debug(stack[i].start, 0);
        //}

        switch (t->type) {
            case RE_TOK_TYPE_CONCAT:       // concat
                g1 = POP();
                g0 = POP();
                DEBUG("TYPE: CONCAT\n");
                group_patch_outlist(&g0, &g1.start);
                g = group_init(nfa, g0.start, g1.out);
                PUSH(g);
                break;
            case RE_TOK_TYPE_QUESTION:       // zero or one
                g = POP();
                DEBUG("TYPE: QUESTION: %c\n", g.start->t->c0);
                s = nfa_state_init(nfa, t, STATE_TYPE_SPLIT, g.start, NULL);
                l = ol_init(GET_OL(), &s->out1);
                l = outlist_join(g.out, l);
                g = group_init(nfa, s, l);
                PUSH(g);
                break;
            case RE_TOK_TYPE_PIPE:       // alternate
                g1 = POP();
                g0 = POP();
                DEBUG("TYPE: PIPE:   %c | %c\n", g0.start->t->c0, g1.start->t->c0);
                s = nfa_state_init(nfa, t, STATE_TYPE_SPLIT, g0.start, g1.start);
                l = outlist_join(g0.out, g1.out);
                PUSH(group_init(nfa, s, l));

                break;
            case RE_TOK_TYPE_STAR:       // zero or more
                g = POP();
                DEBUG("TYPE: STAR:   %c\n", g.start->t->c0);
                s = nfa_state_init(nfa, t, STATE_TYPE_SPLIT, g.start, NULL);
                group_patch_outlist(&g, &s);
                l = ol_init(GET_OL(), &s->out1);
                PUSH(group_init(nfa, g.start, l));
                break;
            case RE_TOK_TYPE_PLUS:       // one or more
                g = POP();
                DEBUG("TYPE: PLUS:   %c\n", g.start->t->c0);
                s = nfa_state_init(nfa, t, STATE_TYPE_SPLIT, g.start, NULL);
                group_patch_outlist(&g, &s);
                l = ol_init(GET_OL(), &s->out1);
                PUSH(group_init(nfa, s, l));
                break;
            default:        // it is a normal character
                DEBUG("TYPE: CHAR:   %c\n", t->c0);
                s = nfa_state_init(nfa, t, STATE_TYPE_NONE, NULL, NULL);
                l = ol_init(GET_OL(), &s->out);
                g = group_init(nfa, s, l);
                PUSH(g);
                break;
        }
    }

    stack_debug(stack, MAX_GROUP_STACK);

    g = POP();

    // connect last state that indicates a succesfull match
    struct State *match_state = nfa_state_init(nfa, NULL, STATE_TYPE_MATCH, NULL, NULL);

    group_patch_outlist(&g, &match_state);

    //state_debug(g.start, 0);
    nfa->start = g.start;

    return g.start;

    #undef POP
    #undef PUSH
    #undef GET_OL
}

struct State* nfa_compile_rec(struct NFA *nfa, struct ReToken *tokens, struct State *s_prev)
{
    struct ReToken *t = &tokens[0];
    if (t->type == RE_TOK_TYPE_UNDEFINED) {
        DEBUG("END OF TOKENS\n");
        return NULL;
    }

    struct State *s_cur;
    struct State *s_next;
    //struct State *s = nfa_state_init(nfa, t, STATE_TYPE_SPLIT, NULL, NULL);

    switch (t->type) {
        case RE_TOK_TYPE_CONCAT:       // concat
            DEBUG("TYPE: CONCAT\n");
            s_cur = nfa_state_init(nfa, t, STATE_TYPE_SPLIT, NULL, NULL);
            s_next = nfa_compile_rec(nfa, tokens+1, s_cur);
            s_cur->out = s_next;
            //s_cur->out1 = s_prev;
            return s_cur;
        case RE_TOK_TYPE_QUESTION:       // zero or one
            DEBUG("TYPE: QUESTION\n");
            break;
        case RE_TOK_TYPE_PIPE:       // alternate
            DEBUG("TYPE: PIPE\n");
            break;
        case RE_TOK_TYPE_STAR:       // zero or more
            DEBUG("TYPE: STAR\n");
            break;
        case RE_TOK_TYPE_PLUS:       // one or more
            DEBUG("TYPE: PLUS\n");
            break;
        default:        // it is a normal character
            DEBUG("TYPE: CHAR\n");
            s_cur = nfa_state_init(nfa, t, STATE_TYPE_NONE, NULL, NULL);
            s_next = nfa_compile_rec(nfa, tokens+1, s_cur);
            s_cur->out = s_next;
            return s_cur;
    }
}


struct ReToken* re2post(struct ReToken *tokens, size_t size)
{
    struct ReToken out[size];
    memset(&out, 0, sizeof(struct ReToken) * size);
    struct ReToken *outp = out;
    struct ReToken tcat = {.type=RE_TOK_TYPE_CONCAT, .c0=CONCAT_SYM, .negated=0};
    struct ReToken tpipe = {.type=RE_TOK_TYPE_PIPE, .c0='|', .negated=0};

	int nalt, natom;
	static char buf[8000];

	struct {
		int nalt;
		int natom;
	} paren[100], *p;
	
	p = paren;
	nalt = 0;
	natom = 0;

	//if (strlen(re) >= sizeof buf/2)
//		return NULL;

	//for (; *re; re++) {
	for (struct ReToken *t=tokens; t->type != RE_TOK_TYPE_UNDEFINED; t++) {

		switch(t->type) {

            case RE_TOK_TYPE_GROUP_START:
                if (natom > 1){
                    --natom;
                    *outp++ = tcat;
                }
                if (p >= paren+100)
                    return NULL;
                p->nalt = nalt;
                p->natom = natom;
                p++;
                nalt = 0;
                natom = 0;
                break;
            case RE_TOK_TYPE_PIPE:
                if (natom == 0)
                    return NULL;
                while (--natom > 0)
                    *outp++ = tcat;
                nalt++;
                break;
            case RE_TOK_TYPE_GROUP_END:
                if (p == paren)
                    return NULL;
                if (natom == 0)
                    return NULL;
                while (--natom > 0)
                    *outp++ = tcat;
                for (; nalt > 0; nalt--)
                    *outp++ = tpipe;
                --p;
                nalt = p->nalt;
                natom = p->natom;
                natom++;
                break;
            case RE_TOK_TYPE_STAR:
            case RE_TOK_TYPE_PLUS:
            case RE_TOK_TYPE_QUESTION:
                if (natom == 0)
                    return NULL;
                *outp++ = *t;
                break;
            default:
                if (natom > 1){
                    --natom;
                    *outp++ = tcat;
                }
                *outp++ = *t;
                natom++;
                break;
            }
	}
	if (p != paren)
		return NULL;
	while (--natom > 0)
		*outp++ = tcat;
	for (; nalt > 0; nalt--)
		*outp++ = tpipe;

    memcpy(tokens, out, size);
	return tokens;
}

struct ReToken re_str_to_token(const char **s)
{
    /* Reads first meta char from string and convert to Token struct.
     * If one char meta or char, increment pointer +1
     * If two char meta, increment pointer +2 */
    assert(strlen(*s) > 0);

    struct ReToken tok;

    char c = **s;

    if (strlen(*s) > 1 && **s == '\\') {
        c = *((*s)+1);
        (*s)+=2;
        tok.c0 = c;
        switch (c) {
            case 'd':
                tok.type = RE_TOK_TYPE_DIGIT;
                break;
            case 'D':
                tok.type = RE_TOK_TYPE_NON_DIGIT;
                break;
            case 'w':
                tok.type = RE_TOK_TYPE_ALPHA_NUM;
                break;
            case 'W':
                tok.type = RE_TOK_TYPE_ALPHA_NUM;
                break;
            case 's':
                tok.type = RE_TOK_TYPE_SPACE;
                break;
            case 'S':
                tok.type = RE_TOK_TYPE_NON_SPACE;
                break;
            default:
                tok.type = RE_TOK_TYPE_CHAR;
                break;
        }
    }

    else {
        tok.c0 = c;
        (*s)++;
        switch (c) {
            case '*':
                tok.type = RE_TOK_TYPE_STAR;
                break;
            case '+':
                tok.type = RE_TOK_TYPE_PLUS;
                break;
            case '?':
                tok.type = RE_TOK_TYPE_QUESTION;
                break;
            case '{':
                tok.type = RE_TOK_TYPE_RANGE_START;
                break;
            case '}':
                tok.type = RE_TOK_TYPE_RANGE_END;
                break;
            case '(':
                tok.type = RE_TOK_TYPE_GROUP_START;
                break;
            case ')':
                tok.type = RE_TOK_TYPE_GROUP_END;
                break;
            case '[':
                tok.type = RE_TOK_TYPE_CCLASS_START;
                break;
            case ']':
                tok.type = RE_TOK_TYPE_CCLASS_END;
                break;
            case '|':
                tok.type = RE_TOK_TYPE_PIPE;
                break;
            case '\\':
                tok.type = RE_TOK_TYPE_BACKSLASH;
                break;
            // decide between BEGIN and NEGATE
            case '^':
                tok.type = RE_TOK_TYPE_CARET;
                break;
            case '$':
                tok.type = RE_TOK_TYPE_END;
                break;
            case '-':
                tok.type = RE_TOK_TYPE_HYPHEN;
                break;
            case '.':
                tok.type = RE_TOK_TYPE_DOT;
                break;
            case CONCAT_SYM:
                tok.type = RE_TOK_TYPE_CONCAT;
                break;
            default:
                tok.type = RE_TOK_TYPE_CHAR;
                break;
        }
    }
    return tok;
}

static char* re_token_to_str(struct  ReToken *t, char *buf, size_t size)
{
    /* Get string representation of token */
    switch (t->type) {
        case RE_TOK_TYPE_CONCAT:
            snprintf(buf, size, "%c", CONCAT_SYM);
            break;
        case RE_TOK_TYPE_GROUP_START:
            strncat(buf, "(", size);
            break;
        case RE_TOK_TYPE_GROUP_END:
            strncat(buf, ")", size);
            break;
        case RE_TOK_TYPE_RANGE:
            snprintf(buf, size, "%c-%c", t->c0, t->c1);
            break;
        case RE_TOK_TYPE_STAR:
            strncat(buf, "*", size);
            break;
        case RE_TOK_TYPE_PLUS:
            strncat(buf, "+", size);
            break;
        case RE_TOK_TYPE_QUESTION:
            strncat(buf, "?", size);
            break;
        case RE_TOK_TYPE_PIPE:
            strncat(buf, "|", size);
            break;
        case RE_TOK_TYPE_ALPHA_NUM:
            strncat(buf, "\\w", size);
            break;
        case RE_TOK_TYPE_DIGIT:
            strncat(buf, "\\d", size);
            break;
        case RE_TOK_TYPE_SPACE:
            strncat(buf, "\\s", size);
            break;
        case RE_TOK_TYPE_DOT:
            strncat(buf, ".", size);
            break;
        default:
            snprintf(buf, size, "%c", t->c0);
            break;
    }
    return buf;
}

void debug_reg(struct ReToken *tokens)
{
    /* Print out token array */
    struct ReToken *t = tokens;
    while (t->type != RE_TOK_TYPE_UNDEFINED) {

        if (t != tokens)
            printf(" ");

        if (t->negated)
            printf("!");

        char buf[16] = "";
        printf("%s", re_token_to_str(t, buf, 16));
        t++;
    }
    printf("\n");
}

struct ReToken* tokenize(const char *expr, struct ReToken *buf, size_t size)
{
    struct ReToken *p_out = buf;
    const char **p_in = &expr;
    int i = 0;

    while (strlen(*p_in)) {
        struct ReToken t = re_str_to_token(p_in);

        assert(t.type != RE_TOK_TYPE_UNDEFINED);
        if (i >= size) {
            ERROR("Max tokensize reached: %ld\n", size);
            return NULL;
        }
        *p_out++ = t;
        i++;
    }
    return buf;
}

struct ReToken* re_rewrite_range(struct ReToken *tokens, size_t size)
{
    /* Extract range from tokens and rewrite to group.
     * This makes it way easier to do the reverse Polish notation algorithm later.
     *
     * eg: [a-zA-Zbx] -> (range_token | range_token | b | x)
     * 
     * if negated:
     *     [^a-zXC] => (!range_token && !X && !C)
     *
     * Algorithm:
     * for TOKEN in TOKENS
     *     TOKEN_TOP_STACK = pop from stack
     *     if TOKEN_TOP_STACK is hyphen
     *         TOKEN_RANGE_START = pop from stack
     *         create TOKEN_RANGE (TOKEN_RANGE_START - TOKEN)
     *         push TOKEN_RANGE to output array
     *     else
     *         push TOKEN to stack
     */
    struct ReToken *t = tokens;
    struct ReToken out_buf[size];
    struct ReToken *t_out = out_buf;

    memset(&out_buf, 0, sizeof(struct ReToken) * size);

    // Temporary buffer for tokens inside cclass
    struct Cclass {
        unsigned char in_cclass;
        struct ReToken *tokens[MAX_CCLASS];
        int size;
        unsigned char is_negated;
    } cclass;

    struct ReToken *stack[size];

    #define STACK_IS_EMPTY() (stackp == stack)
    #define PUSH(S) *stackp++ = S
    #define POP()   *--stackp
    #define RESET_CCLASS() memset(&cclass, 0, sizeof(struct Cclass))
    RESET_CCLASS();

    for (; t->type != RE_TOK_TYPE_UNDEFINED ; t++) {
        if (t->type == RE_TOK_TYPE_CCLASS_START) {
            cclass.in_cclass = 1;
        }
        else if (cclass.in_cclass && t->type == RE_TOK_TYPE_CARET) {
            DEBUG("Negate all chars in character class\n");
            cclass.is_negated = 1;
        }
        // Cclass end found. Now convert tokens in temporary buffer to a group so it is easier to do RPN later
        else if (t->type == RE_TOK_TYPE_CCLASS_END) {
            *t_out++ = (struct ReToken){.type=RE_TOK_TYPE_GROUP_START, .negated=0};
            struct ReToken **stackp = stack;
            struct ReToken **cclassp = cclass.tokens;

            for (int i=0 ; i<cclass.size ; i++, cclassp++) {

                struct ReToken *t0 = POP();
                if (STACK_IS_EMPTY() &&  t0->type == RE_TOK_TYPE_HYPHEN) {
                    ERROR("Malformed range\n");
                    return NULL;
                }

                // Check if we're dealing with a range in format: [a-z]
                if (t0->type == RE_TOK_TYPE_HYPHEN) {
                    struct ReToken *t1 = POP();
                    *t_out++ = (struct ReToken){.type=RE_TOK_TYPE_RANGE, .c0=t1->c0, .c1=(*cclassp)->c0, .negated=cclass.is_negated};

                    if (i > 0 && !cclass.is_negated)
                        *t_out++ = (struct ReToken){.type=RE_TOK_TYPE_PIPE, .negated=0};
                }
                // It's not a range so we can easily rewrite: [abc] == (a|b|c)
                else {
                    PUSH(t0);
                    PUSH(*cclassp);
                }
            }

            // empty stack into output buffer
            while (!STACK_IS_EMPTY()) {
                struct ReToken *tmp = POP();
                tmp->negated = cclass.is_negated;
                *t_out++ = *tmp;
                if (!cclass.is_negated)
                    *t_out++ = (struct ReToken){.type=RE_TOK_TYPE_PIPE, .negated=0};
            }

            // remove extra pipe
            if (!cclass.is_negated)
                t_out--;
            *t_out++ = (struct ReToken){.type=RE_TOK_TYPE_GROUP_END, .negated=0};
            RESET_CCLASS();
        }
        // Add token to temporary cclass buffer
        else if (cclass.in_cclass) {
            cclass.tokens[cclass.size++] = t;
        }
        else {
            *t_out++ = *t;
        }
    }
    memcpy(tokens, out_buf, size*sizeof(struct ReToken));
    return tokens;

    #undef STACK_IS_EMPTY
    #undef PUSH
    #undef POP
    #undef RESET_CCLASS
}

struct ReToken* re_to_explicit_cat(struct ReToken *tokens, size_t size)
{
    /* Put in explicit cat tokens */
    struct ReToken out_buf[size];
    struct ReToken *outp = out_buf;
    struct ReToken *t0 = tokens;

    // return NULL in case of a buffer overflow
    #define PUSH_OR_RETURN(S) if (outp-out_buf >= size) { ERROR("Failed to add concat symbols, buffer is full: %ld\n", size); return NULL; } else { *outp++ = S; }

    memset(&out_buf, 0, sizeof(struct ReToken) * size);

    for (; t0->type != RE_TOK_TYPE_UNDEFINED ; t0++) {

        PUSH_OR_RETURN(*t0);
        struct ReToken *t1 = t0+1;

        if (t1->type != RE_TOK_TYPE_UNDEFINED) {

            if (t0->type != RE_TOK_TYPE_GROUP_START &&
                t1->type != RE_TOK_TYPE_GROUP_END &&
                t1->type != RE_TOK_TYPE_PIPE &&
                t1->type != RE_TOK_TYPE_QUESTION &&
                t1->type != RE_TOK_TYPE_PLUS &&
                t1->type != RE_TOK_TYPE_STAR &&
                t1->type != RE_TOK_TYPE_CARET &&
                t0->type != RE_TOK_TYPE_CARET &&
                t0->type != RE_TOK_TYPE_PIPE) {
                    if (outp-out_buf >= size) {
                        ERROR("Failed to add concat symbols, buffer is full: %ld\n", size);
                        return NULL;
                    }
                    else {
                        *outp++ = (struct ReToken){.type=RE_TOK_TYPE_CONCAT, .negated=0};
                    }
            }
        }
    }
    memcpy(tokens, out_buf, size*sizeof(struct ReToken));
    return tokens;

    #undef PUSH_OR_RETURN
}

int infix_to_postfix(struct ReToken *tokens, struct ReToken *buf, size_t size)
{
    // USING:
    //   intermediate operator stack
    //   output queue
    //   input array
    //
    // OP precedence: (|&?*+^
    //
    // Read TOKEN
    //   if LETTER => add to queue
    //   if OP
    //     while OP on top of stack with grater precedence
    //       pop OP from stack into output queue
    //     push OP onto stack
    //   if LBRACKET => push onto stack
    //   if RBRACKET
    //     while not LBRACKET on top of stack
    //       pop OP from stack into output queue
    //      pop LBRACKET from stack and throw away
    //
    // when done
    //   while stack not empty
    //     pop OP from stack to queue
    //

    struct ReToken opstack[MAX_REGEX];    // intermediate storage stack for operators
    memset(opstack, 0, sizeof(struct ReToken) * MAX_REGEX);
    memset(buf, 0, sizeof(struct ReToken) * size);

    struct ReToken *outqp = buf;
    struct ReToken *stackp = opstack;
    struct ReToken op;
    struct ReToken *t = tokens;

    // track how many pipes we've seen and if they're in () or not
    int op_pipe = 0;
    int in_group = 0;

    #define PUSH(S) *stackp++ = S
    #define POP()   *--stackp
    #define PUSH_OUT(S) *outqp++ = S

    while (t->type != RE_TOK_TYPE_UNDEFINED) {
        switch (t->type) {
            case RE_TOK_TYPE_GROUP_START:
                PUSH(*t);
                in_group = 1;
                break;
            case RE_TOK_TYPE_GROUP_END:
                while ((op = POP()).type != RE_TOK_TYPE_GROUP_START)
                    PUSH_OUT(op);
                for (int i=0 ; i<op_pipe ; i++) {
                    struct ReToken t_pipe = {.type=RE_TOK_TYPE_PIPE, .negated=0};
                    PUSH_OUT(t_pipe);
                }
                op_pipe = 0;
                in_group = 0;
                break;
            case RE_TOK_TYPE_PIPE:
                op_pipe++;
                break;
            case RE_TOK_TYPE_CONCAT:
                PUSH(*t);
                break;
            case RE_TOK_TYPE_STAR:
            case RE_TOK_TYPE_PLUS:
            case RE_TOK_TYPE_QUESTION:
                while (stackp != opstack) {
                    op = POP();

                    // check precedence (operators are ordered in enum)
                    if (op.c0 >= t->c0) {
                        PUSH(op);
                        break;
                    }
                    PUSH_OUT(op);
                }
                PUSH_OUT(*t);
                break;
            default:
                PUSH_OUT(*t);
                break;
        }
        t++;
    }

    // empty stack into out queue
    while (stackp != opstack)
        PUSH_OUT(POP());

    // put pipes
    for (int i=0 ; i<op_pipe ; i++) {
        struct ReToken t_pipe = {.type=RE_TOK_TYPE_PIPE, .negated=0};
        PUSH_OUT(t_pipe);
    }

    return 0;

    #undef PUSH
    #undef POP
    #undef PUSH_OUT
}

int re_is_match(struct State *s)
{
    return s->type == STATE_TYPE_MATCH;
}

int re_is_endpoint(struct State *s)
{
    return s->out == NULL && s->out1 == NULL;
}

struct MatchList {
    struct State *states[256];
    int n;
};

struct MatchList re_match_list_init()
{
    struct MatchList l;
    memset(&l, 0, sizeof(struct MatchList));
    l.n = 0;
    return l;
}

void re_match_list_append(struct MatchList *l, struct State *s)
{
    if (s == NULL)
        return;

    if (s->type == STATE_TYPE_SPLIT) {
        re_match_list_append(l, s->out);
        re_match_list_append(l, s->out1);
    }
    else {
        l->states[l->n] = s;
        l->n++;
    }
}

void debug_match_list(struct MatchList *l)
{
    struct State *s = *(l->states);
    for (int i=0 ; i<l->n ; i++, s++) {
        if (l->states[i]->type == STATE_TYPE_MATCH) {
            DEBUG("MATCHLIST: [%d] MATCH\n", i);
        }
        else {
            DEBUG("MATCHLIST: [%d] %c\n", i, l->states[i]->t->c0);
            //DEBUG("MATCHLIST: [%d] %c\n", i, s->t->c0);
        }
    }
    DEBUG("\n");
}

int re_is_digit(char c)
{
    return c >= '0' && c <= '9';
}

int re_is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

int re_is_upper(char c)
{
    return c >= 'A' && c <= 'Z';
}

int re_is_lower(char c)
{
    return c >= 'a' && c <= 'z';
}

int re_is_whitespace(char c)
{
    for (int i=0 ; i<strlen(RE_SPACE_CHARS) ; i++) {
        if (c == RE_SPACE_CHARS[i])
            return 1;
    }
    return 0;
}

int re_is_linebreak(char c)
{
    for (int i=0 ; i<strlen(RE_LINE_BREAK_CHARS) ; i++) {
        if (c == RE_LINE_BREAK_CHARS[i])
            return 1;
    }
    return 0;
}

int re_is_in_range(char c, char lc, char rc)
{
    // check range validity
    if (re_is_digit(lc) != re_is_digit(rc)) {
        ERROR("Bad range: %c-%c\n", lc, rc);
        return -1;
    }
    if (re_is_alpha(lc) && (re_is_lower(lc) != re_is_lower(rc))) {
        ERROR("Bad alpha range: %c-%c\n", lc, rc);
        return -1;
    }
    return c >= lc && c <= rc;
}

int re_match_list_has_token(struct MatchList *clist, struct MatchList *nlist, char c)
{
    /* Look for state->t that match given char. Add matches to nlist.
     * Returns amount of matches. */
    struct State *s = clist->states[0];

    for (int i=0 ; i<clist->n ; i++, s++) {
        switch (s->t->type) {
            case RE_TOK_TYPE_RANGE:
                if (re_is_in_range(c, s->t->c0, s->t->c1)) {
                    DEBUG("IN RANGE: %c > %c < %c\n", s->t->c0, c, s->t->c1);
                    re_match_list_append(nlist, clist->states[i]->out);
                    re_match_list_append(nlist, clist->states[i]->out1);
                }
                break;
            case RE_TOK_TYPE_DOT:
                if (!re_is_linebreak(c)) {
                    DEBUG("IS DOT (ANY)\n");
                    re_match_list_append(nlist, clist->states[i]->out);
                    re_match_list_append(nlist, clist->states[i]->out1);
                }
                break;
            case RE_TOK_TYPE_SPACE:
                if (re_is_whitespace(c)) {
                    DEBUG("IS WHITESPACE\n");
                    re_match_list_append(nlist, clist->states[i]->out);
                    re_match_list_append(nlist, clist->states[i]->out1);
                }
                break;
            case RE_TOK_TYPE_NON_SPACE:
                if (!re_is_whitespace(c)) {
                    DEBUG("IS NOT WHITESPACE\n");
                    re_match_list_append(nlist, clist->states[i]->out);
                    re_match_list_append(nlist, clist->states[i]->out1);
                }
                break;
            case RE_TOK_TYPE_ALPHA_NUM:
                if (re_is_alpha(c)) {
                    DEBUG("IS ALPHA NUMERIC\n");
                    re_match_list_append(nlist, clist->states[i]->out);
                    re_match_list_append(nlist, clist->states[i]->out1);
                }
                break;
            case RE_TOK_TYPE_NON_ALPHA_NUM:
                if (!re_is_alpha(c)) {
                    DEBUG("IS NOT ALPHA NUMERIC\n");
                    re_match_list_append(nlist, clist->states[i]->out);
                    re_match_list_append(nlist, clist->states[i]->out1);
                }
                break;
            case RE_TOK_TYPE_DIGIT:
                if (re_is_digit(c)) {
                    DEBUG("IS DIGIT\n");
                    re_match_list_append(nlist, clist->states[i]->out);
                    re_match_list_append(nlist, clist->states[i]->out1);
                }
                break;
            case RE_TOK_TYPE_NON_DIGIT:
                if (!re_is_digit(c)) {
                    DEBUG("IS NOT DIGIT\n");
                    re_match_list_append(nlist, clist->states[i]->out);
                    re_match_list_append(nlist, clist->states[i]->out1);
                }
                break;
            case RE_TOK_TYPE_CHAR:
                if (!s->t->negated && s->t->c0 == c) {
                    DEBUG("FOUND CHAR: '%c'\n", clist->states[i]->t->c0);
                    re_match_list_append(nlist, clist->states[i]->out);
                    re_match_list_append(nlist, clist->states[i]->out1);
                }
                if (s->t->negated && s->t->c0 != c) {
                    DEBUG("FOUND NEGATED CHAR: '%c'\n", clist->states[i]->t->c0);
                    re_match_list_append(nlist, clist->states[i]->out);
                    re_match_list_append(nlist, clist->states[i]->out1);
                }
                break;
            default:
                DEBUG("UNHANDLED: '%c'\n", clist->states[i]->t->c0);
                break;
        }
    }
    return nlist->n;
}

struct State* re_match_list_has_match(struct MatchList *l)
{
    struct State *s = l->states[0];
    for (int i=0 ; i<l->n ; i++, s++) {
        if (l->states[i]->type == STATE_TYPE_MATCH)
            return s;
    }
    return NULL;
}

int re_match(struct NFA *nfa, const char *str, char *buf, size_t bufsiz)
{
    const char *c = str;

    int i = 0;

    // this is where we record the states
    struct MatchList l0 = re_match_list_init();
    struct MatchList l1 = re_match_list_init();

    struct MatchList *clist = &l0;
    struct MatchList *nlist = &l1;
    struct MatchList *bak;

    // add first node
    re_match_list_append(clist, nfa->start);

    for (; *c ; c++) {
        DEBUG("CUR CHAR: '%c'\n", *c);
        *nlist = re_match_list_init();

        if (re_match_list_has_token(clist, nlist, *c)) {

            if (i>=bufsiz-1) {
                ERROR("Ouput buffer full: %d, max=%ld\n", i, bufsiz);
                return -1;
            }

            buf[i++] = *c;
            buf[i] = '\0';

            // switch lists
            bak = clist;
            clist = nlist;
            nlist = bak;

            if (re_match_list_has_match(clist)) {
                DEBUG("WE'VE GOT A MATCH!!!\n");
                return 1;
            }
        }
        else {
            break;
        }
    }
    DEBUG("No Match\n");
    return 0;

}

