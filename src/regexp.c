/* vi:set ts=8 sts=4 sw=4:
 *
 * Handling of regular expressions: vim_regcomp(), vim_regexec(), vim_regsub()
 *
 * NOTICE:
 *
 * This is NOT the original regular expression code as written by Henry
 * Spencer.  This code has been modified specifically for use with the VIM
 * editor, and should not be used separately from Vim.  If you want a good
 * regular expression library, get the original code.  The copyright notice
 * that follows is from the original.
 *
 * END NOTICE
 *
 *	Copyright (c) 1986 by University of Toronto.
 *	Written by Henry Spencer.  Not derived from licensed software.
 *
 *	Permission is granted to anyone to use this software for any
 *	purpose on any computer system, and to redistribute it freely,
 *	subject to the following restrictions:
 *
 *	1. The author is not responsible for the consequences of use of
 *		this software, no matter how awful, even if they arise
 *		from defects in it.
 *
 *	2. The origin of this software must not be misrepresented, either
 *		by explicit claim or by omission.
 *
 *	3. Altered versions must be plainly marked as such, and must not
 *		be misrepresented as being the original software.
 *
 * Beware that some of this code is subtly aware of the way operator
 * precedence is structured in regular expressions.  Serious changes in
 * regular-expression syntax might require a total rethink.
 *
 * Changes have been made by Tony Andrews, Olaf 'Rhialto' Seibert, Robert Webb
 * and Bram Moolenaar.
 * Named character class support added by Walter Briscoe (1998 Jul 01)
 */

#include "vim.h"

#undef DEBUG

/*
 * The "internal use only" fields in regexp.h are present to pass info from
 * compile to execute that permits the execute phase to run lots faster on
 * simple cases.  They are:
 *
 * regstart	char that must begin a match; NUL if none obvious; Can be a
 *		multi-byte character.
 * reganch	is the match anchored (at beginning-of-line only)?
 * regmust	string (pointer into program) that match must include, or NULL
 * regmlen	length of regmust string
 * regflags	RF_ values or'ed together
 *
 * Regstart and reganch permit very fast decisions on suitable starting points
 * for a match, cutting down the work a lot.  Regmust permits fast rejection
 * of lines that cannot possibly match.  The regmust tests are costly enough
 * that vim_regcomp() supplies a regmust only if the r.e. contains something
 * potentially expensive (at present, the only such thing detected is * or +
 * at the start of the r.e., which can involve a lot of backup).  Regmlen is
 * supplied because the test in vim_regexec() needs it and vim_regcomp() is
 * computing it anyway.
 */

/*
 * Structure for regexp "program".  This is essentially a linear encoding
 * of a nondeterministic finite-state machine (aka syntax charts or
 * "railroad normal form" in parsing technology).  Each node is an opcode
 * plus a "next" pointer, possibly plus an operand.  "Next" pointers of
 * all nodes except BRANCH and BRACES_COMPLEX implement concatenation; a "next"
 * pointer with a BRANCH on both ends of it is connecting two alternatives.
 * (Here we have one of the subtle syntax dependencies:	an individual BRANCH
 * (as opposed to a collection of them) is never concatenated with anything
 * because of operator precedence).  The "next" pointer of a BRACES_COMPLEX
 * node points to the node after the stuff to be repeated.  The operand of some
 * types of node is a literal string; for others, it is a node leading into a
 * sub-FSM.  In particular, the operand of a BRANCH node is the first node of
 * the branch.	(NB this is *not* a tree structure: the tail of the branch
 * connects to the thing following the set of BRANCHes.)
 *
 * pattern	is coded like:
 *
 *                        +-----------------+
 *                        |                 V
 * <aa>\|<bb>	BRANCH <aa> BRANCH <bb> --> END
 *                   |      ^    |          ^
 *                   +------+    +----------+
 *
 *
 *                     +------------------+
 *                     V                  |
 * <aa>*	BRANCH BRANCH <aa> --> BACK BRANCH --> NOTHING --> END
 *                   |      |               ^                      ^
 *                   |      +---------------+                      |
 *                   +---------------------------------------------+
 *
 *
 *                                      +-------------------------+
 *                                      V                         |
 * <aa>\{}	BRANCH BRACE_LIMITS --> BRACE_COMPLEX <aa> --> BACK  END
 *                   |                              |                ^
 *                   |                              +----------------+
 *                   +-----------------------------------------------+
 *
 *
 * <aa>\@!<bb>	BRANCH NOMATCH <aa> --> END  <bb> --> END
 *                   |       |                ^       ^
 *                   |       +----------------+       |
 *                   +--------------------------------+
 *
 *                                                    +---------+
 *                                                    |         V
 * \z[abc]	BRANCH BRANCH  a  BRANCH  b  BRANCH  c  BRANCH  NOTHING --> END
 *                   |      |          |          |     ^                   ^
 *                   |      |          |          +-----+                   |
 *                   |      |          +----------------+                   |
 *                   |      +---------------------------+                   |
 *                   +------------------------------------------------------+
 *
 * They all start with a BRANCH for "\|" alternaties, even when there is only
 * one alternative.
 */

/*
 * The opcodes are:
 */

/* definition	number		   opnd?    meaning */
#define END		0	/*	End of program or NOMATCH operand. */
#define BOL		1	/*	Match "" at beginning of line. */
#define EOL		2	/*	Match "" at end of line. */
#define BRANCH		3	/* node Match this alternative, or the
				 *	next... */
#define BACK		4	/*	Match "", "next" ptr points backward. */
#define EXACTLY		5	/* str	Match this string. */
#define NOTHING		6	/*	Match empty string. */
#define STAR		7	/* node Match this (simple) thing 0 or more
				 *	times. */
#define PLUS		8	/* node Match this (simple) thing 1 or more
				 *	times. */
#define MATCH		9	/* node match the operand zero-width */
#define NOMATCH		10	/* node check for no match with operand */
#define BEHIND		11	/* node look behind for a match with operand */
#define NOBEHIND	12	/* node look behind for no match with operand */
#define SUBPAT		13	/* node match the operand here */
#define BRACE_SIMPLE	14	/* node Match this (simple) thing between m and
				 *	n times (\{m,n\}). */
#define BOW		15	/*	Match "" after [^a-zA-Z0-9_] */
#define EOW		16	/*	Match "" at    [^a-zA-Z0-9_] */
#define BRACE_LIMITS	17	/* nr nr  define the min & max for BRACE_SIMPLE
				 *	and BRACE_COMPLEX. */
#define NEWL		18	/*	Match line-break */


/* character classes: 20-48 normal, 50-78 include a line-break */
#define ADD_NL		30
#define FIRST_NL	ANY + ADD_NL
#define ANY		20	/*	Match any one character. */
#define ANYOF		21	/* str	Match any character in this string. */
#define ANYBUT		22	/* str	Match any character not in this
				 *	string. */
#define IDENT		23	/*	Match identifier char */
#define SIDENT		24	/*	Match identifier char but no digit */
#define KWORD		25	/*	Match keyword char */
#define SKWORD		26	/*	Match word char but no digit */
#define FNAME		27	/*	Match file name char */
#define SFNAME		28	/*	Match file name char but no digit */
#define PRINT		29	/*	Match printable char */
#define SPRINT		30	/*	Match printable char but no digit */
#define WHITE		31	/*	Match whitespace char */
#define NWHITE		32	/*	Match non-whitespace char */
#define DIGIT		33	/*	Match digit char */
#define NDIGIT		34	/*	Match non-digit char */
#define HEX		35	/*	Match hex char */
#define NHEX		36	/*	Match non-hex char */
#define OCTAL		37	/*	Match octal char */
#define NOCTAL		38	/*	Match non-octal char */
#define WORD		39	/*	Match word char */
#define NWORD		40	/*	Match non-word char */
#define HEAD		41	/*	Match head char */
#define NHEAD		42	/*	Match non-head char */
#define ALPHA		43	/*	Match alpha char */
#define NALPHA		44	/*	Match non-alpha char */
#define LOWER		45	/*	Match lowercase char */
#define NLOWER		46	/*	Match non-lowercase char */
#define UPPER		47	/*	Match uppercase char */
#define NUPPER		48	/*	Match non-uppercase char */
#define LAST_NL		NUPPER + ADD_NL
#define WITH_NL(op)	((op) >= FIRST_NL && (op) <= LAST_NL)

#define MOPEN		80  /* -89	 Mark this point in input as start of
				 *	 \( subexpr.  MOPEN + 0 marks start of
				 *	 match. */
#define MCLOSE		90  /* -99	 Analogous to MOPEN.  MCLOSE + 0 marks
				 *	 end of match. */
#define BACKREF		100 /* -109 node Match same string again \1-\9 */

#ifdef FEAT_SYN_HL
# define ZOPEN		110 /* -119	 Mark this point in input as start of
				 *	 \z( subexpr. */
# define ZCLOSE		120 /* -129	 Analogous to ZOPEN. */
# define ZREF		130 /* -139 node Match external submatch \z1-\z9 */
#endif

#define BRACE_COMPLEX	140 /* -149 node Match nodes between m & n times */

#define NOPEN		150	/*	Mark this point in input as start of
					\%( subexpr. */
#define NCLOSE		151	/*	Analogous to NOPEN. */

#define MULTIBYTECODE	200	/* mbc	Match one multi-byte character */
#define RE_BOF		201	/*	Match "" at beginning of file. */
#define RE_EOF		202	/*	Match "" at end of file. */
#define CURSOR		203	/*	Match location of cursor. */

#define RE_LNUM		204	/* nr cmp  Match line number */
#define RE_COL		205	/* nr cmp  Match column number */
#define RE_VCOL		206	/* nr cmp  Match virtual column number */

/*
 * Magic characters have a special meaning, they don't match literally.
 * Magic characters are negative.  This separates them from literal characters
 * (possibly multi-byte).  Only ASCII characters can be Magic.
 */
#define Magic(x)	((int)(x) - 256)
#define un_Magic(x)	((x) + 256)
#define is_Magic(x)	((x) < 0)

static int no_Magic __ARGS((int x));
static int toggle_Magic __ARGS((int x));

    static int
no_Magic(x)
    int		x;
{
    if (is_Magic(x))
	return un_Magic(x);
    return x;
}

    static int
toggle_Magic(x)
    int		x;
{
    if (is_Magic(x))
	return un_Magic(x);
    return Magic(x);
}

/*
 * The first byte of the regexp internal "program" is actually this magic
 * number; the start node begins in the second byte.  It's used to catch the
 * most severe mutilation of the program by the caller.
 */

#define REGMAGIC	0234

/*
 * Opcode notes:
 *
 * BRANCH	The set of branches constituting a single choice are hooked
 *		together with their "next" pointers, since precedence prevents
 *		anything being concatenated to any individual branch.  The
 *		"next" pointer of the last BRANCH in a choice points to the
 *		thing following the whole choice.  This is also where the
 *		final "next" pointer of each individual branch points; each
 *		branch starts with the operand node of a BRANCH node.
 *
 * BACK		Normal "next" pointers all implicitly point forward; BACK
 *		exists to make loop structures possible.
 *
 * STAR,PLUS	'=', and complex '*' and '+', are implemented as circular
 *		BRANCH structures using BACK.  Simple cases (one character
 *		per match) are implemented with STAR and PLUS for speed
 *		and to minimize recursive plunges.
 *		Note: We would like to use "\?" instead of "\=", but a "\?"
 *		can be part of a pattern to escape the special meaning of '?'
 *		at the end of the pattern in "?pattern?e".
 *
 * BRACE_LIMITS	This is always followed by a BRACE_SIMPLE or BRACE_COMPLEX
 *		node, and defines the min and max limits to be used for that
 *		node.
 *
 * MOPEN,MCLOSE	...are numbered at compile time.
 * ZOPEN,ZCLOSE	...ditto
 */

/*
 * A node is one char of opcode followed by two chars of "next" pointer.
 * "Next" pointers are stored as two 8-bit bytes, high order first.  The
 * value is a positive offset from the opcode of the node containing it.
 * An operand, if any, simply follows the node.  (Note that much of the
 * code generation knows about this implicit relationship.)
 *
 * Using two bytes for the "next" pointer is vast overkill for most things,
 * but allows patterns to get big without disasters.
 */
#define OP(p)		((int)*(p))
#define NEXT(p)		(((*((p) + 1) & 0377) << 8) + (*((p) + 2) & 0377))
#define OPERAND(p)	((p) + 3)
/* Obtain an operand that was stored as four bytes, MSB first. */
#define OPERAND_MIN(p)	(((long)(p)[3] << 24) + ((long)(p)[4] << 16) \
			+ ((long)(p)[5] << 8) + (long)(p)[6])
/* Obtain a second operand stored as four bytes. */
#define OPERAND_MAX(p)	OPERAND_MIN((p) + 4)
/* Obtain a second single-byte operand stored after a four bytes operand. */
#define OPERAND_CMP(p)	(p)[7]

/*
 * Utility definitions.
 */
#define UCHARAT(p)	((int)*(char_u *)(p))

/* Used for an error (down from) vim_regcomp(): give the error message, set
 * rc_did_emsg and return NULL */
#define EMSG_RET_NULL(m) { EMSG(_(m)); rc_did_emsg = TRUE; return NULL; }
#define EMSG_M_RET_NULL(m, c) { EMSG2(_(m), c ? "" : "\\"); rc_did_emsg = TRUE; return NULL; }
#define EMSG_RET_FAIL(m) { EMSG(_(m)); rc_did_emsg = TRUE; return FAIL; }

#define MAX_LIMIT	(32767L << 16L)

static int re_multi_type __ARGS((int));
static int cstrncmp __ARGS((char_u *s1, char_u *s2, int n));
static char_u *cstrchr __ARGS((char_u *, int));

#ifdef DEBUG
static void	regdump __ARGS((char_u *, regprog_T *));
static char_u	*regprop __ARGS((char_u *));
#endif

#define NOT_MULTI	0
#define MULTI_ONE	1
#define MULTI_MULT	2
/*
 * Return NOT_MULTI if c is not a "multi" operator.
 * Return MULTI_ONE if c is a single "multi" operator.
 * Return MULTI_MULT if c is a multi "multi" operator.
 */
    static int
re_multi_type(c)
    int c;
{
    if (c == Magic('@') || c == Magic('=') || c == Magic('?'))
	return MULTI_ONE;
    if (c == Magic('*') || c == Magic('+') || c == Magic('{'))
	return MULTI_MULT;
    return NOT_MULTI;
}

/*
 * Flags to be passed up and down.
 */
#define HASWIDTH	0x1	/* Known never to match null string. */
#define SIMPLE		0x2	/* Simple enough to be STAR/PLUS operand. */
#define SPSTART		0x4	/* Starts with * or +. */
#define HASNL		0x8	/* Contains some \n. */
#define WORST		0	/* Worst case. */

/*
 * When regcode is set to this value, code is not emitted and size is computed
 * instead.
 */
#define JUST_CALC_SIZE	((char_u *) -1)

static char_u		*reg_prev_sub;

/*
 * REGEXP_INRANGE contains all characters which are always special in a []
 * range after '\'.
 * REGEXP_ABBR contains all characters which act as abbreviations after '\'.
 * These are:
 *  \n	- New line (NL).
 *  \r	- Carriage Return (CR).
 *  \t	- Tab (TAB).
 *  \e	- Escape (ESC).
 *  \b	- Backspace (Ctrl_H).
 */
static char_u REGEXP_INRANGE[] = "]^-n\\";
static char_u REGEXP_ABBR[] = "nrteb";

static int	backslash_trans __ARGS((int c));
static int	skip_class_name __ARGS((char_u **pp));
static char_u	*skip_anyof __ARGS((char_u *p));
static void	init_class_tab __ARGS((void));

/*
 * Translate '\x' to its control character, except "\n", which is Magic.
 */
    static int
backslash_trans(c)
    int		c;
{
    switch (c)
    {
	case 'r':   return CR;
	case 't':   return TAB;
	case 'e':   return ESC;
	case 'b':   return BS;
    }
    return c;
}

/*
 * Check for a character class name.  "pp" points to the '['.
 * Returns one of the CLASS_ items. CLASS_NONE means that no item was
 * recognized.  Otherwise "pp" is advanced to after the item.
 */
    static int
skip_class_name(pp)
    char_u	**pp;
{
    static const char *(class_names[]) =
    {
	"alnum:]",
#define CLASS_ALNUM 0
	"alpha:]",
#define CLASS_ALPHA 1
	"blank:]",
#define CLASS_BLANK 2
	"cntrl:]",
#define CLASS_CNTRL 3
	"digit:]",
#define CLASS_DIGIT 4
	"graph:]",
#define CLASS_GRAPH 5
	"lower:]",
#define CLASS_LOWER 6
	"print:]",
#define CLASS_PRINT 7
	"punct:]",
#define CLASS_PUNCT 8
	"space:]",
#define CLASS_SPACE 9
	"upper:]",
#define CLASS_UPPER 10
	"xdigit:]",
#define CLASS_XDIGIT 11
	"tab:]",
#define CLASS_TAB 12
	"return:]",
#define CLASS_RETURN 13
	"backspace:]",
#define CLASS_BACKSPACE 14
	"escape:]",
#define CLASS_ESCAPE 15
    };
#define CLASS_NONE 99
    int i;

    if ((*pp)[1] == ':')
    {
	for (i = 0; i < sizeof(class_names) / sizeof(*class_names); ++i)
	    if (STRNCMP(*pp + 2, class_names[i], STRLEN(class_names[i])) == 0)
	    {
		*pp += STRLEN(class_names[i]) + 2;
		return i;
	    }
    }
    return CLASS_NONE;
}

/*
 * Skip over a "[]" range.
 * "p" must point to the character after the '['.
 * The returned pointer is on the matching ']', or the terminating NUL.
 */
    static char_u *
skip_anyof(p)
    char_u	*p;
{
    int		cpo_lit;	/* 'cpoptions' contains 'l' flag */
#ifdef FEAT_MBYTE
    int		l;
#endif

    cpo_lit = (!reg_syn && vim_strchr(p_cpo, CPO_LITERAL) != NULL);

    if (*p == '^')	/* Complement of range. */
	++p;
    if (*p == ']' || *p == '-')
	++p;
    while (*p != NUL && *p != ']')
    {
#ifdef FEAT_MBYTE
	if (has_mbyte && (l = (*mb_ptr2len_check)(p)) > 1)
	    p += l;
	else
#endif
	    if (*p == '-')
	    {
		++p;
		if (*p != ']' && *p != NUL)
		{
#ifdef FEAT_MBYTE
		    if (has_mbyte)
			p += (*mb_ptr2len_check)(p);
		    else
#endif
			++p;
		}
	    }
	else if (*p == '\\'
		&& (vim_strchr(REGEXP_INRANGE, p[1]) != NULL
		    || (!cpo_lit && vim_strchr(REGEXP_ABBR, p[1]) != NULL)))
	    p += 2;
	else if (*p == '[')
	{
	    if (skip_class_name(&p) == CLASS_NONE)
		++p; /* It was not a class name */
	}
	else
	    ++p;
    }

    return p;
}

/*
 * Specific version of character class functions.
 * Using a table to keep this fast.
 */
static short	class_tab[256];

#define	    RI_DIGIT	0x01
#define	    RI_HEX	0x02
#define	    RI_OCTAL	0x04
#define	    RI_WORD	0x08
#define	    RI_HEAD	0x10
#define	    RI_ALPHA	0x20
#define	    RI_LOWER	0x40
#define	    RI_UPPER	0x80
#define	    RI_WHITE	0x100

    static void
init_class_tab()
{
    int		i;
    static int	done = FALSE;

    if (done)
	return;

    for (i = 0; i < 256; ++i)
    {
	if (i >= '0' && i <= '7')
	    class_tab[i] = RI_DIGIT + RI_HEX + RI_OCTAL + RI_WORD;
	else if (i >= '8' && i <= '9')
	    class_tab[i] = RI_DIGIT + RI_HEX + RI_WORD;
	else if (i >= 'a' && i <= 'f')
	    class_tab[i] = RI_HEX + RI_WORD + RI_HEAD + RI_ALPHA + RI_LOWER;
#ifdef EBCDIC
	else if ((i >= 'g' && i <= 'i') || (i >= 'j' && i <= 'r')
						    || (i >= 's' && i <= 'z'))
#else
	else if (i >= 'g' && i <= 'z')
#endif
	    class_tab[i] = RI_WORD + RI_HEAD + RI_ALPHA + RI_LOWER;
	else if (i >= 'A' && i <= 'F')
	    class_tab[i] = RI_HEX + RI_WORD + RI_HEAD + RI_ALPHA + RI_UPPER;
#ifdef EBCDIC
	else if ((i >= 'G' && i <= 'I') || ( i >= 'J' && i <= 'R')
						    || (i >= 'S' && i <= 'Z'))
#else
	else if (i >= 'G' && i <= 'Z')
#endif
	    class_tab[i] = RI_WORD + RI_HEAD + RI_ALPHA + RI_UPPER;
	else if (i == '_')
	    class_tab[i] = RI_WORD + RI_HEAD;
	else
	    class_tab[i] = 0;
    }
    class_tab[' '] |= RI_WHITE;
    class_tab['\t'] |= RI_WHITE;
    done = TRUE;
}

#ifdef FEAT_MBYTE
# define ri_digit(c)	(c < 0x100 && (class_tab[c] & RI_DIGIT))
# define ri_hex(c)	(c < 0x100 && (class_tab[c] & RI_HEX))
# define ri_octal(c)	(c < 0x100 && (class_tab[c] & RI_OCTAL))
# define ri_word(c)	(c < 0x100 && (class_tab[c] & RI_WORD))
# define ri_head(c)	(c < 0x100 && (class_tab[c] & RI_HEAD))
# define ri_alpha(c)	(c < 0x100 && (class_tab[c] & RI_ALPHA))
# define ri_lower(c)	(c < 0x100 && (class_tab[c] & RI_LOWER))
# define ri_upper(c)	(c < 0x100 && (class_tab[c] & RI_UPPER))
# define ri_white(c)	(c < 0x100 && (class_tab[c] & RI_WHITE))
#else
# define ri_digit(c)	(class_tab[c] & RI_DIGIT)
# define ri_hex(c)	(class_tab[c] & RI_HEX)
# define ri_octal(c)	(class_tab[c] & RI_OCTAL)
# define ri_word(c)	(class_tab[c] & RI_WORD)
# define ri_head(c)	(class_tab[c] & RI_HEAD)
# define ri_alpha(c)	(class_tab[c] & RI_ALPHA)
# define ri_lower(c)	(class_tab[c] & RI_LOWER)
# define ri_upper(c)	(class_tab[c] & RI_UPPER)
# define ri_white(c)	(class_tab[c] & RI_WHITE)
#endif

/* flags for regflags */
#define RF_ICASE    1	/* ignore case */
#define RF_NOICASE  2	/* don't ignore case */
#define RF_HASNL    4	/* can match a NL */

/*
 * Global work variables for vim_regcomp().
 */

static char_u	*regparse;	/* Input-scan pointer. */
static int	prevchr_len;	/* byte length of previous char */
static int	num_complex_braces; /* Complex \{...} count */
static int	regnpar;	/* () count. */
#ifdef FEAT_SYN_HL
static int	regnzpar;	/* \z() count. */
static int	re_has_z;	/* \z item detected */
#endif
static char_u	*regcode;	/* Code-emit pointer, or JUST_CALC_SIZE */
static long	regsize;	/* Code size. */
static char_u	had_endbrace[NSUBEXP];	/* flags, TRUE if end of () found */
static unsigned	regflags;	/* RF_ flags for prog */
static long	brace_min[10];	/* Minimums for complex brace repeats */
static long	brace_max[10];	/* Maximums for complex brace repeats */
static int	brace_count[10]; /* Current counts for complex brace repeats */
#if defined(FEAT_SYN_HL) || defined(PROTO)
static int	had_eol;	/* TRUE when EOL found by vim_regcomp() */
#endif
static int	one_exactly = FALSE;	/* only do one char for EXACTLY */

static int	reg_magic;	/* magicness of the pattern: */
#define MAGIC_NONE	1	/* "\V" very unmagic */
#define MAGIC_OFF	2	/* "\M" or 'magic' off */
#define MAGIC_ON	3	/* "\m" or 'magic' */
#define MAGIC_ALL	4	/* "\v" very magic */

/*
 * META contains all characters that may be magic, except '^' and '$'.
 */

static char_u META[] = "%&()*+.123456789<=>@ACDFHIKLMOPSUVWX[_acdfhiklmnopsuvwxz{|~";

static int	curchr;

/* arguments for reg() */
#define REG_NOPAREN	0	/* toplevel reg() */
#define REG_PAREN	1	/* \(\) */
#define REG_ZPAREN	2	/* \z(\) */
#define REG_NPAREN	3	/* \%(\) */

/*
 * Forward declarations for vim_regcomp()'s friends.
 */
static void	initchr __ARGS((char_u *));
static int	getchr __ARGS((void));
static void	skipchr_keepstart __ARGS((void));
static int	peekchr __ARGS((void));
static void	skipchr __ARGS((void));
static void	ungetchr __ARGS((void));
static void	regcomp_start __ARGS((char_u *expr, int magic));
static char_u	*reg __ARGS((int, int *));
static char_u	*regbranch __ARGS((int *flagp));
static char_u	*regconcat __ARGS((int *flagp));
static char_u	*regpiece __ARGS((int *));
static char_u	*regatom __ARGS((int *));
static char_u	*regnode __ARGS((int));
static int	prog_magic_wrong __ARGS((void));
static char_u	*regnext __ARGS((char_u *));
static void	regc __ARGS((int b));
#ifdef FEAT_MBYTE
static void	regmbc __ARGS((int c));
#endif
static void	reginsert __ARGS((int, char_u *));
static void	reginsert_limits __ARGS((int, long, long, char_u *));
static char_u	*re_put_long __ARGS((char_u *pr, long_u val));
static int	read_limits __ARGS((long *, long *));
static void	regtail __ARGS((char_u *, char_u *));
static void	regoptail __ARGS((char_u *, char_u *));

#if defined(FEAT_SEARCH_EXTRA) || defined(PROTO)
/*
 * Return TRUE if compiled regular expression "prog" can match a line break.
 */
    int
re_multiline(prog)
    regprog_T *prog;
{
    return (prog->regflags & RF_HASNL);
}
#endif

/*
 * Skip past regular expression.
 * Stop at end of 'p' of where 'dirc' is found ('/', '?', etc).
 * Take care of characters with a backslash in front of it.
 * Skip strings inside [ and ].
 */
    char_u *
skip_regexp(p, dirc, magic)
    char_u	*p;
    int		dirc;
    int		magic;
{
    int		mymagic;

    if (magic)
	mymagic = MAGIC_ON;
    else
	mymagic = MAGIC_OFF;

    for (; p[0] != NUL; ++p)
    {
	if (p[0] == dirc)	/* found end of regexp */
	    break;
	if ((p[0] == '[' && mymagic >= MAGIC_ON)
		|| (p[0] == '\\' && p[1] == '[' && mymagic <= MAGIC_OFF))
	{
	    p = skip_anyof(p + 1);
	    if (p[0] == NUL)
		break;
	}
	else if (p[0] == '\\' && p[1] != NUL)
	{
	    ++p;    /* skip next character */
	    if (*p == 'v')
		mymagic = MAGIC_ALL;
	    else if (*p == 'V')
		mymagic = MAGIC_NONE;
	}
#ifdef FEAT_MBYTE
	else if (has_mbyte)
	    p += (*mb_ptr2len_check)(p) - 1;
#endif
    }
    return p;
}

/*
 * vim_regcomp - compile a regular expression into internal code
 *
 * We can't allocate space until we know how big the compiled form will be,
 * but we can't compile it (and thus know how big it is) until we've got a
 * place to put the code.  So we cheat:  we compile it twice, once with code
 * generation turned off and size counting turned on, and once "for real".
 * This also means that we don't allocate space until we are sure that the
 * thing really will compile successfully, and we never have to move the
 * code and thus invalidate pointers into it.  (Note that it has to be in
 * one piece because vim_free() must be able to free it all.)
 *
 * Whether upper/lower case is to be ignored is decided when executing the
 * program, it does not matter here.
 *
 * Beware that the optimization-preparation code in here knows about some
 * of the structure of the compiled regexp.
 */
    regprog_T *
vim_regcomp(expr, magic)
    char_u	*expr;
    int		magic;
{
    regprog_T	*r;
    char_u	*scan;
    char_u	*longest;
    int		len;
    int		flags;

    if (expr == NULL)
	EMSG_RET_NULL(e_null);

    init_class_tab();

    /*
     * First pass: determine size, legality.
     */
    regcomp_start(expr, magic);
    regcode = JUST_CALC_SIZE;
    regc(REGMAGIC);
    if (reg(REG_NOPAREN, &flags) == NULL)
	return NULL;

    /* Small enough for pointer-storage convention? */
#ifdef SMALL_MALLOC		/* 16 bit storage allocation */
    if (regsize >= 65536L - 256L)
	EMSG_RET_NULL(N_("(ep5) Pattern too long"));
#endif

    /* Allocate space. */
    r = (regprog_T *)lalloc(sizeof(regprog_T) + regsize, TRUE);
    if (r == NULL)
	return NULL;

    /*
     * Second pass: emit code.
     */
    regcomp_start(expr, magic);
    regcode = r->program;
    regc(REGMAGIC);
    if (reg(REG_NOPAREN, &flags) == NULL)
    {
	vim_free(r);
	return NULL;
    }

    /* Dig out information for optimizations. */
    r->regstart = NUL;		/* Worst-case defaults. */
    r->reganch = 0;
    r->regmust = NULL;
    r->regmlen = 0;
    r->regflags = regflags;
    if (flags & HASNL)
	r->regflags |= RF_HASNL;
#ifdef FEAT_SYN_HL
    /* Remember whether this pattern has any \z specials in it. */
    r->reghasz = re_has_z;
#endif
    scan = r->program + 1;	/* First BRANCH. */
    if (OP(regnext(scan)) == END)   /* Only one top-level choice. */
    {
	scan = OPERAND(scan);

	/* Starting-point info. */
	if (OP(scan) == BOL || OP(scan) == RE_BOF)
	{
	    r->reganch++;
	    scan = regnext(scan);
	}

	if (OP(scan) == EXACTLY)
	{
#ifdef FEAT_MBYTE
	    if (has_mbyte)
		r->regstart = (*mb_ptr2char)(OPERAND(scan));
	    else
#endif
		r->regstart = *OPERAND(scan);
	}
	else if ((OP(scan) == BOW
		    || OP(scan) == EOW
		    || OP(scan) == NOTHING
		    || OP(scan) == MOPEN + 0 || OP(scan) == NOPEN
		    || OP(scan) == MCLOSE + 0 || OP(scan) == NCLOSE)
		 && OP(regnext(scan)) == EXACTLY)
	{
#ifdef FEAT_MBYTE
	    if (has_mbyte)
		r->regstart = (*mb_ptr2char)(OPERAND(regnext(scan)));
	    else
#endif
		r->regstart = *OPERAND(regnext(scan));
	}

	/*
	 * If there's something expensive in the r.e., find the longest
	 * literal string that must appear and make it the regmust.  Resolve
	 * ties in favor of later strings, since the regstart check works
	 * with the beginning of the r.e. and avoiding duplication
	 * strengthens checking.  Not a strong reason, but sufficient in the
	 * absence of others.
	 */
	/*
	 * When the r.e. starts with BOW, it is faster to look for a regmust
	 * first. Used a lot for "#" and "*" commands. (Added by mool).
	 */
	if ((flags & SPSTART || OP(scan) == BOW || OP(scan) == EOW)
							  && !(flags & HASNL))
	{
	    longest = NULL;
	    len = 0;
	    for (; scan != NULL; scan = regnext(scan))
		if (OP(scan) == EXACTLY && STRLEN(OPERAND(scan)) >= (size_t)len)
		{
		    longest = OPERAND(scan);
		    len = STRLEN(OPERAND(scan));
		}
	    r->regmust = longest;
	    r->regmlen = len;
	}
    }
#ifdef DEBUG
    regdump(expr, r);
#endif
    return r;
}

/*
 * Setup to parse the regexp.  Used once to get the length and once to do it.
 */
    static void
regcomp_start(expr, magic)
    char_u	*expr;
    int		magic;
{
    initchr(expr);
    if (magic)
	reg_magic = MAGIC_ON;
    else
	reg_magic = MAGIC_OFF;
    num_complex_braces = 0;
    regnpar = 1;
    vim_memset(had_endbrace, 0, sizeof(had_endbrace));
#ifdef FEAT_SYN_HL
    regnzpar = 1;
    re_has_z = 0;
#endif
    regsize = 0L;
    regflags = 0;
#if defined(FEAT_SYN_HL) || defined(PROTO)
    had_eol = FALSE;
#endif
}

#if defined(FEAT_SYN_HL) || defined(PROTO)
/*
 * Check if during the previous call to vim_regcomp the EOL item "$" has been
 * found.  This is messy, but it works fine.
 */
    int
vim_regcomp_had_eol()
{
    return had_eol;
}
#endif

/*
 * reg - regular expression, i.e. main body or parenthesized thing
 *
 * Caller must absorb opening parenthesis.
 *
 * Combining parenthesis handling with the base level of regular expression
 * is a trifle forced, but the need to tie the tails of the branches to what
 * follows makes it hard to avoid.
 */
    static char_u *
reg(paren, flagp)
    int		paren;	/* REG_NOPAREN, REG_PAREN, REG_NPAREN or REG_ZPAREN */
    int		*flagp;
{
    char_u	*ret;
    char_u	*br;
    char_u	*ender;
    int		parno = 0;
    int		flags;

    *flagp = HASWIDTH;		/* Tentatively. */

#ifdef FEAT_SYN_HL
    if (paren == REG_ZPAREN)
    {
	/* Make a ZOPEN node. */
	if (regnzpar >= NSUBEXP)
	    EMSG_RET_NULL("E50: Too many \\z(");
	parno = regnzpar;
	regnzpar++;
	ret = regnode(ZOPEN + parno);
    }
    else
#endif
	if (paren == REG_PAREN)
    {
	/* Make a MOPEN node. */
	if (regnpar >= NSUBEXP)
	    EMSG_M_RET_NULL("E51: Too many %s(", reg_magic == MAGIC_ALL);
	parno = regnpar;
	++regnpar;
	ret = regnode(MOPEN + parno);
    }
    else if (paren == REG_NPAREN)
    {
	/* Make a NOPEN node. */
	ret = regnode(NOPEN);
    }
    else
	ret = NULL;

    /* Pick up the branches, linking them together. */
    br = regbranch(&flags);
    if (br == NULL)
	return NULL;
    if (ret != NULL)
	regtail(ret, br);	/* [MZ]OPEN -> first. */
    else
	ret = br;
    /* If one of the branches can be zero-width, the whole thing can.
     * If one of the branches has * at start or matches a line-break, the
     * whole thing can. */
    if (!(flags & HASWIDTH))
	*flagp &= ~HASWIDTH;
    *flagp |= flags & (SPSTART | HASNL);
    while (peekchr() == Magic('|'))
    {
	skipchr();
	br = regbranch(&flags);
	if (br == NULL)
	    return NULL;
	regtail(ret, br);	/* BRANCH -> BRANCH. */
	if (!(flags & HASWIDTH))
	    *flagp &= ~HASWIDTH;
	*flagp |= flags & (SPSTART | HASNL);
    }

    /* Make a closing node, and hook it on the end. */
    ender = regnode(
#ifdef FEAT_SYN_HL
	    paren == REG_ZPAREN ? ZCLOSE + parno :
#endif
	    paren == REG_PAREN ? MCLOSE + parno :
	    paren == REG_NPAREN ? NCLOSE : END);
    regtail(ret, ender);

    /* Hook the tails of the branches to the closing node. */
    for (br = ret; br != NULL; br = regnext(br))
	regoptail(br, ender);

    /* Check for proper termination. */
    if (paren != REG_NOPAREN && getchr() != Magic(')'))
    {
#ifdef FEAT_SYN_HL
	if (paren == REG_ZPAREN)
	    EMSG_RET_NULL("E52: Unmatched \\z(")
	else
#endif
	    if (paren == REG_NPAREN)
	    EMSG_M_RET_NULL("E53: Unmatched %s%%(", reg_magic == MAGIC_ALL)
	else
	    EMSG_M_RET_NULL("E54: Unmatched %s(", reg_magic == MAGIC_ALL)
    }
    else if (paren == REG_NOPAREN && peekchr() != NUL)
    {
	if (curchr == Magic(')'))
	    EMSG_M_RET_NULL("E55: Unmatched %s)", reg_magic == MAGIC_ALL)
	else
	    EMSG_RET_NULL(e_trailing)	/* "Can't happen". */
	/* NOTREACHED */
    }
    /*
     * Here we set the flag allowing back references to this set of
     * parentheses.
     */
    if (paren == REG_PAREN)
	had_endbrace[parno] = TRUE;	/* have seen the close paren */
    return ret;
}

/*
 * regbranch - one alternative of an | operator
 *
 * Implements the & operator.
 */
    static char_u *
regbranch(flagp)
    int		*flagp;
{
    char_u	*ret;
    char_u	*chain = NULL;
    char_u	*latest;
    int		flags;

    *flagp = WORST | HASNL;		/* Tentatively. */

    ret = regnode(BRANCH);
    for (;;)
    {
	latest = regconcat(&flags);
	if (latest == NULL)
	    return NULL;
	/* If one of the branches has width, the whole thing has.  If one of
	 * the branches anchors at start-of-line, the whole thing does. */
	*flagp |= flags & (HASWIDTH | SPSTART);
	/* If one of the branches doesn't match a line-break, the whole thing
	 * doesn't. */
	*flagp &= ~HASNL | (flags & HASNL);
	if (chain != NULL)
	    regtail(chain, latest);
	if (peekchr() != Magic('&'))
	    break;
	skipchr();
	regtail(latest, regnode(END)); /* operand ends */
	reginsert(MATCH, latest);
	chain = latest;
    }

    return ret;
}

/*
 * regbranch - one alternative of an | or & operator
 *
 * Implements the concatenation operator.
 */
    static char_u *
regconcat(flagp)
    int		*flagp;
{
    char_u	*first = NULL;
    char_u	*chain = NULL;
    char_u	*latest;
    int		flags;
    int		cont = TRUE;

    *flagp = WORST;		/* Tentatively. */

    while (cont)
    {
	switch (peekchr())
	{
	    case NUL:
	    case Magic('|'):
	    case Magic('&'):
	    case Magic(')'):
			    cont = FALSE;
			    break;
	    case Magic('c'):
			    regflags |= RF_ICASE;
			    skipchr_keepstart();
			    break;
	    case Magic('C'):
			    regflags |= RF_NOICASE;
			    skipchr_keepstart();
			    break;
	    case Magic('v'):
			    reg_magic = MAGIC_ALL;
			    skipchr_keepstart();
			    break;
	    case Magic('m'):
			    reg_magic = MAGIC_ON;
			    skipchr_keepstart();
			    break;
	    case Magic('M'):
			    reg_magic = MAGIC_OFF;
			    skipchr_keepstart();
			    break;
	    case Magic('V'):
			    reg_magic = MAGIC_NONE;
			    skipchr_keepstart();
			    break;
	    default:
			    latest = regpiece(&flags);
			    if (latest == NULL)
				return NULL;
			    *flagp |= flags & (HASWIDTH | HASNL);
			    if (chain == NULL)	/* First piece. */
				*flagp |= flags & SPSTART;
			    else
				regtail(chain, latest);
			    chain = latest;
			    if (first == NULL)
				first = latest;
			    break;
	}
    }
    if (first == NULL)		/* Loop ran zero times. */
	first = regnode(NOTHING);
    return first;
}

/*
 * regpiece - something followed by possible [*+=]
 *
 * Note that the branching code sequences used for = and the general cases
 * of * and + are somewhat optimized:  they use the same NOTHING node as
 * both the endmarker for their branch list and the body of the last branch.
 * It might seem that this node could be dispensed with entirely, but the
 * endmarker role is not redundant.
 */
    static char_u *
regpiece(flagp)
    int		    *flagp;
{
    char_u	    *ret;
    int		    op;
    char_u	    *next;
    int		    flags;
    long	    minval;
    long	    maxval;

    ret = regatom(&flags);
    if (ret == NULL)
	return NULL;

    op = peekchr();
    if (re_multi_type(op) == NOT_MULTI)
    {
	*flagp = flags;
	return ret;
    }
    if (!(flags & HASWIDTH) && re_multi_type(op) == MULTI_MULT)
    {
	if (op == Magic('*'))
	    EMSG_M_RET_NULL("E56: %s* operand could be empty",
						       reg_magic >= MAGIC_ON);
	if (op == Magic('+'))
	    EMSG_M_RET_NULL("E57: %s+ operand could be empty",
						       reg_magic == MAGIC_ALL);
	EMSG_M_RET_NULL("E58: %s{ operand could be empty",
						      reg_magic == MAGIC_ALL);
    }
    *flagp = (WORST | SPSTART | (flags & HASNL));	/* default flags */

    skipchr();
    switch (op)
    {
	case Magic('*'):
	    if (flags & SIMPLE)
		reginsert(STAR, ret);
	    else
	    {
		/* Emit x* as (x&|), where & means "self". */
		reginsert(BRANCH, ret); /* Either x */
		regoptail(ret, regnode(BACK));	/* and loop */
		regoptail(ret, ret);	/* back */
		regtail(ret, regnode(BRANCH));	/* or */
		regtail(ret, regnode(NOTHING)); /* null. */
	    }
	    break;

	case Magic('+'):
	    if (flags & SIMPLE)
		reginsert(PLUS, ret);
	    else
	    {
		/* Emit x+ as x(&|), where & means "self". */
		next = regnode(BRANCH); /* Either */
		regtail(ret, next);
		regtail(regnode(BACK), ret);	/* loop back */
		regtail(next, regnode(BRANCH)); /* or */
		regtail(ret, regnode(NOTHING)); /* null. */
	    }
	    *flagp = (WORST | HASWIDTH | (flags & HASNL));
	    break;

	case Magic('@'):
	    {
		int	op = END;

		switch (no_Magic(getchr()))
		{
		    case '=': op = MATCH; break;		  /* \@= */
		    case '!': op = NOMATCH; break;		  /* \@! */
		    case '>': op = SUBPAT; break;		  /* \@> */
		    case '<': switch (no_Magic(getchr()))
			      {
				  case '=': op = BEHIND; break;	  /* \@<= */
				  case '!': op = NOBEHIND; break; /* \@<! */
			      }
		}
		if (op == END)
		    EMSG_M_RET_NULL("E59: invalid character after %s@",
						      reg_magic == MAGIC_ALL);
		regtail(ret, regnode(END)); /* operand ends */
		reginsert(op, ret);
		break;
	    }

	case Magic('?'):
	case Magic('='):
	    /* Emit x= as (x|) */
	    reginsert(BRANCH, ret); /* Either x */
	    regtail(ret, regnode(BRANCH));	/* or */
	    next = regnode(NOTHING);/* null. */
	    regtail(ret, next);
	    regoptail(ret, next);
	    break;

	case Magic('{'):
	    if (!read_limits(&minval, &maxval))
		return NULL;
	    if (flags & SIMPLE)
	    {
		reginsert(BRACE_SIMPLE, ret);
		reginsert_limits(BRACE_LIMITS, minval, maxval, ret);
	    }
	    else
	    {
		if (num_complex_braces >= 10)
		    EMSG_M_RET_NULL("E60: Too many complex %s{...}s",
						      reg_magic == MAGIC_ALL);
		reginsert(BRACE_COMPLEX + num_complex_braces, ret);
		regoptail(ret, regnode(BACK));
		regoptail(ret, ret);
		reginsert_limits(BRACE_LIMITS, minval, maxval, ret);
		++num_complex_braces;
	    }
	    if (minval > 0)
		*flagp = (HASWIDTH | (flags & HASNL));
	    break;
    }
    if (re_multi_type(peekchr()) != NOT_MULTI)
    {
	/* Can't have a multi follow a multi. */
	if (peekchr() == Magic('*'))
	    sprintf((char *)IObuff, _("E61: Nested %s*"),
					    reg_magic >= MAGIC_ON ? "" : "\\");
	else
	    sprintf((char *)IObuff, _("E62: Nested %s%c"),
		reg_magic == MAGIC_ALL ? "" : "\\", no_Magic(peekchr()));
	EMSG_RET_NULL(IObuff);
    }

    return ret;
}

/*
 * regatom - the lowest level
 *
 * Optimization:  gobbles an entire sequence of ordinary characters so that
 * it can turn them into a single node, which is smaller to store and
 * faster to run.  Don't do this when one_exactly is set.
 */
    static char_u *
regatom(flagp)
    int		   *flagp;
{
    char_u	    *ret;
    int		    flags;
    int		    cpo_lit;	    /* 'cpoptions' contains 'l' flag */
    int		    c;
    static char_u   *classchars = (char_u *)".iIkKfFpPsSdDxXoOwWhHaAlLuU";
    static int	    classcodes[] = {ANY, IDENT, SIDENT, KWORD, SKWORD,
				    FNAME, SFNAME, PRINT, SPRINT,
				    WHITE, NWHITE, DIGIT, NDIGIT,
				    HEX, NHEX, OCTAL, NOCTAL,
				    WORD, NWORD, HEAD, NHEAD,
				    ALPHA, NALPHA, LOWER, NLOWER,
				    UPPER, NUPPER
				    };
    char_u	    *p;
    int		    extra = 0;

    *flagp = WORST;		/* Tentatively. */
    cpo_lit = (!reg_syn && vim_strchr(p_cpo, CPO_LITERAL) != NULL);

    c = getchr();
    switch (c)
    {
      case Magic('^'):
	ret = regnode(BOL);
	break;

      case Magic('$'):
	ret = regnode(EOL);
#if defined(FEAT_SYN_HL) || defined(PROTO)
	had_eol = TRUE;
#endif
	break;

      case Magic('<'):
	ret = regnode(BOW);
	break;

      case Magic('>'):
	ret = regnode(EOW);
	break;

      case Magic('_'):
	c = no_Magic(getchr());
	if (c == '^')		/* "\_^" is start-of-line */
	{
	    ret = regnode(BOL);
	    break;
	}
	if (c == '$')		/* "\_$" is end-of-line */
	{
	    ret = regnode(EOL);
#if defined(FEAT_SYN_HL) || defined(PROTO)
	    had_eol = TRUE;
#endif
	    break;
	}

	extra = ADD_NL;
	*flagp |= HASNL;

	/* "\_[" is character range plus newline */
	if (c == '[')
	    goto collection;

	/* "\_x" is character class plus newline */
	/*FALLTHROUGH*/

	/*
	 * Character classes.
	 */
      case Magic('.'):
      case Magic('i'):
      case Magic('I'):
      case Magic('k'):
      case Magic('K'):
      case Magic('f'):
      case Magic('F'):
      case Magic('p'):
      case Magic('P'):
      case Magic('s'):
      case Magic('S'):
      case Magic('d'):
      case Magic('D'):
      case Magic('x'):
      case Magic('X'):
      case Magic('o'):
      case Magic('O'):
      case Magic('w'):
      case Magic('W'):
      case Magic('h'):
      case Magic('H'):
      case Magic('a'):
      case Magic('A'):
      case Magic('l'):
      case Magic('L'):
      case Magic('u'):
      case Magic('U'):
	p = vim_strchr(classchars, no_Magic(c));
	if (p == NULL)
	    EMSG_RET_NULL("E63: invalid use of \\_");
	ret = regnode(classcodes[p - classchars] + extra);
	*flagp |= HASWIDTH | SIMPLE;
	break;

      case Magic('n'):
	ret = regnode(NEWL);
	*flagp |= HASWIDTH | HASNL;
	break;

      case Magic('('):
	ret = reg(REG_PAREN, &flags);
	if (ret == NULL)
	    return NULL;
	*flagp |= flags & (HASWIDTH | SPSTART | HASNL);
	break;

      case NUL:
      case Magic('|'):
      case Magic('&'):
      case Magic(')'):
	EMSG_RET_NULL(e_internal);	    /* Supposed to be caught earlier. */
	/* NOTREACHED */

      case Magic('='):
      case Magic('?'):
      case Magic('+'):
      case Magic('@'):
      case Magic('{'):
      case Magic('*'):
	c = no_Magic(c);
	sprintf((char *)IObuff, _("E64: %s%c follows nothing"),
		(c == '*' ? reg_magic >= MAGIC_ON : reg_magic == MAGIC_ALL)
		? "" : "\\", c);
	EMSG_RET_NULL(IObuff);
	/* NOTREACHED */

      case Magic('~'):		/* previous substitute pattern */
	    if (reg_prev_sub)
	    {
		char_u	    *p;

		ret = regnode(EXACTLY);
		p = reg_prev_sub;
		while (*p != NUL)
		    regc(*p++);
		regc(NUL);
		if (*reg_prev_sub != NUL)
		{
		    *flagp |= HASWIDTH;
		    if ((p - reg_prev_sub) == 1)
			*flagp |= SIMPLE;
		}
	    }
	    else
		EMSG_RET_NULL(e_nopresub);
	    break;

      case Magic('1'):
      case Magic('2'):
      case Magic('3'):
      case Magic('4'):
      case Magic('5'):
      case Magic('6'):
      case Magic('7'):
      case Magic('8'):
      case Magic('9'):
	    {
		int		    refnum;

		refnum = c - Magic('0');
		/*
		 * Check if the back reference is legal. We must have seen the
		 * close brace.
		 * TODO: Should also check that we don't refer to something
		 * that is repeated (+*=): what instance of the repetition
		 * should we match?
		 */
		if (had_endbrace[refnum])
		    ret = regnode(BACKREF + refnum);
		else
		    EMSG_RET_NULL("E65: Illegal back reference");
	    }
	    break;

#ifdef FEAT_SYN_HL
      case Magic('z'):
	{
	    c = no_Magic(getchr());
	    switch (c)
	    {
		case '(': if (reg_do_extmatch != REX_SET)
			      EMSG_RET_NULL("E66: \\z( not allowed here");
			  ret = reg(REG_ZPAREN, &flags);
			  if (ret == NULL)
			      return NULL;
			  *flagp |= flags & (HASWIDTH | SPSTART | HASNL);
			  re_has_z = REX_SET;
			  break;

		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': if (reg_do_extmatch != REX_USE)
			      EMSG_RET_NULL("E67: \\z1 et al. not allowed here");
			  ret = regnode(ZREF + c - '0');
			  re_has_z = REX_USE;
			  break;

		case 's': ret = regnode(MOPEN + 0);
			  break;

		case 'e': ret = regnode(MCLOSE + 0);
			  break;

		default:  EMSG_RET_NULL("E68: Invalid character after \\z");
	    }
	}
	break;

      case Magic('%'):
	{
	    c = no_Magic(getchr());
	    switch (c)
	    {
		/* () without a back reference */
		case '(':
		    ret = reg(REG_NPAREN, &flags);
		    if (ret == NULL)
			return NULL;
		    *flagp |= flags & (HASWIDTH | SPSTART | HASNL);
		    break;

		/* Catch \%^ and \%$ regardless of where they appear in the
		 * pattern -- regardless of whether or not it makes sense. */
		case '^':
		    ret = regnode(RE_BOF);
		    break;

		case '$':
		    ret = regnode(RE_EOF);
		    break;

		case '#':
		    ret = regnode(CURSOR);
		    break;

		/* \%[abc]: Emit as a list of branches, all ending at the last
		 * branch which matches nothing. */
		case '[':
			  {
			      char_u	*lastbranch;
			      char_u	*lastnode = NULL;
			      char_u	*br;

			      ret = NULL;
			      while ((c = getchr()) != (reg_magic == MAGIC_ALL
							  ? Magic(']') : ']'))
			      {
				  if (c == NUL)
				      EMSG_M_RET_NULL("E69: Missing ] after %s%%[",
						      reg_magic == MAGIC_ALL);
				  br = regnode(BRANCH);
				  if (ret == NULL)
				      ret = br;
				  else
				      regtail(lastnode, br);

				  ungetchr();
				  one_exactly = TRUE;
				  lastnode = regatom(flagp);
				  one_exactly = FALSE;
				  if (lastnode == NULL)
				      return NULL;
			      }
			      if (ret == NULL)
				  EMSG_M_RET_NULL("E70: Empty %s%%[]",
						      reg_magic == MAGIC_ALL);
			      lastbranch = regnode(BRANCH);
			      br = regnode(NOTHING);
			      if (ret != JUST_CALC_SIZE)
			      {
				  regtail(lastnode, br);
				  regtail(lastbranch, br);
				  /* connect all branches to the NOTHING
				   * branch at the end */
				  for (br = ret; br != lastnode; )
				  {
				      if (OP(br) == BRANCH)
				      {
					  regtail(br, lastbranch);
					  br = OPERAND(br);
				      }
				      else
					  br = regnext(br);
				  }
			      }
			      *flagp &= ~HASWIDTH;
			      break;
			  }

		default:
			  if (isdigit(c) || c == '<' || c == '>')
			  {
			      long_u	n = 0;
			      int	cmp;

			      cmp = c;
			      if (cmp == '<' || cmp == '>')
				  c = getchr();
			      while (isdigit(c))
			      {
				  n = n * 10 + (c - '0');
				  c = getchr();
			      }
			      if (c == 'l' || c == 'c' || c == 'v')
			      {
				  if (c == 'l')
				      ret = regnode(RE_LNUM);
				  else if (c == 'c')
				      ret = regnode(RE_COL);
				  else
				      ret = regnode(RE_VCOL);
				  if (ret == JUST_CALC_SIZE)
				      regsize += 5;
				  else
				  {
				      /* put the number and the optional
				       * comparator after the opcode */
				      regcode = re_put_long(regcode, n);
				      *regcode++ = cmp;
				  }
				  break;
			      }
			  }

			  EMSG_M_RET_NULL("E71: Invalid character after %s%%",
						      reg_magic == MAGIC_ALL);
	    }
	}
	break;
#endif

      case Magic('['):
collection:
	{
	    char_u	*p;

	    /*
	     * If there is no matching ']', we assume the '[' is a normal
	     * character.  This makes 'incsearch' and ":help [" work.
	     */
	    p = skip_anyof(regparse);
	    if (*p == ']')	/* there is a matching ']' */
	    {
		int	startc = -1;	/* > 0 when next '-' is a range */
		int	endc;

		/*
		 * In a character class, different parsing rules apply.
		 * Not even \ is special anymore, nothing is.
		 */
		if (*regparse == '^')	    /* Complement of range. */
		{
		    ret = regnode(ANYBUT + extra);
		    regparse++;
		}
		else
		    ret = regnode(ANYOF + extra);

		/* At the start ']' and '-' mean the literal character. */
		if (*regparse == ']' || *regparse == '-')
		    regc(*regparse++);

		while (*regparse != NUL && *regparse != ']')
		{
		    if (*regparse == '-')
		    {
			++regparse;
			/* The '-' is not used for a range at the end and
			 * after or before a '\n'. */
			if (*regparse == ']' || *regparse == NUL
				|| startc == -1
				|| (regparse[0] == '\\' && regparse[1] == 'n'))
			{
			    regc('-');
			    startc = '-';	/* [--x] is a range */
			}
			else
			{
#ifdef FEAT_MBYTE
			    if (has_mbyte)
				endc = mb_ptr2char_adv(&regparse);
			    else
#endif
				endc = *regparse++;
			    if (startc > endc)
				EMSG_RET_NULL(e_invrange);
#ifdef FEAT_MBYTE
			    if (has_mbyte && ((*mb_char2len)(startc) > 1
						 || (*mb_char2len)(endc) > 1))
			    {
				/* Limit to a range of 256 chars */
				if (endc > startc + 256)
				    EMSG_RET_NULL(e_invrange);
				while (++startc <= endc)
				    regmbc(startc);
			    }
			    else
#endif
			    {
#ifdef EBCDIC
				int	alpha_only = FALSE;

				/* for alphabetical range skip the gaps
				 * 'i'-'j', 'r'-'s', 'I'-'J' and 'R'-'S'.  */
				if (isalpha(startc) && isalpha(endc))
				    alpha_only = TRUE;
#endif
				while (++startc <= endc)
#ifdef EBCDIC
				    if (!alpha_only || isalpha(startc))
#endif
					regc(startc);
			    }
			    startc = -1;
			}
		    }
		    /*
		     * Only "\]", "\^", "\]" and "\\" are special in Vi.  Vim
		     * accepts "\t", "\e", etc., but only when the 'l' flag in
		     * 'cpoptions' is not included.
		     */
		    else if (*regparse == '\\'
			    && (vim_strchr(REGEXP_INRANGE, regparse[1]) != NULL
				|| (!cpo_lit
				    && vim_strchr(REGEXP_ABBR,
						       regparse[1]) != NULL)))
		    {
			regparse++;
			if (*regparse == 'n')
			{
			    /* '\n' in range: also match NL */
			    if (ret != JUST_CALC_SIZE)
			    {
				if (*ret == ANYBUT)
				    *ret = ANYBUT + ADD_NL;
				else if (*ret == ANYOF)
				    *ret = ANYOF + ADD_NL;
				/* else: must have had a \n already */
			    }
			    *flagp |= HASNL;
			    regparse++;
			    startc = -1;
			}
			else
			{
			    startc = backslash_trans(*regparse++);
			    regc(startc);
			}
		    }
		    else if (*regparse == '[')
		    {
			int c_class;
			int cu;

			c_class = skip_class_name(&regparse);
			startc = -1;
			/* Characters assumed to be 8 bits! */
			switch (c_class)
			{
			    case CLASS_NONE:
				/* literal '[', allow [[-x] as a range */
				startc = *regparse++;
				regc(startc);
				break;
			    case CLASS_ALNUM:
				for (cu = 1; cu <= 255; cu++)
				    if (isalnum(cu))
					regc(cu);
				break;
			    case CLASS_ALPHA:
				for (cu = 1; cu <= 255; cu++)
				    if (isalpha(cu))
					regc(cu);
				break;
			    case CLASS_BLANK:
				regc(' ');
				regc('\t');
				break;
			    case CLASS_CNTRL:
				for (cu = 1; cu <= 255; cu++)
				    if (iscntrl(cu))
					regc(cu);
				break;
			    case CLASS_DIGIT:
				for (cu = 1; cu <= 255; cu++)
				    if (isdigit(cu))
					regc(cu);
				break;
			    case CLASS_GRAPH:
				for (cu = 1; cu <= 255; cu++)
				    if (isgraph(cu))
					regc(cu);
				break;
			    case CLASS_LOWER:
				for (cu = 1; cu <= 255; cu++)
				    if (islower(cu))
					regc(cu);
				break;
			    case CLASS_PRINT:
				for (cu = 1; cu <= 255; cu++)
				    if (vim_isprintc(cu))
					regc(cu);
				break;
			    case CLASS_PUNCT:
				for (cu = 1; cu <= 255; cu++)
				    if (ispunct(cu))
					regc(cu);
				break;
			    case CLASS_SPACE:
				for (cu = 9; cu <= 13; cu++)
				    regc(cu);
				regc(' ');
				break;
			    case CLASS_UPPER:
				for (cu = 1; cu <= 255; cu++)
				    if (isupper(cu))
					regc(cu);
				break;
			    case CLASS_XDIGIT:
				for (cu = 1; cu <= 255; cu++)
				    if (isxdigit(cu))
					regc(cu);
				break;
			    case CLASS_TAB:
				regc('\t');
				break;
			    case CLASS_RETURN:
				regc('\r');
				break;
			    case CLASS_BACKSPACE:
				regc('\b');
				break;
			    case CLASS_ESCAPE:
				regc('\033');
				break;
			}
		    }
		    else
		    {
#ifdef FEAT_MBYTE
			if (has_mbyte)
			{
			    int	len;

			    /* produce a multibyte character, including any
			     * following composing characters */
			    startc = mb_ptr2char(regparse);
			    len = (*mb_ptr2len_check)(regparse);
			    if (enc_utf8 && utf_char2len(startc) != len)
				startc = -1;	/* composing chars */
			    while (--len >= 0)
				regc(*regparse++);
			}
			else
#endif
			{
			    startc = *regparse++;
			    regc(startc);
			}
		    }
		}
		regc(NUL);
		prevchr_len = 1;	/* last char was the ']' */
		if (*regparse != ']')
		    EMSG_RET_NULL(e_toomsbra);	/* Cannot happen? */
		skipchr();	    /* let's be friends with the lexer again */
		*flagp |= HASWIDTH | SIMPLE;
		break;
	    }
	}
	/* FALLTHROUGH */

      default:
	{
	    int		len;

#ifdef FEAT_MBYTE
	    /* A multi-byte character is handled as a separate atom if it's
	     * before a multi. */
	    if (has_mbyte && (*mb_char2len)(c) > 1
				     && re_multi_type(peekchr()) != NOT_MULTI)
	    {
		ret = regnode(MULTIBYTECODE);
		regmbc(c);
		*flagp |= HASWIDTH | SIMPLE;
		break;
	    }
#endif

	    ret = regnode(EXACTLY);

	    /*
	     * Append characters as long as:
	     * - there is no following multi, we then need the character in
	     *   front of it as a single character operand
	     * - not running into a Magic character
	     * - "one_exactly" is not set
	     * But always emit at least one character.  Might be a Multi,
	     * e.g., a "[" without matching "]".
	     */
	    for (len = 0; c != NUL && (len == 0
			|| (re_multi_type(peekchr()) == NOT_MULTI
			    && !one_exactly
			    && !is_Magic(c))); ++len)
	    {
		c = no_Magic(c);
#ifdef FEAT_MBYTE
		if (has_mbyte)
		{
		    regmbc(c);
		    if (enc_utf8)
		    {
			int	off;

			/* Need to get composing character too, directly
			 * access regparse for that, because skipchr() skips
			 * over composing chars. */
			ungetchr();
			if (*regparse == '\\' && regparse[1] != NUL)
			    off = 1;
			else
			    off = 0;
			for (;;)
			{
			    off += utf_ptr2len_check(regparse + off);
			    c = utf_ptr2char(regparse + off);
			    if (!utf_iscomposing(c))
				break;
			    regmbc(c);
			}
			skipchr();
		    }
		}
		else
#endif
		    regc(c);
		c = getchr();
	    }
	    ungetchr();

	    regc(NUL);
	    *flagp |= HASWIDTH;
	    if (len == 1)
		*flagp |= SIMPLE;
	}
	break;
    }

    return ret;
}

/*
 * emit a node
 * Return pointer to generated code.
 */
    static char_u *
regnode(op)
    int		op;
{
    char_u  *ret;

    ret = regcode;
    if (ret == JUST_CALC_SIZE)
	regsize += 3;
    else
    {
	*regcode++ = op;
	*regcode++ = NUL;		/* Null "next" pointer. */
	*regcode++ = NUL;
    }
    return ret;
}

/*
 * Emit (if appropriate) a byte of code
 */
    static void
regc(b)
    int		b;
{
    if (regcode == JUST_CALC_SIZE)
	regsize++;
    else
	*regcode++ = b;
}

#ifdef FEAT_MBYTE
/*
 * Emit (if appropriate) a multi-byte character of code
 */
    static void
regmbc(c)
    int		c;
{
    if (regcode == JUST_CALC_SIZE)
	regsize += (*mb_char2len)(c);
    else
	regcode += (*mb_char2bytes)(c, regcode);
}
#endif

/*
 * reginsert - insert an operator in front of already-emitted operand
 *
 * Means relocating the operand.
 */
    static void
reginsert(op, opnd)
    int		op;
    char_u     *opnd;
{
    char_u	*src;
    char_u	*dst;
    char_u	*place;

    if (regcode == JUST_CALC_SIZE)
    {
	regsize += 3;
	return;
    }
    src = regcode;
    regcode += 3;
    dst = regcode;
    while (src > opnd)
	*--dst = *--src;

    place = opnd;		/* Op node, where operand used to be. */
    *place++ = op;
    *place++ = NUL;
    *place = NUL;
}

/*
 * reginsert_limits - insert an operator in front of already-emitted operand.
 * The operator has the given limit values as operands.  Also set next pointer.
 *
 * Means relocating the operand.
 */
    static void
reginsert_limits(op, minval, maxval, opnd)
    int		op;
    long	minval;
    long	maxval;
    char_u	*opnd;
{
    char_u	*src;
    char_u	*dst;
    char_u	*place;

    if (regcode == JUST_CALC_SIZE)
    {
	regsize += 11;
	return;
    }
    src = regcode;
    regcode += 11;
    dst = regcode;
    while (src > opnd)
	*--dst = *--src;

    place = opnd;		/* Op node, where operand used to be. */
    *place++ = op;
    *place++ = NUL;
    *place++ = NUL;
    place = re_put_long(place, (long_u)minval);
    place = re_put_long(place, (long_u)maxval);
    regtail(opnd, place);
}

/*
 * Write a long as four bytes at "p" and return pointer to the next char.
 */
    static char_u *
re_put_long(p, val)
    char_u	*p;
    long_u	val;
{
    *p++ = (char_u) ((val >> 24) & 0377);
    *p++ = (char_u) ((val >> 16) & 0377);
    *p++ = (char_u) ((val >> 8) & 0377);
    *p++ = (char_u) (val & 0377);
    return p;
}

/*
 * regtail - set the next-pointer at the end of a node chain
 */
    static void
regtail(p, val)
    char_u	*p;
    char_u	*val;
{
    char_u	*scan;
    char_u	*temp;
    int		offset;

    if (p == JUST_CALC_SIZE)
	return;

    /* Find last node. */
    scan = p;
    for (;;)
    {
	temp = regnext(scan);
	if (temp == NULL)
	    break;
	scan = temp;
    }

    if (OP(scan) == BACK)
	offset = (int)(scan - val);
    else
	offset = (int)(val - scan);
    *(scan + 1) = (char_u) (((unsigned)offset >> 8) & 0377);
    *(scan + 2) = (char_u) (offset & 0377);
}

/*
 * regoptail - regtail on item after a BRANCH; nop if none
 */
    static void
regoptail(p, val)
    char_u	*p;
    char_u	*val;
{
    /* When op is neither BRANCH nor BRACE_COMPLEX0-9, it is "operandless" */
    if (p == NULL || p == JUST_CALC_SIZE
	    || (OP(p) != BRANCH
		&& (OP(p) < BRACE_COMPLEX || OP(p) > BRACE_COMPLEX + 9)))
	return;
    regtail(OPERAND(p), val);
}

/*
 * getchr() - get the next character from the pattern. We know about
 * magic and such, so therefore we need a lexical analyzer.
 */

/* static int	    curchr; */
static int	prevprevchr;
static int	prevchr;
static int	nextchr;    /* used for ungetchr() */
/*
 * Note: prevchr is sometimes -1 when we are not at the start,
 * eg in /[ ^I]^ the pattern was never found even if it existed, because ^ was
 * taken to be magic -- webb
 */
static int	at_start;	/* True when on the first character */
static int	prev_at_start;  /* True when on the second character */

    static void
initchr(str)
    char_u *str;
{
    regparse = str;
    prevchr_len = 0;
    curchr = prevprevchr = prevchr = nextchr = -1;
    at_start = TRUE;
    prev_at_start = FALSE;
}

    static int
peekchr()
{
    if (curchr == -1)
    {
	switch (curchr = regparse[0])
	{
	case '.':
	case '[':
	case '~':
	    /* magic when 'magic' is on */
	    if (reg_magic >= MAGIC_ON)
		curchr = Magic(curchr);
	    break;
	case '(':
	case ')':
	case '{':
	case '%':
	case '+':
	case '=':
	case '?':
	case '@':
	case '!':
	case '&':
	case '|':
	case '<':
	case '>':
	case '#':	/* future ext. */
	case '"':	/* future ext. */
	case '\'':	/* future ext. */
	case ',':	/* future ext. */
	case '-':	/* future ext. */
	case ':':	/* future ext. */
	case ';':	/* future ext. */
	case '`':	/* future ext. */
	case '/':	/* Can't be used in / command */
	    /* magic only after "\v" */
	    if (reg_magic == MAGIC_ALL)
		curchr = Magic(curchr);
	    break;
	case '*':
	    /* * is not magic as the very first character, eg "?*ptr" and when
	     * after '^', eg "/^*ptr" */
	    if (reg_magic >= MAGIC_ON && !at_start
				 && !(prev_at_start && prevchr == Magic('^')))
		curchr = Magic('*');
	    break;
	case '^':
	    /* '^' is only magic as the very first character and if it's after
	     * "\(", "\|", "\&' or "\n" */
	    if (reg_magic >= MAGIC_OFF && (at_start
			|| prevchr == Magic('(')
			|| prevchr == Magic('|')
			|| prevchr == Magic('&')
			|| prevchr == Magic('n')
			|| (no_Magic(prevchr) == '('
			    && prevprevchr == Magic('%'))))
	    {
		curchr = Magic('^');
		at_start = TRUE;
		prev_at_start = FALSE;
	    }
	    break;
	case '$':
	    /* '$' is only magic as the very last char and if it's in front of
	     * either "\|", "\)", "\&", or "\n" */
	    {
		char_u *p = regparse + 1;

		/* ignore \c \C \m and \M after '$' */
		if (p[0] == '\\' && (p[1] == 'c' || p[1] == 'C'
					       || p[1] == 'm' || p[1] == 'M'))
		    p += 2;
		if (reg_magic >= MAGIC_OFF
			&& (p[0] == NUL
			    || (p[0] == '\\'
				&& (p[1] == '|' || p[1] == '&' || p[1] == ')'
				    || p[1] == 'n'))))
		    curchr = Magic('$');
	    }
	    break;
	case '\\':
	    if (regparse[1] == NUL)
		curchr = '\\';	/* trailing '\' */
	    else if (vim_strchr(META, regparse[1]))
	    {
		/*
		 * META contains everything that may be magic sometimes,
		 * except ^ and $ ("\^" and "\$" are never magic).  We now
		 * fetch the next character and toggle its magicness.
		 * Therefore, \ is so meta-magic that it is not in META.
		 */
		curchr = -1;
		prev_at_start = at_start;
		at_start = FALSE;	/* be able to say "/\*ptr" */
		++regparse;
		peekchr();
		--regparse;
		curchr = toggle_Magic(curchr);
	    }
	    else if (vim_strchr(REGEXP_ABBR, regparse[1]))
	    {
		/*
		 * Handle abbreviations, like "\t" for TAB -- webb
		 */
		curchr = backslash_trans(regparse[1]);
	    }
	    else
	    {
		/*
		 * Next character can never be (made) magic?
		 * Then backslashing it won't do anything.
		 */
#ifdef FEAT_MBYTE
		if (has_mbyte)
		    curchr = (*mb_ptr2char)(regparse + 1);
		else
#endif
		    curchr = regparse[1];
	    }
	    break;

#ifdef FEAT_MBYTE
	default:
	    if (has_mbyte)
		curchr = (*mb_ptr2char)(regparse);
#endif
	}
    }

    return curchr;
}

/*
 * Eat one lexed character.  Do this in a way that we can undo it.
 */
    static void
skipchr()
{
    /* peekchr() eats a backslash, do the same here */
    if (*regparse == '\\')
	prevchr_len = 1;
    else
	prevchr_len = 0;
    if (regparse[prevchr_len] != NUL)
    {
#ifdef FEAT_MBYTE
	if (has_mbyte)
	    prevchr_len += (*mb_ptr2len_check)(regparse + prevchr_len);
	else
#endif
	    ++prevchr_len;
    }
    regparse += prevchr_len;
    prev_at_start = at_start;
    at_start = FALSE;
    prevprevchr = prevchr;
    prevchr = curchr;
    curchr = nextchr;	    /* use previously unget char, or -1 */
    nextchr = -1;
}

/*
 * Skip a character while keeping the value of prev_at_start for at_start.
 */
    static void
skipchr_keepstart()
{
    int as = prev_at_start;

    skipchr();
    at_start = as;
}

    static int
getchr()
{
    int chr = peekchr();

    skipchr();
    return chr;
}

/*
 * put character back.  Works only once!
 */
    static void
ungetchr()
{
    nextchr = curchr;
    curchr = prevchr;
    prevchr = prevprevchr;
    at_start = prev_at_start;
    prev_at_start = FALSE;

    /* Backup regparse, so that it's at the same position as before the
     * getchr(). */
    regparse -= prevchr_len;
}

/*
 * read_limits - Read two integers to be taken as a minimum and maximum.
 * If the first character is '-', then the range is reversed.
 * Should end with 'end'.  If minval is missing, zero is default, if maxval is
 * missing, a very big number is the default.
 */
    static int
read_limits(minval, maxval)
    long	*minval;
    long	*maxval;
{
    int		reverse = FALSE;
    char_u	*first_char;
    long	tmp;

    if (*regparse == '-')
    {
	/* Starts with '-', so reverse the range later */
	regparse++;
	reverse = TRUE;
    }
    first_char = regparse;
    *minval = getdigits(&regparse);
    if (*regparse == ',')	    /* There is a comma */
    {
	if (isdigit(*++regparse))
	    *maxval = getdigits(&regparse);
	else
	    *maxval = MAX_LIMIT;
    }
    else if (isdigit(*first_char))
	*maxval = *minval;	    /* It was \{n} or \{-n} */
    else
	*maxval = MAX_LIMIT;	    /* It was \{} or \{-} */
    if (*regparse == '\\')
	regparse++;	/* Allow either \{...} or \{...\} */
    if (       (*regparse != '}' && *regparse != NUL)
	    || (*maxval == 0 && *minval == 0))
    {
	sprintf((char *)IObuff, _("Syntax error in %s{...}"),
					  reg_magic == MAGIC_ALL ? "" : "\\");
	EMSG_RET_FAIL(IObuff);
    }

    /*
     * Reverse the range if there was a '-', or make sure it is in the right
     * order otherwise.
     */
    if ((!reverse && *minval > *maxval) || (reverse && *minval < *maxval))
    {
	tmp = *minval;
	*minval = *maxval;
	*maxval = tmp;
    }
    skipchr();		/* let's be friends with the lexer again */
    return OK;
}

/*
 * vim_regexec and friends
 */

/*
 * Global work variables for vim_regexec().
 */

/* The current match-position is remembered with these variables: */
static linenr_T	reglnum;	/* line number, relative to first line */
static char_u	*regline;	/* start of current line */
static char_u	*reginput;	/* current input, points into "regline" */

static int	need_clear_subexpr;	/* subexpressions still need to be
					 * cleared */
#ifdef FEAT_SYN_HL
static int	need_clear_zsubexpr = FALSE;	/* extmatch subexpressions
						 * still need to be cleared */
#endif

static int	out_of_stack;	/* TRUE when ran out of stack space */

/*
 * Structure used to save the current input state, when it needs to be
 * restored after trying a match.  Used by reg_save() and reg_restore().
 */
typedef struct
{
    union
    {
	char_u	*ptr;	/* reginput pointer, for single-line regexp */
	pos_T	pos;	/* reginput pos, for multi-line regexp */
    } rs_u;
} regsave_T;

/* struct to save start/end pointer/position in for \(\) */
typedef struct
{
    union
    {
	char_u	*ptr;
	pos_T	pos;
    } se_u;
} save_se_T;

static char_u	*reg_getline __ARGS((linenr_T lnum));
static long	vim_regexec_both __ARGS((char_u *line, colnr_T col));
static long	regtry __ARGS((regprog_T *prog, colnr_T col));
static void	cleanup_subexpr __ARGS((void));
#ifdef FEAT_SYN_HL
static void	cleanup_zsubexpr __ARGS((void));
#endif
static void	reg_nextline __ARGS((void));
static void	reg_save __ARGS((regsave_T *save));
static void	reg_restore __ARGS((regsave_T *save));
static int	reg_save_equal __ARGS((regsave_T *save));
static void	save_se __ARGS((save_se_T *savep, pos_T *posp, char_u **pp));
static void	restore_se __ARGS((save_se_T *savep, pos_T *posp, char_u **pp));
static int	re_num_cmp __ARGS((long_u val, char_u *scan));
static int	regmatch __ARGS((char_u *prog));
static int	regrepeat __ARGS((char_u *p, long maxcount));

#ifdef DEBUG
int		regnarrate = 0;
#endif

/*
 * Internal copy of 'ignorecase'.  It is set at each call to vim_regexec().
 * Normally it gets the value of "rm_ic" or "rmm_ic", but when the pattern
 * contains '\c' or '\C' the value is overruled.
 */
static int	ireg_ic;

/*
 * Sometimes need to save a copy of a line.  Since alloc()/free() is very
 * slow, we keep one allocated piece of memory and only re-allocate it when
 * it's too small.  It's freed in vim_regexec_both() when finished.
 */
static char_u	*reg_tofree;
static unsigned	reg_tofreelen;

/*
 * These variables are set when executing a regexp to speed up the execution.
 * Which ones are set depends on whethere a single-line or multi-line match is
 * done:
 *			single-line		multi-line
 * reg_match		&regmatch_T		NULL
 * reg_mmatch		NULL			&regmmatch_T
 * reg_startp		reg_match->startp	<invalid>
 * reg_endp		reg_match->endp		<invalid>
 * reg_startpos		<invalid>		reg_mmatch->startpos
 * reg_endpos		<invalid>		reg_mmatch->endpos
 * reg_win		NULL			window in which to search
 * reg_buf		<invalid>		buffer in which to search
 * reg_firstlnum	<invalid>		first line in which to search
 * reg_maxline		0			last line nr
 */
static regmatch_T	*reg_match;
static regmmatch_T	*reg_mmatch;
static char_u		**reg_startp;
static char_u		**reg_endp;
static pos_T		*reg_startpos;
static pos_T		*reg_endpos;
static win_T		*reg_win;
static buf_T		*reg_buf;
static linenr_T		reg_firstlnum;
static linenr_T		reg_maxline;

/*
 * Get pointer to the line "lnum", which is relative to "reg_firstlnum".
 */
    static char_u *
reg_getline(lnum)
    linenr_T	lnum;
{
    /* when looking behind for a match/no-match lnum is negative.  But we
     * can't go before line 1 */
    if (reg_firstlnum + lnum < 1)
	return NULL;
    return ml_get_buf(reg_buf, reg_firstlnum + lnum, FALSE);
}

#ifdef FEAT_SYN_HL
static char_u	*reg_startzp[NSUBEXP];	/* Workspace to mark beginning */
static char_u	*reg_endzp[NSUBEXP];	/*   and end of \z(...\) matches */
static pos_T	reg_startzpos[NSUBEXP];	/* idem, beginning pos */
static pos_T	reg_endzpos[NSUBEXP];	/* idem, end pos */
#endif

/* TRUE if using multi-line regexp. */
#define REG_MULTI	(reg_match == NULL)

/*
 * Match a regexp against a string.
 * "rmp->regprog" is a compiled regexp as returned by vim_regcomp().
 * Uses curbuf for line count and 'iskeyword'.
 *
 * Return TRUE if there is a match, FALSE if not.
 */
    int
vim_regexec(rmp, line, col)
    regmatch_T	*rmp;
    char_u	*line;	/* string to match against */
    colnr_T	col;	/* column to start looking for match */
{
    reg_match = rmp;
    reg_mmatch = NULL;
    reg_maxline = 0;
    reg_win = NULL;
    ireg_ic = rmp->rm_ic;
    return (vim_regexec_both(line, col) != 0);
}

/*
 * Match a regexp against multiple lines.
 * "rmp->regprog" is a compiled regexp as returned by vim_regcomp().
 * Uses curbuf for line count and 'iskeyword'.
 *
 * Return zero if there is no match.  Return number of lines contained in the
 * match otherwise.
 */
    long
vim_regexec_multi(rmp, win, buf, lnum, col)
    regmmatch_T	*rmp;
    win_T	*win;		/* window in which to search or NULL */
    buf_T	*buf;		/* buffer in which to search */
    linenr_T	lnum;		/* nr of line to start looking for match */
    colnr_T	col;		/* column to start looking for match */
{
    long	r;
    buf_T	*save_curbuf = curbuf;

    reg_match = NULL;
    reg_mmatch = rmp;
    reg_buf = buf;
    reg_win = win;
    reg_firstlnum = lnum;
    reg_maxline = reg_buf->b_ml.ml_line_count - lnum;
    ireg_ic = rmp->rmm_ic;

    /* Need to switch to buffer "buf" to make vim_iswordc() work. */
    curbuf = buf;
    r = vim_regexec_both(NULL, col);
    curbuf = save_curbuf;

    return r;
}

/*
 * Match a regexp against a string ("line" points to the string) or multiple
 * lines ("line" is NULL, use reg_getline()).
 */
#ifdef HAVE_SETJMP_H
    static long
vim_regexec_both(line_arg, col_arg)
    char_u	*line_arg;
    colnr_T	col_arg;	/* column to start looking for match */
#else
    static long
vim_regexec_both(line, col)
    char_u	*line;
    colnr_T	col;		/* column to start looking for match */
#endif
{
    regprog_T	*prog;
    char_u	*s;
    long	retval;
#ifdef HAVE_SETJMP_H
    char_u	*line;
    colnr_T	col;
#endif

    reg_tofree = NULL;
#ifdef HAVE_SETJMP_H
    /*
     * Matching with a regexp may cause a very deep recursive call of
     * regmatch().  Vim will crash when running out of stack space.  Catch
     * this here if the system supports it.
     */
    mch_startjmp();
    if (SETJMP(lc_jump_env) != 0)
    {
	mch_didjmp();
#ifdef SIGHASARG
	if (lc_signal != SIGINT)
#endif
	    EMSG(_("(ey8) Crash intercepted; regexp too complex?"));
	retval = 0L;
	goto theend;
    }

    /* Trick to avoid "might be clobbered by `longjmp'" warning from gcc. */
    line = line_arg;
    col = col_arg;
#endif
    retval = 0L;

    if (REG_MULTI)
    {
	prog = reg_mmatch->regprog;
	line = reg_getline((linenr_T)0);
	reg_startpos = reg_mmatch->startpos;
	reg_endpos = reg_mmatch->endpos;
    }
    else
    {
	prog = reg_match->regprog;
	reg_startp = reg_match->startp;
	reg_endp = reg_match->endp;
    }

    /* Be paranoid... */
    if (prog == NULL || line == NULL)
    {
	EMSG(_(e_null));
	goto theend;
    }

    /* Check validity of program. */
    if (prog_magic_wrong())
	goto theend;

    /* If pattern contains "\c" or "\C": overrule value of ireg_ic */
    if (prog->regflags & RF_ICASE)
	ireg_ic = TRUE;
    else if (prog->regflags & RF_NOICASE)
	ireg_ic = FALSE;

    /* If there is a "must appear" string, look for it. */
    if (prog->regmust != NULL)
    {
	int c;

#ifdef FEAT_MBYTE
	if (has_mbyte)
	    c = (*mb_ptr2char)(prog->regmust);
	else
#endif
	    c = *prog->regmust;
	s = line + col;
	while ((s = cstrchr(s, c)) != NULL)
	{
	    if (cstrncmp(s, prog->regmust, prog->regmlen) == 0)
		break;		/* Found it. */
#ifdef FEAT_MBYTE
	    if (has_mbyte)
		s += (*mb_ptr2len_check)(s);
	    else
#endif
		++s;
	}
	if (s == NULL)		/* Not present. */
	    goto theend;
    }

    regline = line;
    reglnum = 0;
    out_of_stack = FALSE;

    /* Simplest case: Anchored match need be tried only once. */
    if (prog->reganch)
    {
	int	c;

#ifdef FEAT_MBYTE
	if (has_mbyte)
	    c = (*mb_ptr2char)(regline + col);
	else
#endif
	    c = regline[col];
	if (prog->regstart != NUL
		&& prog->regstart != c
		&& (!ireg_ic
#ifdef FEAT_MBYTE
		    || c > 255 || prog->regstart > 255
#endif
		    || TO_LOWER(prog->regstart) != TO_LOWER(c)))
	    retval = 0;
	else
	    retval = regtry(prog, col);
    }
    else
    {
	/* Messy cases:  unanchored match. */
	while (!got_int && !out_of_stack)
	{
	    if (prog->regstart != NUL)
	    {
		/* Skip until the char we know it must start with. */
		s = cstrchr(regline + col, prog->regstart);
		if (s == NULL)
		{
		    retval = 0;
		    break;
		}
		col = s - regline;
	    }

	    retval = regtry(prog, col);
	    if (retval > 0)
		break;

	    /* if not currently on the first line, get it again */
	    if (reglnum != 0)
	    {
		regline = reg_getline((linenr_T)0);
		reglnum = 0;
	    }
	    if (regline[col] == NUL)
		break;
#ifdef FEAT_MBYTE
	    if (has_mbyte)
		col += (*mb_ptr2len_check)(regline + col);
	    else
#endif
		++col;
	}
    }

    if (out_of_stack)
	EMSG(_("(ey9) pattern caused out-of-stack error"));

theend:
    /* Didn't find a match. */
    vim_free(reg_tofree);
#ifdef HAVE_SETJMP_H
    mch_endjmp();
#endif
    return retval;
}

#ifdef FEAT_SYN_HL
static reg_extmatch_T *make_extmatch __ARGS((void));

/*
 * Create a new extmatch and mark it as referenced once.
 */
    static reg_extmatch_T *
make_extmatch()
{
    reg_extmatch_T	*em;

    em = (reg_extmatch_T *)alloc_clear((unsigned)sizeof(reg_extmatch_T));
    if (em != NULL)
	em->refcnt = 1;
    return em;
}

/*
 * Add a reference to an extmatch.
 */
    reg_extmatch_T *
ref_extmatch(em)
    reg_extmatch_T	*em;
{
    if (em != NULL)
	em->refcnt++;
    return em;
}

/*
 * Remove a reference to an extmatch.  If there are no references left, free
 * the info.
 */
    void
unref_extmatch(em)
    reg_extmatch_T	*em;
{
    int i;

    if (em != NULL && --em->refcnt <= 0)
    {
	for (i = 0; i < NSUBEXP; ++i)
	    vim_free(em->matches[i]);
	vim_free(em);
    }
}
#endif

/*
 * regtry - try match of "prog" with at regline["col"].
 * Returns 0 for failure, number of lines contained in the match otherwise.
 */
    static long
regtry(prog, col)
    regprog_T	*prog;
    colnr_T	col;
{
    reginput = regline + col;
    need_clear_subexpr = TRUE;
#ifdef FEAT_SYN_HL
    /* Clear the external match subpointers if necessary. */
    if (prog->reghasz == REX_SET)
	need_clear_zsubexpr = TRUE;
#endif

    if (regmatch(prog->program + 1))
    {
	cleanup_subexpr();
	if (REG_MULTI)
	{
	    if (reg_startpos[0].lnum < 0)
	    {
		reg_startpos[0].lnum = 0;
		reg_startpos[0].col = col;
	    }
	    if (reg_endpos[0].lnum < 0)
	    {
		reg_endpos[0].lnum = reglnum;
		reg_endpos[0].col = reginput - regline;
	    }
#ifdef FEAT_VIRTUALEDIT
	    {
		int i;

		for (i = 0; i < NSUBEXP; ++i)
		{
		    reg_startpos[i].coladd = 0;
		    reg_endpos[i].coladd = 0;
		}
	    }
#endif
	}
	else
	{
	    if (reg_startp[0] == NULL)
		reg_startp[0] = regline + col;
	    if (reg_endp[0] == NULL)
		reg_endp[0] = reginput;
	}
#ifdef FEAT_SYN_HL
	/* Package any found \z(...\) matches for export. Default is none. */
	unref_extmatch(re_extmatch_out);
	re_extmatch_out = NULL;

	if (prog->reghasz == REX_SET)
	{
	    int		i;

	    cleanup_zsubexpr();
	    re_extmatch_out = make_extmatch();
	    for (i = 0; i < NSUBEXP; i++)
	    {
		if (REG_MULTI)
		{
		    /* Only accept single line matches. */
		    if (reg_startzpos[i].lnum >= 0
			    && reg_endzpos[i].lnum == reg_startzpos[i].lnum)
			re_extmatch_out->matches[i] =
			    vim_strnsave(reg_getline(reg_startzpos[i].lnum)
						       + reg_startzpos[i].col,
				    reg_endzpos[i].col - reg_startzpos[i].col);
		}
		else
		{
		    if (reg_startzp[i] != NULL && reg_endzp[i] != NULL)
			re_extmatch_out->matches[i] =
			    vim_strnsave(reg_startzp[i],
				    (int)(reg_endzp[i] - reg_startzp[i]));
		}
	    }
	}
#endif
	return 1 + reglnum;
    }
    return 0;
}

#ifdef FEAT_MBYTE
/* multi-byte: advance reginput with a function */
# define ADVANCE_REGINPUT() advance_reginput()

static void advance_reginput __ARGS((void));
static int reg_prev_class __ARGS((void));

    static void
advance_reginput()
{
    if (has_mbyte)
	reginput += (*mb_ptr2len_check)(reginput);
    else
	++reginput;
}

/*
 * Get class of previous character.
 */
    static int
reg_prev_class()
{
    if (reginput > regline)
	return mb_get_class(reginput - 1
				     - (*mb_head_off)(regline, reginput - 1));
    return -1;
}

#else
/* No multi-byte: It's too simple to make a function for. */
# define ADVANCE_REGINPUT() ++reginput
#endif

/*
 * The arguments from BRACE_LIMITS are stored here.  They are actually local
 * to regmatch(), but they are here to reduce the amount of stack space used
 * (it can be called recursively many times).
 */
static long	bl_minval;
static long	bl_maxval;

/*
 * regmatch - main matching routine
 *
 * Conceptually the strategy is simple: Check to see whether the current
 * node matches, call self recursively to see whether the rest matches,
 * and then act accordingly.  In practice we make some effort to avoid
 * recursion, in particular by going through "ordinary" nodes (that don't
 * need to know whether the rest of the match failed) by a loop instead of
 * by recursion.
 *
 * Returns TRUE when there is a match.  Leaves reginput and reglnum just after
 * the last matched character.
 * Returns FALSE when there is no match.  Leaves reginput and reglnum in an
 * undefined state!
 */
    static int
regmatch(scan)
    char_u	*scan;		/* Current node. */
{
    char_u	*next;		/* Next node. */
    int		op;
    int		c;

#ifdef HAVE_GETRLIMIT
    /* Check if we are running out of stack space.  Could be caused by
     * recursively calling ourselves. */
    if (out_of_stack || mch_stackcheck((char *)&op) == FAIL)
    {
	out_of_stack = TRUE;
	return FALSE;
    }
#endif

    /* Some patterns my cause a long time to match, even though they are not
     * illegal.  E.g., "\([a-z]\+\)\+Q".  Allow breaking them with CTRL-C. */
    fast_breakcheck();

#ifdef DEBUG
    if (scan != NULL && regnarrate)
    {
	mch_errmsg(regprop(scan));
	mch_errmsg("(\n");
    }
#endif
    while (scan != NULL)
    {
	if (got_int)
	    return FALSE;
#ifdef DEBUG
	if (regnarrate)
	{
	    mch_errmsg(regprop(scan));
	    mch_errmsg("...\n");
# ifdef FEAT_SYN_HL
	    if (re_extmatch_in != NULL)
	    {
		int i;

		mch_errmsg(_("External submatches:\n"));
		for (i = 0; i < NSUBEXP; i++)
		{
		    mch_errmsg("    \"");
		    if (re_extmatch_in->matches[i] != NULL)
			mch_errmsg(re_extmatch_in->matches[i]);
		    mch_errmsg("\"\n");
		}
	    }
# endif
	}
#endif
	next = regnext(scan);

	op = OP(scan);
	/* Check for character class with NL added. */
	if (WITH_NL(op) && *reginput == NUL && reglnum < reg_maxline)
	{
	    reg_nextline();
	}
	else
	{
	  if (WITH_NL(op))
	    op -= ADD_NL;
#ifdef FEAT_MBYTE
	  if (has_mbyte)
	      c = (*mb_ptr2char)(reginput);
	  else
#endif
	      c = *reginput;
	  switch (op)
	  {
	  case BOL:
	    if (reginput != regline)
		return FALSE;
	    break;

	  case EOL:
	    if (c != NUL)
		return FALSE;
	    break;

	  case RE_BOF:
	    /* Passing -1 to the getline() function provided for the search
	     * should always return NULL if the current line is the first
	     * line of the file. */
	    if (reglnum != 0 || reginput != regline
			|| (REG_MULTI && reg_getline((linenr_T)-1) != NULL))
		return FALSE;
	    break;

	  case RE_EOF:
	    if (reglnum != reg_maxline || c != NUL)
		return FALSE;
	    break;

	  case CURSOR:
	    /* Check if the buffer is in a window and compare the
	     * reg_win->w_cursor position to the match position. */
	    if (reg_win == NULL
		    || (reglnum + reg_firstlnum != reg_win->w_cursor.lnum)
		    || ((colnr_T)(reginput - regline) != reg_win->w_cursor.col))
		return FALSE;
	    break;

	  case RE_LNUM:
	    if (!REG_MULTI || !re_num_cmp((long_u)(reglnum + reg_firstlnum),
									scan))
		return FALSE;
	    break;

	  case RE_COL:
	    if (!re_num_cmp((long_u)(reginput - regline) + 1, scan))
		return FALSE;
	    break;

	  case RE_VCOL:
	    if (!re_num_cmp((long_u)win_linetabsize(
			    reg_win == NULL ? curwin : reg_win,
			    regline, (colnr_T)(reginput - regline)) + 1, scan))
		return FALSE;
	    break;

	  case BOW:	/* \<word; reginput points to w */
	    if (c == NUL)	/* Can't match at end of line */
		return FALSE;
#ifdef FEAT_MBYTE
	    if (has_mbyte)
	    {
		int this_class;

		/* Get class of current and previous char (if it exists). */
                this_class = mb_get_class(reginput);
		if (this_class <= 1)
		    return FALSE;	/* not on a word at all */
		if (reg_prev_class() == this_class)
		    return FALSE;	/* previous char is in same word */
	    }
#endif
	    else
	    {
		if (!vim_iswordc(c)
			|| (reginput > regline && vim_iswordc(reginput[-1])))
		    return FALSE;
	    }
	    break;

	  case EOW:	/* word\>; reginput points after d */
	    if (reginput == regline)    /* Can't match at start of line */
		return FALSE;
#ifdef FEAT_MBYTE
	    if (has_mbyte)
	    {
		int this_class, prev_class;

		/* Get class of current and previous char (if it exists). */
                this_class = mb_get_class(reginput);
		prev_class = reg_prev_class();
		if (this_class == prev_class)
		    return FALSE;
		if (prev_class == 0 || prev_class == 1)
		    return FALSE;
	    }
	    else
#endif
	    {
		if (!vim_iswordc(reginput[-1]))
		    return FALSE;
		if (reginput[0] != NUL && vim_iswordc(c))
		    return FALSE;
	    }
	    break; /* Matched with EOW */

	  case ANY:
	    if (c == NUL)
		return FALSE;
	    ADVANCE_REGINPUT();
	    break;

	  case IDENT:
	    if (!vim_isIDc(c))
		return FALSE;
	    ADVANCE_REGINPUT();
	    break;

	  case SIDENT:
	    if (isdigit(*reginput) || !vim_isIDc(c))
		return FALSE;
	    ADVANCE_REGINPUT();
	    break;

	  case KWORD:
	    if (!vim_iswordp(reginput))
		return FALSE;
	    ADVANCE_REGINPUT();
	    break;

	  case SKWORD:
	    if (isdigit(*reginput) || !vim_iswordp(reginput))
		return FALSE;
	    ADVANCE_REGINPUT();
	    break;

	  case FNAME:
	    if (!vim_isfilec(c))
		return FALSE;
	    ADVANCE_REGINPUT();
	    break;

	  case SFNAME:
	    if (isdigit(*reginput) || !vim_isfilec(c))
		return FALSE;
	    ADVANCE_REGINPUT();
	    break;

	  case PRINT:
	    if (ptr2cells(reginput) != 1)
		return FALSE;
	    ADVANCE_REGINPUT();
	    break;

	  case SPRINT:
	    if (isdigit(*reginput) || ptr2cells(reginput) != 1)
		return FALSE;
	    ADVANCE_REGINPUT();
	    break;

	  case WHITE:
	    if (!vim_iswhite(c))
		return FALSE;
	    ADVANCE_REGINPUT();
	    break;

	  case NWHITE:
	    if (c == NUL || vim_iswhite(c))
		return FALSE;
	    ADVANCE_REGINPUT();
	    break;

	  case DIGIT:
	    if (!ri_digit(c))
		return FALSE;
	    ADVANCE_REGINPUT();
	    break;

	  case NDIGIT:
	    if (c == NUL || ri_digit(c))
		return FALSE;
	    ADVANCE_REGINPUT();
	    break;

	  case HEX:
	    if (!ri_hex(c))
		return FALSE;
	    ADVANCE_REGINPUT();
	    break;

	  case NHEX:
	    if (c == NUL || ri_hex(c))
		return FALSE;
	    ADVANCE_REGINPUT();
	    break;

	  case OCTAL:
	    if (!ri_octal(c))
		return FALSE;
	    ADVANCE_REGINPUT();
	    break;

	  case NOCTAL:
	    if (c == NUL || ri_octal(c))
		return FALSE;
	    ADVANCE_REGINPUT();
	    break;

	  case WORD:
	    if (!ri_word(c))
		return FALSE;
	    ADVANCE_REGINPUT();
	    break;

	  case NWORD:
	    if (c == NUL || ri_word(c))
		return FALSE;
	    ADVANCE_REGINPUT();
	    break;

	  case HEAD:
	    if (!ri_head(c))
		return FALSE;
	    ADVANCE_REGINPUT();
	    break;

	  case NHEAD:
	    if (c == NUL || ri_head(c))
		return FALSE;
	    ADVANCE_REGINPUT();
	    break;

	  case ALPHA:
	    if (!ri_alpha(c))
		return FALSE;
	    ADVANCE_REGINPUT();
	    break;

	  case NALPHA:
	    if (c == NUL || ri_alpha(c))
		return FALSE;
	    ADVANCE_REGINPUT();
	    break;

	  case LOWER:
	    if (!ri_lower(c))
		return FALSE;
	    ADVANCE_REGINPUT();
	    break;

	  case NLOWER:
	    if (c == NUL || ri_lower(c))
		return FALSE;
	    ADVANCE_REGINPUT();
	    break;

	  case UPPER:
	    if (!ri_upper(c))
		return FALSE;
	    ADVANCE_REGINPUT();
	    break;

	  case NUPPER:
	    if (c == NUL || ri_upper(c))
		return FALSE;
	    ADVANCE_REGINPUT();
	    break;

	  case EXACTLY:
	    {
		int	len;
		char_u	*opnd;

		opnd = OPERAND(scan);
		/* Inline the first byte, for speed. */
		if (*opnd != *reginput
			&& (!ireg_ic || TO_LOWER(*opnd) != TO_LOWER(*reginput)))
		    return FALSE;
		if (opnd[1] == NUL)
		    ++reginput;		/* matched a single char */
		else
		{
		    len = STRLEN(opnd);
		    /* Need to match first byte again for multi-byte. */
		    if (cstrncmp(opnd, reginput, len) != 0)
			return FALSE;
#ifdef FEAT_MBYTE
		    /* Check for following composing character. */
		    if (enc_utf8 && utf_iscomposing(
					       utf_ptr2char(reginput + len)))
			return FALSE;
#endif
		    reginput += len;
		}
	    }
	    break;

	  case ANYOF:
	  case ANYBUT:
	    if (c == NUL)
		return FALSE;
	    if ((cstrchr(OPERAND(scan), c) == NULL) == (op == ANYOF))
		return FALSE;
	    ADVANCE_REGINPUT();
	    break;

#ifdef FEAT_MBYTE
	  case MULTIBYTECODE:
	    if (has_mbyte)
	    {
		int	i, len;
		char_u	*opnd;

		opnd = OPERAND(scan);
		/* Safety check (just in case 'encoding' was changed since
		 * compiling the program). */
		if ((len = (*mb_ptr2len_check)(opnd)) < 2)
		    return FALSE;
		for (i = 0; i < len; ++i)
		    if (opnd[i] != reginput[i])
			return FALSE;
		reginput += len;
	    }
	    else
		return FALSE;
	    break;
#endif

	  case NOTHING:
	    break;

	  case BACK:
	    break;

	  case MOPEN + 0:   /* Match start: \zs */
	  case MOPEN + 1:   /* \( */
	  case MOPEN + 2:
	  case MOPEN + 3:
	  case MOPEN + 4:
	  case MOPEN + 5:
	  case MOPEN + 6:
	  case MOPEN + 7:
	  case MOPEN + 8:
	  case MOPEN + 9:
	    {
		int		no;
		save_se_T	save;

		no = op - MOPEN;
		cleanup_subexpr();
		save_se(&save, &reg_startpos[no], &reg_startp[no]);

		if (regmatch(next))
		    return TRUE;

		restore_se(&save, &reg_startpos[no], &reg_startp[no]);
		return FALSE;
	    }
	    /* break; Not Reached */

	  case NOPEN:	    /* \%( */
	  case NCLOSE:	    /* \) after \%( */
		if (regmatch(next))
		    return TRUE;
		return FALSE;
		/* break; Not Reached */

#ifdef FEAT_SYN_HL
	  case ZOPEN + 1:
	  case ZOPEN + 2:
	  case ZOPEN + 3:
	  case ZOPEN + 4:
	  case ZOPEN + 5:
	  case ZOPEN + 6:
	  case ZOPEN + 7:
	  case ZOPEN + 8:
	  case ZOPEN + 9:
	    {
		int		no;
		save_se_T	save;

		no = op - ZOPEN;
		cleanup_zsubexpr();
		save_se(&save, &reg_startzpos[no], &reg_startzp[no]);

		if (regmatch(next))
		    return TRUE;

		restore_se(&save, &reg_startzpos[no], &reg_startzp[no]);
		return FALSE;
	    }
	    /* break; Not Reached */
#endif

	  case MCLOSE + 0:  /* Match end: \ze */
	  case MCLOSE + 1:  /* \) */
	  case MCLOSE + 2:
	  case MCLOSE + 3:
	  case MCLOSE + 4:
	  case MCLOSE + 5:
	  case MCLOSE + 6:
	  case MCLOSE + 7:
	  case MCLOSE + 8:
	  case MCLOSE + 9:
	    {
		int		no;
		save_se_T	save;

		no = op - MCLOSE;
		cleanup_subexpr();
		save_se(&save, &reg_endpos[no], &reg_endp[no]);

		if (regmatch(next))
		    return TRUE;

		restore_se(&save, &reg_endpos[no], &reg_endp[no]);
		return FALSE;
	    }
	    /* break; Not Reached */

#ifdef FEAT_SYN_HL
	  case ZCLOSE + 1:  /* \) after \z( */
	  case ZCLOSE + 2:
	  case ZCLOSE + 3:
	  case ZCLOSE + 4:
	  case ZCLOSE + 5:
	  case ZCLOSE + 6:
	  case ZCLOSE + 7:
	  case ZCLOSE + 8:
	  case ZCLOSE + 9:
	    {
		int		no;
		save_se_T	save;

		no = op - ZCLOSE;
		cleanup_zsubexpr();
		save_se(&save, &reg_endzpos[no], &reg_endzp[no]);

		if (regmatch(next))
		    return TRUE;

		restore_se(&save, &reg_endzpos[no], &reg_endzp[no]);
		return FALSE;
	    }
	    /* break; Not Reached */
#endif

	  case BACKREF + 1:
	  case BACKREF + 2:
	  case BACKREF + 3:
	  case BACKREF + 4:
	  case BACKREF + 5:
	  case BACKREF + 6:
	  case BACKREF + 7:
	  case BACKREF + 8:
	  case BACKREF + 9:
	    {
		int		no;
		int		len;
		linenr_T	clnum;
		colnr_T		ccol;
		char_u		*p;

		no = op - BACKREF;
		cleanup_subexpr();
		if (!REG_MULTI)		/* Single-line regexp */
		{
		    if (reg_endp[no] == NULL)
		    {
			/* Backref was not set: Match an empty string. */
			len = 0;
		    }
		    else
		    {
			/* Compare current input with back-ref in the same
			 * line. */
			len = reg_endp[no] - reg_startp[no];
			if (cstrncmp(reg_startp[no], reginput, len) != 0)
			    return FALSE;
		    }
		}
		else				/* Multi-line regexp */
		{
		    if (reg_endpos[no].lnum < 0)
		    {
			/* Backref was not set: Match an empty string. */
			len = 0;
		    }
		    else
		    {
			if (reg_startpos[no].lnum == reglnum
				&& reg_endpos[no].lnum == reglnum)
			{
			    /* Compare back-ref within the current line. */
			    len = reg_endpos[no].col - reg_startpos[no].col;
			    if (cstrncmp(regline + reg_startpos[no].col,
							  reginput, len) != 0)
				return FALSE;
			}
			else
			{
			    /* Messy situation: Need to compare between two
			     * lines. */
			    ccol = reg_startpos[no].col;
			    clnum = reg_startpos[no].lnum;
			    for (;;)
			    {
				/* Since getting one line may invalidate
				 * the other, need to make copy.  Slow! */
				if (regline != reg_tofree)
				{
				    len = STRLEN(regline);
				    if (reg_tofree == NULL
						 || len >= (int)reg_tofreelen)
				    {
					len += 50;	/* get some extra */
					vim_free(reg_tofree);
					reg_tofree = alloc(len);
					if (reg_tofree == NULL)
					    return FALSE; /* out of memory! */
					reg_tofreelen = len;
				    }
				    STRCPY(reg_tofree, regline);
				    reginput = reg_tofree
						       + (reginput - regline);
				    regline = reg_tofree;
				}

				/* Get the line to compare with. */
				p = reg_getline(clnum);
				if (clnum == reg_endpos[no].lnum)
				    len = reg_endpos[no].col - ccol;
				else
				    len = STRLEN(p + ccol);

				if (cstrncmp(p + ccol, reginput, len) != 0)
				    return FALSE;	/* doesn't match */
				if (clnum == reg_endpos[no].lnum)
				    break;		/* match and at end! */
				if (reglnum == reg_maxline)
				    return FALSE;	/* text too short */

				/* Advance to next line. */
				reg_nextline();
				++clnum;
				ccol = 0;
				if (got_int)
				    return FALSE;
			    }

			    /* found a match!  Note that regline may now point
			     * to a copy of the line, that should not matter. */
			}
		    }
		}

		/* Matched the backref, skip over it. */
		reginput += len;
	    }
	    break;

#ifdef FEAT_SYN_HL
	  case ZREF + 1:
	  case ZREF + 2:
	  case ZREF + 3:
	  case ZREF + 4:
	  case ZREF + 5:
	  case ZREF + 6:
	  case ZREF + 7:
	  case ZREF + 8:
	  case ZREF + 9:
	    {
		int	no;
		int	len;

		cleanup_zsubexpr();
		no = op - ZREF;
		if (re_extmatch_in != NULL
			&& re_extmatch_in->matches[no] != NULL)
		{
		    len = (int)STRLEN(re_extmatch_in->matches[no]);
		    if (cstrncmp(re_extmatch_in->matches[no],
							  reginput, len) != 0)
			return FALSE;
		    reginput += len;
		}
		else
		{
		    /* Backref was not set: Match an empty string. */
		}
	    }
	    break;
#endif

	  case BRANCH:
	    {
		if (OP(next) != BRANCH) /* No choice. */
		    next = OPERAND(scan);	/* Avoid recursion. */
		else
		{
		    regsave_T	save;

		    do
		    {
			reg_save(&save);
			if (regmatch(OPERAND(scan)))
			    return TRUE;
			reg_restore(&save);
			scan = regnext(scan);
		    } while (scan != NULL && OP(scan) == BRANCH);
		    return FALSE;
		    /* NOTREACHED */
		}
	    }
	    break;

	  case BRACE_LIMITS:
	    {
		int	no;

		if (OP(next) == BRACE_SIMPLE)
		{
		    bl_minval = OPERAND_MIN(scan);
		    bl_maxval = OPERAND_MAX(scan);
		}
		else if (OP(next) >= BRACE_COMPLEX
			&& OP(next) < BRACE_COMPLEX + 10)
		{
		    no = OP(next) - BRACE_COMPLEX;
		    brace_min[no] = OPERAND_MIN(scan);
		    brace_max[no] = OPERAND_MAX(scan);
		    brace_count[no] = 0;
		}
		else
		{
		    EMSG(_(e_internal));	    /* Shouldn't happen */
		    return FALSE;
		}
	    }
	    break;

	  case BRACE_COMPLEX + 0:
	  case BRACE_COMPLEX + 1:
	  case BRACE_COMPLEX + 2:
	  case BRACE_COMPLEX + 3:
	  case BRACE_COMPLEX + 4:
	  case BRACE_COMPLEX + 5:
	  case BRACE_COMPLEX + 6:
	  case BRACE_COMPLEX + 7:
	  case BRACE_COMPLEX + 8:
	  case BRACE_COMPLEX + 9:
	    {
		int		no;
		regsave_T	save;

		no = op - BRACE_COMPLEX;
		++brace_count[no];

		/* If not matched enough times yet, try one more */
		if (brace_count[no] <= (brace_min[no] <= brace_max[no]
				    ? brace_min[no] : brace_max[no]))
		{
		    reg_save(&save);
		    if (regmatch(OPERAND(scan)))
			return TRUE;
		    reg_restore(&save);
		    --brace_count[no];	/* failed, decrement match count */
		    return FALSE;
		}

		/* If matched enough times, may try matching some more */
		if (brace_min[no] <= brace_max[no])
		{
		    /* Range is the normal way around, use longest match */
		    if (brace_count[no] <= brace_max[no])
		    {
			reg_save(&save);
			if (regmatch(OPERAND(scan)))
			    return TRUE;	/* matched some more times */
			reg_restore(&save);
			--brace_count[no];  /* matched just enough times */
			/* continue with the items after \{} */
		    }
		}
		else
		{
		    /* Range is backwards, use shortest match first */
		    if (brace_count[no] <= brace_min[no])
		    {
			reg_save(&save);
			if (regmatch(next))
			    return TRUE;
			reg_restore(&save);
			next = OPERAND(scan);
			/* must try to match one more item */
		    }
		}
	    }
	    break;

	  case BRACE_SIMPLE:
	  case STAR:
	  case PLUS:
	    {
		int		nextb;		/* next byte */
		int		nextb_ic;	/* next byte reverse case */
		long		count;
		regsave_T	save;
		long		minval;
		long		maxval;

		/*
		 * Lookahead to avoid useless match attempts when we know
		 * what character comes next.
		 */
		if (OP(next) == EXACTLY)
		{
		    nextb = *OPERAND(next);
		    if (ireg_ic)
		    {
			if (isupper(nextb))
			    nextb_ic = TO_LOWER(nextb);
			else
			    nextb_ic = TO_UPPER(nextb);
		    }
		    else
			nextb_ic = nextb;
		}
		else
		{
		    nextb = NUL;
		    nextb_ic = NUL;
		}
		if (op != BRACE_SIMPLE)
		{
		    minval = (op == STAR) ? 0 : 1;
		    maxval = MAX_LIMIT;
		}
		else
		{
		    minval = bl_minval;
		    maxval = bl_maxval;
		}

		/*
		 * When maxval > minval, try matching as much as possible, up
		 * to maxval.  When maxval < minval, try matching at least the
		 * minimal number (since the range is backwards, that's also
		 * maxval!).
		 */
		count = regrepeat(OPERAND(scan), maxval);
		if (got_int)
		    return FALSE;
		if (minval <= maxval)
		{
		    /* Range is the normal way around, use longest match */
		    while (count >= minval)
		    {
			/* If it could match, try it. */
			if (nextb == NUL || *reginput == nextb
						    || *reginput == nextb_ic)
			{
			    reg_save(&save);
			    if (regmatch(next))
				return TRUE;
			    reg_restore(&save);
			}
			/* Couldn't or didn't match -- back up one char. */
			--count;
			if (reginput == regline && count >= minval)
			{
			    /* backup to last char of previous line */
			    --reglnum;
			    regline = reg_getline(reglnum);
			    reginput = regline + STRLEN(regline);
			    fast_breakcheck();
			    if (got_int)
				return FALSE;
			}
			else
			{
			    --reginput;
#ifdef FEAT_MBYTE
			    if (has_mbyte)
				reginput -= (*mb_head_off)(regline, reginput);
#endif
			}
		    }
		}
		else
		{
		    /* Range is backwards, use shortest match first.
		     * Careful: maxval and minval are exchanged! */
		    if (count < maxval)
			return FALSE;
		    for (;;)
		    {
			/* If it could work, try it. */
			if (nextb == NUL || *reginput == nextb
						    || *reginput == nextb_ic)
			{
			    reg_save(&save);
			    if (regmatch(next))
				return TRUE;
			    reg_restore(&save);
			}
			/* Couldn't or didn't match: try advancing one char. */
			if (count == minval
					 || regrepeat(OPERAND(scan), 1L) == 0)
			    break;
			++count;
			if (got_int)
			    return FALSE;
		    }
		}
		return FALSE;
	    }
	    /* break; Not Reached */

	  case NOMATCH:
	    {
		regsave_T	save;

		/* If the operand matches, we fail.  Otherwise backup and
		 * continue with the next item. */
		reg_save(&save);
		if (regmatch(OPERAND(scan)))
		    return FALSE;
		reg_restore(&save);
	    }
	    break;

	  case MATCH:
	  case SUBPAT:
	    {
		regsave_T	save;

		/* If the operand doesn't match, we fail.  Otherwise backup
		 * and continue with the next item. */
		reg_save(&save);
		if (!regmatch(OPERAND(scan)))
		    return FALSE;
		if (op == MATCH)	    /* zero-width */
		    reg_restore(&save);
	    }
	    break;

	  case BEHIND:
	  case NOBEHIND:
	    {
		regsave_T	save_before, save_after, save_start;
		int		needmatch = (op == BEHIND);
		long		col;

		/*
		 * Look back in the input of the operand matches or not. This
		 * must be done at every position in the input and checking if
		 * the match ends at the current position.
		 * First check if the next item matches, that's probably
		 * faster.
		 */
		reg_save(&save_before);
		if (regmatch(next))
		{
		    /* save the position after the found match for next */
		    reg_save(&save_after);

		    /* start looking for a match with operand at the current
		     * postion.  Go back one character until we find the
		     * result, hitting the start of the line or the previous
		     * line (for multi-line matching).  */
		    save_start = save_before;
		    for (col = 0; ; ++col)
		    {
			reg_restore(&save_start);
			if (regmatch(OPERAND(scan))
				&& reg_save_equal(&save_before))
			{
			    /* found a match that ends where "next" started */
			    if (needmatch)
			    {
				reg_restore(&save_after);
				return TRUE;
			    }
			    return FALSE;
			}
			/*
			 * No match: Go back one character.  May go to
			 * previous line once.
			 */
			if (REG_MULTI)
			{
			    if (save_start.rs_u.pos.col == 0)
			    {
				if (save_start.rs_u.pos.lnum
						< save_before.rs_u.pos.lnum
					|| reg_getline(
					    --save_start.rs_u.pos.lnum) == NULL)
				    break;
				reg_restore(&save_start);
				save_start.rs_u.pos.col = STRLEN(regline);
			    }
			    else
				--save_start.rs_u.pos.col;
			}
			else
			{
			    if (save_start.rs_u.ptr == regline)
				break;
			    --save_start.rs_u.ptr;
			}
		    }
		    /* NOBEHIND succeeds when no match was found */
		    if (!needmatch)
		    {
			reg_restore(&save_after);
			return TRUE;
		    }
		}
		return FALSE;
	    }

	  case NEWL:
	    if (c != NUL || reglnum == reg_maxline)
		return FALSE;
	    reg_nextline();
	    break;

	  case END:
	    return TRUE;	/* Success! */

	  default:
	    EMSG(_(e_re_corr));
#ifdef DEBUG
	    printf("Illegal op code %d\n", op);
#endif
	    return FALSE;
	  }
	}

	scan = next;
    }

    /*
     * We get here only if there's trouble -- normally "case END" is the
     * terminating point.
     */
    EMSG(_(e_re_corr));
#ifdef DEBUG
    printf("Premature EOL\n");
#endif
    return FALSE;
}

#ifdef FEAT_MBYTE
# define ADVANCE_P(x) if (has_mbyte) x += (*mb_ptr2len_check)(x); else ++x
#else
# define ADVANCE_P(x) ++x
#endif

/*
 * regrepeat - repeatedly match something simple, return how many.
 * Advances reginput (and reglnum) to just after the matched chars.
 */
    static int
regrepeat(p, maxcount)
    char_u	*p;
    long	maxcount;   /* maximum number of matches allowed */
{
    long	count = 0;
    char_u	*scan;
    char_u	*opnd;
    int		mask;
    int		testval = 0;

    scan = reginput;	    /* Make local copy of reginput for speed. */
    opnd = OPERAND(p);
    switch (OP(p))
    {
      case ANY:
      case ANY + ADD_NL:
	while (count < maxcount)
	{
	    /* Matching anything means we continue until end-of-line (or
	     * end-of-file for ANY + ADD_NL), only limited by maxcount. */
	    while (*scan != NUL && count < maxcount)
	    {
		++count;
		ADVANCE_P(scan);
	    }
	    if (!WITH_NL(OP(p)) || reglnum == reg_maxline || count == maxcount)
		break;
	    ++count;		/* count the line-break */
	    reg_nextline();
	    scan = reginput;
	    if (got_int)
		break;
	}
	break;

      case IDENT:
      case IDENT + ADD_NL:
	testval = TRUE;
	/*FALLTHROUGH*/
      case SIDENT:
      case SIDENT + ADD_NL:
	while (count < maxcount)
	{
	    if (vim_isIDc(*scan) && (testval || !isdigit(*scan)))
		++scan;
	    else if (*scan == NUL)
	    {
		if (!WITH_NL(OP(p)) || reglnum == reg_maxline)
		    break;
		reg_nextline();
		scan = reginput;
		if (got_int)
		    break;
	    }
	    else
		break;
	    ++count;
	}
	break;

      case KWORD:
      case KWORD + ADD_NL:
	testval = TRUE;
	/*FALLTHROUGH*/
      case SKWORD:
      case SKWORD + ADD_NL:
	while (count < maxcount)
	{
	    if (vim_iswordp(scan) && (testval || !isdigit(*scan)))
	    {
		ADVANCE_P(scan);
	    }
	    else if (*scan == NUL)
	    {
		if (!WITH_NL(OP(p)) || reglnum == reg_maxline)
		    break;
		reg_nextline();
		scan = reginput;
		if (got_int)
		    break;
	    }
	    else
		break;
	    ++count;
	}
	break;

      case FNAME:
      case FNAME + ADD_NL:
	testval = TRUE;
	/*FALLTHROUGH*/
      case SFNAME:
      case SFNAME + ADD_NL:
	while (count < maxcount)
	{
	    if (vim_isfilec(*scan) && (testval || !isdigit(*scan)))
	    {
		ADVANCE_P(scan);
	    }
	    else if (*scan == NUL)
	    {
		if (!WITH_NL(OP(p)) || reglnum == reg_maxline)
		    break;
		reg_nextline();
		scan = reginput;
		if (got_int)
		    break;
	    }
	    else
		break;
	    ++count;
	}
	break;

      case PRINT:
      case PRINT + ADD_NL:
	testval = TRUE;
	/*FALLTHROUGH*/
      case SPRINT:
      case SPRINT + ADD_NL:
	while (count < maxcount)
	{
	    if (*scan == NUL)
	    {
		if (!WITH_NL(OP(p)) || reglnum == reg_maxline)
		    break;
		reg_nextline();
		scan = reginput;
		if (got_int)
		    break;
	    }
	    else if (ptr2cells(scan) == 1 && (testval || !isdigit(*scan)))
	    {
		ADVANCE_P(scan);
	    }
	    else
		break;
	    ++count;
	}
	break;

      case WHITE:
      case WHITE + ADD_NL:
	testval = mask = RI_WHITE;
do_class:
	while (count < maxcount)
	{
#ifdef FEAT_MBYTE
	    int		l;
#endif
	    if (*scan == NUL)
	    {
		if (!WITH_NL(OP(p)) || reglnum == reg_maxline)
		    break;
		reg_nextline();
		scan = reginput;
		if (got_int)
		    break;
	    }
#ifdef FEAT_MBYTE
	    else if (has_mbyte && (l = (*mb_ptr2len_check)(scan)) > 1)
	    {
		if (testval != 0)
		    break;
		scan += l;
	    }
#endif
	    else if ((class_tab[*scan] & mask) == testval)
		++scan;
	    else
		break;
	    ++count;
	}
	break;

      case NWHITE:
      case NWHITE + ADD_NL:
	mask = RI_WHITE;
	goto do_class;
      case DIGIT:
      case DIGIT + ADD_NL:
	testval = mask = RI_DIGIT;
	goto do_class;
      case NDIGIT:
      case NDIGIT + ADD_NL:
	mask = RI_DIGIT;
	goto do_class;
      case HEX:
      case HEX + ADD_NL:
	testval = mask = RI_HEX;
	goto do_class;
      case NHEX:
      case NHEX + ADD_NL:
	mask = RI_HEX;
	goto do_class;
      case OCTAL:
      case OCTAL + ADD_NL:
	testval = mask = RI_OCTAL;
	goto do_class;
      case NOCTAL:
      case NOCTAL + ADD_NL:
	mask = RI_OCTAL;
	goto do_class;
      case WORD:
      case WORD + ADD_NL:
	testval = mask = RI_WORD;
	goto do_class;
      case NWORD:
      case NWORD + ADD_NL:
	mask = RI_WORD;
	goto do_class;
      case HEAD:
      case HEAD + ADD_NL:
	testval = mask = RI_HEAD;
	goto do_class;
      case NHEAD:
      case NHEAD + ADD_NL:
	mask = RI_HEAD;
	goto do_class;
      case ALPHA:
      case ALPHA + ADD_NL:
	testval = mask = RI_ALPHA;
	goto do_class;
      case NALPHA:
      case NALPHA + ADD_NL:
	mask = RI_ALPHA;
	goto do_class;
      case LOWER:
      case LOWER + ADD_NL:
	testval = mask = RI_LOWER;
	goto do_class;
      case NLOWER:
      case NLOWER + ADD_NL:
	mask = RI_LOWER;
	goto do_class;
      case UPPER:
      case UPPER + ADD_NL:
	testval = mask = RI_UPPER;
	goto do_class;
      case NUPPER:
      case NUPPER + ADD_NL:
	mask = RI_UPPER;
	goto do_class;

      case EXACTLY:
	{
	    int	    cu, cl;

	    /* This doesn't do a multi-byte character, because a MULTIBYTECODE
	     * would have been used for it. */
	    if (ireg_ic)
	    {
		cu = TO_UPPER(*opnd);
		cl = TO_LOWER(*opnd);
		while (count < maxcount && (*scan == cu || *scan == cl))
		{
		    count++;
		    scan++;
		}
	    }
	    else
	    {
		cu = *opnd;
		while (count < maxcount && *scan == cu)
		{
		    count++;
		    scan++;
		}
	    }
	    break;
	}

#ifdef FEAT_MBYTE
      case MULTIBYTECODE:
	{
	    int		i, len;

	    /* Safety check (just in case 'encoding' was changed since
	     * compiling the program). */
	    if ((len = (*mb_ptr2len_check)(opnd)) > 1)
		while (count < maxcount)
		{
		    for (i = 0; i < len; ++i)
			if (opnd[i] != scan[i])
			    break;
		    if (i < len)
			break;
		    scan += len;
		    ++count;
		}
	}
	break;
#endif

      case ANYOF:
      case ANYOF + ADD_NL:
	testval = TRUE;
	/*FALLTHROUGH*/

      case ANYBUT:
      case ANYBUT + ADD_NL:
	while (count < maxcount)
	{
#ifdef FEAT_MBYTE
	    int len;
#endif
	    if (*scan == NUL)
	    {
		if (!WITH_NL(OP(p)) || reglnum == reg_maxline)
		    break;
		reg_nextline();
		scan = reginput;
		if (got_int)
		    break;
	    }
#ifdef FEAT_MBYTE
	    else if (has_mbyte && (len = (*mb_ptr2len_check)(scan)) > 1)
	    {
		if ((cstrchr(opnd, (*mb_ptr2char)(scan)) == NULL) == testval)
		    break;
		scan += len;
	    }
#endif
	    else
	    {
		if ((cstrchr(opnd, *scan) == NULL) == testval)
		    break;
		++scan;
	    }
	    ++count;
	}
	break;

      case NEWL:
	while (count < maxcount && *scan == NUL && reglnum < reg_maxline)
	{
	    count++;
	    reg_nextline();
	    scan = reginput;
	    if (got_int)
		break;
	}
	break;

      default:			/* Oh dear.  Called inappropriately. */
	EMSG(_(e_re_corr));
#ifdef DEBUG
	printf("Called regrepeat with op code %d\n", OP(p));
#endif
	break;
    }

    reginput = scan;

    return (int)count;
}

/*
 * regnext - dig the "next" pointer out of a node
 */
    static char_u *
regnext(p)
    char_u  *p;
{
    int	    offset;

    if (p == JUST_CALC_SIZE)
	return NULL;

    offset = NEXT(p);
    if (offset == 0)
	return NULL;

    if (OP(p) == BACK)
	return p - offset;
    else
	return p + offset;
}

/*
 * Check the regexp program for its magic number.
 * Return TRUE if it's wrong.
 */
    static int
prog_magic_wrong()
{
    if (UCHARAT(REG_MULTI
		? reg_mmatch->regprog->program
		: reg_match->regprog->program) != REGMAGIC)
    {
	EMSG(_(e_re_corr));
	return TRUE;
    }
    return FALSE;
}

/*
 * Cleanup the subexpressions, if this wasn't done yet.
 * This construction is used to clear the subexpressions only when they are
 * used (to increase speed).
 */
    static void
cleanup_subexpr()
{
    if (need_clear_subexpr)
    {
	if (REG_MULTI)
	{
	    /* Use 0xff to set lnum to -1 */
	    vim_memset(reg_startpos, 0xff, sizeof(pos_T) * NSUBEXP);
	    vim_memset(reg_endpos, 0xff, sizeof(pos_T) * NSUBEXP);
	}
	else
	{
	    vim_memset(reg_startp, 0, sizeof(char_u *) * NSUBEXP);
	    vim_memset(reg_endp, 0, sizeof(char_u *) * NSUBEXP);
	}
	need_clear_subexpr = FALSE;
    }
}

#ifdef FEAT_SYN_HL
    static void
cleanup_zsubexpr()
{
    if (need_clear_zsubexpr)
    {
	if (REG_MULTI)
	{
	    /* Use 0xff to set lnum to -1 */
	    vim_memset(reg_startzpos, 0xff, sizeof(pos_T) * NSUBEXP);
	    vim_memset(reg_endzpos, 0xff, sizeof(pos_T) * NSUBEXP);
	}
	else
	{
	    vim_memset(reg_startzp, 0, sizeof(char_u *) * NSUBEXP);
	    vim_memset(reg_endzp, 0, sizeof(char_u *) * NSUBEXP);
	}
	need_clear_zsubexpr = FALSE;
    }
}
#endif

/*
 * Advance reglnum, regline and reginput to the next line.
 */
    static void
reg_nextline()
{
    regline = reg_getline(++reglnum);
    reginput = regline;
    fast_breakcheck();
}

/*
 * Save the input line and position in a regsave_T.
 */
    static void
reg_save(save)
    regsave_T	*save;
{
    if (REG_MULTI)
    {
	save->rs_u.pos.col = reginput - regline;
	save->rs_u.pos.lnum = reglnum;
    }
    else
	save->rs_u.ptr = reginput;
}

/*
 * Restore the input line and position from a regsave_T.
 */
    static void
reg_restore(save)
    regsave_T	*save;
{
    if (REG_MULTI)
    {
	if (reglnum != save->rs_u.pos.lnum)
	{
	    /* only call reg_getline() when the line number changed to save
	     * a bit of time */
	    reglnum = save->rs_u.pos.lnum;
	    regline = reg_getline(reglnum);
	}
	reginput = regline + save->rs_u.pos.col;
    }
    else
	reginput = save->rs_u.ptr;
}

/*
 * Return TRUE if current position is equal to saved position.
 */
    static int
reg_save_equal(save)
    regsave_T	*save;
{
    if (REG_MULTI)
	return reglnum == save->rs_u.pos.lnum
				  && reginput == regline + save->rs_u.pos.col;
    return reginput == save->rs_u.ptr;
}

/*
 * Tentatively set the sub-expression start to the current position (after
 * calling regmatch() they will have changed).  Need to save the existing
 * values for when there is no match.
 * Use pointer or position, depending on REG_MULTI.
 */
    static void
save_se(savep, posp, pp)
    save_se_T	*savep;
    pos_T	*posp;
    char_u	**pp;
{
    if (REG_MULTI)
    {
	savep->se_u.pos = *posp;
	posp->lnum = reglnum;
	posp->col = reginput - regline;
    }
    else
    {
	savep->se_u.ptr = *pp;
	*pp = reginput;
    }
}

/*
 * We were wrong, restore the sub-expressions.
 */
    static void
restore_se(savep, posp, pp)
    save_se_T	*savep;
    pos_T	*posp;
    char_u	**pp;
{
    if (REG_MULTI)
	*posp = savep->se_u.pos;
    else
	*pp = savep->se_u.ptr;
}

/*
 * Compare a number with the operand of RE_LNUM, RE_COL or RE_VCOL.
 */
    static int
re_num_cmp(val, scan)
    long_u	val;
    char_u	*scan;
{
    long_u  n = OPERAND_MIN(scan);

    if (OPERAND_CMP(scan) == '>')
	return val > n;
    if (OPERAND_CMP(scan) == '<')
	return val < n;
    return val == n;
}


#ifdef DEBUG

/*
 * regdump - dump a regexp onto stdout in vaguely comprehensible form
 */
    static void
regdump(pattern, r)
    char_u	*pattern;
    regprog_T	*r;
{
    char_u  *s;
    int	    op = EXACTLY;	/* Arbitrary non-END op. */
    char_u  *next;
    char_u  *end = NULL;

    printf("\r\nregcomp(%s):\r\n", pattern);

    s = r->program + 1;
    /*
     * Loop until we find the END that isn't before a referred next (an END
     * can also appear in a NOMATCH operand).
     */
    while (op != END || s <= end)
    {
	op = OP(s);
	printf("%2d%s", (int)(s - r->program), regprop(s)); /* Where, what. */
	next = regnext(s);
	if (next == NULL)	/* Next ptr. */
	    printf("(0)");
	else
	    printf("(%d)", (int)((s - r->program) + (next - s)));
	if (end < next)
	    end = next;
	if (op == BRACE_LIMITS)
	{
	    /* Two short ints */
	    printf(" minval %ld, maxval %ld", OPERAND_MIN(s), OPERAND_MAX(s));
	    s += 8;
	}
	s += 3;
	if (op == ANYOF || op == ANYOF + ADD_NL
		|| op == ANYBUT || op == ANYBUT + ADD_NL
		|| op == EXACTLY)
	{
	    /* Literal string, where present. */
	    while (*s != NUL)
		printf("%c", *s++);
	    s++;
	}
	printf("\r\n");
    }

    /* Header fields of interest. */
    if (r->regstart != NUL)
	printf("start `%s' 0x%x; ", r->regstart < 256
		? (char *)transchar(r->regstart)
		: "multibyte", r->regstart);
    if (r->reganch)
	printf("anchored; ");
    if (r->regmust != NULL)
	printf("must have \"%s\"", r->regmust);
    printf("\r\n");
}

/*
 * regprop - printable representation of opcode
 */
    static char_u *
regprop(op)
    char_u	   *op;
{
    char_u	    *p;
    static char_u   buf[50];

    (void) strcpy(buf, ":");

    switch (OP(op))
    {
      case BOL:
	p = "BOL";
	break;
      case EOL:
	p = "EOL";
	break;
      case RE_BOF:
	p = "BOF";
	break;
      case RE_EOF:
	p = "EOF";
	break;
      case CURSOR:
	p = "CURSOR";
	break;
      case RE_LNUM:
	p = "RE_LNUM";
	break;
      case RE_COL:
	p = "RE_COL";
	break;
      case RE_VCOL:
	p = "RE_VCOL";
	break;
      case BOW:
	p = "BOW";
	break;
      case EOW:
	p = "EOW";
	break;
      case ANY:
	p = "ANY";
	break;
      case ANY + ADD_NL:
	p = "ANY+NL";
	break;
      case ANYOF:
	p = "ANYOF";
	break;
      case ANYOF + ADD_NL:
	p = "ANYOF+NL";
	break;
      case ANYBUT:
	p = "ANYBUT";
	break;
      case ANYBUT + ADD_NL:
	p = "ANYBUT+NL";
	break;
      case IDENT:
	p = "IDENT";
	break;
      case IDENT + ADD_NL:
	p = "IDENT+NL";
	break;
      case SIDENT:
	p = "SIDENT";
	break;
      case SIDENT + ADD_NL:
	p = "SIDENT+NL";
	break;
      case KWORD:
	p = "KWORD";
	break;
      case KWORD + ADD_NL:
	p = "KWORD+NL";
	break;
      case SKWORD:
	p = "SKWORD";
	break;
      case SKWORD + ADD_NL:
	p = "SKWORD+NL";
	break;
      case FNAME:
	p = "FNAME";
	break;
      case FNAME + ADD_NL:
	p = "FNAME+NL";
	break;
      case SFNAME:
	p = "SFNAME";
	break;
      case SFNAME + ADD_NL:
	p = "SFNAME+NL";
	break;
      case PRINT:
	p = "PRINT";
	break;
      case PRINT + ADD_NL:
	p = "PRINT+NL";
	break;
      case SPRINT:
	p = "SPRINT";
	break;
      case SPRINT + ADD_NL:
	p = "SPRINT+NL";
	break;
      case WHITE:
	p = "WHITE";
	break;
      case WHITE + ADD_NL:
	p = "WHITE+NL";
	break;
      case NWHITE:
	p = "NWHITE";
	break;
      case NWHITE + ADD_NL:
	p = "NWHITE+NL";
	break;
      case DIGIT:
	p = "DIGIT";
	break;
      case DIGIT + ADD_NL:
	p = "DIGIT+NL";
	break;
      case NDIGIT:
	p = "NDIGIT";
	break;
      case NDIGIT + ADD_NL:
	p = "NDIGIT+NL";
	break;
      case HEX:
	p = "HEX";
	break;
      case HEX + ADD_NL:
	p = "HEX+NL";
	break;
      case NHEX:
	p = "NHEX";
	break;
      case NHEX + ADD_NL:
	p = "NHEX+NL";
	break;
      case OCTAL:
	p = "OCTAL";
	break;
      case OCTAL + ADD_NL:
	p = "OCTAL+NL";
	break;
      case NOCTAL:
	p = "NOCTAL";
	break;
      case NOCTAL + ADD_NL:
	p = "NOCTAL+NL";
	break;
      case WORD:
	p = "WORD";
	break;
      case WORD + ADD_NL:
	p = "WORD+NL";
	break;
      case NWORD:
	p = "NWORD";
	break;
      case NWORD + ADD_NL:
	p = "NWORD+NL";
	break;
      case HEAD:
	p = "HEAD";
	break;
      case HEAD + ADD_NL:
	p = "HEAD+NL";
	break;
      case NHEAD:
	p = "NHEAD";
	break;
      case NHEAD + ADD_NL:
	p = "NHEAD+NL";
	break;
      case ALPHA:
	p = "ALPHA";
	break;
      case ALPHA + ADD_NL:
	p = "ALPHA+NL";
	break;
      case NALPHA:
	p = "NALPHA";
	break;
      case NALPHA + ADD_NL:
	p = "NALPHA+NL";
	break;
      case LOWER:
	p = "LOWER";
	break;
      case LOWER + ADD_NL:
	p = "LOWER+NL";
	break;
      case NLOWER:
	p = "NLOWER";
	break;
      case NLOWER + ADD_NL:
	p = "NLOWER+NL";
	break;
      case UPPER:
	p = "UPPER";
	break;
      case UPPER + ADD_NL:
	p = "UPPER+NL";
	break;
      case NUPPER:
	p = "NUPPER";
	break;
      case NUPPER + ADD_NL:
	p = "NUPPER+NL";
	break;
      case BRANCH:
	p = "BRANCH";
	break;
      case EXACTLY:
	p = "EXACTLY";
	break;
      case NOTHING:
	p = "NOTHING";
	break;
      case BACK:
	p = "BACK";
	break;
      case END:
	p = "END";
	break;
      case MOPEN + 0:
	p = "MATCH START";
	break;
      case MOPEN + 1:
      case MOPEN + 2:
      case MOPEN + 3:
      case MOPEN + 4:
      case MOPEN + 5:
      case MOPEN + 6:
      case MOPEN + 7:
      case MOPEN + 8:
      case MOPEN + 9:
	sprintf(buf + STRLEN(buf), "MOPEN%d", OP(op) - MOPEN);
	p = NULL;
	break;
      case MCLOSE + 0:
	p = "MATCH END";
	break;
      case MCLOSE + 1:
      case MCLOSE + 2:
      case MCLOSE + 3:
      case MCLOSE + 4:
      case MCLOSE + 5:
      case MCLOSE + 6:
      case MCLOSE + 7:
      case MCLOSE + 8:
      case MCLOSE + 9:
	sprintf(buf + STRLEN(buf), "MCLOSE%d", OP(op) - MCLOSE);
	p = NULL;
	break;
      case BACKREF + 1:
      case BACKREF + 2:
      case BACKREF + 3:
      case BACKREF + 4:
      case BACKREF + 5:
      case BACKREF + 6:
      case BACKREF + 7:
      case BACKREF + 8:
      case BACKREF + 9:
	sprintf(buf + STRLEN(buf), "BACKREF%d", OP(op) - BACKREF);
	p = NULL;
	break;
      case NOPEN:
	p = "NOPEN";
	break;
      case NCLOSE:
	p = "NCLOSE";
	break;
#ifdef FEAT_SYN_HL
      case ZOPEN + 1:
      case ZOPEN + 2:
      case ZOPEN + 3:
      case ZOPEN + 4:
      case ZOPEN + 5:
      case ZOPEN + 6:
      case ZOPEN + 7:
      case ZOPEN + 8:
      case ZOPEN + 9:
	sprintf(buf + STRLEN(buf), "ZOPEN%d", OP(op) - ZOPEN);
	p = NULL;
	break;
      case ZCLOSE + 1:
      case ZCLOSE + 2:
      case ZCLOSE + 3:
      case ZCLOSE + 4:
      case ZCLOSE + 5:
      case ZCLOSE + 6:
      case ZCLOSE + 7:
      case ZCLOSE + 8:
      case ZCLOSE + 9:
	sprintf(buf + STRLEN(buf), "ZCLOSE%d", OP(op) - ZCLOSE);
	p = NULL;
	break;
      case ZREF + 1:
      case ZREF + 2:
      case ZREF + 3:
      case ZREF + 4:
      case ZREF + 5:
      case ZREF + 6:
      case ZREF + 7:
      case ZREF + 8:
      case ZREF + 9:
	sprintf(buf + STRLEN(buf), "ZREF%d", OP(op) - ZREF);
	p = NULL;
	break;
#endif
      case STAR:
	p = "STAR";
	break;
      case PLUS:
	p = "PLUS";
	break;
      case NOMATCH:
	p = "NOMATCH";
	break;
      case MATCH:
	p = "MATCH";
	break;
      case BEHIND:
	p = "BEHIND";
	break;
      case NOBEHIND:
	p = "NOBEHIND";
	break;
      case SUBPAT:
	p = "SUBPAT";
	break;
      case BRACE_LIMITS:
	p = "BRACE_LIMITS";
	break;
      case BRACE_SIMPLE:
	p = "BRACE_SIMPLE";
	break;
      case BRACE_COMPLEX + 0:
      case BRACE_COMPLEX + 1:
      case BRACE_COMPLEX + 2:
      case BRACE_COMPLEX + 3:
      case BRACE_COMPLEX + 4:
      case BRACE_COMPLEX + 5:
      case BRACE_COMPLEX + 6:
      case BRACE_COMPLEX + 7:
      case BRACE_COMPLEX + 8:
      case BRACE_COMPLEX + 9:
	sprintf(buf + STRLEN(buf), "BRACE_COMPLEX%d", OP(op) - BRACE_COMPLEX);
	p = NULL;
	break;
#ifdef FEAT_MBYTE
      case MULTIBYTECODE:
	p = "MULTIBYTECODE";
	break;
#endif
      case NEWL:
	p = "NEWL";
	break;
      default:
	sprintf(buf + STRLEN(buf), "corrupt %d", OP(op));
	p = NULL;
	break;
    }
    if (p != NULL)
	(void) strcat(buf, p);
    return buf;
}
#endif

/*
 * Compare two strings, ignore case if ireg_ic set.
 * Return 0 if strings match, non-zero otherwise.
 */
    static int
cstrncmp(s1, s2, n)
    char_u	*s1, *s2;
    int		n;
{
    if (!ireg_ic)
	return STRNCMP(s1, s2, n);
    return STRNICMP(s1, s2, n);
}

/*
 * cstrchr: This function is used a lot for simple searches, keep it fast!
 */
    static char_u *
cstrchr(s, c)
    char_u	*s;
    int		c;
{
    char_u	*p;
    int		cc;

    if (!ireg_ic
#ifdef FEAT_MBYTE
	    || mb_char2len(c) > 1
#endif
	    )
	return vim_strchr(s, c);

    /* tolower() and toupper() can be slow, comparing twice should be a lot
     * faster (esp. when using MS Visual C++!) */
    if (isupper(c))
	cc = TO_LOWER(c);
    else if (islower(c))
	cc = TO_UPPER(c);
    else
	return vim_strchr(s, c);

    for (p = s; *p; ++p)
    {
	if (*p == c || *p == cc)
	    return p;
#ifdef FEAT_MBYTE
	if (has_mbyte)
	    p += (*mb_ptr2len_check)(p) - 1;
#endif
    }
    return NULL;
}

/***************************************************************
 *		      regsub stuff			       *
 ***************************************************************/

/* This stuff below really confuses cc on an SGI -- webb */
#ifdef __sgi
# undef __ARGS
# define __ARGS(x)  ()
#endif

/*
 * We should define ftpr as a pointer to a function returning a pointer to
 * a function returning a pointer to a function ...
 * This is impossible, so we declare a pointer to a function returning a
 * pointer to a function returning void. This should work for all compilers.
 */
typedef void (*(*fptr) __ARGS((char_u *, int)))();

static fptr do_upper __ARGS((char_u *, int));
static fptr do_Upper __ARGS((char_u *, int));
static fptr do_lower __ARGS((char_u *, int));
static fptr do_Lower __ARGS((char_u *, int));

static int vim_regsub_both __ARGS((char_u *source, char_u *dest, int copy, int magic, int backslash));

    static fptr
do_upper(d, c)
    char_u *d;
    int c;
{
    *d = TO_UPPER(c);

    return (fptr)NULL;
}

    static fptr
do_Upper(d, c)
    char_u *d;
    int c;
{
    *d = TO_UPPER(c);

    return (fptr)do_Upper;
}

    static fptr
do_lower(d, c)
    char_u *d;
    int c;
{
    *d = TO_LOWER(c);

    return (fptr)NULL;
}

    static fptr
do_Lower(d, c)
    char_u	*d;
    int		c;
{
    *d = TO_LOWER(c);

    return (fptr)do_Lower;
}

/*
 * regtilde(): Replace tildes in the pattern by the old pattern.
 *
 * Short explanation of the tilde: It stands for the previous replacement
 * pattern.  If that previous pattern also contains a ~ we should go back a
 * step further...  But we insert the previous pattern into the current one
 * and remember that.
 * This still does not handle the case where "magic" changes. TODO?
 *
 * The tildes are parsed once before the first call to vim_regsub().
 */
    char_u *
regtilde(source, magic)
    char_u	*source;
    int		magic;
{
    char_u	*newsub = source;
    char_u	*tmpsub;
    char_u	*p;
    int		len;
    int		prevlen;

    for (p = newsub; *p; ++p)
    {
	if ((*p == '~' && magic) || (*p == '\\' && *(p + 1) == '~' && !magic))
	{
	    if (reg_prev_sub != NULL)
	    {
		/* length = len(newsub) - 1 + len(prev_sub) + 1 */
		prevlen = STRLEN(reg_prev_sub);
		tmpsub = alloc((unsigned)(STRLEN(newsub) + prevlen));
		if (tmpsub != NULL)
		{
		    /* copy prefix */
		    len = (int)(p - newsub);	/* not including ~ */
		    mch_memmove(tmpsub, newsub, (size_t)len);
		    /* interpretate tilde */
		    mch_memmove(tmpsub + len, reg_prev_sub, (size_t)prevlen);
		    /* copy postfix */
		    if (!magic)
			++p;			/* back off \ */
		    STRCPY(tmpsub + len + prevlen, p + 1);

		    if (newsub != source)	/* already allocated newsub */
			vim_free(newsub);
		    newsub = tmpsub;
		    p = newsub + len + prevlen;
		}
	    }
	    else if (magic)
		STRCPY(p, p + 1);		/* remove '~' */
	    else
		STRCPY(p, p + 2);		/* remove '\~' */
	    --p;
	}
	else if (*p == '\\' && p[1])		/* skip escaped characters */
	    ++p;
    }

    vim_free(reg_prev_sub);
    if (newsub != source)	/* newsub was allocated, just keep it */
	reg_prev_sub = newsub;
    else			/* no ~ found, need to save newsub  */
	reg_prev_sub = vim_strsave(newsub);
    return newsub;
}

#ifdef FEAT_EVAL
static int can_f_submatch = FALSE;	/* TRUE when submatch() can be used */
#endif

#if defined(FEAT_MODIFY_FNAME) || defined(FEAT_EVAL) || defined(PROTO)
/*
 * vim_regsub() - perform substitutions after a vim_regexec() or
 * vim_regexec_multi() match.
 *
 * If "copy" is TRUE really copy into "dest".
 * If "copy" is FALSE nothing is copied, this is just to find out the length
 * of the result.
 *
 * If "backslash" is TRUE, a backslash will be removed later, need to double
 * them to keep them, and insert a backslash before a CR to avoid it being
 * replaced with a line break later.
 *
 * Note: The matched text must not change between the call of
 * vim_regexec()/vim_regexec_multi() and vim_regsub()!  It would make the back
 * references invalid!
 *
 * Returns the size of the replacement, including terminating NUL.
 */
    int
vim_regsub(rmp, source, dest, copy, magic, backslash)
    regmatch_T	*rmp;
    char_u	*source;
    char_u	*dest;
    int		copy;
    int		magic;
    int		backslash;
{
    reg_match = rmp;
    reg_mmatch = NULL;
    reg_maxline = 0;
    return vim_regsub_both(source, dest, copy, magic, backslash);
}
#endif

    int
vim_regsub_multi(rmp, lnum, source, dest, copy, magic, backslash)
    regmmatch_T	*rmp;
    linenr_T	lnum;
    char_u	*source;
    char_u	*dest;
    int		copy;
    int		magic;
    int		backslash;
{
    reg_match = NULL;
    reg_mmatch = rmp;
    reg_buf = curbuf;		/* always works on the current buffer! */
    reg_firstlnum = lnum;
    reg_maxline = curbuf->b_ml.ml_line_count - lnum;
    return vim_regsub_both(source, dest, copy, magic, backslash);
}

    static int
vim_regsub_both(source, dest, copy, magic, backslash)
    char_u	*source;
    char_u	*dest;
    int		copy;
    int		magic;
    int		backslash;
{
    char_u	*src;
    char_u	*dst;
    char_u	*s;
    int		c;
    int		no = -1;
    fptr	func = (fptr)NULL;
    linenr_T	clnum = 0;	/* init for GCC */
    int		len = 0;	/* init for GCC */
#ifdef FEAT_EVAL
    static char_u *eval_result = NULL;
#endif

    /* Be paranoid... */
    if (source == NULL || dest == NULL)
    {
	EMSG(_(e_null));
	return 0;
    }
    if (prog_magic_wrong())
	return 0;
    src = source;
    dst = dest;

    /*
     * When the substitute part starts with "\=" evaluate it as an expression.
     */
    if (source[0] == '\\' && source[1] == '=')
    {
#ifdef FEAT_EVAL
	/* To make sure that the length doesn't change between checking the
	 * length and copying the string, and to speed up things, the
	 * resulting string is saved from the call with "copy" == FALSE to the
	 * call with "copy" == TRUE. */
	if (copy)
	{
	    if (eval_result != NULL)
	    {
		STRCPY(dest, eval_result);
		dst += STRLEN(eval_result);
		vim_free(eval_result);
		eval_result = NULL;
	    }
	}
	else
	{
	    vim_free(eval_result);
	    can_f_submatch = TRUE;
	    eval_result = eval_to_string(source + 2, NULL);
	    can_f_submatch = FALSE;
	    if (eval_result != NULL)
		dst += STRLEN(eval_result);
	}
#endif
    }
    else
      while ((c = *src++) != NUL)
      {
	if (c == '&' && magic)
	    no = 0;
	else if (c == '\\' && *src != NUL)
	{
	    if (*src == '&' && !magic)
	    {
		++src;
		no = 0;
	    }
	    else if ('0' <= *src && *src <= '9')
	    {
		no = *src++ - '0';
	    }
	    else if (vim_strchr((char_u *)"uUlLeE", *src))
	    {
		switch (*src++)
		{
		case 'u':   func = (fptr)do_upper;
			    continue;
		case 'U':   func = (fptr)do_Upper;
			    continue;
		case 'l':   func = (fptr)do_lower;
			    continue;
		case 'L':   func = (fptr)do_Lower;
			    continue;
		case 'e':
		case 'E':   func = (fptr)NULL;
			    continue;
		}
	    }
	}
	if (no < 0)	      /* Ordinary character. */
	{
#ifdef FEAT_MBYTE
	    int len;
#endif

	    if (c == '\\' && *src != NUL)
	    {
		/* Check for abbreviations -- webb */
		switch (*src)
		{
		    case 'r':	c = CR;		++src;	break;
		    case 'n':	c = NL;		++src;	break;
		    case 't':	c = TAB;	++src;	break;
		 /* Oh no!  \e already has meaning in subst pat :-( */
		 /* case 'e':   c = ESC;	++src;	break; */
		    case 'b':	c = Ctrl_H;	++src;	break;

		    /* If "backslash" is TRUE the backslash will be removed
		     * later.  Used to insert a literal CR. */
		    default:	if (backslash)
				{
				    if (copy)
					*dst = '\\';
				    ++dst;
				}
				c = *src++;
		}
	    }

	    /* Write to buffer, if copy is set. */
#ifdef FEAT_MBYTE
	    if (has_mbyte && (len = (*mb_ptr2len_check)(src - 1)) > 1)
	    {
		if (copy)
		    mch_memmove(dst, src - 1, len);
		dst += len - 1;
		src += len - 1;
	    }
	    else
	    {
#endif
		if (copy)
		{
		    if (func == (fptr)NULL)	/* just copy */
			*dst = c;
		    else			/* change case */
			func = (fptr)(func(dst, c));
		    /* Turbo C complains without the typecast */
		}
#ifdef FEAT_MBYTE
	    }
#endif
	    dst++;
	}
	else
	{
	    if (REG_MULTI)
	    {
		clnum = reg_mmatch->startpos[no].lnum;
		if (clnum < 0 || reg_mmatch->endpos[no].lnum < 0)
		    s = NULL;
		else
		{
		    s = reg_getline(clnum) + reg_mmatch->startpos[no].col;
		    if (reg_mmatch->endpos[no].lnum == clnum)
			len = reg_mmatch->endpos[no].col
					       - reg_mmatch->startpos[no].col;
		    else
			len = STRLEN(s);
		}
	    }
	    else
	    {
		s = reg_match->startp[no];
		if (reg_match->endp[no] == NULL)
		    s = NULL;
		else
		    len = reg_match->endp[no] - s;
	    }
	    if (s != NULL)
	    {
		for (;;)
		{
		    if (len == 0)
		    {
			if (REG_MULTI)
			{
			    if (reg_mmatch->endpos[no].lnum == clnum)
				break;
			    if (copy)
				*dst = CR;
			    ++dst;
			    s = reg_getline(++clnum);
			    if (reg_mmatch->endpos[no].lnum == clnum)
				len = reg_mmatch->endpos[no].col;
			    else
				len = STRLEN(s);
			}
			else
			    break;
		    }
		    else if (*s == NUL) /* we hit NUL. */
		    {
			if (copy)
			    EMSG(_(e_re_damg));
			goto exit;
		    }
		    else
		    {
			if (backslash && (*s == CR || *s == '\\'))
			{
			    /*
			     * Insert a backslash in front of a CR, otherwise
			     * it will be replaced by a line break.
			     * Number of backslashes will be halved later,
			     * double them here.
			     */
			    if (copy)
			    {
				dst[0] = '\\';
				dst[1] = *s;
			    }
			    dst += 2;
			}
			else
			{
			    if (copy)
			    {
				if (func == (fptr)NULL)	    /* just copy */
				    *dst = *s;
				else			    /* change case */
				    func = (fptr)(func(dst, *s));
				/* Turbo C complains without the typecast */
			    }
			    ++dst;
			}
			++s;
			--len;
		    }
		}
	    }
	    no = -1;
	}
      }
    if (copy)
	*dst = NUL;

exit:
    return (int)((dst - dest) + 1);
}

#ifdef FEAT_EVAL
/*
 * Used for the submatch() function: get the string from tne n'th submatch in
 * allocated memory.
 * Returns NULL when not in a ":s" command and for a non-existing submatch.
 */
    char_u *
reg_submatch(no)
    int		no;
{
    char_u	*retval = NULL;
    char_u	*s;
    int		len;
    int		round;
    linenr_T	lnum;

    if (!can_f_submatch)
	return NULL;

    if (REG_MULTI)
    {
	/*
	 * First round: compute the length and allocate memory.
	 * Second round: copy the text.
	 */
	for (round = 1; round <= 2; ++round)
	{
	    lnum = reg_mmatch->startpos[no].lnum;
	    if (lnum < 0 || reg_mmatch->endpos[no].lnum < 0)
		return NULL;

	    s = reg_getline(lnum) + reg_mmatch->startpos[no].col;
	    if (reg_mmatch->endpos[no].lnum == lnum)
	    {
		/* Within one line: take form start to end col. */
		len = reg_mmatch->endpos[no].col - reg_mmatch->startpos[no].col;
		if (round == 2)
		{
		    STRNCPY(retval, s, len);
		    retval[len] = NUL;
		}
		++len;
	    }
	    else
	    {
		/* Multiple lines: take start line from start col, middle
		 * lines completely and end line up to end col. */
		len = STRLEN(s);
		if (round == 2)
		{
		    STRCPY(retval, s);
		    retval[len] = '\r';
		}
		++len;
		++lnum;
		while (lnum < reg_mmatch->endpos[no].lnum)
		{
		    s = reg_getline(lnum++);
		    if (round == 2)
			STRCPY(retval + len, s);
		    len += STRLEN(s);
		    if (round == 2)
			retval[len] = '\r';
		    ++len;
		}
		if (round == 2)
		    STRNCPY(retval + len, reg_getline(lnum),
						  reg_mmatch->endpos[no].col);
		len += reg_mmatch->endpos[no].col;
		if (round == 2)
		    retval[len] = NUL;
		++len;
	    }

	    if (round == 1)
	    {
		retval = lalloc((long_u)len, TRUE);
		if (s == NULL)
		    return NULL;
	    }
	}
    }
    else
    {
	if (reg_match->endp[no] == NULL)
	    retval = NULL;
	else
	{
	    s = reg_match->startp[no];
	    retval = vim_strnsave(s, (int)(reg_match->endp[no] - s));
	}
    }

    return retval;
}
#endif
