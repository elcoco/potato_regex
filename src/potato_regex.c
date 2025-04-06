#include "potato_regex.h"

static struct ReState* re_state_init(struct Regex *re, struct ReToken *t, enum ReStateType type, struct ReState *s_out, struct ReState *s_out1);
static struct OutList* ol_init(struct OutList *l, struct ReState **s);
static struct Group group_init(struct ReState *s_start, struct OutList *out);

static void   group_patch_outlist(struct Group *g, struct ReState **s);
static struct OutList* outlist_join(struct OutList *l0, struct OutList *l1);

//static struct ReToken re_str_to_token(const char **s);
static struct ReToken* re_token_from_str(struct ReToken *tok, const char **s);
static char* re_token_to_str(struct  ReToken *t);
static const char* re_token_type_to_str(enum ReTokenType type);
static int re_match_list_has_token(struct MatchList *clist, struct MatchList *nlist, char c);

static struct ReToken* re_tokenlist_token_init(struct TokenList *tl, enum ReTokenType type);
static int re_token_match_class(struct ReToken *t, char c);
static int re_is_in_range(char c, char lc, char rc);
static int re_token_match_chr(struct ReToken *t, char c);

static struct ReState* re_compile(struct Regex *re, struct TokenList *tl);

struct TokenList infix;
//struct ReToken tpool[RE_MAX_TOKEN_POOL];
//int tpool_n = 0;

static int re_is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int re_is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int re_is_lower(char c)
{
    return c >= 'a' && c <= 'z';
}

static int re_is_whitespace(char c)
{
    for (unsigned int i=0 ; i<strlen(RE_RE_SPACE_CHARS) ; i++) {
        if (c == RE_RE_SPACE_CHARS[i])
            return 1;
    }
    return 0;
}

static int re_is_linebreak(char c)
{
    for (unsigned int i=0 ; i<strlen(RE_RE_LINE_BREAK_CHARS) ; i++) {
        if (c == RE_RE_LINE_BREAK_CHARS[i])
            return 1;
    }
    return 0;
}

static int re_is_in_range(char c, char lc, char rc)
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


/* ///// STATE ///////////////////////////////////////
 * States are chained to form a tree like structure that we can use to match characters to.
 */
static struct ReState* re_state_init(struct Regex *re, struct ReToken *t, enum ReStateType type, struct ReState *s_out, struct ReState *s_out1)
{
    /* Find unused state in pool */
    struct ReState *s = re->spool;
    for (int i=0 ; i<RE_MAX_STATE_POOL ; i++, s++) {
        if (!s->is_alloc) {
            s->is_alloc = 1;
            s->t = t;
            s->out = s_out;
            s->out1 = s_out1;
            s->type = type;
            return s;
        }
    }
    ERROR("Max states reached: %d\n", RE_MAX_STATE_POOL);
    return NULL;
}

void re_state_debug(struct ReState *s, int level)
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
            printf("SPLIT: %s %s\n", re_token_type_to_str(s->t->type), re_token_to_str(s->t));

            // don't follow start and plus because that would create endless loop
            if (s->t->type == RE_TOK_TYPE_PLUS || s->t->type == RE_TOK_TYPE_STAR) {
                for (int i=0 ; i<level*spaces ; i++)
                    printf(" ");
                printf("  RECURSIVE: %s %s\n", re_token_type_to_str(s->out->t->type), re_token_to_str(s->out->t));
                re_state_debug(s->out1, level+1);
                return;
            }
            break;
        default:
            printf("State: ");
            printf("%s %s\n", re_token_type_to_str(s->t->type), re_token_to_str(s->t));
            break;

    }
    re_state_debug(s->out, level+1);
    re_state_debug(s->out1, level+1);
}

/* ///// OUTLIST /////////////////////////////////////////////////
 * The list in a group that holds all state endpoints in a group
 */
static struct OutList* ol_init(struct OutList *l, struct ReState **s)
{
    l->s = s;
    l->next = NULL;
    return l;
}

static struct OutList* outlist_join(struct OutList *l0, struct OutList *l1)
{
    /* Joint out of  to start of g1 */
    struct OutList *bak = l0;
    while (l0->next != NULL)
        l0 = l0->next;

    l0->next = l1;
    return bak;
}


/* ///// GROUP ///////////////////////////////////////////////////
 * Groups are used to create blocks of connected states during the compile process.
 * They are segments that can be joined or extended etc.
 */
static struct Group group_init(struct ReState *s_start, struct OutList *out)
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

static void group_patch_outlist(struct Group *g, struct ReState **s)
{
    /* Tie all State end pointers in outlist to s.
     * Effectively connecting endpoints in group to other group */
    struct OutList *lp = g->out;
    while (lp != NULL) {
        *(lp->s) = *s;
        lp = lp->next;
    }
}


/* ///// MATCH LIST ////////////////////////////////
 * Is used while matching the input string against the NFA state machine.
 * They hold the states that need to be checked against a character
 */
static struct MatchList re_match_list_init()
{
    struct MatchList l;
    memset(&l, 0, sizeof(struct MatchList));
    l.n = 0;
    return l;
}

static void re_match_list_append(struct MatchList *l, struct ReState *s)
{
    if (s == NULL)
        return;

    //DEBUG("MATCHLIST APPEND: %s %s\n", re_token_type_to_str(s->t->type), re_token_to_str(s->t));

    if (s->type == STATE_TYPE_SPLIT) {
        re_match_list_append(l, s->out);
        re_match_list_append(l, s->out1);
    }
    else {
        l->states[l->n] = s;
        l->n++;
    }
}

static int re_match_list_has_token(struct MatchList *clist, struct MatchList *nlist, char c)
{
    /* Look for state->t that match given char. Add matches to nlist.
     * Returns amount of matches. */
    struct ReState **s = clist->states;

    for (int i=0 ; i<clist->n ; i++, s++) {
        //DEBUG("   [%d] TRYING FROM MATCHLIST: %s %s\n", i, re_token_type_to_str((*s)->t->type), re_token_to_str((*s)->t));
        if (re_token_match_chr((*s)->t, c)) {
            DEBUG("  ACCEPTED: %s %s\n", re_token_type_to_str((*s)->t->type), re_token_to_str((*s)->t));
            re_match_list_append(nlist, (*s)->out);
            re_match_list_append(nlist, (*s)->out1);
        }
    }
    return nlist->n;
}

static struct ReState* re_match_list_has_match(struct MatchList *l)
{
    struct ReState **s = l->states;
    for (int i=0 ; i<l->n ; i++, s++) {
        if ((*s)->type == STATE_TYPE_MATCH)
            return *s;
    }
    return NULL;
}


/* ///// TOKENLIST ///////////////////////////////
   Is an array of enums that represent the parsed regex string
*/
struct TokenList re_tokenlist_init()
{
    struct TokenList tl;
    for (int i=0 ; i<RE_MAX_REGEX ; i++) {
        tl.tokens[i] = NULL;
    }
    tl.n = 0;
    return tl;
}

int re_tokenlist_append(struct TokenList *tl, struct ReToken *t)
{
    if (tl->n >= RE_MAX_REGEX) {
        ERROR("List full, max=%d\n", RE_MAX_REGEX);
        return -1;
    }

    tl->tokens[tl->n++] = t;
    return 1;
}

static int re_tokenlist_delete_at_index(struct TokenList *tl, int index)
{
    if (index > tl->n-1) {
        ERROR("Failed to delete token, index out of bounds: %d>%d\n", index, tl->n-1);
        return -1;
    }
    DEBUG("DELETE @ %d -> %s %s\n", index, re_token_type_to_str((*(tl->tokens+index))->type), re_token_to_str(*(tl->tokens+index)));

    for (int i=index ; i<tl->n ; i++) {
        if (i < tl->n-1)
            *(tl->tokens+i) = *(tl->tokens+i+1);
        else
            break;
    }
    tl->n--;
    return 1;
}

static int re_tokenlist_insert_at_index(struct TokenList *tl, int index, struct ReToken *t, int max)
{
    if (tl->n+1 > max) {
        ERROR("Failed to insert token, buffer too small: %d\n", max);
        return -1;
    }
    if (index != 0 && index > tl->n-1) {
        ERROR("Failed to insert token, index out of bounds: %d>%d\n", index, tl->n-1);
        return -1;
    }

    DEBUG("INSERT @ %d -> %s %s\n", index, re_token_type_to_str(t->type), re_token_to_str(t));

    // shift everything at index to the right starting at the end
    for (int i=tl->n-1 ; i>=index ; i--)
        *(tl->tokens+i+1) = *(tl->tokens+i);

    *(tl->tokens + index) = t;
    tl->n++;
    return 1;
}

void re_tokenlist_debug(struct TokenList *tl)
{
    /* Print out token array */
    struct ReToken **t = tl->tokens;
    for (int i=0 ; i<tl->n ; i++, t++) {

        if (t != tl->tokens)
            printf(" ");

        printf("%s", re_token_to_str(*t));
    }
    printf("\n");
}

struct TokenList* re_tokenlist_from_str(const char *expr, struct TokenList *tl)
{
    /* Convert string to tokens */
    const char **p_in = &expr;

    while (strlen(*p_in)) {
        struct ReToken *t = re_tokenlist_token_init(tl, RE_TOK_TYPE_UNDEFINED);
        if (t == NULL)
            return NULL;

        re_token_from_str(t, p_in);

        assert(t->type != RE_TOK_TYPE_UNDEFINED);
        if (re_tokenlist_append(tl, t) < 0)
            return NULL;
    }
    return tl;
}

struct TokenList* re_tokenlist_parse_cclass(struct TokenList *tl)
{
    /* Parse tokens in character class.
     * Create a token of the RE_TOK_TYPE_CCLASS and move all tokens from within the character class
     * into a linked list.
     * If ^ is at start, set RE_TOK_TYPE_CCLASS_NEGATED
     */

    // Keep track of cclass
    struct Cclass {
        struct ReToken *t;
        unsigned char in_cclass;
        int size;
    } cclass;

    #define RESET_CCLASS() memset(&cclass, 0, sizeof(struct Cclass))
    RESET_CCLASS();

    for (int i=0 ; i<tl->n ; i++) {
        struct ReToken *t = *(tl->tokens+i);

        if (t->type == RE_TOK_TYPE_CCLASS_START) {
            cclass.in_cclass = 1;
            if (!re_tokenlist_delete_at_index(tl, i))
                return NULL;
            cclass.t = re_tokenlist_token_init(tl, RE_TOK_TYPE_CCLASS);
            cclass.t->c0 = 'x';
            if (!re_tokenlist_insert_at_index(tl, i, cclass.t, RE_MAX_REGEX))
                return NULL;
        }
        else if (t->type == RE_TOK_TYPE_CCLASS_END) {
            if (!cclass.in_cclass) {
                ERROR("Malformed character class, unexpected ']'\n");
                return NULL;
            }

            if (!re_tokenlist_delete_at_index(tl, i--))
                return NULL;
            RESET_CCLASS();

        }
        else if (cclass.in_cclass && cclass.size == 0 && t->type == RE_TOK_TYPE_CARET) {
            cclass.t->type = RE_TOK_TYPE_CCLASS_NEGATED;
            if (!re_tokenlist_delete_at_index(tl, i--))
                return NULL;
        }
        else if (cclass.in_cclass) {
            // Add token to linked list and remove from tokenlist
            cclass.t->next = t;
            cclass.t = cclass.t->next;
            if (!re_tokenlist_delete_at_index(tl, i))
                return NULL;
            cclass.size++;
            i--;
        }
    }
    if (cclass.in_cclass) {
        ERROR("Failed to find end of character class\n");
        return NULL;
    }
    return tl;
}

struct TokenList* re_tokenlist_to_postfix_bak(struct TokenList *tl)
{
    struct ReToken *tcat = (re_tokenlist_token_init(tl, RE_TOK_TYPE_CONCAT));
    struct ReToken *tpipe = (re_tokenlist_token_init(tl, RE_TOK_TYPE_PIPE));
    if (!tcat || !tpipe)
        return NULL;
    tcat->c0 = RE_CONCAT_SYM;
    tpipe->c0 = '|';

	int npipe, natom;

    // track pipes and cats in group: (...|...)
	struct {
		int npipe;
		int natom;
	} group[100], *p;
	
	p = group;
	npipe = 0;
	natom = 0;

	for (int i=0; i<tl->n; i++) {
        //DEBUG("npipe,natom: %d %d: ", npipe, natom);
        //re_tokenlist_debug(tl);
        struct ReToken *t = *(tl->tokens + i);
		switch(t->type) {

            case RE_TOK_TYPE_GROUP_START:
                DEBUG("GROUP START: %d\n", i);
                if (!re_tokenlist_delete_at_index(tl, i--))
                    return NULL;

                if (natom > 1){
                    --natom;
                    if (!re_tokenlist_insert_at_index(tl, ++i, tcat, RE_MAX_REGEX))
                        return NULL;
                }
                if (p >= group+100) {
                    ERROR("Failed to get group, pool empty!\n");
                    return NULL;
                }
                p->npipe = npipe;
                p->natom = natom;
                p++;
                npipe = 0;
                natom = 0;
                break;
            case RE_TOK_TYPE_PIPE:
                // Remove pipe from tokenlist and either place it at group end or at end of expression
                if (natom == 0) {
                    ERROR("FAIL!\n");
                    return NULL;
                }
                if (!re_tokenlist_delete_at_index(tl, i--))
                    return NULL;

                while (--natom > 0) {
                    if (!re_tokenlist_insert_at_index(tl, ++i, tcat, RE_MAX_REGEX))
                        return NULL;
                }
                npipe++;
                break;
            case RE_TOK_TYPE_GROUP_END:
                if (p == group) {
                    ERROR("Unexpected ')' found!\n");
                    return NULL;
                }
                if (natom == 0)
                    return NULL;
                if (!re_tokenlist_delete_at_index(tl, i--))
                    return NULL;
                // NOTE if deleted, we should not increment i!!!!!!!!!!!

                // put all pipes and cats at end of group
                while (--natom > 0) {
                    if (!re_tokenlist_insert_at_index(tl, ++i, tcat, RE_MAX_REGEX))
                        return NULL;
                }
                for (; npipe > 0; npipe--) {
                    if (!re_tokenlist_insert_at_index(tl, ++i, tpipe, RE_MAX_REGEX))
                        return NULL;
                }
                --p;
                npipe = p->npipe;
                natom = p->natom;
                natom++;
                break;
            case RE_TOK_TYPE_STAR:
            case RE_TOK_TYPE_PLUS:
            case RE_TOK_TYPE_QUESTION:
                if (natom == 0)
                    return NULL;
                break;
            default:
                if (natom > 1){
                    --natom;
                    if (!re_tokenlist_insert_at_index(tl, i++, tcat, RE_MAX_REGEX))
                        return NULL;
                }
                natom++;
                break;
            }
	}
    // all groups should be processed so p should be the first and only group
	if (p != group) {
        ERROR("Group not closed!\n");
		return NULL;
    }
	while (--natom > 0) {
		if (!re_tokenlist_append(tl, tcat))
            return NULL;
    }
	for (; npipe > 0; npipe--) {
		if (!re_tokenlist_append(tl, tpipe))
            return NULL;
    }
	return tl;
}

struct TokenList* re_tokenlist_to_postfix(struct TokenList *tl_in, struct TokenList *tl_out)
{
    struct ReToken *tcat = re_tokenlist_token_init(tl_in, RE_TOK_TYPE_CONCAT);
    struct ReToken *tpipe = re_tokenlist_token_init(tl_in, RE_TOK_TYPE_PIPE);
    if (!tcat || !tpipe)
        return NULL;

    tcat->c0 = RE_CONCAT_SYM;
    tpipe->c0 = '|';

	int nalt, natom;

	struct {
		int nalt;
		int natom;
	} paren[100], *p;
	
	p = paren;
	nalt = 0;
	natom = 0;

    struct ReToken **t = tl_in->tokens;
	for (int i=0; i<tl_in->n; i++,t++) {
        assert((*t)->type != RE_TOK_TYPE_UNDEFINED);
		switch((*t)->type) {

            case RE_TOK_TYPE_GROUP_START:
                if (natom > 1){
                    --natom;
                    if (!re_tokenlist_append(tl_out, tcat))
                        return NULL;
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
                while (--natom > 0) {
                    if (!re_tokenlist_append(tl_out, tcat))
                        return NULL;
                }
                nalt++;
                break;
            case RE_TOK_TYPE_GROUP_END:
                if (p == paren)
                    return NULL;
                if (natom == 0)
                    return NULL;
                while (--natom > 0) {
                    if (!re_tokenlist_append(tl_out, tcat))
                        return NULL;
                }
                for (; nalt > 0; nalt--) {
                    if (!re_tokenlist_append(tl_out, tpipe))
                        return NULL;
                }
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
                if (!re_tokenlist_append(tl_out, *t))
                    return NULL;
                break;
            default:
                if (natom > 1){
                    --natom;
                    if (!re_tokenlist_append(tl_out, tcat))
                        return NULL;
                }
                if (!re_tokenlist_append(tl_out, *t))
                    return NULL;
                natom++;
                break;
            }
	}
	if (p != paren)
		return NULL;
	while (--natom > 0) {
		if (!re_tokenlist_append(tl_out, tcat))
            return NULL;
    }
	for (; nalt > 0; nalt--) {
		if (!re_tokenlist_append(tl_out, tpipe))
            return NULL;
    }

	return tl_out;
}

static struct ReToken* re_tokenlist_token_init(struct TokenList *tl, enum ReTokenType type)
{
    /* Get new token from pool */

    if (tl->pooln +1 > RE_MAX_TOKEN_POOL) {
        ERROR("No more tokens in pool: %d\n", RE_MAX_TOKEN_POOL);
        return NULL;
    }
    struct ReToken *t = tl->pool + tl->pooln++;
    memset(t, 0, sizeof(struct ReToken));
    t->type = type;
    return t;
}


/*
int re_tokenlist_to_postfix_old(struct ReToken *tokens, struct ReToken *buf, size_t size)
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

    struct ReToken opstack[RE_MAX_REGEX];    // intermediate storage stack for operators
    memset(opstack, 0, sizeof(struct ReToken) * RE_MAX_REGEX);
    memset(buf, 0, sizeof(struct ReToken) * size);

    struct ReToken *outqp = buf;
    struct ReToken *stackp = opstack;
    struct ReToken op;
    struct ReToken *t = tokens;

    // track how many pipes we've seen and if they're in () or not
    int op_pipe = 0;

    #define PUSH(S) *stackp++ = S
    #define POP()   *--stackp
    #define PUSH_OUT(S) *outqp++ = S

    while (t->type != RE_TOK_TYPE_UNDEFINED) {
        switch (t->type) {
            case RE_TOK_TYPE_GROUP_START:
                PUSH(*t);
                break;
            case RE_TOK_TYPE_GROUP_END:
                while ((op = POP()).type != RE_TOK_TYPE_GROUP_START)
                    PUSH_OUT(op);
                for (int i=0 ; i<op_pipe ; i++) {
                    struct ReToken *t_pipe = (re_token_init(RE_TOK_TYPE_PIPE));
                    PUSH_OUT(*t_pipe);
                }
                op_pipe = 0;
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
        struct ReToken *t_pipe = (re_token_init(RE_TOK_TYPE_PIPE));
        PUSH_OUT(*t_pipe);
    }

    return 0;

    #undef PUSH
    #undef POP
    #undef PUSH_OUT
}
*/

struct ReToken* re_tokenlist_to_explicit_cat_old(struct ReToken *tokens, size_t size)
{
    /* Put in explicit cat tokens */
    struct ReToken out_buf[size];
    struct ReToken *outp = out_buf;
    struct ReToken *t0 = tokens;

    // return NULL in case of a buffer overflow
    #define PUSH_OR_RETURN(S) if ((long unsigned int)(outp-out_buf) >= size) { ERROR("Failed to add concat symbols, buffer is full: %ld\n", size); return NULL; } else { *outp++ = S; }

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
                    if ((long unsigned int)(outp-out_buf) >= size) {
                        ERROR("Failed to add concat symbols, buffer is full: %ld\n", size);
                        return NULL;
                    }
                    else {
                        *outp++ = (struct ReToken){.type=RE_TOK_TYPE_CONCAT};
                    }
            }
        }
    }
    memcpy(tokens, out_buf, size*sizeof(struct ReToken));
    return tokens;

    #undef PUSH_OR_RETURN
}

///// TOKEN //////////////////////////////////////////////////////////
static const char* re_token_type_to_str(enum ReTokenType type)
{
    static char buf[RE_MAX_TOKEN_TYPE_STR_REPR] = "";
    buf[0]= '\0';
    snprintf(buf, sizeof(buf)-1, "%s%s%s", PRBLUE, token_type_table[type], PRRESET);
    return buf;
}

static char* re_token_to_str(struct  ReToken *t)
{
    /* Get string representation of token */
    assert(t != NULL);
    static char buf[RE_MAX_TOKEN_STR_REPR] = "";
    struct ReToken *tcclass;
    buf[0] = '\0';
    switch (t->type) {
        case RE_TOK_TYPE_CCLASS:
        case RE_TOK_TYPE_CCLASS_NEGATED:
            tcclass = t->next;
            char tmp[RE_MAX_TOKEN_STR_REPR] = "[";
            while (tcclass != NULL) {
                if (tcclass != t->next)
                    strncat(tmp, "->", sizeof(tmp) - strlen(tmp) -1);
                strncat(tmp, re_token_to_str(tcclass), sizeof(tmp) - strlen(tmp) -1);
                tcclass = tcclass->next;
            }
            strncat(tmp, "]", sizeof(tmp) - strlen(tmp) -1);
            memcpy(buf, tmp, sizeof(buf));
            break;
        case RE_TOK_TYPE_CONCAT:
            snprintf(buf, sizeof(buf), "%s%c%s", PRRED, RE_CONCAT_SYM, PRRESET);
            break;
        case RE_TOK_TYPE_GROUP_START:
            snprintf(buf, sizeof(buf), "%s%c%s", PRRED, '(', PRRESET);
            break;
        case RE_TOK_TYPE_GROUP_END:
            snprintf(buf, sizeof(buf), "%s%c%s", PRRED, ')', PRRESET);
            break;
        case RE_TOK_TYPE_RANGE:
            snprintf(buf, sizeof(buf), "%s%c-%c%s", PRRED, t->c0, t->c1, PRRESET);
            break;
        case RE_TOK_TYPE_STAR:
            snprintf(buf, sizeof(buf), "%s%c%s", PRRED, '*', PRRESET);
            break;
        case RE_TOK_TYPE_PLUS:
            snprintf(buf, sizeof(buf), "%s%c%s", PRRED, '+', PRRESET);
            break;
        case RE_TOK_TYPE_QUESTION:
            snprintf(buf, sizeof(buf), "%s%c%s", PRRED, '?', PRRESET);
            break;
        case RE_TOK_TYPE_PIPE:
            snprintf(buf, sizeof(buf), "%s%c%s", PRRED, '|', PRRESET);
            break;
        case RE_TOK_TYPE_ALPHA_NUM:
            snprintf(buf, sizeof(buf), "%s%s%s", PRRED, "\\w", PRRESET);
            break;
        case RE_TOK_TYPE_DIGIT:
            snprintf(buf, sizeof(buf), "%s%s%s", PRRED, "\\d", PRRESET);
            break;
        case RE_TOK_TYPE_SPACE:
            snprintf(buf, sizeof(buf), "%s%s%s", PRRED, "\\s", PRRESET);
            break;
        case RE_TOK_TYPE_DOT:
            snprintf(buf, sizeof(buf), "%s%s%s", PRRED, ".", PRRESET);
            break;
        default:
            snprintf(buf, sizeof(buf), "%s%c%s", PRRED, t->c0, PRRESET);
            break;
    }
    return buf;
}

static int re_token_match_chr(struct ReToken *t, char c)
{
    /* Check if token matches char */
    switch (t->type) {
        case RE_TOK_TYPE_RANGE:
            return re_is_in_range(c, t->c0, t->c1);
        case RE_TOK_TYPE_DOT:
            return !re_is_linebreak(c);
        case RE_TOK_TYPE_SPACE:
            return re_is_whitespace(c);
        case RE_TOK_TYPE_NON_SPACE:
            return !re_is_whitespace(c);
        case RE_TOK_TYPE_ALPHA_NUM:
            return re_is_alpha(c);
        case RE_TOK_TYPE_NON_ALPHA_NUM:
            return !re_is_alpha(c);
        case RE_TOK_TYPE_DIGIT:
            return re_is_digit(c);
        case RE_TOK_TYPE_NON_DIGIT:
            return !re_is_digit(c);
        case RE_TOK_TYPE_CCLASS_NEGATED:
        case RE_TOK_TYPE_CCLASS:
            return re_token_match_class(t, c);
        case RE_TOK_TYPE_CHAR:
            return t->c0 == c;
        default:
            ERROR("UNHANDLED: TYPE: %s, %s\n", re_token_type_to_str(t->type), re_token_to_str(t));
            return 0;
    }
}

static int re_token_match_class(struct ReToken *token, char c)
{
    /* Check char against full character class.
     * Depending on type, it checks for negated or non negated */
    struct ReToken *t = token->next;
    int has_match = 0;
    while (t != NULL) {
        if (re_token_match_chr(t, c)) {
            if (token->type == RE_TOK_TYPE_CCLASS)
                return 1;
            else
                has_match = 1;
        }
        t = t->next;
    }
    if (token->type == RE_TOK_TYPE_CCLASS_NEGATED)
        return !has_match;

    return 0;
}

static struct ReToken* re_token_from_str(struct ReToken *tok, const char **s)
{
    /* Reads first meta char from string and convert to Token struct.
     * If one char meta or char, increment pointer +1
     * If two char meta, increment pointer +2 */
    assert(strlen(*s) > 0);
    assert(tok != NULL);

    //struct ReToken *tok = re_token_init(tl, RE_TOK_TYPE_UNDEFINED);

    char c = **s;

    if (strlen(*s) > 2 && *(*s+1) == '-') {
        tok->type = RE_TOK_TYPE_RANGE;
        tok->c0 = c;
        (*s)+=2;
        tok->c1 = **s;
        (*s)++;
        if (!re_is_in_range(tok->c0, tok->c0, tok->c1))
            return NULL;
    }

    else if (strlen(*s) > 1 && **s == '\\') {
        c = *((*s)+1);
        (*s)+=2;
        tok->c0 = c;
        switch (c) {
            case 'd':
                tok->type = RE_TOK_TYPE_DIGIT;
                break;
            case 'D':
                tok->type = RE_TOK_TYPE_NON_DIGIT;
                break;
            case 'w':
                tok->type = RE_TOK_TYPE_ALPHA_NUM;
                break;
            case 'W':
                tok->type = RE_TOK_TYPE_ALPHA_NUM;
                break;
            case 's':
                tok->type = RE_TOK_TYPE_SPACE;
                break;
            case 'S':
                tok->type = RE_TOK_TYPE_NON_SPACE;
                break;
            default:
                tok->type = RE_TOK_TYPE_CHAR;
                break;
        }
    }

    else {
        tok->c0 = c;
        (*s)++;
        switch (c) {
            case '*':
                tok->type = RE_TOK_TYPE_STAR;
                break;
            case '+':
                tok->type = RE_TOK_TYPE_PLUS;
                break;
            case '?':
                tok->type = RE_TOK_TYPE_QUESTION;
                break;
            case '{':
                tok->type = RE_TOK_TYPE_RANGE_START;
                break;
            case '}':
                tok->type = RE_TOK_TYPE_RANGE_END;
                break;
            case '(':
                tok->type = RE_TOK_TYPE_GROUP_START;
                break;
            case ')':
                tok->type = RE_TOK_TYPE_GROUP_END;
                break;
            case '[':
                tok->type = RE_TOK_TYPE_CCLASS_START;
                break;
            case ']':
                tok->type = RE_TOK_TYPE_CCLASS_END;
                break;
            case '|':
                tok->type = RE_TOK_TYPE_PIPE;
                break;
            case '\\':
                tok->type = RE_TOK_TYPE_BACKSLASH;
                break;
            // decide between BEGIN and NEGATE
            case '^':
                tok->type = RE_TOK_TYPE_CARET;
                break;
            case '$':
                tok->type = RE_TOK_TYPE_END;
                break;
            case '-':
                tok->type = RE_TOK_TYPE_HYPHEN;
                break;
            case '.':
                tok->type = RE_TOK_TYPE_DOT;
                break;
            case RE_CONCAT_SYM:
                tok->type = RE_TOK_TYPE_CONCAT;
                break;
            default:
                tok->type = RE_TOK_TYPE_CHAR;
                break;
        }
    }
    return tok;
}


///// REGEX MAIN STRUCT //////////////////////////////////////////
struct Regex* re_init(struct Regex *re, const char *expr)
{
    /* Initialize main struct.
     * Tokenize expression.
     * Parse tokens in cclass
     * Convert tokens to postfix
     * Compile tokens into NFA
     * ...
     * PROFIT! */

    memset(re, 0, sizeof(struct Regex));

    infix = re_tokenlist_init();
    re->tokens = re_tokenlist_init();

    if (re_tokenlist_from_str(expr, &re->tokens) == NULL)
        return NULL;

    DEBUG("TOKENIZED: ");
    re_tokenlist_debug(&re->tokens);

    if (re_tokenlist_parse_cclass(&re->tokens) == NULL)
        return NULL;


    DEBUG("INFIX: ");
    re_tokenlist_debug(&re->tokens);

    if (re_tokenlist_to_postfix_bak(&re->tokens) == NULL)
        return NULL;

    DEBUG("POSTFIX: ");
    re_tokenlist_debug(&re->tokens);

    if (re_compile(re, &re->tokens) == NULL)
        return NULL;

    DEBUG("NFA:\n");
    re_state_debug(re->start, 0);
    return re;
}

static struct ReState* re_compile(struct Regex *re, struct TokenList *tl)
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
    struct Group stack[RE_MAX_GROUP_STACK];
    struct OutList lpool[RE_MAX_OUT_LIST_POOL];
    struct Group *stackp = stack;
    struct Group g, g0, g1; // the paths groups take from stack

    struct ReState *s;
    struct OutList *outlistp = lpool;
    struct OutList *l;

    #define PUSH(S) *stackp++ = S
    #define POP()   *--stackp
    #define GET_OL() outlistp++

    //DEBUG("Compiling pattern: '%s'\n", pattern);

    struct ReToken **t = tl->tokens;
    for (int i=0 ; i<tl->n ; i++, t++) {

        //for (int i=0 ; i<stackp-stack ; i++) {
        //    DEBUG("GROUP: %d ******************************\n", i);
        //    re_state_debug(stack[i].start, 0);
        //}

        switch ((*t)->type) {
            case RE_TOK_TYPE_CONCAT:       // concat
                g1 = POP();
                g0 = POP();
                group_patch_outlist(&g0, &g1.start);
                g = group_init(g0.start, g1.out);
                PUSH(g);
                break;
            case RE_TOK_TYPE_QUESTION:       // zero or one
                g = POP();
                s = re_state_init(re, *t, STATE_TYPE_SPLIT, g.start, NULL);
                l = ol_init(GET_OL(), &s->out1);
                l = outlist_join(g.out, l);
                g = group_init(s, l);
                PUSH(g);
                break;
            case RE_TOK_TYPE_PIPE:       // alternate
                g1 = POP();
                g0 = POP();
                s = re_state_init(re, *t, STATE_TYPE_SPLIT, g0.start, g1.start);
                l = outlist_join(g0.out, g1.out);
                PUSH(group_init(s, l));

                break;
            case RE_TOK_TYPE_STAR:       // zero or more
                g = POP();
                s = re_state_init(re, *t, STATE_TYPE_SPLIT, g.start, NULL);
                group_patch_outlist(&g, &s);
                l = ol_init(GET_OL(), &s->out1);
                PUSH(group_init(g.start, l));
                break;
            case RE_TOK_TYPE_PLUS:       // one or more
                g = POP();
                s = re_state_init(re, *t, STATE_TYPE_SPLIT, g.start, NULL);
                group_patch_outlist(&g, &s);
                l = ol_init(GET_OL(), &s->out1);
                PUSH(group_init(s, l));
                break;
            default:        // it is a normal character
                s = re_state_init(re, *t, STATE_TYPE_NONE, NULL, NULL);
                l = ol_init(GET_OL(), &s->out);
                g = group_init(s, l);
                PUSH(g);
                break;
        }
    }
    g = POP();

    // connect last state that indicates a succesfull match
    struct ReState *match_state = re_state_init(re, NULL, STATE_TYPE_MATCH, NULL, NULL);

    group_patch_outlist(&g, &match_state);

    //re_state_debug(g.start, 0);
    re->start = g.start;

    return g.start;

    #undef POP
    #undef PUSH
    #undef GET_OL
}

void re_match_debug(struct ReMatch *m)
{
    if (m->state >= 0) {
        DEBUG("RESULT: %s\n", m->result);
        DEBUG("START:  %d\n", m->istart);
        DEBUG("END:    %d\n", m->iend);
        DEBUG("ENDP:   %s\n", m->endp);
    }
    else {
        DEBUG("Parsing failed\n");
    }
}

static void debug_match_list(struct MatchList *l)
{
    struct ReState **s = l->states;
    for (int i=0 ; i<l->n ; i++, s++) {
        if ((*s)->type == STATE_TYPE_MATCH) {
            DEBUG("MATCHLIST: [%d] MATCH\n", i);
        }
        else {
            DEBUG("MATCHLIST: [%d] %s\n", i, re_token_to_str((*s)->t));
        }
    }
    DEBUG("\n");
}

struct ReMatch re_match(struct Regex *re, const char *str, char *buf, size_t bufsiz)
{
    /* Run NFA state machine on string to check for a match */
    const char *c = str;
    struct ReMatch m;
    memset(&m, 0, sizeof(struct ReMatch));
    m.state = -1;

    unsigned int i = 0;

    // this is where we record the states
    struct MatchList l0 = re_match_list_init();
    struct MatchList l1 = re_match_list_init();

    // These pointers are swapped between iterations.
    // clist holds current states that need to be checked.
    // nlist (becomes cclist) holds the next states that need to be checked on next iteration
    struct MatchList *clist = &l0;
    struct MatchList *nlist = &l1;
    struct MatchList *bak;

    int is_anchored = 0;

    // add first node, or second if we're anchored at start of string
    if (re->start->t->type == RE_TOK_TYPE_CARET) {
        DEBUG("IS ANCHORED AT START\n");
        is_anchored = 1;
        re_match_list_append(clist, re->start->out);
    }
    else {
        re_match_list_append(clist, re->start);
    }


    DEBUG("INPUT STRING: %s\n", str);

    for (; *c ; c++) {
        DEBUG("MATCHING CHAR: '%c'\n", *c);
        *nlist = re_match_list_init();

        // Check all paths in clist and check for matches against c.
        // Add all matches to nlist so we can process them on the next run.
        if (re_match_list_has_token(clist, nlist, *c) > 0) {
            if (i>=bufsiz-1) {
                ERROR("Ouput buffer full: %d, max=%ld\n", i, bufsiz);
                return m;
            }

            buf[i++] = *c;
            buf[i] = '\0';

            // switch lists
            bak = clist;
            clist = nlist;
            nlist = bak;

            if (re_match_list_has_match(clist)) {
                debug_match_list(clist);
                m.endp = c;
                m.iend = i-1;
                m.state = 1;
                m.result = buf;
                DEBUG("SUCCESS\n");
                return m;
            }
        }
        else {
            break;
        }
    }
    DEBUG("No Match\n");
    return m;
}
