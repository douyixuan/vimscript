/* vi:set ts=8 sts=4 sw=4 noet:
 *
 * VIM - Vi IMproved	by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 * See README.txt for an overview of the Vim source code.
 */

/*
 * syntax.c: code for syntax highlighting
 */

#include "vim.h"

#if defined(FEAT_SYN_HL) || defined(PROTO)

// different types of offsets that are possible
#define SPO_MS_OFF	0	// match  start offset
#define SPO_ME_OFF	1	// match  end	offset
#define SPO_HS_OFF	2	// highl. start offset
#define SPO_HE_OFF	3	// highl. end	offset
#define SPO_RS_OFF	4	// region start offset
#define SPO_RE_OFF	5	// region end	offset
#define SPO_LC_OFF	6	// leading context offset
#define SPO_COUNT	7

static char *(spo_name_tab[SPO_COUNT]) =
	    {"ms=", "me=", "hs=", "he=", "rs=", "re=", "lc="};

/*
 * The patterns that are being searched for are stored in a syn_pattern.
 * A match item consists of one pattern.
 * A start/end item consists of n start patterns and m end patterns.
 * A start/skip/end item consists of n start patterns, one skip pattern and m
 * end patterns.
 * For the latter two, the patterns are always consecutive: start-skip-end.
 *
 * A character offset can be given for the matched text (_m_start and _m_end)
 * and for the actually highlighted text (_h_start and _h_end).
 *
 * Note that ordering of members is optimized to reduce padding.
 */
typedef struct syn_pattern
{
    char	 sp_type;		// see SPTYPE_ defines below
    char	 sp_syncing;		// this item used for syncing
    short	 sp_syn_match_id;	// highlight group ID of pattern
    short	 sp_off_flags;		// see below
    int		 sp_offsets[SPO_COUNT];	// offsets
    int		 sp_flags;		// see HL_ defines below
#ifdef FEAT_CONCEAL
    int		 sp_cchar;		// conceal substitute character
#endif
    int		 sp_ic;			// ignore-case flag for sp_prog
    int		 sp_sync_idx;		// sync item index (syncing only)
    int		 sp_line_id;		// ID of last line where tried
    int		 sp_startcol;		// next match in sp_line_id line
    short	*sp_cont_list;		// cont. group IDs, if non-zero
    short	*sp_next_list;		// next group IDs, if non-zero
    struct sp_syn sp_syn;		// struct passed to in_id_list()
    char_u	*sp_pattern;		// regexp to match, pattern
    regprog_T	*sp_prog;		// regexp to match, program
#ifdef FEAT_PROFILE
    syn_time_T	 sp_time;
#endif
} synpat_T;

// The sp_off_flags are computed like this:
// offset from the start of the matched text: (1 << SPO_XX_OFF)
// offset from the end	 of the matched text: (1 << (SPO_XX_OFF + SPO_COUNT))
// When both are present, only one is used.

#define SPTYPE_MATCH	1	// match keyword with this group ID
#define SPTYPE_START	2	// match a regexp, start of item
#define SPTYPE_END	3	// match a regexp, end of item
#define SPTYPE_SKIP	4	// match a regexp, skip within item


#define SYN_ITEMS(buf)	((synpat_T *)((buf)->b_syn_patterns.ga_data))

#define NONE_IDX	(-2)	// value of sp_sync_idx for "NONE"

/*
 * Flags for b_syn_sync_flags:
 */
#define SF_CCOMMENT	0x01	// sync on a C-style comment
#define SF_MATCH	0x02	// sync by matching a pattern

#define SYN_STATE_P(ssp)    ((bufstate_T *)((ssp)->ga_data))

#define MAXKEYWLEN	80	    // maximum length of a keyword

/*
 * The attributes of the syntax item that has been recognized.
 */
static int current_attr = 0;	    // attr of current syntax word
#ifdef FEAT_EVAL
static int current_id = 0;	    // ID of current char for syn_get_id()
static int current_trans_id = 0;    // idem, transparency removed
#endif
#ifdef FEAT_CONCEAL
static int current_flags = 0;
static int current_seqnr = 0;
static int current_sub_char = 0;
#endif

typedef struct syn_cluster_S
{
    char_u	    *scl_name;	    // syntax cluster name
    char_u	    *scl_name_u;    // uppercase of scl_name
    short	    *scl_list;	    // IDs in this syntax cluster
} syn_cluster_T;

/*
 * Methods of combining two clusters
 */
#define CLUSTER_REPLACE	    1	// replace first list with second
#define CLUSTER_ADD	    2	// add second list to first
#define CLUSTER_SUBTRACT    3	// subtract second list from first

#define SYN_CLSTR(buf)	((syn_cluster_T *)((buf)->b_syn_clusters.ga_data))

/*
 * Syntax group IDs have different types:
 *     0 - 19999  normal syntax groups
 * 20000 - 20999  ALLBUT indicator (current_syn_inc_tag added)
 * 21000 - 21999  TOP indicator (current_syn_inc_tag added)
 * 22000 - 22999  CONTAINED indicator (current_syn_inc_tag added)
 * 23000 - 32767  cluster IDs (subtract SYNID_CLUSTER for the cluster ID)
 */
#define SYNID_ALLBUT	MAX_HL_ID   // syntax group ID for contains=ALLBUT
#define SYNID_TOP	21000	    // syntax group ID for contains=TOP
#define SYNID_CONTAINED	22000	    // syntax group ID for contains=CONTAINED
#define SYNID_CLUSTER	23000	    // first syntax group ID for clusters

#define MAX_SYN_INC_TAG	999	    // maximum before the above overflow
#define MAX_CLUSTER_ID  (32767 - SYNID_CLUSTER)

/*
 * Annoying Hack(TM):  ":syn include" needs this pointer to pass to
 * expand_filename().  Most of the other syntax commands don't need it, so
 * instead of passing it to them, we stow it here.
 */
static char_u **syn_cmdlinep;

/*
 * Another Annoying Hack(TM):  To prevent rules from other ":syn include"'d
 * files from leaking into ALLBUT lists, we assign a unique ID to the
 * rules in each ":syn include"'d file.
 */
static int current_syn_inc_tag = 0;
static int running_syn_inc_tag = 0;

/*
 * In a hashtable item "hi_key" points to "keyword" in a keyentry.
 * This avoids adding a pointer to the hashtable item.
 * KE2HIKEY() converts a var pointer to a hashitem key pointer.
 * HIKEY2KE() converts a hashitem key pointer to a var pointer.
 * HI2KE() converts a hashitem pointer to a var pointer.
 */
static keyentry_T dumkey;
#define KE2HIKEY(kp)  ((kp)->keyword)
#define HIKEY2KE(p)   ((keyentry_T *)((p) - (dumkey.keyword - (char_u *)&dumkey)))
#define HI2KE(hi)      HIKEY2KE((hi)->hi_key)

/*
 * To reduce the time spent in keepend(), remember at which level in the state
 * stack the first item with "keepend" is present.  When "-1", there is no
 * "keepend" on the stack.
 */
static int keepend_level = -1;

static char msg_no_items[] = N_("No Syntax items defined for this buffer");

/*
 * For the current state we need to remember more than just the idx.
 * When si_m_endpos.lnum is 0, the items other than si_idx are unknown.
 * (The end positions have the column number of the next char)
 */
typedef struct state_item
{
    int		si_idx;			// index of syntax pattern or
					// KEYWORD_IDX
    int		si_id;			// highlight group ID for keywords
    int		si_trans_id;		// idem, transparency removed
    int		si_m_lnum;		// lnum of the match
    int		si_m_startcol;		// starting column of the match
    lpos_T	si_m_endpos;		// just after end posn of the match
    lpos_T	si_h_startpos;		// start position of the highlighting
    lpos_T	si_h_endpos;		// end position of the highlighting
    lpos_T	si_eoe_pos;		// end position of end pattern
    int		si_end_idx;		// group ID for end pattern or zero
    int		si_ends;		// if match ends before si_m_endpos
    int		si_attr;		// attributes in this state
    long	si_flags;		// HL_HAS_EOL flag in this state, and
					// HL_SKIP* for si_next_list
#ifdef FEAT_CONCEAL
    int		si_seqnr;		// sequence number
    int		si_cchar;		// substitution character for conceal
#endif
    short	*si_cont_list;		// list of contained groups
    short	*si_next_list;		// nextgroup IDs after this item ends
    reg_extmatch_T *si_extmatch;	// \z(...\) matches from start
					// pattern
} stateitem_T;

#define KEYWORD_IDX	(-1)	    // value of si_idx for keywords
#define ID_LIST_ALL	((short *)-1) // valid of si_cont_list for containing all
				    // but contained groups

#ifdef FEAT_CONCEAL
static int next_seqnr = 1;		// value to use for si_seqnr
#endif

/*
 * Struct to reduce the number of arguments to get_syn_options(), it's used
 * very often.
 */
typedef struct
{
    int		flags;		// flags for contained and transparent
    int		keyword;	// TRUE for ":syn keyword"
    int		*sync_idx;	// syntax item for "grouphere" argument, NULL
				// if not allowed
    char	has_cont_list;	// TRUE if "cont_list" can be used
    short	*cont_list;	// group IDs for "contains" argument
    short	*cont_in_list;	// group IDs for "containedin" argument
    short	*next_list;	// group IDs for "nextgroup" argument
} syn_opt_arg_T;

/*
 * The next possible match in the current line for any pattern is remembered,
 * to avoid having to try for a match in each column.
 * If next_match_idx == -1, not tried (in this line) yet.
 * If next_match_col == MAXCOL, no match found in this line.
 * (All end positions have the column of the char after the end)
 */
static int next_match_col;		// column for start of next match
static lpos_T next_match_m_endpos;	// position for end of next match
static lpos_T next_match_h_startpos;	// pos. for highl. start of next match
static lpos_T next_match_h_endpos;	// pos. for highl. end of next match
static int next_match_idx;		// index of matched item
static long next_match_flags;		// flags for next match
static lpos_T next_match_eos_pos;	// end of start pattn (start region)
static lpos_T next_match_eoe_pos;	// pos. for end of end pattern
static int next_match_end_idx;		// ID of group for end pattn or zero
static reg_extmatch_T *next_match_extmatch = NULL;

/*
 * A state stack is an array of integers or stateitem_T, stored in a
 * garray_T.  A state stack is invalid if its itemsize entry is zero.
 */
#define INVALID_STATE(ssp)  ((ssp)->ga_itemsize == 0)
#define VALID_STATE(ssp)    ((ssp)->ga_itemsize != 0)

#define FOR_ALL_SYNSTATES(sb, sst) \
    for ((sst) = (sb)->b_sst_first; (sst) != NULL; (sst) = (sst)->sst_next)

/*
 * The current state (within the line) of the recognition engine.
 * When current_state.ga_itemsize is 0 the current state is invalid.
 */
static win_T	*syn_win;		// current window for highlighting
static buf_T	*syn_buf;		// current buffer for highlighting
static synblock_T *syn_block;		// current buffer for highlighting
static linenr_T current_lnum = 0;	// lnum of current state
static colnr_T	current_col = 0;	// column of current state
static int	current_state_stored = 0; // TRUE if stored current state
					  // after setting current_finished
static int	current_finished = 0;	// current line has been finished
static garray_T current_state		// current stack of state_items
		= {0, 0, 0, 0, NULL};
static short	*current_next_list = NULL; // when non-zero, nextgroup list
static int	current_next_flags = 0; // flags for current_next_list
static int	current_line_id = 0;	// unique number for current line

#define CUR_STATE(idx)	((stateitem_T *)(current_state.ga_data))[idx]

static void syn_sync(win_T *wp, linenr_T lnum, synstate_T *last_valid);
static int syn_match_linecont(linenr_T lnum);
static void syn_start_line(void);
static void syn_update_ends(int startofline);
static void syn_stack_alloc(void);
static int syn_stack_cleanup(void);
static void syn_stack_free_entry(synblock_T *block, synstate_T *p);
static synstate_T *syn_stack_find_entry(linenr_T lnum);
static synstate_T *store_current_state(void);
static void load_current_state(synstate_T *from);
static void invalidate_current_state(void);
static int syn_stack_equal(synstate_T *sp);
static void validate_current_state(void);
static int syn_finish_line(int syncing);
static int syn_current_attr(int syncing, int displaying, int *can_spell, int keep_state);
static int did_match_already(int idx, garray_T *gap);
static stateitem_T *push_next_match(stateitem_T *cur_si);
static void check_state_ends(void);
static void update_si_attr(int idx);
static void check_keepend(void);
static void update_si_end(stateitem_T *sip, int startcol, int force);
static short *copy_id_list(short *list);
static int in_id_list(stateitem_T *item, short *cont_list, struct sp_syn *ssp, int flags);
static int push_current_state(int idx);
static void pop_current_state(void);
#ifdef FEAT_PROFILE
static void syn_clear_time(syn_time_T *tt);
static void syntime_clear(void);
static void syntime_report(void);
static int syn_time_on = FALSE;
# define IF_SYN_TIME(p) (p)
#else
# define IF_SYN_TIME(p) NULL
typedef int syn_time_T;
#endif

static void syn_stack_apply_changes_block(synblock_T *block, buf_T *buf);
static void find_endpos(int idx, lpos_T *startpos, lpos_T *m_endpos, lpos_T *hl_endpos, long *flagsp, lpos_T *end_endpos, int *end_idx, reg_extmatch_T *start_ext);

static void limit_pos(lpos_T *pos, lpos_T *limit);
static void limit_pos_zero(lpos_T *pos, lpos_T *limit);
static void syn_add_end_off(lpos_T *result, regmmatch_T *regmatch, synpat_T *spp, int idx, int extra);
static void syn_add_start_off(lpos_T *result, regmmatch_T *regmatch, synpat_T *spp, int idx, int extra);
static char_u *syn_getcurline(void);
static colnr_T syn_getcurline_len(void);
static int syn_regexec(regmmatch_T *rmp, linenr_T lnum, colnr_T col, syn_time_T *st);
static int check_keyword_id(char_u *line, int startcol, int *endcol, long *flags, short **next_list, stateitem_T *cur_si, int *ccharp);
static void syn_remove_pattern(synblock_T *block, int idx);
static void syn_clear_pattern(synblock_T *block, int i);
static void syn_clear_cluster(synblock_T *block, int i);
static void syn_clear_one(int id, int syncing);
static void syn_cmd_onoff(exarg_T *eap, char *name);
static void syn_lines_msg(void);
static void syn_match_msg(void);
static void syn_list_one(int id, int syncing, int link_only);
static void syn_list_cluster(int id);
static void put_id_list(char_u *name, short *list, int attr);
static void put_pattern(char *s, int c, synpat_T *spp, int attr);
static int syn_list_keywords(int id, hashtab_T *ht, int did_header, int attr);
static void syn_clear_keyword(int id, hashtab_T *ht);
static void clear_keywtab(hashtab_T *ht);
static int syn_scl_namen2id(char_u *linep, int len);
static int syn_check_cluster(char_u *pp, int len);
static int syn_add_cluster(char_u *name);
static void init_syn_patterns(void);
static char_u *get_syn_pattern(char_u *arg, synpat_T *ci);
static int get_id_list(char_u **arg, int keylen, short **list, int skip);
static void syn_combine_list(short **clstr1, short **clstr2, int list_op);

/*
 * Start the syntax recognition for a line.  This function is normally called
 * from the screen updating, once for each displayed line.
 * The buffer is remembered in syn_buf, because get_syntax_attr() doesn't get
 * it.	Careful: curbuf and curwin are likely to point to another buffer and
 * window.
 */
    void
syntax_start(win_T *wp, linenr_T lnum)
{
    synstate_T	*p;
    synstate_T	*last_valid = NULL;
    synstate_T	*last_min_valid = NULL;
    synstate_T	*sp, *prev = NULL;
    linenr_T	parsed_lnum;
    linenr_T	first_stored;
    int		dist;
    static varnumber_T changedtick = 0;	// remember the last change ID

#ifdef FEAT_CONCEAL
    current_sub_char = NUL;
#endif

    /*
     * After switching buffers, invalidate current_state.
     * Also do this when a change was made, the current state may be invalid
     * then.
     */
    if (syn_block != wp->w_s
	    || syn_buf != wp->w_buffer
	    || changedtick != CHANGEDTICK(syn_buf))
    {
	invalidate_current_state();
	syn_buf = wp->w_buffer;
	syn_block = wp->w_s;
    }
    changedtick = CHANGEDTICK(syn_buf);
    syn_win = wp;

    /*
     * Allocate syntax stack when needed.
     */
    syn_stack_alloc();
    if (syn_block->b_sst_array == NULL)
	return;		// out of memory
    syn_block->b_sst_lasttick = display_tick;

    /*
     * If the state of the end of the previous line is useful, store it.
     */
    if (VALID_STATE(&current_state)
	    && current_lnum < lnum
	    && current_lnum < syn_buf->b_ml.ml_line_count)
    {
	(void)syn_finish_line(FALSE);
	if (!current_state_stored)
	{
	    ++current_lnum;
	    (void)store_current_state();
	}

	/*
	 * If the current_lnum is now the same as "lnum", keep the current
	 * state (this happens very often!).  Otherwise invalidate
	 * current_state and figure it out below.
	 */
	if (current_lnum != lnum)
	    invalidate_current_state();
    }
    else
	invalidate_current_state();

    /*
     * Try to synchronize from a saved state in b_sst_array[].
     * Only do this if lnum is not before and not to far beyond a saved state.
     */
    if (INVALID_STATE(&current_state) && syn_block->b_sst_array != NULL)
    {
	// Find last valid saved state before start_lnum.
	FOR_ALL_SYNSTATES(syn_block, p)
	{
	    if (p->sst_lnum > lnum)
		break;
	    if (p->sst_lnum <= lnum && p->sst_change_lnum == 0)
	    {
		last_valid = p;
		if (p->sst_lnum >= lnum - syn_block->b_syn_sync_minlines)
		    last_min_valid = p;
	    }
	}
	if (last_min_valid != NULL)
	    load_current_state(last_min_valid);
    }

    /*
     * If "lnum" is before or far beyond a line with a saved state, need to
     * re-synchronize.
     */
    if (INVALID_STATE(&current_state))
    {
	syn_sync(wp, lnum, last_valid);
	if (current_lnum == 1)
	    // First line is always valid, no matter "minlines".
	    first_stored = 1;
	else
	    // Need to parse "minlines" lines before state can be considered
	    // valid to store.
	    first_stored = current_lnum + syn_block->b_syn_sync_minlines;
    }
    else
	first_stored = current_lnum;

    /*
     * Advance from the sync point or saved state until the current line.
     * Save some entries for syncing with later on.
     */
    if (syn_block->b_sst_len <= Rows)
	dist = 999999;
    else
	dist = syn_buf->b_ml.ml_line_count / (syn_block->b_sst_len - Rows) + 1;
    while (current_lnum < lnum)
    {
	syn_start_line();
	(void)syn_finish_line(FALSE);
	++current_lnum;

	// If we parsed at least "minlines" lines or started at a valid
	// state, the current state is considered valid.
	if (current_lnum >= first_stored)
	{
	    // Check if the saved state entry is for the current line and is
	    // equal to the current state.  If so, then validate all saved
	    // states that depended on a change before the parsed line.
	    if (prev == NULL)
		prev = syn_stack_find_entry(current_lnum - 1);
	    if (prev == NULL)
		sp = syn_block->b_sst_first;
	    else
		sp = prev;
	    while (sp != NULL && sp->sst_lnum < current_lnum)
		sp = sp->sst_next;
	    if (sp != NULL
		    && sp->sst_lnum == current_lnum
		    && syn_stack_equal(sp))
	    {
		parsed_lnum = current_lnum;
		prev = sp;
		while (sp != NULL && sp->sst_change_lnum <= parsed_lnum)
		{
		    if (sp->sst_lnum <= lnum)
			// valid state before desired line, use this one
			prev = sp;
		    else if (sp->sst_change_lnum == 0)
			// past saved states depending on change, break here.
			break;
		    sp->sst_change_lnum = 0;
		    sp = sp->sst_next;
		}
		load_current_state(prev);
	    }
	    // Store the state at this line when it's the first one, the line
	    // where we start parsing, or some distance from the previously
	    // saved state.  But only when parsed at least 'minlines'.
	    else if (prev == NULL
			|| current_lnum == lnum
			|| current_lnum >= prev->sst_lnum + dist)
		prev = store_current_state();
	}

	// This can take a long time: break when CTRL-C pressed.  The current
	// state will be wrong then.
	line_breakcheck();
	if (got_int)
	{
	    current_lnum = lnum;
	    break;
	}
    }

    syn_start_line();
}

/*
 * We cannot simply discard growarrays full of state_items or buf_states; we
 * have to manually release their extmatch pointers first.
 */
    static void
clear_syn_state(synstate_T *p)
{
    int		i;
    garray_T	*gap;

    if (p->sst_stacksize > SST_FIX_STATES)
    {
	gap = &(p->sst_union.sst_ga);
	for (i = 0; i < gap->ga_len; i++)
	    unref_extmatch(SYN_STATE_P(gap)[i].bs_extmatch);
	ga_clear(gap);
    }
    else
    {
	for (i = 0; i < p->sst_stacksize; i++)
	    unref_extmatch(p->sst_union.sst_stack[i].bs_extmatch);
    }
}

/*
 * Cleanup the current_state stack.
 */
    static void
clear_current_state(void)
{
    int		i;
    stateitem_T	*sip;

    sip = (stateitem_T *)(current_state.ga_data);
    for (i = 0; i < current_state.ga_len; i++)
	unref_extmatch(sip[i].si_extmatch);
    ga_clear(&current_state);
}

/*
 * Try to find a synchronisation point for line "lnum".
 *
 * This sets current_lnum and the current state.  One of three methods is
 * used:
 * 1. Search backwards for the end of a C-comment.
 * 2. Search backwards for given sync patterns.
 * 3. Simply start on a given number of lines above "lnum".
 */
    static void
syn_sync(
    win_T	*wp,
    linenr_T	start_lnum,
    synstate_T	*last_valid)
{
    buf_T	*curbuf_save;
    win_T	*curwin_save;
    pos_T	cursor_save;
    int		idx;
    linenr_T	lnum;
    linenr_T	end_lnum;
    linenr_T	break_lnum;
    int		had_sync_point;
    stateitem_T	*cur_si;
    synpat_T	*spp;
    int		found_flags = 0;
    int		found_match_idx = 0;
    linenr_T	found_current_lnum = 0;
    int		found_current_col= 0;
    lpos_T	found_m_endpos;
    colnr_T	prev_current_col;

    /*
     * Clear any current state that might be hanging around.
     */
    invalidate_current_state();

    /*
     * Start at least "minlines" back.  Default starting point for parsing is
     * there.
     * Start further back, to avoid that scrolling backwards will result in
     * resyncing for every line.  Now it resyncs only one out of N lines,
     * where N is minlines * 1.5, or minlines * 2 if minlines is small.
     * Watch out for overflow when minlines is MAXLNUM.
     */
    if (syn_block->b_syn_sync_minlines > start_lnum)
	start_lnum = 1;
    else
    {
	if (syn_block->b_syn_sync_minlines == 1)
	    lnum = 1;
	else if (syn_block->b_syn_sync_minlines < 10)
	    lnum = syn_block->b_syn_sync_minlines * 2;
	else
	    lnum = syn_block->b_syn_sync_minlines * 3 / 2;
	if (syn_block->b_syn_sync_maxlines != 0
				     && lnum > syn_block->b_syn_sync_maxlines)
	    lnum = syn_block->b_syn_sync_maxlines;
	if (lnum >= start_lnum)
	    start_lnum = 1;
	else
	    start_lnum -= lnum;
    }
    current_lnum = start_lnum;

    /*
     * 1. Search backwards for the end of a C-style comment.
     */
    if (syn_block->b_syn_sync_flags & SF_CCOMMENT)
    {
	// Need to make syn_buf the current buffer for a moment, to be able to
	// use find_start_comment().
	curwin_save = curwin;
	curwin = wp;
	curbuf_save = curbuf;
	curbuf = syn_buf;

	/*
	 * Skip lines that end in a backslash.
	 */
	for ( ; start_lnum > 1; --start_lnum)
	{
	    char_u	*l = ml_get(start_lnum - 1);

	    if (*l == NUL || *(l + ml_get_len(start_lnum - 1) - 1) != '\\')
		break;
	}
	current_lnum = start_lnum;

	// set cursor to start of search
	cursor_save = wp->w_cursor;
	wp->w_cursor.lnum = start_lnum;
	wp->w_cursor.col = 0;

	/*
	 * If the line is inside a comment, need to find the syntax item that
	 * defines the comment.
	 * Restrict the search for the end of a comment to b_syn_sync_maxlines.
	 */
	if (find_start_comment((int)syn_block->b_syn_sync_maxlines) != NULL)
	{
	    for (idx = syn_block->b_syn_patterns.ga_len; --idx >= 0; )
		if (SYN_ITEMS(syn_block)[idx].sp_syn.id
						   == syn_block->b_syn_sync_id
			&& SYN_ITEMS(syn_block)[idx].sp_type == SPTYPE_START)
		{
		    validate_current_state();
		    if (push_current_state(idx) == OK)
			update_si_attr(current_state.ga_len - 1);
		    break;
		}
	}

	// restore cursor and buffer
	wp->w_cursor = cursor_save;
	curwin = curwin_save;
	curbuf = curbuf_save;
    }

    /*
     * 2. Search backwards for given sync patterns.
     */
    else if (syn_block->b_syn_sync_flags & SF_MATCH)
    {
	if (syn_block->b_syn_sync_maxlines != 0
			       && start_lnum > syn_block->b_syn_sync_maxlines)
	    break_lnum = start_lnum - syn_block->b_syn_sync_maxlines;
	else
	    break_lnum = 0;

	found_m_endpos.lnum = 0;
	found_m_endpos.col = 0;
	end_lnum = start_lnum;
	lnum = start_lnum;
	while (--lnum > break_lnum)
	{
	    // This can take a long time: break when CTRL-C pressed.
	    line_breakcheck();
	    if (got_int)
	    {
		invalidate_current_state();
		current_lnum = start_lnum;
		break;
	    }

	    // Check if we have run into a valid saved state stack now.
	    if (last_valid != NULL && lnum == last_valid->sst_lnum)
	    {
		load_current_state(last_valid);
		break;
	    }

	    /*
	     * Check if the previous line has the line-continuation pattern.
	     */
	    if (lnum > 1 && syn_match_linecont(lnum - 1))
		continue;

	    /*
	     * Start with nothing on the state stack
	     */
	    validate_current_state();

	    for (current_lnum = lnum; current_lnum < end_lnum; ++current_lnum)
	    {
		syn_start_line();
		for (;;)
		{
		    had_sync_point = syn_finish_line(TRUE);
		    /*
		     * When a sync point has been found, remember where, and
		     * continue to look for another one, further on in the line.
		     */
		    if (had_sync_point && current_state.ga_len)
		    {
			cur_si = &CUR_STATE(current_state.ga_len - 1);
			if (cur_si->si_m_endpos.lnum > start_lnum)
			{
			    // ignore match that goes to after where started
			    current_lnum = end_lnum;
			    break;
			}
			if (cur_si->si_idx < 0)
			{
			    // Cannot happen?
			    found_flags = 0;
			    found_match_idx = KEYWORD_IDX;
			}
			else
			{
			    spp = &(SYN_ITEMS(syn_block)[cur_si->si_idx]);
			    found_flags = spp->sp_flags;
			    found_match_idx = spp->sp_sync_idx;
			}
			found_current_lnum = current_lnum;
			found_current_col = current_col;
			found_m_endpos = cur_si->si_m_endpos;
			/*
			 * Continue after the match (be aware of a zero-length
			 * match).
			 */
			if (found_m_endpos.lnum > current_lnum)
			{
			    current_lnum = found_m_endpos.lnum;
			    current_col = found_m_endpos.col;
			    if (current_lnum >= end_lnum)
				break;
			}
			else if (found_m_endpos.col > current_col)
			    current_col = found_m_endpos.col;
			else
			    ++current_col;

			// syn_current_attr() will have skipped the check for
			// an item that ends here, need to do that now.  Be
			// careful not to go past the NUL.
			prev_current_col = current_col;
			if (syn_getcurline()[current_col] != NUL)
			    ++current_col;
			check_state_ends();
			current_col = prev_current_col;
		    }
		    else
			break;
		}
	    }

	    /*
	     * If a sync point was encountered, break here.
	     */
	    if (found_flags)
	    {
		/*
		 * Put the item that was specified by the sync point on the
		 * state stack.  If there was no item specified, make the
		 * state stack empty.
		 */
		clear_current_state();
		if (found_match_idx >= 0
			&& push_current_state(found_match_idx) == OK)
		    update_si_attr(current_state.ga_len - 1);

		/*
		 * When using "grouphere", continue from the sync point
		 * match, until the end of the line.  Parsing starts at
		 * the next line.
		 * For "groupthere" the parsing starts at start_lnum.
		 */
		if (found_flags & HL_SYNC_HERE)
		{
		    if (current_state.ga_len)
		    {
			cur_si = &CUR_STATE(current_state.ga_len - 1);
			cur_si->si_h_startpos.lnum = found_current_lnum;
			cur_si->si_h_startpos.col = found_current_col;
			update_si_end(cur_si, (int)current_col, TRUE);
			check_keepend();
		    }
		    current_col = found_m_endpos.col;
		    current_lnum = found_m_endpos.lnum;
		    (void)syn_finish_line(FALSE);
		    ++current_lnum;
		}
		else
		    current_lnum = start_lnum;

		break;
	    }

	    end_lnum = lnum;
	    invalidate_current_state();
	}

	// Ran into start of the file or exceeded maximum number of lines
	if (lnum <= break_lnum)
	{
	    invalidate_current_state();
	    current_lnum = break_lnum + 1;
	}
    }

    validate_current_state();
}

    static void
save_chartab(char_u *chartab)
{
    if (syn_block->b_syn_isk == empty_option)
	return;

    mch_memmove(chartab, syn_buf->b_chartab, (size_t)32);
    mch_memmove(syn_buf->b_chartab, syn_win->w_s->b_syn_chartab, (size_t)32);
}

    static void
restore_chartab(char_u *chartab)
{
    if (syn_win->w_s->b_syn_isk != empty_option)
	mch_memmove(syn_buf->b_chartab, chartab, (size_t)32);
}

/*
 * Return TRUE if the line-continuation pattern matches in line "lnum".
 */
    static int
syn_match_linecont(linenr_T lnum)
{
    regmmatch_T regmatch;
    int r;
    char_u	buf_chartab[32];  // chartab array for syn iskyeyword

    if (syn_block->b_syn_linecont_prog == NULL)
	return FALSE;

    // use syntax iskeyword option
    save_chartab(buf_chartab);
    regmatch.rmm_ic = syn_block->b_syn_linecont_ic;
    regmatch.regprog = syn_block->b_syn_linecont_prog;
    r = syn_regexec(&regmatch, lnum, (colnr_T)0,
	    IF_SYN_TIME(&syn_block->b_syn_linecont_time));
    syn_block->b_syn_linecont_prog = regmatch.regprog;
    restore_chartab(buf_chartab);
    return r;
}

/*
 * Prepare the current state for the start of a line.
 */
    static void
syn_start_line(void)
{
    current_finished = FALSE;
    current_col = 0;

    /*
     * Need to update the end of a start/skip/end that continues from the
     * previous line and regions that have "keepend".
     */
    if (current_state.ga_len > 0)
    {
	syn_update_ends(TRUE);
	check_state_ends();
    }

    next_match_idx = -1;
    ++current_line_id;
#ifdef FEAT_CONCEAL
    next_seqnr = 1;
#endif
}

/*
 * Check for items in the stack that need their end updated.
 * When "startofline" is TRUE the last item is always updated.
 * When "startofline" is FALSE the item with "keepend" is forcefully updated.
 */
    static void
syn_update_ends(int startofline)
{
    stateitem_T	*cur_si;
    int		i;
    int		seen_keepend;

    if (startofline)
    {
	// Check for a match carried over from a previous line with a
	// contained region.  The match ends as soon as the region ends.
	for (i = 0; i < current_state.ga_len; ++i)
	{
	    cur_si = &CUR_STATE(i);
	    if (cur_si->si_idx >= 0
		    && (SYN_ITEMS(syn_block)[cur_si->si_idx]).sp_type
							       == SPTYPE_MATCH
		    && cur_si->si_m_endpos.lnum < current_lnum)
	    {
		cur_si->si_flags |= HL_MATCHCONT;
		cur_si->si_m_endpos.lnum = 0;
		cur_si->si_m_endpos.col = 0;
		cur_si->si_h_endpos = cur_si->si_m_endpos;
		cur_si->si_ends = TRUE;
	    }
	}
    }

    /*
     * Need to update the end of a start/skip/end that continues from the
     * previous line.  And regions that have "keepend", because they may
     * influence contained items.  If we've just removed "extend"
     * (startofline == 0) then we should update ends of normal regions
     * contained inside "keepend" because "extend" could have extended
     * these "keepend" regions as well as contained normal regions.
     * Then check for items ending in column 0.
     */
    i = current_state.ga_len - 1;
    if (keepend_level >= 0)
	for ( ; i > keepend_level; --i)
	    if (CUR_STATE(i).si_flags & HL_EXTEND)
		break;

    seen_keepend = FALSE;
    for ( ; i < current_state.ga_len; ++i)
    {
	cur_si = &CUR_STATE(i);
	if ((cur_si->si_flags & HL_KEEPEND)
			    || (seen_keepend && !startofline)
			    || (i == current_state.ga_len - 1 && startofline))
	{
	    cur_si->si_h_startpos.col = 0;	// start highl. in col 0
	    cur_si->si_h_startpos.lnum = current_lnum;

	    if (!(cur_si->si_flags & HL_MATCHCONT))
		update_si_end(cur_si, (int)current_col, !startofline);

	    if (!startofline && (cur_si->si_flags & HL_KEEPEND))
		seen_keepend = TRUE;
	}
    }
    check_keepend();
}

/////////////////////////////////////////
// Handling of the state stack cache.

/*
 * EXPLANATION OF THE SYNTAX STATE STACK CACHE
 *
 * To speed up syntax highlighting, the state stack for the start of some
 * lines is cached.  These entries can be used to start parsing at that point.
 *
 * The stack is kept in b_sst_array[] for each buffer.  There is a list of
 * valid entries.  b_sst_first points to the first one, then follow sst_next.
 * The entries are sorted on line number.  The first entry is often for line 2
 * (line 1 always starts with an empty stack).
 * There is also a list for free entries.  This construction is used to avoid
 * having to allocate and free memory blocks too often.
 *
 * When making changes to the buffer, this is logged in b_mod_*.  When calling
 * update_screen() to update the display, it will call
 * syn_stack_apply_changes() for each displayed buffer to adjust the cached
 * entries.  The entries which are inside the changed area are removed,
 * because they must be recomputed.  Entries below the changed have their line
 * number adjusted for deleted/inserted lines, and have their sst_change_lnum
 * set to indicate that a check must be made if the changed lines would change
 * the cached entry.
 *
 * When later displaying lines, an entry is stored for each line.  Displayed
 * lines are likely to be displayed again, in which case the state at the
 * start of the line is needed.
 * For not displayed lines, an entry is stored for every so many lines.  These
 * entries will be used e.g., when scrolling backwards.  The distance between
 * entries depends on the number of lines in the buffer.  For small buffers
 * the distance is fixed at SST_DIST, for large buffers there is a fixed
 * number of entries SST_MAX_ENTRIES, and the distance is computed.
 */

    static void
syn_stack_free_block(synblock_T *block)
{
    synstate_T	*p;

    if (block->b_sst_array == NULL)
	return;

    FOR_ALL_SYNSTATES(block, p)
	clear_syn_state(p);
    VIM_CLEAR(block->b_sst_array);
    block->b_sst_first = NULL;
    block->b_sst_len = 0;
}
/*
 * Free b_sst_array[] for buffer "buf".
 * Used when syntax items changed to force resyncing everywhere.
 */
    void
syn_stack_free_all(synblock_T *block)
{
#ifdef FEAT_FOLDING
    win_T	*wp;
#endif

    syn_stack_free_block(block);

#ifdef FEAT_FOLDING
    // When using "syntax" fold method, must update all folds.
    FOR_ALL_WINDOWS(wp)
    {
	if (wp->w_s == block && foldmethodIsSyntax(wp))
	    foldUpdateAll(wp);
    }
#endif
}

/*
 * Allocate the syntax state stack for syn_buf when needed.
 * If the number of entries in b_sst_array[] is much too big or a bit too
 * small, reallocate it.
 * Also used to allocate b_sst_array[] for the first time.
 */
    static void
syn_stack_alloc(void)
{
    long	len;
    synstate_T	*to, *from;
    synstate_T	*sstp;

    len = syn_buf->b_ml.ml_line_count / SST_DIST + Rows * 2;
    if (len < SST_MIN_ENTRIES)
	len = SST_MIN_ENTRIES;
    else if (len > SST_MAX_ENTRIES)
	len = SST_MAX_ENTRIES;
    if (syn_block->b_sst_len > len * 2 || syn_block->b_sst_len < len)
    {
	// Allocate 50% too much, to avoid reallocating too often.
	len = syn_buf->b_ml.ml_line_count;
	len = (len + len / 2) / SST_DIST + Rows * 2;
	if (len < SST_MIN_ENTRIES)
	    len = SST_MIN_ENTRIES;
	else if (len > SST_MAX_ENTRIES)
	    len = SST_MAX_ENTRIES;

	if (syn_block->b_sst_array != NULL)
	{
	    // When shrinking the array, cleanup the existing stack.
	    // Make sure that all valid entries fit in the new array.
	    while (syn_block->b_sst_len - syn_block->b_sst_freecount + 2 > len
		    && syn_stack_cleanup())
		;
	    if (len < syn_block->b_sst_len - syn_block->b_sst_freecount + 2)
		len = syn_block->b_sst_len - syn_block->b_sst_freecount + 2;
	}

	sstp = ALLOC_CLEAR_MULT(synstate_T, len);
	if (sstp == NULL)	// out of memory!
	    return;

	to = sstp - 1;
	if (syn_block->b_sst_array != NULL)
	{
	    // Move the states from the old array to the new one.
	    for (from = syn_block->b_sst_first; from != NULL;
							from = from->sst_next)
	    {
		++to;
		*to = *from;
		to->sst_next = to + 1;
	    }
	}
	if (to != sstp - 1)
	{
	    to->sst_next = NULL;
	    syn_block->b_sst_first = sstp;
	    syn_block->b_sst_freecount = len - (int)(to - sstp) - 1;
	}
	else
	{
	    syn_block->b_sst_first = NULL;
	    syn_block->b_sst_freecount = len;
	}

	// Create the list of free entries.
	syn_block->b_sst_firstfree = to + 1;
	while (++to < sstp + len)
	    to->sst_next = to + 1;
	(sstp + len - 1)->sst_next = NULL;

	vim_free(syn_block->b_sst_array);
	syn_block->b_sst_array = sstp;
	syn_block->b_sst_len = len;
    }
}

/*
 * Check for changes in a buffer to affect stored syntax states.  Uses the
 * b_mod_* fields.
 * Called from update_screen(), before screen is being updated, once for each
 * displayed buffer.
 */
    void
syn_stack_apply_changes(buf_T *buf)
{
    win_T	*wp;

    syn_stack_apply_changes_block(&buf->b_s, buf);

    FOR_ALL_WINDOWS(wp)
    {
	if ((wp->w_buffer == buf) && (wp->w_s != &buf->b_s))
	    syn_stack_apply_changes_block(wp->w_s, buf);
    }
}

    static void
syn_stack_apply_changes_block(synblock_T *block, buf_T *buf)
{
    synstate_T	*p, *prev, *np;
    linenr_T	n;

    prev = NULL;
    for (p = block->b_sst_first; p != NULL; )
    {
	if (p->sst_lnum + block->b_syn_sync_linebreaks > buf->b_mod_top)
	{
	    n = p->sst_lnum + buf->b_mod_xlines;
	    if (n <= buf->b_mod_bot)
	    {
		// this state is inside the changed area, remove it
		np = p->sst_next;
		if (prev == NULL)
		    block->b_sst_first = np;
		else
		    prev->sst_next = np;
		syn_stack_free_entry(block, p);
		p = np;
		continue;
	    }
	    // This state is below the changed area.  Remember the line
	    // that needs to be parsed before this entry can be made valid
	    // again.
	    if (p->sst_change_lnum != 0 && p->sst_change_lnum > buf->b_mod_top)
	    {
		if (p->sst_change_lnum + buf->b_mod_xlines > buf->b_mod_top)
		    p->sst_change_lnum += buf->b_mod_xlines;
		else
		    p->sst_change_lnum = buf->b_mod_top;
	    }
	    if (p->sst_change_lnum == 0
		    || p->sst_change_lnum < buf->b_mod_bot)
		p->sst_change_lnum = buf->b_mod_bot;

	    p->sst_lnum = n;
	}
	prev = p;
	p = p->sst_next;
    }
}

/*
 * Reduce the number of entries in the state stack for syn_buf.
 * Returns TRUE if at least one entry was freed.
 */
    static int
syn_stack_cleanup(void)
{
    synstate_T	*p, *prev;
    disptick_T	tick;
    int		above;
    int		dist;
    int		retval = FALSE;

    if (syn_block->b_sst_first == NULL)
	return retval;

    // Compute normal distance between non-displayed entries.
    if (syn_block->b_sst_len <= Rows)
	dist = 999999;
    else
	dist = syn_buf->b_ml.ml_line_count / (syn_block->b_sst_len - Rows) + 1;

    /*
     * Go through the list to find the "tick" for the oldest entry that can
     * be removed.  Set "above" when the "tick" for the oldest entry is above
     * "b_sst_lasttick" (the display tick wraps around).
     */
    tick = syn_block->b_sst_lasttick;
    above = FALSE;
    prev = syn_block->b_sst_first;
    for (p = prev->sst_next; p != NULL; prev = p, p = p->sst_next)
    {
	if (prev->sst_lnum + dist > p->sst_lnum)
	{
	    if (p->sst_tick > syn_block->b_sst_lasttick)
	    {
		if (!above || p->sst_tick < tick)
		    tick = p->sst_tick;
		above = TRUE;
	    }
	    else if (!above && p->sst_tick < tick)
		tick = p->sst_tick;
	}
    }

    /*
     * Go through the list to make the entries for the oldest tick at an
     * interval of several lines.
     */
    prev = syn_block->b_sst_first;
    for (p = prev->sst_next; p != NULL; prev = p, p = p->sst_next)
    {
	if (p->sst_tick == tick && prev->sst_lnum + dist > p->sst_lnum)
	{
	    // Move this entry from used list to free list
	    prev->sst_next = p->sst_next;
	    syn_stack_free_entry(syn_block, p);
	    p = prev;
	    retval = TRUE;
	}
    }
    return retval;
}

/*
 * Free the allocated memory for a syn_state item.
 * Move the entry into the free list.
 */
    static void
syn_stack_free_entry(synblock_T *block, synstate_T *p)
{
    clear_syn_state(p);
    p->sst_next = block->b_sst_firstfree;
    block->b_sst_firstfree = p;
    ++block->b_sst_freecount;
}

/*
 * Find an entry in the list of state stacks at or before "lnum".
 * Returns NULL when there is no entry or the first entry is after "lnum".
 */
    static synstate_T *
syn_stack_find_entry(linenr_T lnum)
{
    synstate_T	*p, *prev;

    prev = NULL;
    for (p = syn_block->b_sst_first; p != NULL; prev = p, p = p->sst_next)
    {
	if (p->sst_lnum == lnum)
	    return p;
	if (p->sst_lnum > lnum)
	    break;
    }
    return prev;
}

/*
 * Try saving the current state in b_sst_array[].
 * The current state must be valid for the start of the current_lnum line!
 */
    static synstate_T *
store_current_state(void)
{
    int		i;
    synstate_T	*p;
    bufstate_T	*bp;
    stateitem_T	*cur_si;
    synstate_T	*sp = syn_stack_find_entry(current_lnum);

    /*
     * If the current state contains a start or end pattern that continues
     * from the previous line, we can't use it.  Don't store it then.
     */
    for (i = current_state.ga_len - 1; i >= 0; --i)
    {
	cur_si = &CUR_STATE(i);
	if (cur_si->si_h_startpos.lnum >= current_lnum
		|| cur_si->si_m_endpos.lnum >= current_lnum
		|| cur_si->si_h_endpos.lnum >= current_lnum
		|| (cur_si->si_end_idx
		    && cur_si->si_eoe_pos.lnum >= current_lnum))
	    break;
    }
    if (i >= 0)
    {
	if (sp != NULL)
	{
	    // find "sp" in the list and remove it
	    if (syn_block->b_sst_first == sp)
		// it's the first entry
		syn_block->b_sst_first = sp->sst_next;
	    else
	    {
		// find the entry just before this one to adjust sst_next
		FOR_ALL_SYNSTATES(syn_block, p)
		    if (p->sst_next == sp)
			break;
		if (p != NULL)	// just in case
		    p->sst_next = sp->sst_next;
	    }
	    syn_stack_free_entry(syn_block, sp);
	    sp = NULL;
	}
    }
    else if (sp == NULL || sp->sst_lnum != current_lnum)
    {
	/*
	 * Add a new entry
	 */
	// If no free items, cleanup the array first.
	if (syn_block->b_sst_freecount == 0)
	{
	    (void)syn_stack_cleanup();
	    // "sp" may have been moved to the freelist now
	    sp = syn_stack_find_entry(current_lnum);
	}
	// Still no free items?  Must be a strange problem...
	if (syn_block->b_sst_freecount == 0)
	    sp = NULL;
	else
	{
	    // Take the first item from the free list and put it in the used
	    // list, after *sp
	    p = syn_block->b_sst_firstfree;
	    syn_block->b_sst_firstfree = p->sst_next;
	    --syn_block->b_sst_freecount;
	    if (sp == NULL)
	    {
		// Insert in front of the list
		p->sst_next = syn_block->b_sst_first;
		syn_block->b_sst_first = p;
	    }
	    else
	    {
		// insert in list after *sp
		p->sst_next = sp->sst_next;
		sp->sst_next = p;
	    }
	    sp = p;
	    sp->sst_stacksize = 0;
	    sp->sst_lnum = current_lnum;
	}
    }
    if (sp != NULL)
    {
	// When overwriting an existing state stack, clear it first
	clear_syn_state(sp);
	sp->sst_stacksize = current_state.ga_len;
	if (current_state.ga_len > SST_FIX_STATES)
	{
	    // Need to clear it, might be something remaining from when the
	    // length was less than SST_FIX_STATES.
	    ga_init2(&sp->sst_union.sst_ga, sizeof(bufstate_T), 1);
	    if (ga_grow(&sp->sst_union.sst_ga, current_state.ga_len) == FAIL)
		sp->sst_stacksize = 0;
	    else
		sp->sst_union.sst_ga.ga_len = current_state.ga_len;
	    bp = SYN_STATE_P(&(sp->sst_union.sst_ga));
	}
	else
	    bp = sp->sst_union.sst_stack;
	for (i = 0; i < sp->sst_stacksize; ++i)
	{
	    bp[i].bs_idx = CUR_STATE(i).si_idx;
	    bp[i].bs_flags = CUR_STATE(i).si_flags;
#ifdef FEAT_CONCEAL
	    bp[i].bs_seqnr = CUR_STATE(i).si_seqnr;
	    bp[i].bs_cchar = CUR_STATE(i).si_cchar;
#endif
	    bp[i].bs_extmatch = ref_extmatch(CUR_STATE(i).si_extmatch);
	}
	sp->sst_next_flags = current_next_flags;
	sp->sst_next_list = current_next_list;
	sp->sst_tick = display_tick;
	sp->sst_change_lnum = 0;
    }
    current_state_stored = TRUE;
    return sp;
}

/*
 * Copy a state stack from "from" in b_sst_array[] to current_state;
 */
    static void
load_current_state(synstate_T *from)
{
    int		i;
    bufstate_T	*bp;

    clear_current_state();
    validate_current_state();
    keepend_level = -1;
    if (from->sst_stacksize
	    && ga_grow(&current_state, from->sst_stacksize) == OK)
    {
	if (from->sst_stacksize > SST_FIX_STATES)
	    bp = SYN_STATE_P(&(from->sst_union.sst_ga));
	else
	    bp = from->sst_union.sst_stack;
	for (i = 0; i < from->sst_stacksize; ++i)
	{
	    CUR_STATE(i).si_idx = bp[i].bs_idx;
	    CUR_STATE(i).si_flags = bp[i].bs_flags;
#ifdef FEAT_CONCEAL
	    CUR_STATE(i).si_seqnr = bp[i].bs_seqnr;
	    CUR_STATE(i).si_cchar = bp[i].bs_cchar;
#endif
	    CUR_STATE(i).si_extmatch = ref_extmatch(bp[i].bs_extmatch);
	    if (keepend_level < 0 && (CUR_STATE(i).si_flags & HL_KEEPEND))
		keepend_level = i;
	    CUR_STATE(i).si_ends = FALSE;
	    CUR_STATE(i).si_m_lnum = 0;
	    if (CUR_STATE(i).si_idx >= 0)
		CUR_STATE(i).si_next_list =
		     (SYN_ITEMS(syn_block)[CUR_STATE(i).si_idx]).sp_next_list;
	    else
		CUR_STATE(i).si_next_list = NULL;
	    update_si_attr(i);
	}
	current_state.ga_len = from->sst_stacksize;
    }
    current_next_list = from->sst_next_list;
    current_next_flags = from->sst_next_flags;
    current_lnum = from->sst_lnum;
}

/*
 * Compare saved state stack "*sp" with the current state.
 * Return TRUE when they are equal.
 */
    static int
syn_stack_equal(synstate_T *sp)
{
    int		i, j;
    bufstate_T	*bp;
    reg_extmatch_T	*six, *bsx;

    // First a quick check if the stacks have the same size end nextlist.
    if (sp->sst_stacksize != current_state.ga_len
	    || sp->sst_next_list != current_next_list)
	return FALSE;

    // Need to compare all states on both stacks.
    if (sp->sst_stacksize > SST_FIX_STATES)
	bp = SYN_STATE_P(&(sp->sst_union.sst_ga));
    else
	bp = sp->sst_union.sst_stack;

    for (i = current_state.ga_len; --i >= 0; )
    {
	// If the item has another index the state is different.
	if (bp[i].bs_idx != CUR_STATE(i).si_idx)
	    break;
	if (bp[i].bs_extmatch == CUR_STATE(i).si_extmatch)
	    continue;
	// When the extmatch pointers are different, the strings in them can
	// still be the same.  Check if the extmatch references are equal.
	bsx = bp[i].bs_extmatch;
	six = CUR_STATE(i).si_extmatch;
	// If one of the extmatch pointers is NULL the states are different.
	if (bsx == NULL || six == NULL)
	    break;
	for (j = 0; j < NSUBEXP; ++j)
	{
	    // Check each referenced match string. They must all be equal.
	    if (bsx->matches[j] != six->matches[j])
	    {
		// If the pointer is different it can still be the same text.
		// Compare the strings, ignore case when the start item has the
		// sp_ic flag set.
		if (bsx->matches[j] == NULL || six->matches[j] == NULL)
		    break;
		if ((SYN_ITEMS(syn_block)[CUR_STATE(i).si_idx]).sp_ic
			? MB_STRICMP(bsx->matches[j], six->matches[j]) != 0
			: STRCMP(bsx->matches[j], six->matches[j]) != 0)
		    break;
	    }
	}
	if (j != NSUBEXP)
	    break;
    }
    return i < 0 ? TRUE : FALSE;
}

/*
 * We stop parsing syntax above line "lnum".  If the stored state at or below
 * this line depended on a change before it, it now depends on the line below
 * the last parsed line.
 * The window looks like this:
 *	    line which changed
 *	    displayed line
 *	    displayed line
 * lnum ->  line below window
 */
    void
syntax_end_parsing(win_T *wp, linenr_T lnum)
{
    synstate_T	*sp;

    if (syn_block != wp->w_s)
	return;  // not the right window
    sp = syn_stack_find_entry(lnum);
    if (sp != NULL && sp->sst_lnum < lnum)
	sp = sp->sst_next;

    if (sp != NULL && sp->sst_change_lnum != 0)
	sp->sst_change_lnum = lnum;
}

/*
 * End of handling of the state stack.
 ****************************************/

    static void
invalidate_current_state(void)
{
    clear_current_state();
    current_state.ga_itemsize = 0;	// mark current_state invalid
    current_next_list = NULL;
    keepend_level = -1;
}

    static void
validate_current_state(void)
{
    current_state.ga_itemsize = sizeof(stateitem_T);
    current_state.ga_growsize = 3;
}

/*
 * Return TRUE if the syntax at start of lnum changed since last time.
 * This will only be called just after get_syntax_attr() for the previous
 * line, to check if the next line needs to be redrawn too.
 */
    int
syntax_check_changed(linenr_T lnum)
{
    int		retval = TRUE;
    synstate_T	*sp;

    /*
     * Check the state stack when:
     * - lnum is just below the previously syntaxed line.
     * - lnum is not before the lines with saved states.
     * - lnum is not past the lines with saved states.
     * - lnum is at or before the last changed line.
     */
    if (VALID_STATE(&current_state) && lnum == current_lnum + 1)
    {
	sp = syn_stack_find_entry(lnum);
	if (sp != NULL && sp->sst_lnum == lnum)
	{
	    /*
	     * finish the previous line (needed when not all of the line was
	     * drawn)
	     */
	    (void)syn_finish_line(FALSE);

	    /*
	     * Compare the current state with the previously saved state of
	     * the line.
	     */
	    if (syn_stack_equal(sp))
		retval = FALSE;

	    /*
	     * Store the current state in b_sst_array[] for later use.
	     */
	    ++current_lnum;
	    (void)store_current_state();
	}
    }

    return retval;
}

/*
 * Finish the current line.
 * This doesn't return any attributes, it only gets the state at the end of
 * the line.  It can start anywhere in the line, as long as the current state
 * is valid.
 */
    static int
syn_finish_line(
    int	    syncing)		// called for syncing
{
    stateitem_T	*cur_si;
    colnr_T	prev_current_col;

    while (!current_finished)
    {
	(void)syn_current_attr(syncing, FALSE, NULL, FALSE);
	/*
	 * When syncing, and found some item, need to check the item.
	 */
	if (syncing && current_state.ga_len)
	{
	    /*
	     * Check for match with sync item.
	     */
	    cur_si = &CUR_STATE(current_state.ga_len - 1);
	    if (cur_si->si_idx >= 0
		    && (SYN_ITEMS(syn_block)[cur_si->si_idx].sp_flags
					  & (HL_SYNC_HERE|HL_SYNC_THERE)))
		return TRUE;

	    // syn_current_attr() will have skipped the check for an item
	    // that ends here, need to do that now.  Be careful not to go
	    // past the NUL.
	    prev_current_col = current_col;
	    if (syn_getcurline()[current_col] != NUL)
		++current_col;
	    check_state_ends();
	    current_col = prev_current_col;
	}
	++current_col;
    }
    return FALSE;
}

/*
 * Return highlight attributes for next character.
 * Must first call syntax_start() once for the line.
 * "col" is normally 0 for the first use in a line, and increments by one each
 * time.  It's allowed to skip characters and to stop before the end of the
 * line.  But only a "col" after a previously used column is allowed.
 * When "can_spell" is not NULL set it to TRUE when spell-checking should be
 * done.
 */
    int
get_syntax_attr(
    colnr_T	col,
    int		*can_spell,
    int		keep_state)	// keep state of char at "col"
{
    int	    attr = 0;

    if (can_spell != NULL)
	// Default: Only do spelling when there is no @Spell cluster or when
	// ":syn spell toplevel" was used.
	*can_spell = syn_block->b_syn_spell == SYNSPL_DEFAULT
		    ? (syn_block->b_spell_cluster_id == 0)
		    : (syn_block->b_syn_spell == SYNSPL_TOP);

    // check for out of memory situation
    if (syn_block->b_sst_array == NULL)
	return 0;

    // After 'synmaxcol' the attribute is always zero.
    if (syn_buf->b_p_smc > 0 && col >= (colnr_T)syn_buf->b_p_smc)
    {
	clear_current_state();
#ifdef FEAT_EVAL
	current_id = 0;
	current_trans_id = 0;
#endif
#ifdef FEAT_CONCEAL
	current_flags = 0;
	current_seqnr = 0;
#endif
	return 0;
    }

    // Make sure current_state is valid
    if (INVALID_STATE(&current_state))
	validate_current_state();

    /*
     * Skip from the current column to "col", get the attributes for "col".
     */
    while (current_col <= col)
    {
	attr = syn_current_attr(FALSE, TRUE, can_spell,
				     current_col == col ? keep_state : FALSE);
	++current_col;
    }

    return attr;
}

/*
 * Get syntax attributes for current_lnum, current_col.
 */
    static int
syn_current_attr(
    int		syncing,		// When 1: called for syncing
    int		displaying,		// result will be displayed
    int		*can_spell,		// return: do spell checking
    int		keep_state)		// keep syntax stack afterwards
{
    int		syn_id;
    lpos_T	endpos;		// was: char_u *endp;
    lpos_T	hl_startpos;	// was: int hl_startcol;
    lpos_T	hl_endpos;
    lpos_T	eos_pos;	// end-of-start match (start region)
    lpos_T	eoe_pos;	// end-of-end pattern
    int		end_idx;	// group ID for end pattern
    int		idx;
    synpat_T	*spp;
    stateitem_T	*cur_si, *sip = NULL;
    int		startcol;
    int		endcol;
    long	flags;
    int		cchar;
    short	*next_list;
    int		found_match;		    // found usable match
    static int	try_next_column = FALSE;    // must try in next col
    int		do_keywords;
    regmmatch_T	regmatch;
    lpos_T	pos;
    int		lc_col;
    reg_extmatch_T *cur_extmatch = NULL;
    char_u	buf_chartab[32];  // chartab array for syn iskyeyword
    char_u	*line;		// current line.  NOTE: becomes invalid after
				// looking for a pattern match!

    // variables for zero-width matches that have a "nextgroup" argument
    int		keep_next_list;
    int		zero_width_next_list = FALSE;
    garray_T	zero_width_next_ga;

    /*
     * No character, no attributes!  Past end of line?
     * Do try matching with an empty line (could be the start of a region).
     */
    line = syn_getcurline();
    if (line[current_col] == NUL && current_col != 0)
    {
	/*
	 * If we found a match after the last column, use it.
	 */
	if (next_match_idx >= 0 && next_match_col >= (int)current_col
						  && next_match_col != MAXCOL)
	    (void)push_next_match(NULL);

	current_finished = TRUE;
	current_state_stored = FALSE;
	return 0;
    }

    // if the current or next character is NUL, we will finish the line now
    if (line[current_col] == NUL || line[current_col + 1] == NUL)
    {
	current_finished = TRUE;
	current_state_stored = FALSE;
    }

    /*
     * When in the previous column there was a match but it could not be used
     * (empty match or already matched in this column) need to try again in
     * the next column.
     */
    if (try_next_column)
    {
	next_match_idx = -1;
	try_next_column = FALSE;
    }

    // Only check for keywords when not syncing and there are some.
    do_keywords = !syncing
		    && (syn_block->b_keywtab.ht_used > 0
			    || syn_block->b_keywtab_ic.ht_used > 0);

    // Init the list of zero-width matches with a nextlist.  This is used to
    // avoid matching the same item in the same position twice.
    ga_init2(&zero_width_next_ga, sizeof(int), 10);

    // use syntax iskeyword option
    save_chartab(buf_chartab);

    /*
     * Repeat matching keywords and patterns, to find contained items at the
     * same column.  This stops when there are no extra matches at the current
     * column.
     */
    do
    {
	found_match = FALSE;
	keep_next_list = FALSE;
	syn_id = 0;


	/*
	 * 1. Check for a current state.
	 *    Only when there is no current state, or if the current state may
	 *    contain other things, we need to check for keywords and patterns.
	 *    Always need to check for contained items if some item has the
	 *    "containedin" argument (takes extra time!).
	 */
	if (current_state.ga_len)
	    cur_si = &CUR_STATE(current_state.ga_len - 1);
	else
	    cur_si = NULL;

	if (syn_block->b_syn_containedin || cur_si == NULL
					      || cur_si->si_cont_list != NULL)
	{
	    /*
	     * 2. Check for keywords, if on a keyword char after a non-keyword
	     *	  char.  Don't do this when syncing.
	     */
	    if (do_keywords)
	    {
	      line = syn_getcurline();
	      if (vim_iswordp_buf(line + current_col, syn_buf)
		      && (current_col == 0
			  || !vim_iswordp_buf(line + current_col - 1
			      - (has_mbyte
				  ? (*mb_head_off)(line, line + current_col - 1)
				  : 0) , syn_buf)))
	      {
		syn_id = check_keyword_id(line, (int)current_col,
					 &endcol, &flags, &next_list, cur_si,
					 &cchar);
		if (syn_id != 0)
		{
		    if (push_current_state(KEYWORD_IDX) == OK)
		    {
			cur_si = &CUR_STATE(current_state.ga_len - 1);
			cur_si->si_m_startcol = current_col;
			cur_si->si_h_startpos.lnum = current_lnum;
			cur_si->si_h_startpos.col = 0;	// starts right away
			cur_si->si_m_endpos.lnum = current_lnum;
			cur_si->si_m_endpos.col = endcol;
			cur_si->si_h_endpos.lnum = current_lnum;
			cur_si->si_h_endpos.col = endcol;
			cur_si->si_ends = TRUE;
			cur_si->si_end_idx = 0;
			cur_si->si_flags = flags;
#ifdef FEAT_CONCEAL
			cur_si->si_seqnr = next_seqnr++;
			cur_si->si_cchar = cchar;
			if (current_state.ga_len > 1)
			    cur_si->si_flags |=
				  CUR_STATE(current_state.ga_len - 2).si_flags
								 & HL_CONCEAL;
#endif
			cur_si->si_id = syn_id;
			cur_si->si_trans_id = syn_id;
			if (flags & HL_TRANSP)
			{
			    if (current_state.ga_len < 2)
			    {
				cur_si->si_attr = 0;
				cur_si->si_trans_id = 0;
			    }
			    else
			    {
				cur_si->si_attr = CUR_STATE(
					current_state.ga_len - 2).si_attr;
				cur_si->si_trans_id = CUR_STATE(
					current_state.ga_len - 2).si_trans_id;
			    }
			}
			else
			    cur_si->si_attr = syn_id2attr(syn_id);
			cur_si->si_cont_list = NULL;
			cur_si->si_next_list = next_list;
			check_keepend();
		    }
		    else
			vim_free(next_list);
		}
	      }
	    }

	    /*
	     * 3. Check for patterns (only if no keyword found).
	     */
	    if (syn_id == 0 && syn_block->b_syn_patterns.ga_len)
	    {
		/*
		 * If we didn't check for a match yet, or we are past it, check
		 * for any match with a pattern.
		 */
		if (next_match_idx < 0 || next_match_col < (int)current_col)
		{
		    /*
		     * Check all relevant patterns for a match at this
		     * position.  This is complicated, because matching with a
		     * pattern takes quite a bit of time, thus we want to
		     * avoid doing it when it's not needed.
		     */
		    next_match_idx = 0;		// no match in this line yet
		    next_match_col = MAXCOL;
		    for (idx = syn_block->b_syn_patterns.ga_len; --idx >= 0; )
		    {
			spp = &(SYN_ITEMS(syn_block)[idx]);
			if (	   spp->sp_syncing == syncing
				&& (displaying || !(spp->sp_flags & HL_DISPLAY))
				&& (spp->sp_type == SPTYPE_MATCH
				    || spp->sp_type == SPTYPE_START)
				&& (current_next_list != NULL
				    ? in_id_list(NULL, current_next_list,
							      &spp->sp_syn, 0)
				    : (cur_si == NULL
					? !(spp->sp_flags & HL_CONTAINED)
					: in_id_list(cur_si,
					    cur_si->si_cont_list, &spp->sp_syn,
					    spp->sp_flags))))
			{
			    int r;

			    // If we already tried matching in this line, and
			    // there isn't a match before next_match_col, skip
			    // this item.
			    if (spp->sp_line_id == current_line_id
				    && spp->sp_startcol >= next_match_col)
				continue;
			    spp->sp_line_id = current_line_id;

			    lc_col = current_col - spp->sp_offsets[SPO_LC_OFF];
			    if (lc_col < 0)
				lc_col = 0;

			    regmatch.rmm_ic = spp->sp_ic;
			    regmatch.regprog = spp->sp_prog;
			    r = syn_regexec(&regmatch,
					     current_lnum,
					     (colnr_T)lc_col,
					     IF_SYN_TIME(&spp->sp_time));
			    spp->sp_prog = regmatch.regprog;
			    if (!r)
			    {
				// no match in this line, try another one
				spp->sp_startcol = MAXCOL;
				continue;
			    }

			    /*
			     * Compute the first column of the match.
			     */
			    syn_add_start_off(&pos, &regmatch,
							 spp, SPO_MS_OFF, -1);
			    if (pos.lnum > current_lnum)
			    {
				// must have used end of match in a next line,
				// we can't handle that
				spp->sp_startcol = MAXCOL;
				continue;
			    }
			    startcol = pos.col;

			    // remember the next column where this pattern
			    // matches in the current line
			    spp->sp_startcol = startcol;

			    /*
			     * If a previously found match starts at a lower
			     * column number, don't use this one.
			     */
			    if (startcol >= next_match_col)
				continue;

			    /*
			     * If we matched this pattern at this position
			     * before, skip it.  Must retry in the next
			     * column, because it may match from there.
			     */
			    if (did_match_already(idx, &zero_width_next_ga))
			    {
				try_next_column = TRUE;
				continue;
			    }

			    endpos.lnum = regmatch.endpos[0].lnum;
			    endpos.col = regmatch.endpos[0].col;

			    // Compute the highlight start.
			    syn_add_start_off(&hl_startpos, &regmatch,
							 spp, SPO_HS_OFF, -1);

			    // Compute the region start.
			    // Default is to use the end of the match.
			    syn_add_end_off(&eos_pos, &regmatch,
							 spp, SPO_RS_OFF, 0);

			    /*
			     * Grab the external submatches before they get
			     * overwritten.  Reference count doesn't change.
			     */
			    unref_extmatch(cur_extmatch);
			    cur_extmatch = re_extmatch_out;
			    re_extmatch_out = NULL;

			    flags = 0;
			    eoe_pos.lnum = 0;	// avoid warning
			    eoe_pos.col = 0;
			    end_idx = 0;
			    hl_endpos.lnum = 0;

			    /*
			     * For a "oneline" the end must be found in the
			     * same line too.  Search for it after the end of
			     * the match with the start pattern.  Set the
			     * resulting end positions at the same time.
			     */
			    if (spp->sp_type == SPTYPE_START
					      && (spp->sp_flags & HL_ONELINE))
			    {
				lpos_T	startpos;

				startpos = endpos;
				find_endpos(idx, &startpos, &endpos, &hl_endpos,
				    &flags, &eoe_pos, &end_idx, cur_extmatch);
				if (endpos.lnum == 0)
				    continue;	    // not found
			    }

			    /*
			     * For a "match" the size must be > 0 after the
			     * end offset needs has been added.  Except when
			     * syncing.
			     */
			    else if (spp->sp_type == SPTYPE_MATCH)
			    {
				syn_add_end_off(&hl_endpos, &regmatch, spp,
							       SPO_HE_OFF, 0);
				syn_add_end_off(&endpos, &regmatch, spp,
							       SPO_ME_OFF, 0);
				if (endpos.lnum == current_lnum
				      && (int)endpos.col + syncing < startcol)
				{
				    /*
				     * If an empty string is matched, may need
				     * to try matching again at next column.
				     */
				    if (regmatch.startpos[0].col
						    == regmatch.endpos[0].col)
					try_next_column = TRUE;
				    continue;
				}
			    }

			    /*
			     * keep the best match so far in next_match_*
			     */
			    // Highlighting must start after startpos and end
			    // before endpos.
			    if (hl_startpos.lnum == current_lnum
					   && (int)hl_startpos.col < startcol)
				hl_startpos.col = startcol;
			    limit_pos_zero(&hl_endpos, &endpos);

			    next_match_idx = idx;
			    next_match_col = startcol;
			    next_match_m_endpos = endpos;
			    next_match_h_endpos = hl_endpos;
			    next_match_h_startpos = hl_startpos;
			    next_match_flags = flags;
			    next_match_eos_pos = eos_pos;
			    next_match_eoe_pos = eoe_pos;
			    next_match_end_idx = end_idx;
			    unref_extmatch(next_match_extmatch);
			    next_match_extmatch = cur_extmatch;
			    cur_extmatch = NULL;
			}
		    }
		}

		/*
		 * If we found a match at the current column, use it.
		 */
		if (next_match_idx >= 0 && next_match_col == (int)current_col)
		{
		    synpat_T	*lspp;

		    // When a zero-width item matched which has a nextgroup,
		    // don't push the item but set nextgroup.
		    lspp = &(SYN_ITEMS(syn_block)[next_match_idx]);
		    if (next_match_m_endpos.lnum == current_lnum
			    && next_match_m_endpos.col == current_col
			    && lspp->sp_next_list != NULL)
		    {
			current_next_list = lspp->sp_next_list;
			current_next_flags = lspp->sp_flags;
			keep_next_list = TRUE;
			zero_width_next_list = TRUE;

			// Add the index to a list, so that we can check
			// later that we don't match it again (and cause an
			// endless loop).
			if (ga_grow(&zero_width_next_ga, 1) == OK)
			{
			    ((int *)(zero_width_next_ga.ga_data))
				[zero_width_next_ga.ga_len++] = next_match_idx;
			}
			next_match_idx = -1;
		    }
		    else
			cur_si = push_next_match(cur_si);
		    found_match = TRUE;
		}
	    }
	}

	/*
	 * Handle searching for nextgroup match.
	 */
	if (current_next_list != NULL && !keep_next_list)
	{
	    /*
	     * If a nextgroup was not found, continue looking for one if:
	     * - this is an empty line and the "skipempty" option was given
	     * - we are on white space and the "skipwhite" option was given
	     */
	    if (!found_match)
	    {
		line = syn_getcurline();
		if (((current_next_flags & HL_SKIPWHITE)
			    && VIM_ISWHITE(line[current_col]))
			|| ((current_next_flags & HL_SKIPEMPTY)
			    && *line == NUL))
		    break;
	    }

	    /*
	     * If a nextgroup was found: Use it, and continue looking for
	     * contained matches.
	     * If a nextgroup was not found: Continue looking for a normal
	     * match.
	     * When did set current_next_list for a zero-width item and no
	     * match was found don't loop (would get stuck).
	     */
	    current_next_list = NULL;
	    next_match_idx = -1;
	    if (!zero_width_next_list)
		found_match = TRUE;
	}

    } while (found_match);

    restore_chartab(buf_chartab);

    /*
     * Use attributes from the current state, if within its highlighting.
     * If not, use attributes from the current-but-one state, etc.
     */
    current_attr = 0;
#ifdef FEAT_EVAL
    current_id = 0;
    current_trans_id = 0;
#endif
#ifdef FEAT_CONCEAL
    current_flags = 0;
    current_seqnr = 0;
#endif
    if (cur_si != NULL)
    {
#ifndef FEAT_EVAL
	int	current_trans_id = 0;
#endif
	for (idx = current_state.ga_len - 1; idx >= 0; --idx)
	{
	    sip = &CUR_STATE(idx);
	    if ((current_lnum > sip->si_h_startpos.lnum
			|| (current_lnum == sip->si_h_startpos.lnum
			    && current_col >= sip->si_h_startpos.col))
		    && (sip->si_h_endpos.lnum == 0
			|| current_lnum < sip->si_h_endpos.lnum
			|| (current_lnum == sip->si_h_endpos.lnum
			    && current_col < sip->si_h_endpos.col)))
	    {
		current_attr = sip->si_attr;
#ifdef FEAT_EVAL
		current_id = sip->si_id;
#endif
		current_trans_id = sip->si_trans_id;
#ifdef FEAT_CONCEAL
		current_flags = sip->si_flags;
		current_seqnr = sip->si_seqnr;
		current_sub_char = sip->si_cchar;
#endif
		break;
	    }
	}

	if (can_spell != NULL)
	{
	    struct sp_syn   sps;

	    /*
	     * set "can_spell" to TRUE if spell checking is supposed to be
	     * done in the current item.
	     */
	    if (syn_block->b_spell_cluster_id == 0)
	    {
		// There is no @Spell cluster: Do spelling for items without
		// @NoSpell cluster.
		if (syn_block->b_nospell_cluster_id == 0
						     || current_trans_id == 0)
		    *can_spell = (syn_block->b_syn_spell != SYNSPL_NOTOP);
		else
		{
		    sps.inc_tag = 0;
		    sps.id = syn_block->b_nospell_cluster_id;
		    sps.cont_in_list = NULL;
		    *can_spell = !in_id_list(sip, sip->si_cont_list, &sps, 0);
		}
	    }
	    else
	    {
		// The @Spell cluster is defined: Do spelling in items with
		// the @Spell cluster.  But not when @NoSpell is also there.
		// At the toplevel only spell check when ":syn spell toplevel"
		// was used.
		if (current_trans_id == 0)
		    *can_spell = (syn_block->b_syn_spell == SYNSPL_TOP);
		else
		{
		    sps.inc_tag = 0;
		    sps.id = syn_block->b_spell_cluster_id;
		    sps.cont_in_list = NULL;
		    *can_spell = in_id_list(sip, sip->si_cont_list, &sps, 0);

		    if (syn_block->b_nospell_cluster_id != 0)
		    {
			sps.id = syn_block->b_nospell_cluster_id;
			if (in_id_list(sip, sip->si_cont_list, &sps, 0))
			    *can_spell = FALSE;
		    }
		}
	    }
	}


	/*
	 * Check for end of current state (and the states before it) at the
	 * next column.  Don't do this for syncing, because we would miss a
	 * single character match.
	 * First check if the current state ends at the current column.  It
	 * may be for an empty match and a containing item might end in the
	 * current column.
	 */
	if (!syncing && !keep_state)
	{
	    check_state_ends();
	    if (current_state.ga_len > 0
				      && syn_getcurline()[current_col] != NUL)
	    {
		++current_col;
		check_state_ends();
		--current_col;
	    }
	}
    }
    else if (can_spell != NULL)
	// Default: Only do spelling when there is no @Spell cluster or when
	// ":syn spell toplevel" was used.
	*can_spell = syn_block->b_syn_spell == SYNSPL_DEFAULT
		    ? (syn_block->b_spell_cluster_id == 0)
		    : (syn_block->b_syn_spell == SYNSPL_TOP);

    // nextgroup ends at end of line, unless "skipnl" or "skipempty" present
    if (current_next_list != NULL
	    && (line = syn_getcurline())[current_col] != NUL
	    && line[current_col + 1] == NUL
	    && !(current_next_flags & (HL_SKIPNL | HL_SKIPEMPTY)))
	current_next_list = NULL;

    if (zero_width_next_ga.ga_len > 0)
	ga_clear(&zero_width_next_ga);

    // No longer need external matches.  But keep next_match_extmatch.
    unref_extmatch(re_extmatch_out);
    re_extmatch_out = NULL;
    unref_extmatch(cur_extmatch);

    return current_attr;
}


/*
 * Check if we already matched pattern "idx" at the current column.
 */
    static int
did_match_already(int idx, garray_T *gap)
{
    int		i;

    for (i = current_state.ga_len; --i >= 0; )
	if (CUR_STATE(i).si_m_startcol == (int)current_col
		&& CUR_STATE(i).si_m_lnum == (int)current_lnum
		&& CUR_STATE(i).si_idx == idx)
	    return TRUE;

    // Zero-width matches with a nextgroup argument are not put on the syntax
    // stack, and can only be matched once anyway.
    for (i = gap->ga_len; --i >= 0; )
	if (((int *)(gap->ga_data))[i] == idx)
	    return TRUE;

    return FALSE;
}

/*
 * Push the next match onto the stack.
 */
    static stateitem_T *
push_next_match(stateitem_T *cur_si)
{
    synpat_T	*spp;
#ifdef FEAT_CONCEAL
    int		 save_flags;
#endif

    spp = &(SYN_ITEMS(syn_block)[next_match_idx]);

    /*
     * Push the item in current_state stack;
     */
    if (push_current_state(next_match_idx) == OK)
    {
	/*
	 * If it's a start-skip-end type that crosses lines, figure out how
	 * much it continues in this line.  Otherwise just fill in the length.
	 */
	cur_si = &CUR_STATE(current_state.ga_len - 1);
	cur_si->si_h_startpos = next_match_h_startpos;
	cur_si->si_m_startcol = current_col;
	cur_si->si_m_lnum = current_lnum;
	cur_si->si_flags = spp->sp_flags;
#ifdef FEAT_CONCEAL
	cur_si->si_seqnr = next_seqnr++;
	cur_si->si_cchar = spp->sp_cchar;
	if (current_state.ga_len > 1)
	    cur_si->si_flags |=
		    CUR_STATE(current_state.ga_len - 2).si_flags & HL_CONCEAL;
#endif
	cur_si->si_next_list = spp->sp_next_list;
	cur_si->si_extmatch = ref_extmatch(next_match_extmatch);
	if (spp->sp_type == SPTYPE_START && !(spp->sp_flags & HL_ONELINE))
	{
	    // Try to find the end pattern in the current line
	    update_si_end(cur_si, (int)(next_match_m_endpos.col), TRUE);
	    check_keepend();
	}
	else
	{
	    cur_si->si_m_endpos = next_match_m_endpos;
	    cur_si->si_h_endpos = next_match_h_endpos;
	    cur_si->si_ends = TRUE;
	    cur_si->si_flags |= next_match_flags;
	    cur_si->si_eoe_pos = next_match_eoe_pos;
	    cur_si->si_end_idx = next_match_end_idx;
	}
	if (keepend_level < 0 && (cur_si->si_flags & HL_KEEPEND))
	    keepend_level = current_state.ga_len - 1;
	check_keepend();
	update_si_attr(current_state.ga_len - 1);

#ifdef FEAT_CONCEAL
	save_flags = cur_si->si_flags & (HL_CONCEAL | HL_CONCEALENDS);
#endif
	/*
	 * If the start pattern has another highlight group, push another item
	 * on the stack for the start pattern.
	 */
	if (	   spp->sp_type == SPTYPE_START
		&& spp->sp_syn_match_id != 0
		&& push_current_state(next_match_idx) == OK)
	{
	    cur_si = &CUR_STATE(current_state.ga_len - 1);
	    cur_si->si_h_startpos = next_match_h_startpos;
	    cur_si->si_m_startcol = current_col;
	    cur_si->si_m_lnum = current_lnum;
	    cur_si->si_m_endpos = next_match_eos_pos;
	    cur_si->si_h_endpos = next_match_eos_pos;
	    cur_si->si_ends = TRUE;
	    cur_si->si_end_idx = 0;
	    cur_si->si_flags = HL_MATCH;
#ifdef FEAT_CONCEAL
	    cur_si->si_seqnr = next_seqnr++;
	    cur_si->si_flags |= save_flags;
	    if (cur_si->si_flags & HL_CONCEALENDS)
		cur_si->si_flags |= HL_CONCEAL;
#endif
	    cur_si->si_next_list = NULL;
	    check_keepend();
	    update_si_attr(current_state.ga_len - 1);
	}
    }

    next_match_idx = -1;	// try other match next time

    return cur_si;
}

/*
 * Check for end of current state (and the states before it).
 */
    static void
check_state_ends(void)
{
    stateitem_T	*cur_si;
    int		had_extend;

    cur_si = &CUR_STATE(current_state.ga_len - 1);
    for (;;)
    {
	if (cur_si->si_ends
		&& (cur_si->si_m_endpos.lnum < current_lnum
		    || (cur_si->si_m_endpos.lnum == current_lnum
			&& cur_si->si_m_endpos.col <= current_col)))
	{
	    /*
	     * If there is an end pattern group ID, highlight the end pattern
	     * now.  No need to pop the current item from the stack.
	     * Only do this if the end pattern continues beyond the current
	     * position.
	     */
	    if (cur_si->si_end_idx
		    && (cur_si->si_eoe_pos.lnum > current_lnum
			|| (cur_si->si_eoe_pos.lnum == current_lnum
			    && cur_si->si_eoe_pos.col > current_col)))
	    {
		cur_si->si_idx = cur_si->si_end_idx;
		cur_si->si_end_idx = 0;
		cur_si->si_m_endpos = cur_si->si_eoe_pos;
		cur_si->si_h_endpos = cur_si->si_eoe_pos;
		cur_si->si_flags |= HL_MATCH;
#ifdef FEAT_CONCEAL
		cur_si->si_seqnr = next_seqnr++;
		if (cur_si->si_flags & HL_CONCEALENDS)
		    cur_si->si_flags |= HL_CONCEAL;
#endif
		update_si_attr(current_state.ga_len - 1);

		// nextgroup= should not match in the end pattern
		current_next_list = NULL;

		// what matches next may be different now, clear it
		next_match_idx = 0;
		next_match_col = MAXCOL;
		break;
	    }

	    // handle next_list, unless at end of line and no "skipnl" or
	    // "skipempty"
	    current_next_list = cur_si->si_next_list;
	    current_next_flags = cur_si->si_flags;
	    if (!(current_next_flags & (HL_SKIPNL | HL_SKIPEMPTY))
		    && syn_getcurline()[current_col] == NUL)
		current_next_list = NULL;

	    // When the ended item has "extend", another item with
	    // "keepend" now needs to check for its end.
	    had_extend = (cur_si->si_flags & HL_EXTEND);

	    pop_current_state();

	    if (current_state.ga_len == 0)
		break;

	    if (had_extend && keepend_level >= 0)
	    {
		syn_update_ends(FALSE);
		if (current_state.ga_len == 0)
		    break;
	    }

	    cur_si = &CUR_STATE(current_state.ga_len - 1);

	    /*
	     * Only for a region the search for the end continues after
	     * the end of the contained item.  If the contained match
	     * included the end-of-line, break here, the region continues.
	     * Don't do this when:
	     * - "keepend" is used for the contained item
	     * - not at the end of the line (could be end="x$"me=e-1).
	     * - "excludenl" is used (HL_HAS_EOL won't be set)
	     */
	    if (cur_si->si_idx >= 0
		    && SYN_ITEMS(syn_block)[cur_si->si_idx].sp_type
								== SPTYPE_START
		    && !(cur_si->si_flags & (HL_MATCH | HL_KEEPEND)))
	    {
		update_si_end(cur_si, (int)current_col, TRUE);
		check_keepend();
		if ((current_next_flags & HL_HAS_EOL)
			&& keepend_level < 0
			&& syn_getcurline()[current_col] == NUL)
		    break;
	    }
	}
	else
	    break;
    }
}

/*
 * Update an entry in the current_state stack for a match or region.  This
 * fills in si_attr, si_next_list and si_cont_list.
 */
    static void
update_si_attr(int idx)
{
    stateitem_T	*sip = &CUR_STATE(idx);
    synpat_T	*spp;

    // This should not happen...
    if (sip->si_idx < 0)
	return;

    spp = &(SYN_ITEMS(syn_block)[sip->si_idx]);
    if (sip->si_flags & HL_MATCH)
	sip->si_id = spp->sp_syn_match_id;
    else
	sip->si_id = spp->sp_syn.id;
    sip->si_attr = syn_id2attr(sip->si_id);
    sip->si_trans_id = sip->si_id;
    if (sip->si_flags & HL_MATCH)
	sip->si_cont_list = NULL;
    else
	sip->si_cont_list = spp->sp_cont_list;

    /*
     * For transparent items, take attr from outer item.
     * Also take cont_list, if there is none.
     * Don't do this for the matchgroup of a start or end pattern.
     */
    if ((spp->sp_flags & HL_TRANSP) && !(sip->si_flags & HL_MATCH))
    {
	if (idx == 0)
	{
	    sip->si_attr = 0;
	    sip->si_trans_id = 0;
	    if (sip->si_cont_list == NULL)
		sip->si_cont_list = ID_LIST_ALL;
	}
	else
	{
	    sip->si_attr = CUR_STATE(idx - 1).si_attr;
	    sip->si_trans_id = CUR_STATE(idx - 1).si_trans_id;
	    if (sip->si_cont_list == NULL)
	    {
		sip->si_flags |= HL_TRANS_CONT;
		sip->si_cont_list = CUR_STATE(idx - 1).si_cont_list;
	    }
	}
    }
}

/*
 * Check the current stack for patterns with "keepend" flag.
 * Propagate the match-end to contained items, until a "skipend" item is found.
 */
    static void
check_keepend(void)
{
    int		i;
    lpos_T	maxpos;
    lpos_T	maxpos_h;
    stateitem_T	*sip;

    /*
     * This check can consume a lot of time; only do it from the level where
     * there really is a keepend.
     */
    if (keepend_level < 0)
	return;

    /*
     * Find the last index of an "extend" item.  "keepend" items before that
     * won't do anything.  If there is no "extend" item "i" will be
     * "keepend_level" and all "keepend" items will work normally.
     */
    for (i = current_state.ga_len - 1; i > keepend_level; --i)
	if (CUR_STATE(i).si_flags & HL_EXTEND)
	    break;

    maxpos.lnum = 0;
    maxpos.col = 0;
    maxpos_h.lnum = 0;
    maxpos_h.col = 0;
    for ( ; i < current_state.ga_len; ++i)
    {
	sip = &CUR_STATE(i);
	if (maxpos.lnum != 0)
	{
	    limit_pos_zero(&sip->si_m_endpos, &maxpos);
	    limit_pos_zero(&sip->si_h_endpos, &maxpos_h);
	    limit_pos_zero(&sip->si_eoe_pos, &maxpos);
	    sip->si_ends = TRUE;
	}
	if (sip->si_ends && (sip->si_flags & HL_KEEPEND))
	{
	    if (maxpos.lnum == 0
		    || maxpos.lnum > sip->si_m_endpos.lnum
		    || (maxpos.lnum == sip->si_m_endpos.lnum
			&& maxpos.col > sip->si_m_endpos.col))
		maxpos = sip->si_m_endpos;
	    if (maxpos_h.lnum == 0
		    || maxpos_h.lnum > sip->si_h_endpos.lnum
		    || (maxpos_h.lnum == sip->si_h_endpos.lnum
			&& maxpos_h.col > sip->si_h_endpos.col))
		maxpos_h = sip->si_h_endpos;
	}
    }
}

/*
 * Update an entry in the current_state stack for a start-skip-end pattern.
 * This finds the end of the current item, if it's in the current line.
 *
 * Return the flags for the matched END.
 */
    static void
update_si_end(
    stateitem_T	*sip,
    int		startcol,   // where to start searching for the end
    int		force)	    // when TRUE overrule a previous end
{
    lpos_T	startpos;
    lpos_T	endpos;
    lpos_T	hl_endpos;
    lpos_T	end_endpos;
    int		end_idx;

    // return quickly for a keyword
    if (sip->si_idx < 0)
	return;

    // Don't update when it's already done.  Can be a match of an end pattern
    // that started in a previous line.  Watch out: can also be a "keepend"
    // from a containing item.
    if (!force && sip->si_m_endpos.lnum >= current_lnum)
	return;

    /*
     * We need to find the end of the region.  It may continue in the next
     * line.
     */
    end_idx = 0;
    startpos.lnum = current_lnum;
    startpos.col = startcol;
    find_endpos(sip->si_idx, &startpos, &endpos, &hl_endpos,
		   &(sip->si_flags), &end_endpos, &end_idx, sip->si_extmatch);

    if (endpos.lnum == 0)
    {
	// No end pattern matched.
	if (SYN_ITEMS(syn_block)[sip->si_idx].sp_flags & HL_ONELINE)
	{
	    // a "oneline" never continues in the next line
	    sip->si_ends = TRUE;
	    sip->si_m_endpos.lnum = current_lnum;
	    sip->si_m_endpos.col = syn_getcurline_len();
	}
	else
	{
	    // continues in the next line
	    sip->si_ends = FALSE;
	    sip->si_m_endpos.lnum = 0;
	}
	sip->si_h_endpos = sip->si_m_endpos;
    }
    else
    {
	// match within this line
	sip->si_m_endpos = endpos;
	sip->si_h_endpos = hl_endpos;
	sip->si_eoe_pos = end_endpos;
	sip->si_ends = TRUE;
	sip->si_end_idx = end_idx;
    }
}

/*
 * Add a new state to the current state stack.
 * It is cleared and the index set to "idx".
 * Return FAIL if it's not possible (out of memory).
 */
    static int
push_current_state(int idx)
{
    if (ga_grow(&current_state, 1) == FAIL)
	return FAIL;
    CLEAR_POINTER(&CUR_STATE(current_state.ga_len));
    CUR_STATE(current_state.ga_len).si_idx = idx;
    ++current_state.ga_len;
    return OK;
}

/*
 * Remove a state from the current_state stack.
 */
    static void
pop_current_state(void)
{
    if (current_state.ga_len)
    {
	unref_extmatch(CUR_STATE(current_state.ga_len - 1).si_extmatch);
	--current_state.ga_len;
    }
    // after the end of a pattern, try matching a keyword or pattern
    next_match_idx = -1;

    // if first state with "keepend" is popped, reset keepend_level
    if (keepend_level >= current_state.ga_len)
	keepend_level = -1;
}

/*
 * Find the end of a start/skip/end syntax region after "startpos".
 * Only checks one line.
 * Also handles a match item that continued from a previous line.
 * If not found, the syntax item continues in the next line.  m_endpos->lnum
 * will be 0.
 * If found, the end of the region and the end of the highlighting is
 * computed.
 */
    static void
find_endpos(
    int		idx,		// index of the pattern
    lpos_T	*startpos,	// where to start looking for an END match
    lpos_T	*m_endpos,	// return: end of match
    lpos_T	*hl_endpos,	// return: end of highlighting
    long	*flagsp,	// return: flags of matching END
    lpos_T	*end_endpos,	// return: end of end pattern match
    int		*end_idx,	// return: group ID for end pat. match, or 0
    reg_extmatch_T *start_ext)	// submatches from the start pattern
{
    colnr_T	matchcol;
    synpat_T	*spp, *spp_skip;
    int		start_idx;
    int		best_idx;
    regmmatch_T	regmatch;
    regmmatch_T	best_regmatch;	    // startpos/endpos of best match
    lpos_T	pos;
    int		had_match = FALSE;
    char_u	buf_chartab[32];  // chartab array for syn option iskyeyword

    // just in case we are invoked for a keyword
    if (idx < 0)
	return;

    /*
     * Check for being called with a START pattern.
     * Can happen with a match that continues to the next line, because it
     * contained a region.
     */
    spp = &(SYN_ITEMS(syn_block)[idx]);
    if (spp->sp_type != SPTYPE_START)
    {
	*hl_endpos = *startpos;
	return;
    }

    /*
     * Find the SKIP or first END pattern after the last START pattern.
     */
    for (;;)
    {
	spp = &(SYN_ITEMS(syn_block)[idx]);
	if (spp->sp_type != SPTYPE_START)
	    break;
	++idx;
    }

    /*
     *	Lookup the SKIP pattern (if present)
     */
    if (spp->sp_type == SPTYPE_SKIP)
    {
	spp_skip = spp;
	++idx;
    }
    else
	spp_skip = NULL;

    // Setup external matches for syn_regexec().
    unref_extmatch(re_extmatch_in);
    re_extmatch_in = ref_extmatch(start_ext);

    matchcol = startpos->col;	// start looking for a match at sstart
    start_idx = idx;		// remember the first END pattern.
    best_regmatch.startpos[0].col = 0;		// avoid compiler warning

    // use syntax iskeyword option
    save_chartab(buf_chartab);

    for (;;)
    {
	/*
	 * Find end pattern that matches first after "matchcol".
	 */
	best_idx = -1;
	for (idx = start_idx; idx < syn_block->b_syn_patterns.ga_len; ++idx)
	{
	    int lc_col = matchcol;
	    int r;

	    spp = &(SYN_ITEMS(syn_block)[idx]);
	    if (spp->sp_type != SPTYPE_END)	// past last END pattern
		break;
	    lc_col -= spp->sp_offsets[SPO_LC_OFF];
	    if (lc_col < 0)
		lc_col = 0;

	    regmatch.rmm_ic = spp->sp_ic;
	    regmatch.regprog = spp->sp_prog;
	    r = syn_regexec(&regmatch, startpos->lnum, lc_col,
						  IF_SYN_TIME(&spp->sp_time));
	    spp->sp_prog = regmatch.regprog;
	    if (r)
	    {
		if (best_idx == -1 || regmatch.startpos[0].col
					      < best_regmatch.startpos[0].col)
		{
		    best_idx = idx;
		    best_regmatch.startpos[0] = regmatch.startpos[0];
		    best_regmatch.endpos[0] = regmatch.endpos[0];
		}
	    }
	}

	/*
	 * If all end patterns have been tried, and there is no match, the
	 * item continues until end-of-line.
	 */
	if (best_idx == -1)
	    break;

	/*
	 * If the skip pattern matches before the end pattern,
	 * continue searching after the skip pattern.
	 */
	if (spp_skip != NULL)
	{
	    int lc_col = matchcol - spp_skip->sp_offsets[SPO_LC_OFF];
	    int r;

	    if (lc_col < 0)
		lc_col = 0;
	    regmatch.rmm_ic = spp_skip->sp_ic;
	    regmatch.regprog = spp_skip->sp_prog;
	    r = syn_regexec(&regmatch, startpos->lnum, lc_col,
					      IF_SYN_TIME(&spp_skip->sp_time));
	    spp_skip->sp_prog = regmatch.regprog;
	    if (r && regmatch.startpos[0].col
					     <= best_regmatch.startpos[0].col)
	    {
		int line_len;

		// Add offset to skip pattern match
		syn_add_end_off(&pos, &regmatch, spp_skip, SPO_ME_OFF, 1);

		// If the skip pattern goes on to the next line, there is no
		// match with an end pattern in this line.
		if (pos.lnum > startpos->lnum)
		    break;

		line_len = ml_get_buf_len(syn_buf, startpos->lnum);

		// take care of an empty match or negative offset
		if (pos.col <= matchcol)
		    ++matchcol;
		else if (pos.col <= regmatch.endpos[0].col)
		    matchcol = pos.col;
		else
		    // Be careful not to jump over the NUL at the end-of-line
		    for (matchcol = regmatch.endpos[0].col;
			    matchcol < line_len && matchcol < pos.col;
								   ++matchcol)
			;

		// if the skip pattern includes end-of-line, break here
		if (matchcol >= line_len)
		    break;

		continue;	    // start with first end pattern again
	    }
	}

	/*
	 * Match from start pattern to end pattern.
	 * Correct for match and highlight offset of end pattern.
	 */
	spp = &(SYN_ITEMS(syn_block)[best_idx]);
	syn_add_end_off(m_endpos, &best_regmatch, spp, SPO_ME_OFF, 1);
	// can't end before the start
	if (m_endpos->lnum == startpos->lnum && m_endpos->col < startpos->col)
	    m_endpos->col = startpos->col;

	syn_add_end_off(end_endpos, &best_regmatch, spp, SPO_HE_OFF, 1);
	// can't end before the start
	if (end_endpos->lnum == startpos->lnum
					   && end_endpos->col < startpos->col)
	    end_endpos->col = startpos->col;
	// can't end after the match
	limit_pos(end_endpos, m_endpos);

	/*
	 * If the end group is highlighted differently, adjust the pointers.
	 */
	if (spp->sp_syn_match_id != spp->sp_syn.id && spp->sp_syn_match_id != 0)
	{
	    *end_idx = best_idx;
	    if (spp->sp_off_flags & (1 << (SPO_RE_OFF + SPO_COUNT)))
	    {
		hl_endpos->lnum = best_regmatch.endpos[0].lnum;
		hl_endpos->col = best_regmatch.endpos[0].col;
	    }
	    else
	    {
		hl_endpos->lnum = best_regmatch.startpos[0].lnum;
		hl_endpos->col = best_regmatch.startpos[0].col;
	    }
	    hl_endpos->col += spp->sp_offsets[SPO_RE_OFF];

	    // can't end before the start
	    if (hl_endpos->lnum == startpos->lnum
					    && hl_endpos->col < startpos->col)
		hl_endpos->col = startpos->col;
	    limit_pos(hl_endpos, m_endpos);

	    // now the match ends where the highlighting ends, it is turned
	    // into the matchgroup for the end
	    *m_endpos = *hl_endpos;
	}
	else
	{
	    *end_idx = 0;
	    *hl_endpos = *end_endpos;
	}

	*flagsp = spp->sp_flags;

	had_match = TRUE;
	break;
    }

    // no match for an END pattern in this line
    if (!had_match)
	m_endpos->lnum = 0;

    restore_chartab(buf_chartab);

    // Remove external matches.
    unref_extmatch(re_extmatch_in);
    re_extmatch_in = NULL;
}

/*
 * Limit "pos" not to be after "limit".
 */
    static void
limit_pos(lpos_T *pos, lpos_T *limit)
{
    if (pos->lnum > limit->lnum)
	*pos = *limit;
    else if (pos->lnum == limit->lnum && pos->col > limit->col)
	pos->col = limit->col;
}

/*
 * Limit "pos" not to be after "limit", unless pos->lnum is zero.
 */
    static void
limit_pos_zero(
    lpos_T	*pos,
    lpos_T	*limit)
{
    if (pos->lnum == 0)
	*pos = *limit;
    else
	limit_pos(pos, limit);
}

/*
 * Add offset to matched text for end of match or highlight.
 */
    static void
syn_add_end_off(
    lpos_T	*result,	// returned position
    regmmatch_T	*regmatch,	// start/end of match
    synpat_T	*spp,		// matched pattern
    int		idx,		// index of offset
    int		extra)		// extra chars for offset to start
{
    int		col;
    int		off;
    char_u	*base;
    char_u	*p;

    if (spp->sp_off_flags & (1 << idx))
    {
	result->lnum = regmatch->startpos[0].lnum;
	col = regmatch->startpos[0].col;
	off = spp->sp_offsets[idx] + extra;
    }
    else
    {
	result->lnum = regmatch->endpos[0].lnum;
	col = regmatch->endpos[0].col;
	off = spp->sp_offsets[idx];
    }
    // Don't go past the end of the line.  Matters for "rs=e+2" when there
    // is a matchgroup. Watch out for match with last NL in the buffer.
    if (result->lnum > syn_buf->b_ml.ml_line_count)
	col = 0;
    else if (off != 0)
    {
	base = ml_get_buf(syn_buf, result->lnum, FALSE);
	p = base + col;
	if (off > 0)
	{
	    while (off-- > 0 && *p != NUL)
		MB_PTR_ADV(p);
	}
	else if (off < 0)
	{
	    while (off++ < 0 && base < p)
		MB_PTR_BACK(base, p);
	}
	col = (int)(p - base);
    }
    result->col = col;
}

/*
 * Add offset to matched text for start of match or highlight.
 * Avoid resulting column to become negative.
 */
    static void
syn_add_start_off(
    lpos_T	*result,	// returned position
    regmmatch_T	*regmatch,	// start/end of match
    synpat_T	*spp,
    int		idx,
    int		extra)	    // extra chars for offset to end
{
    int		col;
    int		off;
    char_u	*base;
    char_u	*p;

    if (spp->sp_off_flags & (1 << (idx + SPO_COUNT)))
    {
	result->lnum = regmatch->endpos[0].lnum;
	col = regmatch->endpos[0].col;
	off = spp->sp_offsets[idx] + extra;
    }
    else
    {
	result->lnum = regmatch->startpos[0].lnum;
	col = regmatch->startpos[0].col;
	off = spp->sp_offsets[idx];
    }
    if (result->lnum > syn_buf->b_ml.ml_line_count)
    {
	// a "\n" at the end of the pattern may take us below the last line
	result->lnum = syn_buf->b_ml.ml_line_count;
	col = ml_get_buf_len(syn_buf, result->lnum);
    }
    if (off != 0)
    {
	base = ml_get_buf(syn_buf, result->lnum, FALSE);
	p = base + col;
	if (off > 0)
	{
	    while (off-- && *p != NUL)
		MB_PTR_ADV(p);
	}
	else if (off < 0)
	{
	    while (off++ && base < p)
		MB_PTR_BACK(base, p);
	}
	col = (int)(p - base);
    }
    result->col = col;
}

/*
 * Get current line in syntax buffer.
 */
    static char_u *
syn_getcurline(void)
{
    return ml_get_buf(syn_buf, current_lnum, FALSE);
}

/*
 * Get length of current line in syntax buffer.
 */
    static colnr_T
syn_getcurline_len(void)
{
    return ml_get_buf_len(syn_buf, current_lnum);
}

/*
 * Call vim_regexec() to find a match with "rmp" in "syn_buf".
 * Returns TRUE when there is a match.
 */
    static int
syn_regexec(
    regmmatch_T	*rmp,
    linenr_T	lnum,
    colnr_T	col,
    syn_time_T  *st UNUSED)
{
    int		r;
    int		timed_out = FALSE;
#ifdef FEAT_PROFILE
    proftime_T	pt;

    if (syn_time_on)
	profile_start(&pt);
#endif

    if (rmp->regprog == NULL)
	// This can happen if a previous call to vim_regexec_multi() tried to
	// use the NFA engine, which resulted in NFA_TOO_EXPENSIVE, and
	// compiling the pattern with the other engine fails.
	return FALSE;

    rmp->rmm_maxcol = syn_buf->b_p_smc;
    r = vim_regexec_multi(rmp, syn_win, syn_buf, lnum, col, &timed_out);

#ifdef FEAT_PROFILE
    if (syn_time_on)
    {
	profile_end(&pt);
	profile_add(&st->total, &pt);
	if (profile_cmp(&pt, &st->slowest) < 0)
	    st->slowest = pt;
	++st->count;
	if (r > 0)
	    ++st->match;
    }
#endif
#ifdef FEAT_RELTIME
    if (timed_out && redrawtime_limit_set && !syn_win->w_s->b_syn_slow)
    {
	syn_win->w_s->b_syn_slow = TRUE;
	msg(_("'redrawtime' exceeded, syntax highlighting disabled"));
    }
#endif

    if (r > 0)
    {
	rmp->startpos[0].lnum += lnum;
	rmp->endpos[0].lnum += lnum;
	return TRUE;
    }
    return FALSE;
}

/*
 * Check one position in a line for a matching keyword.
 * The caller must check if a keyword can start at startcol.
 * Return its ID if found, 0 otherwise.
 */
    static int
check_keyword_id(
    char_u	*line,
    int		startcol,	// position in line to check for keyword
    int		*endcolp,	// return: character after found keyword
    long	*flagsp,	// return: flags of matching keyword
    short	**next_listp,	// return: next_list of matching keyword
    stateitem_T	*cur_si,	// item at the top of the stack
    int		*ccharp UNUSED)	// conceal substitution char
{
    keyentry_T	*kp;
    char_u	*kwp;
    int		round;
    int		kwlen;
    char_u	keyword[MAXKEYWLEN + 1]; // assume max. keyword len is 80
    hashtab_T	*ht;
    hashitem_T	*hi;

    // Find first character after the keyword.  First character was already
    // checked.
    kwp = line + startcol;
    kwlen = 0;
    do
    {
	if (has_mbyte)
	    kwlen += (*mb_ptr2len)(kwp + kwlen);
	else
	    ++kwlen;
    }
    while (vim_iswordp_buf(kwp + kwlen, syn_buf));

    if (kwlen > MAXKEYWLEN)
	return 0;

    /*
     * Must make a copy of the keyword, so we can add a NUL and make it
     * lowercase.
     */
    vim_strncpy(keyword, kwp, kwlen);

    /*
     * Try twice:
     * 1. matching case
     * 2. ignoring case
     */
    for (round = 1; round <= 2; ++round)
    {
	ht = round == 1 ? &syn_block->b_keywtab : &syn_block->b_keywtab_ic;
	if (ht->ht_used == 0)
	    continue;
	if (round == 2)	// ignore case
	    (void)str_foldcase(kwp, kwlen, keyword, MAXKEYWLEN + 1);

	/*
	 * Find keywords that match.  There can be several with different
	 * attributes.
	 * When current_next_list is non-zero accept only that group, otherwise:
	 *  Accept a not-contained keyword at toplevel.
	 *  Accept a keyword at other levels only if it is in the contains list.
	 */
	hi = hash_find(ht, keyword);
	if (!HASHITEM_EMPTY(hi))
	    for (kp = HI2KE(hi); kp != NULL; kp = kp->ke_next)
	    {
		if (current_next_list != 0
			? in_id_list(NULL, current_next_list, &kp->k_syn, 0)
			: (cur_si == NULL
			    ? !(kp->flags & HL_CONTAINED)
			    : in_id_list(cur_si, cur_si->si_cont_list,
				      &kp->k_syn, kp->flags)))
		{
		    *endcolp = startcol + kwlen;
		    *flagsp = kp->flags;
		    *next_listp = kp->next_list;
#ifdef FEAT_CONCEAL
		    *ccharp = kp->k_char;
#endif
		    return kp->k_syn.id;
		}
	    }
    }
    return 0;
}

/*
 * Handle ":syntax conceal" command.
 */
    static void
syn_cmd_conceal(exarg_T *eap UNUSED, int syncing UNUSED)
{
#ifdef FEAT_CONCEAL
    char_u	*arg = eap->arg;
    char_u	*next;

    eap->nextcmd = find_nextcmd(arg);
    if (eap->skip)
	return;

    next = skiptowhite(arg);
    if (*arg == NUL)
    {
	if (curwin->w_s->b_syn_conceal)
	    msg("syntax conceal on");
	else
	    msg("syntax conceal off");
    }
    else if (STRNICMP(arg, "on", 2) == 0 && next - arg == 2)
	curwin->w_s->b_syn_conceal = TRUE;
    else if (STRNICMP(arg, "off", 3) == 0 && next - arg == 3)
	curwin->w_s->b_syn_conceal = FALSE;
    else
	semsg(_(e_illegal_argument_str_2), arg);
#endif
}

/*
 * Handle ":syntax case" command.
 */
    static void
syn_cmd_case(exarg_T *eap, int syncing UNUSED)
{
    char_u	*arg = eap->arg;
    char_u	*next;

    eap->nextcmd = find_nextcmd(arg);
    if (eap->skip)
	return;

    next = skiptowhite(arg);
    if (*arg == NUL)
    {
	if (curwin->w_s->b_syn_ic)
	    msg("syntax case ignore");
	else
	    msg("syntax case match");
    }
    else if (STRNICMP(arg, "match", 5) == 0 && next - arg == 5)
	curwin->w_s->b_syn_ic = FALSE;
    else if (STRNICMP(arg, "ignore", 6) == 0 && next - arg == 6)
	curwin->w_s->b_syn_ic = TRUE;
    else
	semsg(_(e_illegal_argument_str_2), arg);
}

/*
 * Handle ":syntax foldlevel" command.
 */
    static void
syn_cmd_foldlevel(exarg_T *eap, int syncing UNUSED)
{
    char_u *arg = eap->arg;
    char_u *arg_end;

    eap->nextcmd = find_nextcmd(arg);
    if (eap->skip)
	return;

    if (*arg == NUL)
    {
	switch (curwin->w_s->b_syn_foldlevel)
	{
	    case SYNFLD_START:   msg("syntax foldlevel start");   break;
	    case SYNFLD_MINIMUM: msg("syntax foldlevel minimum"); break;
	    default: break;
	}
	return;
    }

    arg_end = skiptowhite(arg);
    if (STRNICMP(arg, "start", 5) == 0 && arg_end - arg == 5)
	curwin->w_s->b_syn_foldlevel = SYNFLD_START;
    else if (STRNICMP(arg, "minimum", 7) == 0 && arg_end - arg == 7)
	curwin->w_s->b_syn_foldlevel = SYNFLD_MINIMUM;
    else
    {
	semsg(_(e_illegal_argument_str_2), arg);
	return;
    }

    arg = skipwhite(arg_end);
    if (*arg != NUL)
    {
	semsg(_(e_illegal_argument_str_2), arg);
    }
}

/*
 * Handle ":syntax spell" command.
 */
    static void
syn_cmd_spell(exarg_T *eap, int syncing UNUSED)
{
    char_u	*arg = eap->arg;
    char_u	*next;

    eap->nextcmd = find_nextcmd(arg);
    if (eap->skip)
	return;

    next = skiptowhite(arg);
    if (*arg == NUL)
    {
	if (curwin->w_s->b_syn_spell == SYNSPL_TOP)
	    msg("syntax spell toplevel");
	else if (curwin->w_s->b_syn_spell == SYNSPL_NOTOP)
	    msg("syntax spell notoplevel");
	else
	    msg("syntax spell default");
    }
    else if (STRNICMP(arg, "toplevel", 8) == 0 && next - arg == 8)
	curwin->w_s->b_syn_spell = SYNSPL_TOP;
    else if (STRNICMP(arg, "notoplevel", 10) == 0 && next - arg == 10)
	curwin->w_s->b_syn_spell = SYNSPL_NOTOP;
    else if (STRNICMP(arg, "default", 7) == 0 && next - arg == 7)
	curwin->w_s->b_syn_spell = SYNSPL_DEFAULT;
    else
    {
	semsg(_(e_illegal_argument_str_2), arg);
	return;
    }

    // assume spell checking changed, force a redraw
    redraw_win_later(curwin, UPD_NOT_VALID);
}

/*
 * Handle ":syntax iskeyword" command.
 */
    static void
syn_cmd_iskeyword(exarg_T *eap, int syncing UNUSED)
{
    char_u	*arg = eap->arg;
    char_u	save_chartab[32];
    char_u	*save_isk;

    if (eap->skip)
	return;

    arg = skipwhite(arg);
    if (*arg == NUL)
    {
	msg_puts("\n");
	if (curwin->w_s->b_syn_isk != empty_option)
	{
	    msg_puts("syntax iskeyword ");
	    msg_outtrans(curwin->w_s->b_syn_isk);
	}
	else
	    msg_outtrans((char_u *)_("syntax iskeyword not set"));
    }
    else
    {
	if (STRNICMP(arg, "clear", 5) == 0)
	{
	    mch_memmove(curwin->w_s->b_syn_chartab, curbuf->b_chartab,
								  (size_t)32);
	    clear_string_option(&curwin->w_s->b_syn_isk);
	}
	else
	{
	    mch_memmove(save_chartab, curbuf->b_chartab, (size_t)32);
	    save_isk = curbuf->b_p_isk;
	    curbuf->b_p_isk = vim_strsave(arg);

	    buf_init_chartab(curbuf, FALSE);
	    mch_memmove(curwin->w_s->b_syn_chartab, curbuf->b_chartab,
								  (size_t)32);
	    mch_memmove(curbuf->b_chartab, save_chartab, (size_t)32);
	    clear_string_option(&curwin->w_s->b_syn_isk);
	    curwin->w_s->b_syn_isk = curbuf->b_p_isk;
	    curbuf->b_p_isk = save_isk;
	}
    }
    redraw_win_later(curwin, UPD_NOT_VALID);
}

/*
 * Clear all syntax info for one buffer.
 */
    void
syntax_clear(synblock_T *block)
{
    int i;

    block->b_syn_error = FALSE;	    // clear previous error
#ifdef FEAT_RELTIME
    block->b_syn_slow = FALSE;	    // clear previous timeout
#endif
    block->b_syn_ic = FALSE;	    // Use case, by default
    block->b_syn_foldlevel = SYNFLD_START;
    block->b_syn_spell = SYNSPL_DEFAULT; // default spell checking
    block->b_syn_containedin = FALSE;
#ifdef FEAT_CONCEAL
    block->b_syn_conceal = FALSE;
#endif

    // free the keywords
    clear_keywtab(&block->b_keywtab);
    clear_keywtab(&block->b_keywtab_ic);

    // free the syntax patterns
    for (i = block->b_syn_patterns.ga_len; --i >= 0; )
	syn_clear_pattern(block, i);
    ga_clear(&block->b_syn_patterns);

    // free the syntax clusters
    for (i = block->b_syn_clusters.ga_len; --i >= 0; )
	syn_clear_cluster(block, i);
    ga_clear(&block->b_syn_clusters);
    block->b_spell_cluster_id = 0;
    block->b_nospell_cluster_id = 0;

    block->b_syn_sync_flags = 0;
    block->b_syn_sync_minlines = 0;
    block->b_syn_sync_maxlines = 0;
    block->b_syn_sync_linebreaks = 0;

    vim_regfree(block->b_syn_linecont_prog);
    block->b_syn_linecont_prog = NULL;
    VIM_CLEAR(block->b_syn_linecont_pat);
#ifdef FEAT_FOLDING
    block->b_syn_folditems = 0;
#endif
    clear_string_option(&block->b_syn_isk);

    // free the stored states
    syn_stack_free_all(block);
    invalidate_current_state();

    // Reset the counter for ":syn include"
    running_syn_inc_tag = 0;
}

/*
 * Get rid of ownsyntax for window "wp".
 */
    void
reset_synblock(win_T *wp)
{
    if (wp->w_s != &wp->w_buffer->b_s)
    {
	syntax_clear(wp->w_s);
	vim_free(wp->w_s);
	wp->w_s = &wp->w_buffer->b_s;
    }
}

/*
 * Clear syncing info for one buffer.
 */
    static void
syntax_sync_clear(void)
{
    int i;

    // free the syntax patterns
    for (i = curwin->w_s->b_syn_patterns.ga_len; --i >= 0; )
	if (SYN_ITEMS(curwin->w_s)[i].sp_syncing)
	    syn_remove_pattern(curwin->w_s, i);

    curwin->w_s->b_syn_sync_flags = 0;
    curwin->w_s->b_syn_sync_minlines = 0;
    curwin->w_s->b_syn_sync_maxlines = 0;
    curwin->w_s->b_syn_sync_linebreaks = 0;

    vim_regfree(curwin->w_s->b_syn_linecont_prog);
    curwin->w_s->b_syn_linecont_prog = NULL;
    VIM_CLEAR(curwin->w_s->b_syn_linecont_pat);
    clear_string_option(&curwin->w_s->b_syn_isk);

    syn_stack_free_all(curwin->w_s);	// Need to recompute all syntax.
}

/*
 * Remove one pattern from the buffer's pattern list.
 */
    static void
syn_remove_pattern(
    synblock_T	*block,
    int		idx)
{
    synpat_T	*spp;

    spp = &(SYN_ITEMS(block)[idx]);
#ifdef FEAT_FOLDING
    if (spp->sp_flags & HL_FOLD)
	--block->b_syn_folditems;
#endif
    syn_clear_pattern(block, idx);
    mch_memmove(spp, spp + 1,
		   sizeof(synpat_T) * (block->b_syn_patterns.ga_len - idx - 1));
    --block->b_syn_patterns.ga_len;
}

/*
 * Clear and free one syntax pattern.  When clearing all, must be called from
 * last to first!
 */
    static void
syn_clear_pattern(synblock_T *block, int i)
{
    vim_free(SYN_ITEMS(block)[i].sp_pattern);
    vim_regfree(SYN_ITEMS(block)[i].sp_prog);
    // Only free sp_cont_list and sp_next_list of first start pattern
    if (i == 0 || SYN_ITEMS(block)[i - 1].sp_type != SPTYPE_START)
    {
	vim_free(SYN_ITEMS(block)[i].sp_cont_list);
	vim_free(SYN_ITEMS(block)[i].sp_next_list);
	vim_free(SYN_ITEMS(block)[i].sp_syn.cont_in_list);
    }
}

/*
 * Clear and free one syntax cluster.
 */
    static void
syn_clear_cluster(synblock_T *block, int i)
{
    vim_free(SYN_CLSTR(block)[i].scl_name);
    vim_free(SYN_CLSTR(block)[i].scl_name_u);
    vim_free(SYN_CLSTR(block)[i].scl_list);
}

/*
 * Handle ":syntax clear" command.
 */
    static void
syn_cmd_clear(exarg_T *eap, int syncing)
{
    char_u	*arg = eap->arg;
    char_u	*arg_end;
    int		id;

    eap->nextcmd = find_nextcmd(arg);
    if (eap->skip)
	return;

    /*
     * We have to disable this within ":syn include @group filename",
     * because otherwise @group would get deleted.
     * Only required for Vim 5.x syntax files, 6.0 ones don't contain ":syn
     * clear".
     */
    if (curwin->w_s->b_syn_topgrp != 0)
	return;

    if (ends_excmd2(eap->cmd, arg))
    {
	/*
	 * No argument: Clear all syntax items.
	 */
	if (syncing)
	    syntax_sync_clear();
	else
	{
	    syntax_clear(curwin->w_s);
	    if (curwin->w_s == &curwin->w_buffer->b_s)
		do_unlet((char_u *)"b:current_syntax", TRUE);
	    do_unlet((char_u *)"w:current_syntax", TRUE);
	}
    }
    else
    {
	/*
	 * Clear the group IDs that are in the argument.
	 */
	while (!ends_excmd2(eap->cmd, arg))
	{
	    arg_end = skiptowhite(arg);
	    if (*arg == '@')
	    {
		id = syn_scl_namen2id(arg + 1, (int)(arg_end - arg - 1));
		if (id == 0)
		{
		    semsg(_(e_no_such_syntax_cluster_str_1), arg);
		    break;
		}
		else
		{
		    /*
		     * We can't physically delete a cluster without changing
		     * the IDs of other clusters, so we do the next best thing
		     * and make it empty.
		     */
		    short scl_id = id - SYNID_CLUSTER;

		    VIM_CLEAR(SYN_CLSTR(curwin->w_s)[scl_id].scl_list);
		}
	    }
	    else
	    {
		id = syn_namen2id(arg, (int)(arg_end - arg));
		if (id == 0)
		{
		    semsg(_(e_no_such_highlight_group_name_str), arg);
		    break;
		}
		else
		    syn_clear_one(id, syncing);
	    }
	    arg = skipwhite(arg_end);
	}
    }
    redraw_curbuf_later(UPD_SOME_VALID);
    syn_stack_free_all(curwin->w_s);		// Need to recompute all syntax.
}

/*
 * Clear one syntax group for the current buffer.
 */
    static void
syn_clear_one(int id, int syncing)
{
    synpat_T	*spp;
    int		idx;

    // Clear keywords only when not ":syn sync clear group-name"
    if (!syncing)
    {
	(void)syn_clear_keyword(id, &curwin->w_s->b_keywtab);
	(void)syn_clear_keyword(id, &curwin->w_s->b_keywtab_ic);
    }

    // clear the patterns for "id"
    for (idx = curwin->w_s->b_syn_patterns.ga_len; --idx >= 0; )
    {
	spp = &(SYN_ITEMS(curwin->w_s)[idx]);
	if (spp->sp_syn.id != id || spp->sp_syncing != syncing)
	    continue;
	syn_remove_pattern(curwin->w_s, idx);
    }
}

/*
 * Handle ":syntax on" command.
 */
    static void
syn_cmd_on(exarg_T *eap, int syncing UNUSED)
{
    syn_cmd_onoff(eap, "syntax");
}

/*
 * Handle ":syntax enable" command.
 */
    static void
syn_cmd_enable(exarg_T *eap, int syncing UNUSED)
{
    set_internal_string_var((char_u *)"g:syntax_cmd", (char_u *)"enable");
    syn_cmd_onoff(eap, "syntax");
    do_unlet((char_u *)"g:syntax_cmd", TRUE);
}

/*
 * Handle ":syntax reset" command.
 * It actually resets highlighting, not syntax.
 */
    static void
syn_cmd_reset(exarg_T *eap, int syncing UNUSED)
{
    set_nextcmd(eap, eap->arg);
    if (!eap->skip)
    {
	set_internal_string_var((char_u *)"g:syntax_cmd", (char_u *)"reset");
	do_cmdline_cmd((char_u *)"runtime! syntax/syncolor.vim");
	do_unlet((char_u *)"g:syntax_cmd", TRUE);
    }
}

/*
 * Handle ":syntax manual" command.
 */
    static void
syn_cmd_manual(exarg_T *eap, int syncing UNUSED)
{
    syn_cmd_onoff(eap, "manual");
}

/*
 * Handle ":syntax off" command.
 */
    static void
syn_cmd_off(exarg_T *eap, int syncing UNUSED)
{
    syn_cmd_onoff(eap, "nosyntax");
}

    static void
syn_cmd_onoff(exarg_T *eap, char *name)
{
    char_u	buf[100];

    set_nextcmd(eap, eap->arg);
    if (!eap->skip)
    {
	STRCPY(buf, "so ");
	vim_snprintf((char *)buf + 3, sizeof(buf) - 3, SYNTAX_FNAME, name);
	do_cmdline_cmd(buf);
    }
}

/*
 * Handle ":syntax [list]" command: list current syntax words.
 */
    static void
syn_cmd_list(
    exarg_T	*eap,
    int		syncing)	    // when TRUE: list syncing items
{
    char_u	*arg = eap->arg;
    int		id;
    char_u	*arg_end;

    eap->nextcmd = find_nextcmd(arg);
    if (eap->skip)
	return;

    if (!syntax_present(curwin))
    {
	msg(_(msg_no_items));
	return;
    }

    if (syncing)
    {
	if (curwin->w_s->b_syn_sync_flags & SF_CCOMMENT)
	{
	    msg_puts(_("syncing on C-style comments"));
	    syn_lines_msg();
	    syn_match_msg();
	    return;
	}
	else if (!(curwin->w_s->b_syn_sync_flags & SF_MATCH))
	{
	    if (curwin->w_s->b_syn_sync_minlines == 0)
		msg_puts(_("no syncing"));
	    else
	    {
		if (curwin->w_s->b_syn_sync_minlines == MAXLNUM)
		    msg_puts(_("syncing starts at the first line"));
		else
		{
		    msg_puts(_("syncing starts "));
		    msg_outnum(curwin->w_s->b_syn_sync_minlines);
		    msg_puts(_(" lines before top line"));
		}
		syn_match_msg();
	    }
	    return;
	}
	msg_puts_title(_("\n--- Syntax sync items ---"));
	if (curwin->w_s->b_syn_sync_minlines > 0
		|| curwin->w_s->b_syn_sync_maxlines > 0
		|| curwin->w_s->b_syn_sync_linebreaks > 0)
	{
	    msg_puts(_("\nsyncing on items"));
	    syn_lines_msg();
	    syn_match_msg();
	}
    }
    else
	msg_puts_title(_("\n--- Syntax items ---"));
    if (ends_excmd2(eap->cmd, arg))
    {
	/*
	 * No argument: List all group IDs and all syntax clusters.
	 */
	for (id = 1; id <= highlight_num_groups() && !got_int; ++id)
	    syn_list_one(id, syncing, FALSE);
	for (id = 0; id < curwin->w_s->b_syn_clusters.ga_len && !got_int; ++id)
	    syn_list_cluster(id);
    }
    else
    {
	/*
	 * List the group IDs and syntax clusters that are in the argument.
	 */
	while (!ends_excmd2(eap->cmd, arg) && !got_int)
	{
	    arg_end = skiptowhite(arg);
	    if (*arg == '@')
	    {
		id = syn_scl_namen2id(arg + 1, (int)(arg_end - arg - 1));
		if (id == 0)
		    semsg(_(e_no_such_syntax_cluster_str_2), arg);
		else
		    syn_list_cluster(id - SYNID_CLUSTER);
	    }
	    else
	    {
		id = syn_namen2id(arg, (int)(arg_end - arg));
		if (id == 0)
		    semsg(_(e_no_such_highlight_group_name_str), arg);
		else
		    syn_list_one(id, syncing, TRUE);
	    }
	    arg = skipwhite(arg_end);
	}
    }
    set_nextcmd(eap, arg);
}

    static void
syn_lines_msg(void)
{
    if (curwin->w_s->b_syn_sync_maxlines > 0
				      || curwin->w_s->b_syn_sync_minlines > 0)
    {
	msg_puts("; ");
	if (curwin->w_s->b_syn_sync_minlines == MAXLNUM)
	    msg_puts(_("from the first line"));
	else
	{
	    if (curwin->w_s->b_syn_sync_minlines > 0)
	    {
		msg_puts(_("minimal "));
		msg_outnum(curwin->w_s->b_syn_sync_minlines);
		if (curwin->w_s->b_syn_sync_maxlines)
		    msg_puts(", ");
	    }
	    if (curwin->w_s->b_syn_sync_maxlines > 0)
	    {
		msg_puts(_("maximal "));
		msg_outnum(curwin->w_s->b_syn_sync_maxlines);
	    }
	    msg_puts(_(" lines before top line"));
	}
    }
}

    static void
syn_match_msg(void)
{
    if (curwin->w_s->b_syn_sync_linebreaks > 0)
    {
	msg_puts(_("; match "));
	msg_outnum(curwin->w_s->b_syn_sync_linebreaks);
	msg_puts(_(" line breaks"));
    }
}

static int  last_matchgroup;

static void syn_list_flags(keyvalue_T *nlist, int nr_entries, int flags, int attr);

/*
 * List one syntax item, for ":syntax" or "syntax list syntax_name".
 */
    static void
syn_list_one(
    int		id,
    int		syncing,	    // when TRUE: list syncing items
    int		link_only)	    // when TRUE; list link-only too
{
    int		attr;
    int		idx;
    int		did_header = FALSE;
    synpat_T	*spp;
    static keyvalue_T namelist1[] =
		{
		    KEYVALUE_ENTRY(HL_DISPLAY, "display"),
		    KEYVALUE_ENTRY(HL_CONTAINED, "contained"),
		    KEYVALUE_ENTRY(HL_ONELINE, "oneline"),
		    KEYVALUE_ENTRY(HL_KEEPEND, "keepend"),
		    KEYVALUE_ENTRY(HL_EXTEND, "extend"),
		    KEYVALUE_ENTRY(HL_EXCLUDENL, "excludenl"),
		    KEYVALUE_ENTRY(HL_TRANSP, "transparent"),
		    KEYVALUE_ENTRY(HL_FOLD, "fold")
#ifdef FEAT_CONCEAL
		    ,
		    KEYVALUE_ENTRY(HL_CONCEAL, "conceal"),
		    KEYVALUE_ENTRY(HL_CONCEALENDS, "concealends")
#endif
		};
    static keyvalue_T namelist2[] =
		{
		    KEYVALUE_ENTRY(HL_SKIPWHITE, "skipwhite"),
		    KEYVALUE_ENTRY(HL_SKIPNL, "skipnl"),
		    KEYVALUE_ENTRY(HL_SKIPEMPTY, "skipempty")
		};

    attr = HL_ATTR(HLF_D);		// highlight like directories

    // list the keywords for "id"
    if (!syncing)
    {
	did_header = syn_list_keywords(id, &curwin->w_s->b_keywtab, FALSE, attr);
	did_header = syn_list_keywords(id, &curwin->w_s->b_keywtab_ic,
							    did_header, attr);
    }

    // list the patterns for "id"
    for (idx = 0; idx < curwin->w_s->b_syn_patterns.ga_len && !got_int; ++idx)
    {
	spp = &(SYN_ITEMS(curwin->w_s)[idx]);
	if (spp->sp_syn.id != id || spp->sp_syncing != syncing)
	    continue;

	(void)syn_list_header(did_header, 999, id);
	did_header = TRUE;
	last_matchgroup = 0;
	if (spp->sp_type == SPTYPE_MATCH)
	{
	    put_pattern("match", ' ', spp, attr);
	    msg_putchar(' ');
	}
	else if (spp->sp_type == SPTYPE_START)
	{
	    while (SYN_ITEMS(curwin->w_s)[idx].sp_type == SPTYPE_START)
		put_pattern("start", '=', &SYN_ITEMS(curwin->w_s)[idx++], attr);
	    if (SYN_ITEMS(curwin->w_s)[idx].sp_type == SPTYPE_SKIP)
		put_pattern("skip", '=', &SYN_ITEMS(curwin->w_s)[idx++], attr);
	    while (idx < curwin->w_s->b_syn_patterns.ga_len
			  && SYN_ITEMS(curwin->w_s)[idx].sp_type == SPTYPE_END)
		put_pattern("end", '=', &SYN_ITEMS(curwin->w_s)[idx++], attr);
	    --idx;
	    msg_putchar(' ');
	}
	syn_list_flags(namelist1, (int)ARRAY_LENGTH(namelist1), spp->sp_flags, attr);

	if (spp->sp_cont_list != NULL)
	    put_id_list((char_u *)"contains", spp->sp_cont_list, attr);

	if (spp->sp_syn.cont_in_list != NULL)
	    put_id_list((char_u *)"containedin",
					      spp->sp_syn.cont_in_list, attr);

	if (spp->sp_next_list != NULL)
	{
	    put_id_list((char_u *)"nextgroup", spp->sp_next_list, attr);
	    syn_list_flags(namelist2, (int)ARRAY_LENGTH(namelist2), spp->sp_flags, attr);
	}
	if (spp->sp_flags & (HL_SYNC_HERE|HL_SYNC_THERE))
	{
	    if (spp->sp_flags & HL_SYNC_HERE)
		msg_puts_attr("grouphere", attr);
	    else
		msg_puts_attr("groupthere", attr);
	    msg_putchar(' ');
	    if (spp->sp_sync_idx >= 0)
		msg_outtrans(highlight_group_name(SYN_ITEMS(curwin->w_s)
				   [spp->sp_sync_idx].sp_syn.id - 1));
	    else
		msg_puts("NONE");
	    msg_putchar(' ');
	}
    }

    // list the link, if there is one
    if (highlight_link_id(id - 1) && (did_header || link_only) && !got_int)
    {
	(void)syn_list_header(did_header, 999, id);
	msg_puts_attr("links to", attr);
	msg_putchar(' ');
	msg_outtrans(highlight_group_name(highlight_link_id(id - 1) - 1));
    }
}

    static void
syn_list_flags(keyvalue_T *nlist, int nr_entries, int flags, int attr)
{
    int		i;

    for (i = 0; i < nr_entries; ++i)
	if (flags & nlist[i].key)
	{
	    msg_puts_attr((char *)nlist[i].value.string, attr);
	    msg_putchar(' ');
	}
}

/*
 * List one syntax cluster, for ":syntax" or "syntax list syntax_name".
 */
    static void
syn_list_cluster(int id)
{
    int	    endcol = 15;

    // slight hack:  roughly duplicate the guts of syn_list_header()
    msg_putchar('\n');
    msg_outtrans(SYN_CLSTR(curwin->w_s)[id].scl_name);

    if (msg_col >= endcol)	// output at least one space
	endcol = msg_col + 1;
    if (Columns <= (long)endcol)	// avoid hang for tiny window
	endcol = (int)(Columns - 1);

    msg_advance(endcol);
    if (SYN_CLSTR(curwin->w_s)[id].scl_list != NULL)
    {
	put_id_list((char_u *)"cluster", SYN_CLSTR(curwin->w_s)[id].scl_list,
		    HL_ATTR(HLF_D));
    }
    else
    {
	msg_puts_attr("cluster", HL_ATTR(HLF_D));
	msg_puts("=NONE");
    }
}

    static void
put_id_list(char_u *name, short *list, int attr)
{
    short		*p;

    msg_puts_attr((char *)name, attr);
    msg_putchar('=');
    for (p = list; *p; ++p)
    {
	if (*p >= SYNID_ALLBUT && *p < SYNID_TOP)
	{
	    if (p[1])
		msg_puts("ALLBUT");
	    else
		msg_puts("ALL");
	}
	else if (*p >= SYNID_TOP && *p < SYNID_CONTAINED)
	{
	    msg_puts("TOP");
	}
	else if (*p >= SYNID_CONTAINED && *p < SYNID_CLUSTER)
	{
	    msg_puts("CONTAINED");
	}
	else if (*p >= SYNID_CLUSTER)
	{
	    short scl_id = *p - SYNID_CLUSTER;

	    msg_putchar('@');
	    msg_outtrans(SYN_CLSTR(curwin->w_s)[scl_id].scl_name);
	}
	else
	    msg_outtrans(highlight_group_name(*p - 1));
	if (p[1])
	    msg_putchar(',');
    }
    msg_putchar(' ');
}

    static void
put_pattern(
    char	*s,
    int		c,
    synpat_T	*spp,
    int		attr)
{
    long	n;
    int		mask;
    int		first;
    static char	*sepchars = "/+=-#@\"|'^&";
    int		i;

    // May have to write "matchgroup=group"
    if (last_matchgroup != spp->sp_syn_match_id)
    {
	last_matchgroup = spp->sp_syn_match_id;
	msg_puts_attr("matchgroup", attr);
	msg_putchar('=');
	if (last_matchgroup == 0)
	    msg_outtrans((char_u *)"NONE");
	else
	    msg_outtrans(highlight_group_name(last_matchgroup - 1));
	msg_putchar(' ');
    }

    // output the name of the pattern and an '=' or ' '
    msg_puts_attr(s, attr);
    msg_putchar(c);

    // output the pattern, in between a char that is not in the pattern
    for (i = 0; vim_strchr(spp->sp_pattern, sepchars[i]) != NULL; )
	if (sepchars[++i] == NUL)
	{
	    i = 0;	// no good char found, just use the first one
	    break;
	}
    msg_putchar(sepchars[i]);
    msg_outtrans(spp->sp_pattern);
    msg_putchar(sepchars[i]);

    // output any pattern options
    first = TRUE;
    for (i = 0; i < SPO_COUNT; ++i)
    {
	mask = (1 << i);
	if (spp->sp_off_flags & (mask + (mask << SPO_COUNT)))
	{
	    if (!first)
		msg_putchar(',');	// separate with commas
	    msg_puts(spo_name_tab[i]);
	    n = spp->sp_offsets[i];
	    if (i != SPO_LC_OFF)
	    {
		if (spp->sp_off_flags & mask)
		    msg_putchar('s');
		else
		    msg_putchar('e');
		if (n > 0)
		    msg_putchar('+');
	    }
	    if (n || i == SPO_LC_OFF)
		msg_outnum(n);
	    first = FALSE;
	}
    }
    msg_putchar(' ');
}

/*
 * List or clear the keywords for one syntax group.
 * Return TRUE if the header has been printed.
 */
    static int
syn_list_keywords(
    int		id,
    hashtab_T	*ht,
    int		did_header,		// header has already been printed
    int		attr)
{
    int		outlen;
    hashitem_T	*hi;
    keyentry_T	*kp;
    int		todo;
    int		prev_contained = 0;
    short	*prev_next_list = NULL;
    short	*prev_cont_in_list = NULL;
    int		prev_skipnl = 0;
    int		prev_skipwhite = 0;
    int		prev_skipempty = 0;

    /*
     * Unfortunately, this list of keywords is not sorted on alphabet but on
     * hash value...
     */
    todo = (int)ht->ht_used;
    for (hi = ht->ht_array; todo > 0 && !got_int; ++hi)
    {
	if (!HASHITEM_EMPTY(hi))
	{
	    --todo;
	    for (kp = HI2KE(hi); kp != NULL && !got_int; kp = kp->ke_next)
	    {
		if (kp->k_syn.id == id)
		{
		    if (prev_contained != (kp->flags & HL_CONTAINED)
			    || prev_skipnl != (kp->flags & HL_SKIPNL)
			    || prev_skipwhite != (kp->flags & HL_SKIPWHITE)
			    || prev_skipempty != (kp->flags & HL_SKIPEMPTY)
			    || prev_cont_in_list != kp->k_syn.cont_in_list
			    || prev_next_list != kp->next_list)
			outlen = 9999;
		    else
			outlen = (int)STRLEN(kp->keyword);
		    // output "contained" and "nextgroup" on each line
		    if (syn_list_header(did_header, outlen, id))
		    {
			prev_contained = 0;
			prev_next_list = NULL;
			prev_cont_in_list = NULL;
			prev_skipnl = 0;
			prev_skipwhite = 0;
			prev_skipempty = 0;
		    }
		    did_header = TRUE;
		    if (prev_contained != (kp->flags & HL_CONTAINED))
		    {
			msg_puts_attr("contained", attr);
			msg_putchar(' ');
			prev_contained = (kp->flags & HL_CONTAINED);
		    }
		    if (kp->k_syn.cont_in_list != prev_cont_in_list)
		    {
			put_id_list((char_u *)"containedin",
						kp->k_syn.cont_in_list, attr);
			msg_putchar(' ');
			prev_cont_in_list = kp->k_syn.cont_in_list;
		    }
		    if (kp->next_list != prev_next_list)
		    {
			put_id_list((char_u *)"nextgroup", kp->next_list, attr);
			msg_putchar(' ');
			prev_next_list = kp->next_list;
			if (kp->flags & HL_SKIPNL)
			{
			    msg_puts_attr("skipnl", attr);
			    msg_putchar(' ');
			    prev_skipnl = (kp->flags & HL_SKIPNL);
			}
			if (kp->flags & HL_SKIPWHITE)
			{
			    msg_puts_attr("skipwhite", attr);
			    msg_putchar(' ');
			    prev_skipwhite = (kp->flags & HL_SKIPWHITE);
			}
			if (kp->flags & HL_SKIPEMPTY)
			{
			    msg_puts_attr("skipempty", attr);
			    msg_putchar(' ');
			    prev_skipempty = (kp->flags & HL_SKIPEMPTY);
			}
		    }
		    msg_outtrans(kp->keyword);
		}
	    }
	}
    }

    return did_header;
}

    static void
syn_clear_keyword(int id, hashtab_T *ht)
{
    hashitem_T	*hi;
    keyentry_T	*kp;
    keyentry_T	*kp_prev;
    keyentry_T	*kp_next;
    int		todo;

    hash_lock(ht);
    todo = (int)ht->ht_used;
    FOR_ALL_HASHTAB_ITEMS(ht, hi, todo)
    {
	if (!HASHITEM_EMPTY(hi))
	{
	    --todo;
	    kp_prev = NULL;
	    for (kp = HI2KE(hi); kp != NULL; )
	    {
		if (kp->k_syn.id == id)
		{
		    kp_next = kp->ke_next;
		    if (kp_prev == NULL)
		    {
			if (kp_next == NULL)
			    hash_remove(ht, hi, "syntax clear keyword");
			else
			    hi->hi_key = KE2HIKEY(kp_next);
		    }
		    else
			kp_prev->ke_next = kp_next;
		    vim_free(kp->next_list);
		    vim_free(kp->k_syn.cont_in_list);
		    vim_free(kp);
		    kp = kp_next;
		}
		else
		{
		    kp_prev = kp;
		    kp = kp->ke_next;
		}
	    }
	}
    }
    hash_unlock(ht);
}

/*
 * Clear a whole keyword table.
 */
    static void
clear_keywtab(hashtab_T *ht)
{
    hashitem_T	*hi;
    int		todo;
    keyentry_T	*kp;
    keyentry_T	*kp_next;

    todo = (int)ht->ht_used;
    FOR_ALL_HASHTAB_ITEMS(ht, hi, todo)
    {
	if (!HASHITEM_EMPTY(hi))
	{
	    --todo;
	    for (kp = HI2KE(hi); kp != NULL; kp = kp_next)
	    {
		kp_next = kp->ke_next;
		vim_free(kp->next_list);
		vim_free(kp->k_syn.cont_in_list);
		vim_free(kp);
	    }
	}
    }
    hash_clear(ht);
    hash_init(ht);
}

/*
 * Add a keyword to the list of keywords.
 */
    static void
add_keyword(
    char_u	*name,	    // name of keyword
    size_t	namelen,    // length of keyword (excluding the NUL)
    int		id,	    // group ID for this keyword
    int		flags,	    // flags for this keyword
    short	*cont_in_list, // containedin for this keyword
    short	*next_list, // nextgroup for this keyword
    int		conceal_char)
{
    keyentry_T	*kp;
    hashtab_T	*ht;
    hashitem_T	*hi;
    char_u	*name_ic;
    size_t	name_iclen;
    long_u	hash;
    char_u	name_folded[MAXKEYWLEN + 1];

    if (curwin->w_s->b_syn_ic)
    {
	name_ic = str_foldcase(name, (int)namelen,
						 name_folded, MAXKEYWLEN + 1);
	name_iclen = STRLEN(name_ic);
    }
    else
    {
	name_ic = name;
	name_iclen = namelen;
    }
    kp = alloc(offsetof(keyentry_T, keyword) + name_iclen + 1);
    if (kp == NULL)
	return;
    STRCPY(kp->keyword, name_ic);
    kp->k_syn.id = id;
    kp->k_syn.inc_tag = current_syn_inc_tag;
    kp->flags = flags;
    kp->k_char = conceal_char;
    kp->k_syn.cont_in_list = copy_id_list(cont_in_list);
    if (cont_in_list != NULL)
	curwin->w_s->b_syn_containedin = TRUE;
    kp->next_list = copy_id_list(next_list);

    if (curwin->w_s->b_syn_ic)
	ht = &curwin->w_s->b_keywtab_ic;
    else
	ht = &curwin->w_s->b_keywtab;

    hash = hash_hash(kp->keyword);
    hi = hash_lookup(ht, kp->keyword, hash);
    if (HASHITEM_EMPTY(hi))
    {
	// new keyword, add to hashtable
	kp->ke_next = NULL;
	hash_add_item(ht, hi, kp->keyword, hash);
    }
    else
    {
	// keyword already exists, prepend to list
	kp->ke_next = HI2KE(hi);
	hi->hi_key = KE2HIKEY(kp);
    }
}

/*
 * Get the start and end of the group name argument.
 * Return a pointer to the first argument.
 * Return NULL if the end of the command was found instead of further args.
 */
    static char_u *
get_group_name(
    char_u	*arg,		// start of the argument
    char_u	**name_end)	// pointer to end of the name
{
    char_u	*rest;

    *name_end = skiptowhite(arg);
    rest = skipwhite(*name_end);

    /*
     * Check if there are enough arguments.  The first argument may be a
     * pattern, where '|' is allowed, so only check for NUL.
     */
    if (ends_excmd(*arg) || *rest == NUL)
	return NULL;
    return rest;
}

/*
 * Check for syntax command option arguments.
 * This can be called at any place in the list of arguments, and just picks
 * out the arguments that are known.  Can be called several times in a row to
 * collect all options in between other arguments.
 * Return a pointer to the next argument (which isn't an option).
 * Return NULL for any error;
 */
    static char_u *
get_syn_options(
    char_u	    *start,		// next argument to be checked
    syn_opt_arg_T   *opt,		// various things
    int		    *conceal_char UNUSED,
    int		    skip)		// TRUE if skipping over command
{
    char_u	*arg = start;
    char_u	*gname_start, *gname;
    int		syn_id;
    int		len;
    char	*p;
    int		i;
    int		fidx;
    static struct flag
    {
	char	*name;
	int	argtype;
	int	flags;
    } flagtab[] = { {"cCoOnNtTaAiInNeEdD",	0,	HL_CONTAINED},
		    {"oOnNeElLiInNeE",		0,	HL_ONELINE},
		    {"kKeEeEpPeEnNdD",		0,	HL_KEEPEND},
		    {"eExXtTeEnNdD",		0,	HL_EXTEND},
		    {"eExXcClLuUdDeEnNlL",	0,	HL_EXCLUDENL},
		    {"tTrRaAnNsSpPaArReEnNtT",	0,	HL_TRANSP},
		    {"sSkKiIpPnNlL",		0,	HL_SKIPNL},
		    {"sSkKiIpPwWhHiItTeE",	0,	HL_SKIPWHITE},
		    {"sSkKiIpPeEmMpPtTyY",	0,	HL_SKIPEMPTY},
		    {"gGrRoOuUpPhHeErReE",	0,	HL_SYNC_HERE},
		    {"gGrRoOuUpPtThHeErReE",	0,	HL_SYNC_THERE},
		    {"dDiIsSpPlLaAyY",		0,	HL_DISPLAY},
		    {"fFoOlLdD",		0,	HL_FOLD},
		    {"cCoOnNcCeEaAlL",		0,	HL_CONCEAL},
		    {"cCoOnNcCeEaAlLeEnNdDsS",	0,	HL_CONCEALENDS},
		    {"cCcChHaArR",		11,	0},
		    {"cCoOnNtTaAiInNsS",	1,	0},
		    {"cCoOnNtTaAiInNeEdDiInN",	2,	0},
		    {"nNeExXtTgGrRoOuUpP",	3,	0},
		};
    static char *first_letters = "cCoOkKeEtTsSgGdDfFnN";

    if (arg == NULL)		// already detected error
	return NULL;

#ifdef FEAT_CONCEAL
    if (curwin->w_s->b_syn_conceal)
	opt->flags |= HL_CONCEAL;
#endif

    for (;;)
    {
	/*
	 * This is used very often when a large number of keywords is defined.
	 * Need to skip quickly when no option name is found.
	 * Also avoid tolower(), it's slow.
	 */
	if (strchr(first_letters, *arg) == NULL)
	    break;

	for (fidx = ARRAY_LENGTH(flagtab); --fidx >= 0; )
	{
	    p = flagtab[fidx].name;
	    for (i = 0, len = 0; p[i] != NUL; i += 2, ++len)
		if (arg[len] != p[i] && arg[len] != p[i + 1])
		    break;
	    if (p[i] == NUL && (VIM_ISWHITE(arg[len])
				    || (flagtab[fidx].argtype > 0
					 ? arg[len] == '='
					 : ends_excmd2(start, arg + len))))
	    {
		if (opt->keyword
			&& (flagtab[fidx].flags == HL_DISPLAY
			    || flagtab[fidx].flags == HL_FOLD
			    || flagtab[fidx].flags == HL_EXTEND))
		    // treat "display", "fold" and "extend" as a keyword
		    fidx = -1;
		break;
	    }
	}
	if (fidx < 0)	    // no match found
	    break;

	if (flagtab[fidx].argtype == 1)
	{
	    if (!opt->has_cont_list)
	    {
		emsg(_(e_contains_argument_not_accepted_here));
		return NULL;
	    }
	    if (get_id_list(&arg, 8, &opt->cont_list, skip) == FAIL)
		return NULL;
	}
	else if (flagtab[fidx].argtype == 2)
	{
	    if (get_id_list(&arg, 11, &opt->cont_in_list, skip) == FAIL)
		return NULL;
	}
	else if (flagtab[fidx].argtype == 3)
	{
	    if (get_id_list(&arg, 9, &opt->next_list, skip) == FAIL)
		return NULL;
	}
	else if (flagtab[fidx].argtype == 11 && arg[5] == '=')
	{
	    // cchar=?
	    if (has_mbyte)
	    {
#ifdef FEAT_CONCEAL
		*conceal_char = mb_ptr2char(arg + 6);
#endif
		arg += mb_ptr2len(arg + 6) - 1;
	    }
	    else
	    {
#ifdef FEAT_CONCEAL
		*conceal_char = arg[6];
#else
		;
#endif
	    }
#ifdef FEAT_CONCEAL
	    if (!vim_isprintc_strict(*conceal_char))
	    {
		emsg(_(e_invalid_cchar_value));
		return NULL;
	    }
#endif
	    arg = skipwhite(arg + 7);
	}
	else
	{
	    opt->flags |= flagtab[fidx].flags;
	    arg = skipwhite(arg + len);

	    if (flagtab[fidx].flags == HL_SYNC_HERE
		    || flagtab[fidx].flags == HL_SYNC_THERE)
	    {
		if (opt->sync_idx == NULL)
		{
		    emsg(_(e_groupthere_not_accepted_here));
		    return NULL;
		}
		gname_start = arg;
		arg = skiptowhite(arg);
		if (gname_start == arg)
		    return NULL;
		gname = vim_strnsave(gname_start, arg - gname_start);
		if (gname == NULL)
		    return NULL;
		if (STRCMP(gname, "NONE") == 0)
		    *opt->sync_idx = NONE_IDX;
		else
		{
		    syn_id = syn_name2id(gname);
		    for (i = curwin->w_s->b_syn_patterns.ga_len; --i >= 0; )
			if (SYN_ITEMS(curwin->w_s)[i].sp_syn.id == syn_id
			      && SYN_ITEMS(curwin->w_s)[i].sp_type
							       == SPTYPE_START)
			{
			    *opt->sync_idx = i;
			    break;
			}
		    if (i < 0)
		    {
			semsg(_(e_didnt_find_region_item_for_str), gname);
			vim_free(gname);
			return NULL;
		    }
		}

		vim_free(gname);
		arg = skipwhite(arg);
	    }
#ifdef FEAT_FOLDING
	    else if (flagtab[fidx].flags == HL_FOLD
						&& foldmethodIsSyntax(curwin))
		// Need to update folds later.
		foldUpdateAll(curwin);
#endif
	}
    }

    return arg;
}

/*
 * Adjustments to syntax item when declared in a ":syn include"'d file.
 * Set the contained flag, and if the item is not already contained, add it
 * to the specified top-level group, if any.
 */
    static void
syn_incl_toplevel(int id, int *flagsp)
{
    if ((*flagsp & HL_CONTAINED) || curwin->w_s->b_syn_topgrp == 0)
	return;
    *flagsp |= HL_CONTAINED | HL_INCLUDED_TOPLEVEL;
    if (curwin->w_s->b_syn_topgrp >= SYNID_CLUSTER)
    {
	// We have to alloc this, because syn_combine_list() will free it.
	short	    *grp_list = ALLOC_MULT(short, 2);
	int	    tlg_id = curwin->w_s->b_syn_topgrp - SYNID_CLUSTER;

	if (grp_list != NULL)
	{
	    grp_list[0] = id;
	    grp_list[1] = 0;
	    syn_combine_list(&SYN_CLSTR(curwin->w_s)[tlg_id].scl_list,
						       &grp_list, CLUSTER_ADD);
	}
    }
}

/*
 * Handle ":syntax include [@{group-name}] filename" command.
 */
    static void
syn_cmd_include(exarg_T *eap, int syncing UNUSED)
{
    char_u	*arg = eap->arg;
    int		sgl_id = 1;
    char_u	*group_name_end;
    char_u	*rest;
    char	*errormsg = NULL;
    int		prev_toplvl_grp;
    int		prev_syn_inc_tag;
    int		source = FALSE;

    eap->nextcmd = find_nextcmd(arg);
    if (eap->skip)
	return;

    if (arg[0] == '@')
    {
	++arg;
	rest = get_group_name(arg, &group_name_end);
	if (rest == NULL)
	{
	    emsg(_(e_filename_required));
	    return;
	}
	sgl_id = syn_check_cluster(arg, (int)(group_name_end - arg));
	if (sgl_id == 0)
	    return;
	// separate_nextcmd() and expand_filename() depend on this
	eap->arg = rest;
    }

    /*
     * Everything that's left, up to the next command, should be the
     * filename to include.
     */
    eap->argt |= (EX_XFILE | EX_NOSPC);
    separate_nextcmd(eap, FALSE);
    if (*eap->arg == '<' || *eap->arg == '$' || mch_isFullName(eap->arg))
    {
	// For an absolute path, "$VIM/..." or "<sfile>.." we ":source" the
	// file.  Need to expand the file name first.  In other cases
	// ":runtime!" is used.
	source = TRUE;
	if (expand_filename(eap, syn_cmdlinep, &errormsg) == FAIL)
	{
	    if (errormsg != NULL)
		emsg(errormsg);
	    return;
	}
    }

    /*
     * Save and restore the existing top-level grouplist id and ":syn
     * include" tag around the actual inclusion.
     */
    if (running_syn_inc_tag >= MAX_SYN_INC_TAG)
    {
	emsg(_(e_too_many_syntax_includes));
	return;
    }
    prev_syn_inc_tag = current_syn_inc_tag;
    current_syn_inc_tag = ++running_syn_inc_tag;
    prev_toplvl_grp = curwin->w_s->b_syn_topgrp;
    curwin->w_s->b_syn_topgrp = sgl_id;
    if (source ? do_source(eap->arg, FALSE, DOSO_NONE, NULL) == FAIL
				: source_runtime(eap->arg, DIP_ALL) == FAIL)
	semsg(_(e_cant_open_file_str), eap->arg);
    curwin->w_s->b_syn_topgrp = prev_toplvl_grp;
    current_syn_inc_tag = prev_syn_inc_tag;
}

/*
 * Handle ":syntax keyword {group-name} [{option}] keyword .." command.
 */
    static void
syn_cmd_keyword(exarg_T *eap, int syncing UNUSED)
{
    char_u	*arg = eap->arg;
    char_u	*group_name_end;
    int		syn_id;
    char_u	*rest;
    char_u	*keyword_copy = NULL;
    char_u	*p;
    char_u	*kw;
    syn_opt_arg_T syn_opt_arg;
    int		cnt;
    int		conceal_char = NUL;

    rest = get_group_name(arg, &group_name_end);

    if (rest != NULL)
    {
	if (eap->skip)
	    syn_id = -1;
	else
	    syn_id = syn_check_group(arg, (int)(group_name_end - arg));
	if (syn_id != 0)
	    // allocate a buffer, for removing backslashes in the keyword
	    keyword_copy = alloc(STRLEN(rest) + 1);
	if (keyword_copy != NULL)
	{
	    syn_opt_arg.flags = 0;
	    syn_opt_arg.keyword = TRUE;
	    syn_opt_arg.sync_idx = NULL;
	    syn_opt_arg.has_cont_list = FALSE;
	    syn_opt_arg.cont_in_list = NULL;
	    syn_opt_arg.next_list = NULL;

	    /*
	     * The options given apply to ALL keywords, so all options must be
	     * found before keywords can be created.
	     * 1: collect the options and copy the keywords to keyword_copy.
	     */
	    cnt = 0;
	    p = keyword_copy;
	    for ( ; rest != NULL && !ends_excmd2(eap->arg, rest);
							rest = skipwhite(rest))
	    {
		rest = get_syn_options(rest, &syn_opt_arg, &conceal_char,
								    eap->skip);
		if (rest == NULL || ends_excmd2(eap->arg, rest))
		    break;
		// Copy the keyword, removing backslashes, and add a NUL.
		while (*rest != NUL && !VIM_ISWHITE(*rest))
		{
		    if (*rest == '\\' && rest[1] != NUL)
			++rest;
		    *p++ = *rest++;
		}
		*p++ = NUL;
		++cnt;
	    }

	    if (!eap->skip)
	    {
		size_t	kwlen = 0;

		// Adjust flags for use of ":syn include".
		syn_incl_toplevel(syn_id, &syn_opt_arg.flags);

		/*
		 * 2: Add an entry for each keyword.
		 */
		for (kw = keyword_copy; --cnt >= 0; kw += kwlen + 1)
		{
		    for (p = vim_strchr(kw, '['); ; )
		    {
			if (p == NULL)
			    kwlen = STRLEN(kw);
			else
			{
			    *p = NUL;
			    kwlen = (size_t)(p - kw);
			}
			add_keyword(kw, kwlen, syn_id, syn_opt_arg.flags,
				syn_opt_arg.cont_in_list,
					 syn_opt_arg.next_list, conceal_char);
			if (p == NULL)
			    break;
			if (p[1] == NUL)
			{
			    semsg(_(e_error_missing_rsb_str), kw);
			    goto error;
			}
			if (p[1] == ']')
			{
			    if (p[2] != NUL)
			    {
				semsg(_(e_trailing_char_after_rsb_str_str),
								    kw, &p[2]);
				goto error;
			    }
			    kw = p + 1;		// skip over the "]"
			    kwlen = 1;
			    break;
			}
			if (has_mbyte)
			{
			    int l = (*mb_ptr2len)(p + 1);

			    mch_memmove(p, p + 1, l);
			    p += l;
			}
			else
			{
			    p[0] = p[1];
			    ++p;
			}
		    }
		}
	    }
error:
	    vim_free(keyword_copy);
	    vim_free(syn_opt_arg.cont_in_list);
	    vim_free(syn_opt_arg.next_list);
	}
    }

    if (rest != NULL)
	set_nextcmd(eap, rest);
    else
	semsg(_(e_invalid_argument_str), arg);

    redraw_curbuf_later(UPD_SOME_VALID);
    syn_stack_free_all(curwin->w_s);		// Need to recompute all syntax.
}

/*
 * Handle ":syntax match {name} [{options}] {pattern} [{options}]".
 *
 * Also ":syntax sync match {name} [[grouphere | groupthere] {group-name}] .."
 */
    static void
syn_cmd_match(
    exarg_T	*eap,
    int		syncing)	    // TRUE for ":syntax sync match .. "
{
    char_u	*arg = eap->arg;
    char_u	*group_name_end;
    char_u	*rest;
    synpat_T	item;		// the item found in the line
    int		syn_id;
    int		idx;
    syn_opt_arg_T syn_opt_arg;
    int		sync_idx = 0;
    int		conceal_char = NUL;
    int		orig_called_emsg = called_emsg;

    // Isolate the group name, check for validity
    rest = get_group_name(arg, &group_name_end);

    // Get options before the pattern
    syn_opt_arg.flags = 0;
    syn_opt_arg.keyword = FALSE;
    syn_opt_arg.sync_idx = syncing ? &sync_idx : NULL;
    syn_opt_arg.has_cont_list = TRUE;
    syn_opt_arg.cont_list = NULL;
    syn_opt_arg.cont_in_list = NULL;
    syn_opt_arg.next_list = NULL;
    rest = get_syn_options(rest, &syn_opt_arg, &conceal_char, eap->skip);

    // get the pattern.
    init_syn_patterns();
    CLEAR_FIELD(item);
    rest = get_syn_pattern(rest, &item);
    if (vim_regcomp_had_eol() && !(syn_opt_arg.flags & HL_EXCLUDENL))
	syn_opt_arg.flags |= HL_HAS_EOL;

    // Get options after the pattern
    rest = get_syn_options(rest, &syn_opt_arg, &conceal_char, eap->skip);

    if (rest != NULL)		// all arguments are valid
    {
	/*
	 * Check for trailing command and illegal trailing arguments.
	 */
	set_nextcmd(eap, rest);
	if (!ends_excmd2(eap->cmd, rest) || eap->skip)
	    rest = NULL;
	else if (ga_grow(&curwin->w_s->b_syn_patterns, 1) == OK
		&& (syn_id = syn_check_group(arg,
					   (int)(group_name_end - arg))) != 0)
	{
	    syn_incl_toplevel(syn_id, &syn_opt_arg.flags);
	    /*
	     * Store the pattern in the syn_items list
	     */
	    idx = curwin->w_s->b_syn_patterns.ga_len;
	    SYN_ITEMS(curwin->w_s)[idx] = item;
	    SYN_ITEMS(curwin->w_s)[idx].sp_syncing = syncing;
	    SYN_ITEMS(curwin->w_s)[idx].sp_type = SPTYPE_MATCH;
	    SYN_ITEMS(curwin->w_s)[idx].sp_syn.id = syn_id;
	    SYN_ITEMS(curwin->w_s)[idx].sp_syn.inc_tag = current_syn_inc_tag;
	    SYN_ITEMS(curwin->w_s)[idx].sp_flags = syn_opt_arg.flags;
	    SYN_ITEMS(curwin->w_s)[idx].sp_sync_idx = sync_idx;
	    SYN_ITEMS(curwin->w_s)[idx].sp_cont_list = syn_opt_arg.cont_list;
	    SYN_ITEMS(curwin->w_s)[idx].sp_syn.cont_in_list =
						     syn_opt_arg.cont_in_list;
#ifdef FEAT_CONCEAL
	    SYN_ITEMS(curwin->w_s)[idx].sp_cchar = conceal_char;
#endif
	    if (syn_opt_arg.cont_in_list != NULL)
		curwin->w_s->b_syn_containedin = TRUE;
	    SYN_ITEMS(curwin->w_s)[idx].sp_next_list = syn_opt_arg.next_list;
	    ++curwin->w_s->b_syn_patterns.ga_len;

	    // remember that we found a match for syncing on
	    if (syn_opt_arg.flags & (HL_SYNC_HERE|HL_SYNC_THERE))
		curwin->w_s->b_syn_sync_flags |= SF_MATCH;
#ifdef FEAT_FOLDING
	    if (syn_opt_arg.flags & HL_FOLD)
		++curwin->w_s->b_syn_folditems;
#endif

	    redraw_curbuf_later(UPD_SOME_VALID);
	    syn_stack_free_all(curwin->w_s);	// Need to recompute all syntax.
	    return;	// don't free the progs and patterns now
	}
    }

    /*
     * Something failed, free the allocated memory.
     */
    vim_regfree(item.sp_prog);
    vim_free(item.sp_pattern);
    vim_free(syn_opt_arg.cont_list);
    vim_free(syn_opt_arg.cont_in_list);
    vim_free(syn_opt_arg.next_list);

    if (rest == NULL && called_emsg == orig_called_emsg)
	semsg(_(e_invalid_argument_str), arg);
}

/*
 * Handle ":syntax region {group-name} [matchgroup={group-name}]
 *		start {start} .. [skip {skip}] end {end} .. [{options}]".
 */
    static void
syn_cmd_region(
    exarg_T	*eap,
    int		syncing)	    // TRUE for ":syntax sync region .."
{
    char_u		*arg = eap->arg;
    char_u		*group_name_end;
    char_u		*rest;			// next arg, NULL on error
    char_u		*key_end;
    char_u		*key = NULL;
    char_u		*p;
    int			item;
#define ITEM_START	    0
#define ITEM_SKIP	    1
#define ITEM_END	    2
#define ITEM_MATCHGROUP	    3
    struct pat_ptr
    {
	synpat_T	*pp_synp;		// pointer to syn_pattern
	int		pp_matchgroup_id;	// matchgroup ID
	struct pat_ptr	*pp_next;		// pointer to next pat_ptr
    }			*(pat_ptrs[3]);
					// patterns found in the line
    struct pat_ptr	*ppp;
    struct pat_ptr	*ppp_next;
    int			pat_count = 0;		// nr of syn_patterns found
    int			syn_id;
    int			matchgroup_id = 0;
    int			not_enough = FALSE;	// not enough arguments
    int			illegal = FALSE;	// illegal arguments
    int			success = FALSE;
    int			idx;
    syn_opt_arg_T	syn_opt_arg;
    int			conceal_char = NUL;

    // Isolate the group name, check for validity
    rest = get_group_name(arg, &group_name_end);

    pat_ptrs[0] = NULL;
    pat_ptrs[1] = NULL;
    pat_ptrs[2] = NULL;

    init_syn_patterns();

    syn_opt_arg.flags = 0;
    syn_opt_arg.keyword = FALSE;
    syn_opt_arg.sync_idx = NULL;
    syn_opt_arg.has_cont_list = TRUE;
    syn_opt_arg.cont_list = NULL;
    syn_opt_arg.cont_in_list = NULL;
    syn_opt_arg.next_list = NULL;

    /*
     * get the options, patterns and matchgroup.
     */
    while (rest != NULL && !ends_excmd2(eap->cmd, rest))
    {
	// Check for option arguments
	rest = get_syn_options(rest, &syn_opt_arg, &conceal_char, eap->skip);
	if (rest == NULL || ends_excmd2(eap->cmd, rest))
	    break;

	// must be a pattern or matchgroup then
	key_end = rest;
	while (*key_end && !VIM_ISWHITE(*key_end) && *key_end != '=')
	    ++key_end;
	vim_free(key);
	key = vim_strnsave_up(rest, key_end - rest);
	if (key == NULL)			// out of memory
	{
	    rest = NULL;
	    break;
	}
	if (STRCMP(key, "MATCHGROUP") == 0)
	    item = ITEM_MATCHGROUP;
	else if (STRCMP(key, "START") == 0)
	    item = ITEM_START;
	else if (STRCMP(key, "END") == 0)
	    item = ITEM_END;
	else if (STRCMP(key, "SKIP") == 0)
	{
	    if (pat_ptrs[ITEM_SKIP] != NULL)	// one skip pattern allowed
	    {
		illegal = TRUE;
		break;
	    }
	    item = ITEM_SKIP;
	}
	else
	    break;
	rest = skipwhite(key_end);
	if (*rest != '=')
	{
	    rest = NULL;
	    semsg(_(e_missing_equal_str), arg);
	    break;
	}
	rest = skipwhite(rest + 1);
	if (*rest == NUL)
	{
	    not_enough = TRUE;
	    break;
	}

	if (item == ITEM_MATCHGROUP)
	{
	    p = skiptowhite(rest);
	    if ((p - rest == 4 && STRNCMP(rest, "NONE", 4) == 0) || eap->skip)
		matchgroup_id = 0;
	    else
	    {
		matchgroup_id = syn_check_group(rest, (int)(p - rest));
		if (matchgroup_id == 0)
		{
		    illegal = TRUE;
		    break;
		}
	    }
	    rest = skipwhite(p);
	}
	else
	{
	    /*
	     * Allocate room for a syn_pattern, and link it in the list of
	     * syn_patterns for this item, at the start (because the list is
	     * used from end to start).
	     */
	    ppp = ALLOC_ONE(struct pat_ptr);
	    if (ppp == NULL)
	    {
		rest = NULL;
		break;
	    }
	    ppp->pp_next = pat_ptrs[item];
	    pat_ptrs[item] = ppp;
	    ppp->pp_synp = ALLOC_CLEAR_ONE(synpat_T);
	    if (ppp->pp_synp == NULL)
	    {
		rest = NULL;
		break;
	    }

	    /*
	     * Get the syntax pattern and the following offset(s).
	     */
	    // Enable the appropriate \z specials.
	    if (item == ITEM_START)
		reg_do_extmatch = REX_SET;
	    else if (item == ITEM_SKIP || item == ITEM_END)
		reg_do_extmatch = REX_USE;
	    rest = get_syn_pattern(rest, ppp->pp_synp);
	    reg_do_extmatch = 0;
	    if (item == ITEM_END && vim_regcomp_had_eol()
				       && !(syn_opt_arg.flags & HL_EXCLUDENL))
		ppp->pp_synp->sp_flags |= HL_HAS_EOL;
	    ppp->pp_matchgroup_id = matchgroup_id;
	    ++pat_count;
	}
    }
    vim_free(key);
    if (illegal || not_enough)
	rest = NULL;

    /*
     * Must have a "start" and "end" pattern.
     */
    if (rest != NULL && (pat_ptrs[ITEM_START] == NULL ||
						  pat_ptrs[ITEM_END] == NULL))
    {
	not_enough = TRUE;
	rest = NULL;
    }

    if (rest != NULL)
    {
	/*
	 * Check for trailing garbage or command.
	 * If OK, add the item.
	 */
	set_nextcmd(eap, rest);
	if (!ends_excmd(*rest) || eap->skip)
	    rest = NULL;
	else if (ga_grow(&(curwin->w_s->b_syn_patterns), pat_count) == OK
		&& (syn_id = syn_check_group(arg,
					   (int)(group_name_end - arg))) != 0)
	{
	    syn_incl_toplevel(syn_id, &syn_opt_arg.flags);
	    /*
	     * Store the start/skip/end in the syn_items list
	     */
	    idx = curwin->w_s->b_syn_patterns.ga_len;
	    for (item = ITEM_START; item <= ITEM_END; ++item)
	    {
		for (ppp = pat_ptrs[item]; ppp != NULL; ppp = ppp->pp_next)
		{
		    SYN_ITEMS(curwin->w_s)[idx] = *(ppp->pp_synp);
		    SYN_ITEMS(curwin->w_s)[idx].sp_syncing = syncing;
		    SYN_ITEMS(curwin->w_s)[idx].sp_type =
			    (item == ITEM_START) ? SPTYPE_START :
			    (item == ITEM_SKIP) ? SPTYPE_SKIP : SPTYPE_END;
		    SYN_ITEMS(curwin->w_s)[idx].sp_flags |= syn_opt_arg.flags;
		    SYN_ITEMS(curwin->w_s)[idx].sp_syn.id = syn_id;
		    SYN_ITEMS(curwin->w_s)[idx].sp_syn.inc_tag =
							  current_syn_inc_tag;
		    SYN_ITEMS(curwin->w_s)[idx].sp_syn_match_id =
							ppp->pp_matchgroup_id;
#ifdef FEAT_CONCEAL
		    SYN_ITEMS(curwin->w_s)[idx].sp_cchar = conceal_char;
#endif
		    if (item == ITEM_START)
		    {
			SYN_ITEMS(curwin->w_s)[idx].sp_cont_list =
							syn_opt_arg.cont_list;
			SYN_ITEMS(curwin->w_s)[idx].sp_syn.cont_in_list =
						     syn_opt_arg.cont_in_list;
			if (syn_opt_arg.cont_in_list != NULL)
			    curwin->w_s->b_syn_containedin = TRUE;
			SYN_ITEMS(curwin->w_s)[idx].sp_next_list =
							syn_opt_arg.next_list;
		    }
		    ++curwin->w_s->b_syn_patterns.ga_len;
		    ++idx;
#ifdef FEAT_FOLDING
		    if (syn_opt_arg.flags & HL_FOLD)
			++curwin->w_s->b_syn_folditems;
#endif
		}
	    }

	    redraw_curbuf_later(UPD_SOME_VALID);
	    syn_stack_free_all(curwin->w_s);	// Need to recompute all syntax.
	    success = TRUE;	    // don't free the progs and patterns now
	}
    }

    /*
     * Free the allocated memory.
     */
    for (item = ITEM_START; item <= ITEM_END; ++item)
	for (ppp = pat_ptrs[item]; ppp != NULL; ppp = ppp_next)
	{
	    if (!success && ppp->pp_synp != NULL)
	    {
		vim_regfree(ppp->pp_synp->sp_prog);
		vim_free(ppp->pp_synp->sp_pattern);
	    }
	    vim_free(ppp->pp_synp);
	    ppp_next = ppp->pp_next;
	    vim_free(ppp);
	}

    if (!success)
    {
	vim_free(syn_opt_arg.cont_list);
	vim_free(syn_opt_arg.cont_in_list);
	vim_free(syn_opt_arg.next_list);
	if (not_enough)
	    semsg(_(e_not_enough_arguments_syntax_region_str), arg);
	else if (illegal || rest == NULL)
	    semsg(_(e_invalid_argument_str), arg);
    }
}

/*
 * A simple syntax group ID comparison function suitable for use in qsort()
 */
    static int
syn_compare_stub(const void *v1, const void *v2)
{
    const short	*s1 = v1;
    const short	*s2 = v2;

    return (*s1 > *s2 ? 1 : *s1 < *s2 ? -1 : 0);
}

/*
 * Combines lists of syntax clusters.
 * *clstr1 and *clstr2 must both be allocated memory; they will be consumed.
 */
    static void
syn_combine_list(short **clstr1, short **clstr2, int list_op)
{
    int		count1 = 0;
    int		count2 = 0;
    short	*g1;
    short	*g2;
    short	*clstr = NULL;
    int		count;
    int		round;

    /*
     * Handle degenerate cases.
     */
    if (*clstr2 == NULL)
	return;
    if (*clstr1 == NULL || list_op == CLUSTER_REPLACE)
    {
	if (list_op == CLUSTER_REPLACE)
	    vim_free(*clstr1);
	if (list_op == CLUSTER_REPLACE || list_op == CLUSTER_ADD)
	    *clstr1 = *clstr2;
	else
	    vim_free(*clstr2);
	return;
    }

    for (g1 = *clstr1; *g1; g1++)
	++count1;
    for (g2 = *clstr2; *g2; g2++)
	++count2;

    /*
     * For speed purposes, sort both lists.
     */
    qsort(*clstr1, (size_t)count1, sizeof(short), syn_compare_stub);
    qsort(*clstr2, (size_t)count2, sizeof(short), syn_compare_stub);

    /*
     * We proceed in two passes; in round 1, we count the elements to place
     * in the new list, and in round 2, we allocate and populate the new
     * list.  For speed, we use a mergesort-like method, adding the smaller
     * of the current elements in each list to the new list.
     */
    for (round = 1; round <= 2; round++)
    {
	g1 = *clstr1;
	g2 = *clstr2;
	count = 0;

	/*
	 * First, loop through the lists until one of them is empty.
	 */
	while (*g1 && *g2)
	{
	    /*
	     * We always want to add from the first list.
	     */
	    if (*g1 < *g2)
	    {
		if (round == 2)
		    clstr[count] = *g1;
		count++;
		g1++;
		continue;
	    }
	    /*
	     * We only want to add from the second list if we're adding the
	     * lists.
	     */
	    if (list_op == CLUSTER_ADD)
	    {
		if (round == 2)
		    clstr[count] = *g2;
		count++;
	    }
	    if (*g1 == *g2)
		g1++;
	    g2++;
	}

	/*
	 * Now add the leftovers from whichever list didn't get finished
	 * first.  As before, we only want to add from the second list if
	 * we're adding the lists.
	 */
	for (; *g1; g1++, count++)
	    if (round == 2)
		clstr[count] = *g1;
	if (list_op == CLUSTER_ADD)
	    for (; *g2; g2++, count++)
		if (round == 2)
		    clstr[count] = *g2;

	if (round == 1)
	{
	    /*
	     * If the group ended up empty, we don't need to allocate any
	     * space for it.
	     */
	    if (count == 0)
	    {
		clstr = NULL;
		break;
	    }
	    clstr = ALLOC_MULT(short, count + 1);
	    if (clstr == NULL)
		break;
	    clstr[count] = 0;
	}
    }

    /*
     * Finally, put the new list in place.
     */
    vim_free(*clstr1);
    vim_free(*clstr2);
    *clstr1 = clstr;
}

/*
 * Lookup a syntax cluster name and return its ID.
 * If it is not found, 0 is returned.
 */
    static int
syn_scl_name2id(char_u *name)
{
    int		i;
    char_u	*name_u;

    // Avoid using stricmp() too much, it's slow on some systems
    name_u = vim_strsave_up(name);
    if (name_u == NULL)
	return 0;
    for (i = curwin->w_s->b_syn_clusters.ga_len; --i >= 0; )
	if (SYN_CLSTR(curwin->w_s)[i].scl_name_u != NULL
		&& STRCMP(name_u, SYN_CLSTR(curwin->w_s)[i].scl_name_u) == 0)
	    break;
    vim_free(name_u);
    return (i < 0 ? 0 : i + SYNID_CLUSTER);
}

/*
 * Like syn_scl_name2id(), but take a pointer + length argument.
 */
    static int
syn_scl_namen2id(char_u *linep, int len)
{
    char_u  *name;
    int	    id = 0;

    name = vim_strnsave(linep, len);
    if (name == NULL)
	return 0;

    id = syn_scl_name2id(name);
    vim_free(name);
    return id;
}

/*
 * Find syntax cluster name in the table and return its ID.
 * The argument is a pointer to the name and the length of the name.
 * If it doesn't exist yet, a new entry is created.
 * Return 0 for failure.
 */
    static int
syn_check_cluster(char_u *pp, int len)
{
    int		id;
    char_u	*name;

    name = vim_strnsave(pp, len);
    if (name == NULL)
	return 0;

    id = syn_scl_name2id(name);
    if (id == 0)			// doesn't exist yet
	id = syn_add_cluster(name);
    else
	vim_free(name);
    return id;
}

/*
 * Add new syntax cluster and return its ID.
 * "name" must be an allocated string, it will be consumed.
 * Return 0 for failure.
 */
    static int
syn_add_cluster(char_u *name)
{
    int		len;

    /*
     * First call for this growarray: init growing array.
     */
    if (curwin->w_s->b_syn_clusters.ga_data == NULL)
    {
	curwin->w_s->b_syn_clusters.ga_itemsize = sizeof(syn_cluster_T);
	curwin->w_s->b_syn_clusters.ga_growsize = 10;
    }

    len = curwin->w_s->b_syn_clusters.ga_len;
    if (len >= MAX_CLUSTER_ID)
    {
	emsg(_(e_too_many_syntax_clusters));
	vim_free(name);
	return 0;
    }

    /*
     * Make room for at least one other cluster entry.
     */
    if (ga_grow(&curwin->w_s->b_syn_clusters, 1) == FAIL)
    {
	vim_free(name);
	return 0;
    }

    CLEAR_POINTER(&(SYN_CLSTR(curwin->w_s)[len]));
    SYN_CLSTR(curwin->w_s)[len].scl_name = name;
    SYN_CLSTR(curwin->w_s)[len].scl_name_u = vim_strsave_up(name);
    SYN_CLSTR(curwin->w_s)[len].scl_list = NULL;
    ++curwin->w_s->b_syn_clusters.ga_len;

    if (STRICMP(name, "Spell") == 0)
	curwin->w_s->b_spell_cluster_id = len + SYNID_CLUSTER;
    if (STRICMP(name, "NoSpell") == 0)
	curwin->w_s->b_nospell_cluster_id = len + SYNID_CLUSTER;

    return len + SYNID_CLUSTER;
}

/*
 * Handle ":syntax cluster {cluster-name} [contains={groupname},..]
 *		[add={groupname},..] [remove={groupname},..]".
 */
    static void
syn_cmd_cluster(exarg_T *eap, int syncing UNUSED)
{
    char_u	*arg = eap->arg;
    char_u	*group_name_end;
    char_u	*rest;
    int		scl_id;
    short	*clstr_list;
    int		got_clstr = FALSE;
    int		opt_len;
    int		list_op;

    eap->nextcmd = find_nextcmd(arg);
    if (eap->skip)
	return;

    rest = get_group_name(arg, &group_name_end);

    if (rest != NULL)
    {
	scl_id = syn_check_cluster(arg, (int)(group_name_end - arg));
	if (scl_id == 0)
	    return;
	scl_id -= SYNID_CLUSTER;

	for (;;)
	{
	    if (STRNICMP(rest, "add", 3) == 0
		    && (VIM_ISWHITE(rest[3]) || rest[3] == '='))
	    {
		opt_len = 3;
		list_op = CLUSTER_ADD;
	    }
	    else if (STRNICMP(rest, "remove", 6) == 0
		    && (VIM_ISWHITE(rest[6]) || rest[6] == '='))
	    {
		opt_len = 6;
		list_op = CLUSTER_SUBTRACT;
	    }
	    else if (STRNICMP(rest, "contains", 8) == 0
			&& (VIM_ISWHITE(rest[8]) || rest[8] == '='))
	    {
		opt_len = 8;
		list_op = CLUSTER_REPLACE;
	    }
	    else
		break;

	    clstr_list = NULL;
	    if (get_id_list(&rest, opt_len, &clstr_list, eap->skip) == FAIL)
	    {
		semsg(_(e_invalid_argument_str), rest);
		break;
	    }
	    if (scl_id >= 0)
		syn_combine_list(&SYN_CLSTR(curwin->w_s)[scl_id].scl_list,
			     &clstr_list, list_op);
	    else
		vim_free(clstr_list);
	    got_clstr = TRUE;
	}

	if (got_clstr)
	{
	    redraw_curbuf_later(UPD_SOME_VALID);
	    syn_stack_free_all(curwin->w_s);	// Need to recompute all.
	}
    }

    if (!got_clstr)
	emsg(_(e_no_cluster_specified));
    if (rest == NULL || !ends_excmd2(eap->cmd, rest))
	semsg(_(e_invalid_argument_str), arg);
}

/*
 * On first call for current buffer: Init growing array.
 */
    static void
init_syn_patterns(void)
{
    curwin->w_s->b_syn_patterns.ga_itemsize = sizeof(synpat_T);
    curwin->w_s->b_syn_patterns.ga_growsize = 10;
}

/*
 * Get one pattern for a ":syntax match" or ":syntax region" command.
 * Stores the pattern and program in a synpat_T.
 * Returns a pointer to the next argument, or NULL in case of an error.
 */
    static char_u *
get_syn_pattern(char_u *arg, synpat_T *ci)
{
    char_u	*end;
    int		*p;
    int		idx;
    char_u	*cpo_save;

    // need at least three chars
    if (arg == NULL || arg[0] == NUL || arg[1] == NUL || arg[2] == NUL)
	return NULL;

    end = skip_regexp(arg + 1, *arg, TRUE);
    if (*end != *arg)			    // end delimiter not found
    {
	semsg(_(e_pattern_delimiter_not_found_str), arg);
	return NULL;
    }
    // store the pattern and compiled regexp program
    if ((ci->sp_pattern = vim_strnsave(arg + 1, end - arg - 1)) == NULL)
	return NULL;

    // Make 'cpoptions' empty, to avoid the 'l' flag
    cpo_save = p_cpo;
    p_cpo = empty_option;
    ci->sp_prog = vim_regcomp(ci->sp_pattern, RE_MAGIC);
    p_cpo = cpo_save;

    if (ci->sp_prog == NULL)
	return NULL;
    ci->sp_ic = curwin->w_s->b_syn_ic;
#ifdef FEAT_PROFILE
    syn_clear_time(&ci->sp_time);
#endif

    /*
     * Check for a match, highlight or region offset.
     */
    ++end;
    do
    {
	for (idx = SPO_COUNT; --idx >= 0; )
	    if (STRNCMP(end, spo_name_tab[idx], 3) == 0)
		break;
	if (idx >= 0)
	{
	    p = &(ci->sp_offsets[idx]);
	    if (idx != SPO_LC_OFF)
		switch (end[3])
		{
		    case 's':   break;
		    case 'b':   break;
		    case 'e':   idx += SPO_COUNT; break;
		    default:    idx = -1; break;
		}
	    if (idx >= 0)
	    {
		ci->sp_off_flags |= (1 << idx);
		if (idx == SPO_LC_OFF)	    // lc=99
		{
		    end += 3;
		    *p = getdigits(&end);

		    // "lc=" offset automatically sets "ms=" offset
		    if (!(ci->sp_off_flags & (1 << SPO_MS_OFF)))
		    {
			ci->sp_off_flags |= (1 << SPO_MS_OFF);
			ci->sp_offsets[SPO_MS_OFF] = *p;
		    }
		}
		else			    // yy=x+99
		{
		    end += 4;
		    if (*end == '+')
		    {
			++end;
			*p = getdigits(&end);		// positive offset
		    }
		    else if (*end == '-')
		    {
			++end;
			*p = -getdigits(&end);		// negative offset
		    }
		}
		if (*end != ',')
		    break;
		++end;
	    }
	}
    } while (idx >= 0);

    if (!ends_excmd2(arg, end) && !VIM_ISWHITE(*end))
    {
	semsg(_(e_garbage_after_pattern_str), arg);
	return NULL;
    }
    return skipwhite(end);
}

/*
 * Handle ":syntax sync .." command.
 */
    static void
syn_cmd_sync(exarg_T *eap, int syncing UNUSED)
{
    char_u	*arg_start = eap->arg;
    char_u	*arg_end;
    char_u	*key = NULL;
    char_u	*next_arg;
    int		illegal = FALSE;
    int		finished = FALSE;
    long	n;
    char_u	*cpo_save;

    if (ends_excmd2(eap->cmd, arg_start))
    {
	syn_cmd_list(eap, TRUE);
	return;
    }

    while (!ends_excmd2(eap->cmd, arg_start))
    {
	arg_end = skiptowhite(arg_start);
	next_arg = skipwhite(arg_end);
	vim_free(key);
	key = vim_strnsave_up(arg_start, arg_end - arg_start);
	if (key == NULL)
	    break;
	if (STRCMP(key, "CCOMMENT") == 0)
	{
	    if (!eap->skip)
		curwin->w_s->b_syn_sync_flags |= SF_CCOMMENT;
	    if (!ends_excmd2(eap->cmd, next_arg))
	    {
		arg_end = skiptowhite(next_arg);
		if (!eap->skip)
		    curwin->w_s->b_syn_sync_id = syn_check_group(next_arg,
						   (int)(arg_end - next_arg));
		next_arg = skipwhite(arg_end);
	    }
	    else if (!eap->skip)
		curwin->w_s->b_syn_sync_id = syn_name2id((char_u *)"Comment");
	}
	else if (  STRNCMP(key, "LINES", 5) == 0
		|| STRNCMP(key, "MINLINES", 8) == 0
		|| STRNCMP(key, "MAXLINES", 8) == 0
		|| STRNCMP(key, "LINEBREAKS", 10) == 0)
	{
	    if (key[4] == 'S')
		arg_end = key + 6;
	    else if (key[0] == 'L')
		arg_end = key + 11;
	    else
		arg_end = key + 9;
	    if (arg_end[-1] != '=' || !VIM_ISDIGIT(*arg_end))
	    {
		illegal = TRUE;
		break;
	    }
	    n = getdigits(&arg_end);
	    if (!eap->skip)
	    {
		if (key[4] == 'B')
		    curwin->w_s->b_syn_sync_linebreaks = n;
		else if (key[1] == 'A')
		    curwin->w_s->b_syn_sync_maxlines = n;
		else
		    curwin->w_s->b_syn_sync_minlines = n;
	    }
	}
	else if (STRCMP(key, "FROMSTART") == 0)
	{
	    if (!eap->skip)
	    {
		curwin->w_s->b_syn_sync_minlines = MAXLNUM;
		curwin->w_s->b_syn_sync_maxlines = 0;
	    }
	}
	else if (STRCMP(key, "LINECONT") == 0)
	{
	    if (*next_arg == NUL)	   // missing pattern
	    {
		illegal = TRUE;
		break;
	    }
	    if (curwin->w_s->b_syn_linecont_pat != NULL)
	    {
		emsg(_(e_syntax_sync_line_continuations_pattern_specified_twice));
		finished = TRUE;
		break;
	    }
	    arg_end = skip_regexp(next_arg + 1, *next_arg, TRUE);
	    if (*arg_end != *next_arg)	    // end delimiter not found
	    {
		illegal = TRUE;
		break;
	    }

	    if (!eap->skip)
	    {
		// store the pattern and compiled regexp program
		if ((curwin->w_s->b_syn_linecont_pat =
			    vim_strnsave(next_arg + 1,
				      arg_end - next_arg - 1)) == NULL)
		{
		    finished = TRUE;
		    break;
		}
		curwin->w_s->b_syn_linecont_ic = curwin->w_s->b_syn_ic;

		// Make 'cpoptions' empty, to avoid the 'l' flag
		cpo_save = p_cpo;
		p_cpo = empty_option;
		curwin->w_s->b_syn_linecont_prog =
		       vim_regcomp(curwin->w_s->b_syn_linecont_pat, RE_MAGIC);
		p_cpo = cpo_save;
#ifdef FEAT_PROFILE
		syn_clear_time(&curwin->w_s->b_syn_linecont_time);
#endif

		if (curwin->w_s->b_syn_linecont_prog == NULL)
		{
		    VIM_CLEAR(curwin->w_s->b_syn_linecont_pat);
		    finished = TRUE;
		    break;
		}
	    }
	    next_arg = skipwhite(arg_end + 1);
	}
	else
	{
	    eap->arg = next_arg;
	    if (STRCMP(key, "MATCH") == 0)
		syn_cmd_match(eap, TRUE);
	    else if (STRCMP(key, "REGION") == 0)
		syn_cmd_region(eap, TRUE);
	    else if (STRCMP(key, "CLEAR") == 0)
		syn_cmd_clear(eap, TRUE);
	    else
		illegal = TRUE;
	    finished = TRUE;
	    break;
	}
	arg_start = next_arg;
    }
    vim_free(key);
    if (illegal)
	semsg(_(e_illegal_arguments_str), arg_start);
    else if (!finished)
    {
	set_nextcmd(eap, arg_start);
	redraw_curbuf_later(UPD_SOME_VALID);
	syn_stack_free_all(curwin->w_s);	// Need to recompute all syntax.
    }
}

/*
 * Convert a line of highlight group names into a list of group ID numbers.
 * "arg" should point to the "contains" or "nextgroup" keyword.
 * "arg" is advanced to after the last group name.
 * Careful: the argument is modified (NULs added).
 * returns FAIL for some error, OK for success.
 */
    static int
get_id_list(
    char_u	**arg,
    int		keylen,		// length of keyword
    short	**list,		// where to store the resulting list, if not
				// NULL, the list is silently skipped!
    int		skip)
{
    char_u	*p = NULL;
    char_u	*end;
    int		round;
    int		count;
    int		total_count = 0;
    short	*retval = NULL;
    char_u	*name;
    regmatch_T	regmatch;
    int		id;
    int		i;
    int		failed = FALSE;

    /*
     * We parse the list twice:
     * round == 1: count the number of items, allocate the array.
     * round == 2: fill the array with the items.
     * In round 1 new groups may be added, causing the number of items to
     * grow when a regexp is used.  In that case round 1 is done once again.
     */
    for (round = 1; round <= 2; ++round)
    {
	/*
	 * skip "contains"
	 */
	p = skipwhite(*arg + keylen);
	if (*p != '=')
	{
	    semsg(_(e_missing_equal_sign_str), *arg);
	    break;
	}
	p = skipwhite(p + 1);
	if (ends_excmd2(*arg, p))
	{
	    semsg(_(e_empty_argument_str), *arg);
	    break;
	}

	/*
	 * parse the arguments after "contains"
	 */
	count = 0;
	while (!ends_excmd2(*arg, p))
	{
	    for (end = p; *end && !VIM_ISWHITE(*end) && *end != ','; ++end)
		;
	    name = alloc(end - p + 3);	    // leave room for "^$"
	    if (name == NULL)
	    {
		failed = TRUE;
		break;
	    }
	    vim_strncpy(name + 1, p, end - p);
	    if (       STRCMP(name + 1, "ALLBUT") == 0
		    || STRCMP(name + 1, "ALL") == 0
		    || STRCMP(name + 1, "TOP") == 0
		    || STRCMP(name + 1, "CONTAINED") == 0)
	    {
		if (TOUPPER_ASC(**arg) != 'C')
		{
		    semsg(_(e_str_not_allowed_here), name + 1);
		    failed = TRUE;
		    vim_free(name);
		    break;
		}
		if (count != 0)
		{
		    semsg(_(e_str_must_be_first_in_contains_list), name + 1);
		    failed = TRUE;
		    vim_free(name);
		    break;
		}
		if (name[1] == 'A')
		    id = SYNID_ALLBUT;
		else if (name[1] == 'T')
		    id = SYNID_TOP;
		else
		    id = SYNID_CONTAINED;
		id += current_syn_inc_tag;
	    }
	    else if (name[1] == '@')
	    {
		if (skip)
		    id = -1;
		else
		    id = syn_check_cluster(name + 2, (int)(end - p - 1));
	    }
	    else
	    {
		/*
		 * Handle full group name.
		 */
		if (vim_strpbrk(name + 1, (char_u *)"\\.*^$~[") == NULL)
		    id = syn_check_group(name + 1, (int)(end - p));
		else
		{
		    /*
		     * Handle match of regexp with group names.
		     */
		    *name = '^';
		    STRCAT(name, "$");
		    regmatch.regprog = vim_regcomp(name, RE_MAGIC);
		    if (regmatch.regprog == NULL)
		    {
			failed = TRUE;
			vim_free(name);
			break;
		    }

		    regmatch.rm_ic = TRUE;
		    id = 0;
		    for (i = highlight_num_groups(); --i >= 0; )
		    {
			if (vim_regexec(&regmatch, highlight_group_name(i),
								  (colnr_T)0))
			{
			    if (round == 2)
			    {
				// Got more items than expected; can happen
				// when adding items that match:
				// "contains=a.*b,axb".
				// Go back to first round
				if (count >= total_count)
				{
				    vim_free(retval);
				    round = 1;
				}
				else
				    retval[count] = i + 1;
			    }
			    ++count;
			    id = -1;	    // remember that we found one
			}
		    }
		    vim_regfree(regmatch.regprog);
		}
	    }
	    vim_free(name);
	    if (id == 0)
	    {
		semsg(_(e_unknown_group_name_str), p);
		failed = TRUE;
		break;
	    }
	    if (id > 0)
	    {
		if (round == 2)
		{
		    // Got more items than expected, go back to first round
		    if (count >= total_count)
		    {
			vim_free(retval);
			round = 1;
		    }
		    else
			retval[count] = id;
		}
		++count;
	    }
	    p = skipwhite(end);
	    if (*p != ',')
		break;
	    p = skipwhite(p + 1);	// skip comma in between arguments
	}
	if (failed)
	    break;
	if (round == 1)
	{
	    retval = ALLOC_MULT(short, count + 1);
	    if (retval == NULL)
		break;
	    retval[count] = 0;	    // zero means end of the list
	    total_count = count;
	}
    }

    *arg = p;
    if (failed || retval == NULL)
    {
	vim_free(retval);
	return FAIL;
    }

    if (*list == NULL)
	*list = retval;
    else
	vim_free(retval);	// list already found, don't overwrite it

    return OK;
}

/*
 * Make a copy of an ID list.
 */
    static short *
copy_id_list(short *list)
{
    int	    len;
    int	    count;
    short   *retval;

    if (list == NULL)
	return NULL;

    for (count = 0; list[count]; ++count)
	;
    len = (count + 1) * sizeof(short);
    retval = alloc(len);
    if (retval != NULL)
	mch_memmove(retval, list, (size_t)len);

    return retval;
}

/*
 * Check if syntax group "ssp" is in the ID list "list" of "cur_si".
 * "cur_si" can be NULL if not checking the "containedin" list.
 * Used to check if a syntax item is in the "contains" or "nextgroup" list of
 * the current item.
 * This function is called very often, keep it fast!!
 */
    static int
in_id_list(
    stateitem_T	*cur_si,	// current item or NULL
    short	*list,		// id list
    struct sp_syn *ssp,		// group id and ":syn include" tag of group
    int		flags)		// group flags
{
    int		retval;
    short	*scl_list;
    short	item;
    short	id = ssp->id;
    static int	depth = 0;
    int		r;
    int		toplevel;

    // If ssp has a "containedin" list and "cur_si" is in it, return TRUE.
    if (cur_si != NULL && ssp->cont_in_list != NULL
					    && !(cur_si->si_flags & HL_MATCH))
    {
	// Ignore transparent items without a contains argument.  Double check
	// that we don't go back past the first one.
	while ((cur_si->si_flags & HL_TRANS_CONT)
		&& cur_si > (stateitem_T *)(current_state.ga_data))
	    --cur_si;
	// cur_si->si_idx is -1 for keywords, these never contain anything.
	if (cur_si->si_idx >= 0 && in_id_list(NULL, ssp->cont_in_list,
		&(SYN_ITEMS(syn_block)[cur_si->si_idx].sp_syn),
		  SYN_ITEMS(syn_block)[cur_si->si_idx].sp_flags))
	    return TRUE;
    }

    if (list == NULL)
	return FALSE;

    /*
     * If list is ID_LIST_ALL, we are in a transparent item that isn't
     * inside anything.  Only allow not-contained groups.
     */
    if (list == ID_LIST_ALL)
	return !(flags & HL_CONTAINED);

    /*
     * Is this top-level (i.e. not 'contained') in the file it was declared in?
     * For included files, this is different from HL_CONTAINED, which is set
     * unconditionally.
     */
    toplevel = !(flags & HL_CONTAINED) || (flags & HL_INCLUDED_TOPLEVEL);

    /*
     * If the first item is "ALLBUT", return TRUE if "id" is NOT in the
     * contains list.  We also require that "id" is at the same ":syn include"
     * level as the list.
     */
    item = *list;
    if (item >= SYNID_ALLBUT && item < SYNID_CLUSTER)
    {
	if (item < SYNID_TOP)
	{
	    // ALL or ALLBUT: accept all groups in the same file
	    if (item - SYNID_ALLBUT != ssp->inc_tag)
		return FALSE;
	}
	else if (item < SYNID_CONTAINED)
	{
	    // TOP: accept all not-contained groups in the same file
	    if (item - SYNID_TOP != ssp->inc_tag || !toplevel)
		return FALSE;
	}
	else
	{
	    // CONTAINED: accept all contained groups in the same file
	    if (item - SYNID_CONTAINED != ssp->inc_tag || toplevel)
		return FALSE;
	}
	item = *++list;
	retval = FALSE;
    }
    else
	retval = TRUE;

    /*
     * Return "retval" if id is in the contains list.
     */
    while (item != 0)
    {
	if (item == id)
	    return retval;
	if (item >= SYNID_CLUSTER)
	{
	    scl_list = SYN_CLSTR(syn_block)[item - SYNID_CLUSTER].scl_list;
	    // restrict recursiveness to 30 to avoid an endless loop for a
	    // cluster that includes itself (indirectly)
	    if (scl_list != NULL && depth < 30)
	    {
		++depth;
		r = in_id_list(NULL, scl_list, ssp, flags);
		--depth;
		if (r)
		    return retval;
	    }
	}
	item = *++list;
    }
    return !retval;
}

struct subcommand
{
    char    *name;			// subcommand name
    void    (*func)(exarg_T *, int);	// function to call
};

static struct subcommand subcommands[] =
{
    {"case",		syn_cmd_case},
    {"clear",		syn_cmd_clear},
    {"cluster",		syn_cmd_cluster},
    {"conceal",		syn_cmd_conceal},
    {"enable",		syn_cmd_enable},
    {"foldlevel",	syn_cmd_foldlevel},
    {"include",		syn_cmd_include},
    {"iskeyword",	syn_cmd_iskeyword},
    {"keyword",		syn_cmd_keyword},
    {"list",		syn_cmd_list},
    {"manual",		syn_cmd_manual},
    {"match",		syn_cmd_match},
    {"on",		syn_cmd_on},
    {"off",		syn_cmd_off},
    {"region",		syn_cmd_region},
    {"reset",		syn_cmd_reset},
    {"spell",		syn_cmd_spell},
    {"sync",		syn_cmd_sync},
    {"",		syn_cmd_list}
};

/*
 * ":syntax".
 * This searches the subcommands[] table for the subcommand name, and calls a
 * syntax_subcommand() function to do the rest.
 */
    void
ex_syntax(exarg_T *eap)
{
    char_u	*arg = eap->arg;
    char_u	*subcmd_end;
    char_u	*subcmd_name;
    int		i;

    syn_cmdlinep = eap->cmdlinep;

    // isolate subcommand name
    for (subcmd_end = arg; ASCII_ISALPHA(*subcmd_end); ++subcmd_end)
	;
    subcmd_name = vim_strnsave(arg, subcmd_end - arg);
    if (subcmd_name == NULL)
	return;

    if (eap->skip)		// skip error messages for all subcommands
	++emsg_skip;
    for (i = 0; i < (int)ARRAY_LENGTH(subcommands); ++i)
    {
	if (STRCMP(subcmd_name, (char_u *)subcommands[i].name) == 0)
	{
	    eap->arg = skipwhite(subcmd_end);
	    (subcommands[i].func)(eap, FALSE);
	    break;
	}
    }

    if (i == (int)ARRAY_LENGTH(subcommands))
	semsg(_(e_invalid_syntax_subcommand_str), subcmd_name);

    vim_free(subcmd_name);
    if (eap->skip)
	--emsg_skip;
}

    void
ex_ownsyntax(exarg_T *eap)
{
    char_u	*old_value;
    char_u	*new_value;

    if (curwin->w_s == &curwin->w_buffer->b_s)
    {
	curwin->w_s = ALLOC_ONE(synblock_T);
	CLEAR_POINTER(curwin->w_s);
	hash_init(&curwin->w_s->b_keywtab);
	hash_init(&curwin->w_s->b_keywtab_ic);
#ifdef FEAT_SPELL
	// TODO: keep the spell checking as it was.
	curwin->w_p_spell = FALSE;	// No spell checking
	// make sure option values are "empty_option" instead of NULL
	clear_string_option(&curwin->w_s->b_p_spc);
	clear_string_option(&curwin->w_s->b_p_spf);
	clear_string_option(&curwin->w_s->b_p_spl);
	clear_string_option(&curwin->w_s->b_p_spo);
#endif
	clear_string_option(&curwin->w_s->b_syn_isk);
    }

    // save value of b:current_syntax
    old_value = get_var_value((char_u *)"b:current_syntax");
    if (old_value != NULL)
	old_value = vim_strsave(old_value);

    // Apply the "syntax" autocommand event, this finds and loads the syntax
    // file.
    apply_autocmds(EVENT_SYNTAX, eap->arg, curbuf->b_fname, TRUE, curbuf);

    // move value of b:current_syntax to w:current_syntax
    new_value = get_var_value((char_u *)"b:current_syntax");
    if (new_value != NULL)
	set_internal_string_var((char_u *)"w:current_syntax", new_value);

    // restore value of b:current_syntax
    if (old_value == NULL)
	do_unlet((char_u *)"b:current_syntax", TRUE);
    else
    {
	set_internal_string_var((char_u *)"b:current_syntax", old_value);
	vim_free(old_value);
    }
}

    int
syntax_present(win_T *win)
{
    return (win->w_s->b_syn_patterns.ga_len != 0
	    || win->w_s->b_syn_clusters.ga_len != 0
	    || win->w_s->b_keywtab.ht_used > 0
	    || win->w_s->b_keywtab_ic.ht_used > 0);
}


static enum
{
    EXP_SUBCMD,	    // expand ":syn" sub-commands
    EXP_CASE,	    // expand ":syn case" arguments
    EXP_SPELL,	    // expand ":syn spell" arguments
    EXP_SYNC,	    // expand ":syn sync" arguments
    EXP_CLUSTER	    // expand ":syn list @cluster" arguments
} expand_what;

/*
 * Reset include_link, include_default, include_none to 0.
 * Called when we are done expanding.
 */
    void
reset_expand_highlight(void)
{
    include_link = include_default = include_none = 0;
}

/*
 * Handle command line completion for :match and :echohl command: Add "NONE"
 * as highlight group.
 */
    void
set_context_in_echohl_cmd(expand_T *xp, char_u *arg)
{
    xp->xp_context = EXPAND_HIGHLIGHT;
    xp->xp_pattern = arg;
    include_none = 1;
}

/*
 * Handle command line completion for :syntax command.
 */
    void
set_context_in_syntax_cmd(expand_T *xp, char_u *arg)
{
    char_u	*p;

    // Default: expand subcommands
    xp->xp_context = EXPAND_SYNTAX;
    expand_what = EXP_SUBCMD;
    xp->xp_pattern = arg;
    include_link = 0;
    include_default = 0;

    if (*arg == NUL)
	return;

    // (part of) subcommand already typed
    p = skiptowhite(arg);
    if (*p == NUL)
	return;

    // past first word
    xp->xp_pattern = skipwhite(p);
    if (*skiptowhite(xp->xp_pattern) != NUL)
	xp->xp_context = EXPAND_NOTHING;
    else if (STRNICMP(arg, "case", p - arg) == 0)
	expand_what = EXP_CASE;
    else if (STRNICMP(arg, "spell", p - arg) == 0)
	expand_what = EXP_SPELL;
    else if (STRNICMP(arg, "sync", p - arg) == 0)
	expand_what = EXP_SYNC;
    else if (STRNICMP(arg, "list", p - arg) == 0)
    {
	p = skipwhite(p);
	if (*p == '@')
	    expand_what = EXP_CLUSTER;
	else
	    xp->xp_context = EXPAND_HIGHLIGHT;
    }
    else if (STRNICMP(arg, "keyword", p - arg) == 0
	    || STRNICMP(arg, "region", p - arg) == 0
	    || STRNICMP(arg, "match", p - arg) == 0)
	xp->xp_context = EXPAND_HIGHLIGHT;
    else
	xp->xp_context = EXPAND_NOTHING;
}

/*
 * Function given to ExpandGeneric() to obtain the list syntax names for
 * expansion.
 */
    char_u *
get_syntax_name(expand_T *xp, int idx)
{
    switch (expand_what)
    {
	case EXP_SUBCMD:
	    if (idx < 0 || idx >= (int)ARRAY_LENGTH(subcommands))
		return NULL;
	    return (char_u *)subcommands[idx].name;
	case EXP_CASE:
	{
	    static char *case_args[] = {"match", "ignore", NULL};
	    return (char_u *)case_args[idx];
	}
	case EXP_SPELL:
	{
	    static char *spell_args[] =
		{"toplevel", "notoplevel", "default", NULL};
	    return (char_u *)spell_args[idx];
	}
	case EXP_SYNC:
	{
	    static char *sync_args[] =
		{"ccomment", "clear", "fromstart",
		 "linebreaks=", "linecont", "lines=", "match",
		 "maxlines=", "minlines=", "region", NULL};
	    return (char_u *)sync_args[idx];
	}
	case EXP_CLUSTER:
	{
	    if (idx < curwin->w_s->b_syn_clusters.ga_len)
	    {
		vim_snprintf((char *)xp->xp_buf, EXPAND_BUF_LEN, "@%s",
					 SYN_CLSTR(curwin->w_s)[idx].scl_name);
		return xp->xp_buf;
	    }
	    else
		return NULL;
	}
    }
    return NULL;
}


/*
 * Function called for expression evaluation: get syntax ID at file position.
 */
    int
syn_get_id(
    win_T	*wp,
    long	lnum,
    colnr_T	col,
    int		trans,	     // remove transparency
    int		*spellp,     // return: can do spell checking
    int		keep_state)  // keep state of char at "col"
{
    // When the position is not after the current position and in the same
    // line of the same window with the same buffer, need to restart parsing.
    if (wp != syn_win
	    || wp->w_buffer != syn_buf
	    || lnum != current_lnum
	    || col < current_col)
	syntax_start(wp, lnum);
    else if (wp->w_buffer == syn_buf
	    && lnum == current_lnum
	    && col > current_col)
	// next_match may not be correct when moving around, e.g. with the
	// "skip" expression in searchpair()
	next_match_idx = -1;

    (void)get_syntax_attr(col, spellp, keep_state);

    return (trans ? current_trans_id : current_id);
}

#if defined(FEAT_CONCEAL) || defined(PROTO)
/*
 * Get extra information about the syntax item.  Must be called right after
 * get_syntax_attr().
 * Stores the current item sequence nr in "*seqnrp".
 * Returns the current flags.
 */
    int
get_syntax_info(int *seqnrp)
{
    *seqnrp = current_seqnr;
    return current_flags;
}

/*
 * Return conceal substitution character
 */
    int
syn_get_sub_char(void)
{
    return current_sub_char;
}
#endif

#if defined(FEAT_EVAL) || defined(PROTO)
/*
 * Return the syntax ID at position "i" in the current stack.
 * The caller must have called syn_get_id() before to fill the stack.
 * Returns -1 when "i" is out of range.
 */
    int
syn_get_stack_item(int i)
{
    if (i >= current_state.ga_len)
    {
	// Need to invalidate the state, because we didn't properly finish it
	// for the last character, "keep_state" was TRUE.
	invalidate_current_state();
	current_col = MAXCOL;
	return -1;
    }
    return CUR_STATE(i).si_id;
}
#endif

#if defined(FEAT_FOLDING) || defined(PROTO)
    static int
syn_cur_foldlevel(void)
{
    int		level = 0;
    int		i;

    for (i = 0; i < current_state.ga_len; ++i)
	if (CUR_STATE(i).si_flags & HL_FOLD)
	    ++level;
    return level;
}

/*
 * Function called to get folding level for line "lnum" in window "wp".
 */
    int
syn_get_foldlevel(win_T *wp, long lnum)
{
    int		level = 0;
    int		low_level;
    int		cur_level;

    // Return quickly when there are no fold items at all.
    if (wp->w_s->b_syn_folditems != 0
	    && !wp->w_s->b_syn_error
# ifdef SYN_TIME_LIMIT
	    && !wp->w_s->b_syn_slow
# endif
	    )
    {
	syntax_start(wp, lnum);

	// Start with the fold level at the start of the line.
	level = syn_cur_foldlevel();

	if (wp->w_s->b_syn_foldlevel == SYNFLD_MINIMUM)
	{
	    // Find the lowest fold level that is followed by a higher one.
	    cur_level = level;
	    low_level = cur_level;
	    while (!current_finished)
	    {
		(void)syn_current_attr(FALSE, FALSE, NULL, FALSE);
		cur_level = syn_cur_foldlevel();
		if (cur_level < low_level)
		    low_level = cur_level;
		else if (cur_level > low_level)
		    level = low_level;
		++current_col;
	    }
	}
    }
    if (level > wp->w_p_fdn)
    {
	level = wp->w_p_fdn;
	if (level < 0)
	    level = 0;
    }
    return level;
}
#endif

#if defined(FEAT_PROFILE) || defined(PROTO)
/*
 * ":syntime".
 */
    void
ex_syntime(exarg_T *eap)
{
    if (STRCMP(eap->arg, "on") == 0)
	syn_time_on = TRUE;
    else if (STRCMP(eap->arg, "off") == 0)
	syn_time_on = FALSE;
    else if (STRCMP(eap->arg, "clear") == 0)
	syntime_clear();
    else if (STRCMP(eap->arg, "report") == 0)
	syntime_report();
    else
	semsg(_(e_invalid_argument_str), eap->arg);
}

    static void
syn_clear_time(syn_time_T *st)
{
    profile_zero(&st->total);
    profile_zero(&st->slowest);
    st->count = 0;
    st->match = 0;
}

/*
 * Clear the syntax timing for the current buffer.
 */
    static void
syntime_clear(void)
{
    int		idx;
    synpat_T	*spp;

    if (!syntax_present(curwin))
    {
	msg(_(msg_no_items));
	return;
    }
    for (idx = 0; idx < curwin->w_s->b_syn_patterns.ga_len; ++idx)
    {
	spp = &(SYN_ITEMS(curwin->w_s)[idx]);
	syn_clear_time(&spp->sp_time);
    }
}

/*
 * Function given to ExpandGeneric() to obtain the possible arguments of the
 * ":syntime {on,off,clear,report}" command.
 */
    char_u *
get_syntime_arg(expand_T *xp UNUSED, int idx)
{
    switch (idx)
    {
	case 0: return (char_u *)"on";
	case 1: return (char_u *)"off";
	case 2: return (char_u *)"clear";
	case 3: return (char_u *)"report";
    }
    return NULL;
}

typedef struct
{
    proftime_T	total;
    int		count;
    int		match;
    proftime_T	slowest;
    proftime_T	average;
    int		id;
    char_u	*pattern;
} time_entry_T;

    static int
syn_compare_syntime(const void *v1, const void *v2)
{
    const time_entry_T	*s1 = v1;
    const time_entry_T	*s2 = v2;

    return profile_cmp(&s1->total, &s2->total);
}

/*
 * Clear the syntax timing for the current buffer.
 */
    static void
syntime_report(void)
{
    int		idx;
    synpat_T	*spp;
# if defined(FEAT_RELTIME)
    proftime_T	tm;
# endif
    int		len;
    int		patlen;
    proftime_T	total_total;
    int		total_count = 0;
    garray_T    ga;
    time_entry_T *p;

    if (!syntax_present(curwin))
    {
	msg(_(msg_no_items));
	return;
    }

    ga_init2(&ga, sizeof(time_entry_T), 50);
    profile_zero(&total_total);
    for (idx = 0; idx < curwin->w_s->b_syn_patterns.ga_len; ++idx)
    {
	spp = &(SYN_ITEMS(curwin->w_s)[idx]);
	if (spp->sp_time.count > 0)
	{
	    (void)ga_grow(&ga, 1);
	    p = ((time_entry_T *)ga.ga_data) + ga.ga_len;
	    p->total = spp->sp_time.total;
	    profile_add(&total_total, &spp->sp_time.total);
	    p->count = spp->sp_time.count;
	    p->match = spp->sp_time.match;
	    total_count += spp->sp_time.count;
	    p->slowest = spp->sp_time.slowest;
# if defined(FEAT_RELTIME)
	    profile_divide(&spp->sp_time.total, spp->sp_time.count, &tm);
	    p->average = tm;
# endif
	    p->id = spp->sp_syn.id;
	    p->pattern = spp->sp_pattern;
	    ++ga.ga_len;
	}
    }

    // Sort on total time. Skip if there are no items to avoid passing NULL
    // pointer to qsort().
    if (ga.ga_len > 1)
	qsort(ga.ga_data, (size_t)ga.ga_len, sizeof(time_entry_T),
							 syn_compare_syntime);

    msg_puts_title(_("  TOTAL      COUNT  MATCH   SLOWEST     AVERAGE   NAME               PATTERN"));
    msg_puts("\n");
    for (idx = 0; idx < ga.ga_len && !got_int; ++idx)
    {
	p = ((time_entry_T *)ga.ga_data) + idx;

	msg_puts(profile_msg(&p->total));
	msg_puts(" "); // make sure there is always a separating space
	msg_advance(13);
	msg_outnum(p->count);
	msg_puts(" ");
	msg_advance(20);
	msg_outnum(p->match);
	msg_puts(" ");
	msg_advance(26);
	msg_puts(profile_msg(&p->slowest));
	msg_puts(" ");
	msg_advance(38);
	msg_puts(profile_msg(&p->average));
	msg_puts(" ");
	msg_advance(50);
	msg_outtrans(highlight_group_name(p->id - 1));
	msg_puts(" ");

	msg_advance(69);
	if (Columns < 80)
	    len = 20; // will wrap anyway
	else
	    len = Columns - 70;
	patlen = (int)STRLEN(p->pattern);
	if (len > patlen)
	    len = patlen;
	msg_outtrans_len(p->pattern, len);
	msg_puts("\n");
    }
    ga_clear(&ga);
    if (!got_int)
    {
	msg_puts("\n");
	msg_puts(profile_msg(&total_total));
	msg_advance(13);
	msg_outnum(total_count);
	msg_puts("\n");
    }
}
#endif

#endif // FEAT_SYN_HL
