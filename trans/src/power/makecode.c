/* $Id$ */

/*
 * Copyright 2002-2012, The TenDRA Project.
 * Copyright 1997, United Kingdom Secretary of State for Defence.
 * Copyright 1993, Open Software Foundation, Inc.
 *
 * See doc/copyright/ for the full copyright terms.
 */

#include <shared/error.h>

#include <local/cpu.h>

#include <reader/exptypes.h>
#include <reader/externs.h>

#include <construct/label_ops.h>
#include <construct/installglob.h>
#include <construct/f64.h>

#include <main/flags.h>

#include <diag/diag_fns.h>

#include <refactor/optimise.h>

#include "memtdf.h"
#include "codegen.h"
#include "geninst.h"

#include "maxminmacs.h"
#include "comment.h"
#include "muldvrem.h"
#include "proc.h"
#include "translate.h"
#include "eval.h"

#include "makecode.h"
#include "frames.h"
#include "stack.h"
#include "instruct.h"
#include "tempdecs.h"
#include "oprators.h"
#include "record_bit.h"
#include "mask.h"
#include "error.h"
#include "localexpmacs.h"
#include "localflags.h"

#ifdef DEBUG_POWERTRANS
#include "pp.h"
#endif

bool cr0_set;

where nowhere;	/* no particular destination, init in translat.c */

/* Function declarations */
void move_dlts(int,int,int,int);/* Used by movecont_tag */
void move_dgts(int,int,int,int);/* Used by movecont_tag */
int regfrmdest(where *,space);
freg fregfrmdest(bool,where *,space);

static int get_next_mlv_number(void);
void adjust_to_size(int,int,int,int,int);



/* branch table, branch code to instruction */


static Instruction_P branch_tab[] ={
  &INSTRUCTION_I_NIL,  /* not used */
  &INSTRUCTION_i_ble,  /* 1 */
  &INSTRUCTION_i_blt,  /* 2 */
  &INSTRUCTION_i_bge,  /* 3 */
  &INSTRUCTION_i_bgt,  /* 4 */
  &INSTRUCTION_i_bne,  /* 5 */
  &INSTRUCTION_i_beq   /* 6 */
};


#define branches(i)	(branch_tab[i])


						    /*  1  2  3  4  5  6 */
/* used to invert TDF tests */			    /* le lt ge gt ne eq */
prop notbranch[] =
{
  0,				/* NOT USED */
  4,				/* opposite of le is gt */
  3,				/* opposite of lt is ge */
  2,				/* opposite of ge is lt */
  1,				/* opposite of gt is le */
  6,				/* opposite of ne is eq */
  5				/* opposite of eq is ne */
};
						    /*  1  2  3  4  5  6 */
/* used to change TDF test when args commuted */    /* le lt ge gt ne eq */
prop combranch[] =
{
  0,				/* NOT USED */
  3,				/* reverse of le is ge */
  4,				/* reverse of lt is gt */
  1,				/* reverse of ge is le */
  2,				/* reverse of gt is lt */
  5,				/* reverse of ne is ne */
  6				/* reverse of eq is eq */
};


static void testsigned(int r, long lower, long upper, long lab)
{
  int creg1=next_creg();
  int creg2=next_creg();
  cmp_ri_ins(i_cmp,r,lower,creg1);
  bc_ins(i_blt,creg1,lab,UNLIKELY_TO_JUMP);
  cmp_ri_ins(i_cmp,r,upper,creg2);
  bc_ins(i_bgt,creg2,lab,UNLIKELY_TO_JUMP);
  return;
}
static void testusigned(int r, long maxval, long lab)
{
  int creg=next_creg();
  cmp_ri_ins(i_cmpl,r,maxval,creg);
  bc_ins(i_bgt,creg,lab,UNLIKELY_TO_JUMP);
  return;
}

/* find the last test in sequence e which is a branch to second, if any, otherwise nil */
static exp testlast(exp e, exp second)
{
  if (name(e) == test_tag && pt(e) == second)
  {
    return e;
  }
  if (name(e) == seq_tag)
  {

    if (name(bro(son(e))) == test_tag && pt(bro(son(e))) == second)
    {
      /* is the last one of the sequence a test_tag pointing to second */
      return bro(son(e));
    }
    else if (name(bro(son(e))) == top_tag)
    {
      exp list = son(son(e));
      /* find the penultimate exp of the seq_tag */
      for (;;)
      {
	if (last(list))
	{
	  if (name(list) == test_tag && pt(list) == second)
	  {
	    return list;
	  }
	  else
	  {
	    return 0;
	  }
	}
	else
	{
	  list = bro(list);
	}
      }
    }
  }
  return 0;
}

/* Does e, or components of e contain a bitfield? */
/* +++ should detect this earlier and record in props(e) once-and-for-all */
static int has_bitfield(exp e)
{
  if (e == NULL)
    return 0;

  switch (name(e))
  {
  case compound_tag:
    {

      /*
       * (compound_tag <offset> <initialiser> ... )
       */
      /* look at alignment of initialisers */
      e = bro(son(e));
      while (1)
      {
	if (has_bitfield(e))
	  return 1;		/* found bitfield */

	if (last(e))
	  return 0;		/* all done, no bitfield */

	e = bro(bro(e));	/* try next initialiser */
      }
      /*NOTREACHED*/
  default:
      {
	shape s = sh(e);

	FULLCOMMENT4("has_bitfield: compound field sz=%d als=%d,%d,%d",
		shape_size(s), shape_align(s), al1(s), al2(s));
	return shape_size(s)!= 0 && (shape_align(s) == 1 || al1(s) == 1 || al2(s) == 1);
      }
    }
  }
  /*NOTREACHED*/
}

/*
 * Convert all NON-bitfields from byte-offsets back to bit-offsets,
 * so the compound can be output correctly by eval().
 * Permanently undoes the needscan.c:scan() case val_tag:.
 *
 * NB must do this EXACTLY ONCE.
 */
static void fix_nonbitfield(exp e)
{
  if (name(e) == compound_tag)
  {
    e = son(e);
    while (1)
    {
      if (name(e) == val_tag && name(sh(e)) == offsethd && al2(sh(e)) >= 8)
	  no(e) = no(e) << 3;	/* fix it */

      fix_nonbitfield(bro(e));	/* recursively fix the rest of the struct */

      if (last(bro(e)))
	  return;		/* all done */

      e = bro(bro(e));		/* next offset */
    }
  }
  /*NOTREACHED*/
}

/*
 * Some functions to build and maintain a queue of conditional branch
 * instuctions, so the generation of these instructions can be delayed.
 *
 * This is so the compare instruction will not immediately follow compare,
 * thus reducing RS/6000 compare..branch delays.
 *
 * No side effecting instructions should be emitted while branches are queued.
 * Currently only used with case_tag.
 */

/* params of bc_ins() */
typedef struct
{
  Instruction_P branch;
  int	creg;
  int	lab;
} bc_info;



#define	NQUEUE			3	/* any number < nmumber
					 * next_creg() manages (0,1,6,7) */
static bc_info bqueue[NQUEUE];

static int bqueuepos;		/* next free slot in queue */


static void clear_branch_queue(void)
{
  int i;

  bqueuepos = 0;

  for (i = 0; i < NQUEUE; i++)
  {
    bqueue[i].branch = I_NIL;
    bqueue[i].creg = -1;
  }
}


static void issue_bc_ins(int i)
{
  ASSERT(i >= 0 && i < NQUEUE);
  bc_ins(bqueue[i].branch, bqueue[i].creg, bqueue[i].lab,LIKELY_TO_JUMP);
}


static void queue_bc_ins(Instruction_P ins, int creg, int lab)
{
  int i;

  COMMENT2("queue_bc_ins(%s,%d,lab)",(int)ins, creg);

#ifdef DO_ASSERT
  /* check there is not a queued instruction using same creg (now corrupted) */
  for (i = 0; i < NQUEUE; i++)
    ASSERT(bqueue[i].creg != creg);
#endif

  i = bqueuepos;

  ASSERT(i >= 0 && i < NQUEUE);

  /* if queue full, clear one entry */
  if (bqueue[i].branch != I_NIL)
    issue_bc_ins(i);

  bqueue[i].branch = ins;
  bqueue[i].creg = creg;
  bqueue[i].lab = lab;

  bqueuepos++;
  if (bqueuepos == NQUEUE)
    bqueuepos = 0;		/* roll around to zero */
}

static void flush_branch_queue(void)
{
  int i;

  i = bqueuepos;

  do
  {
    if (bqueue[i].branch != I_NIL)
      issue_bc_ins(i);

    i++;
    if (i == NQUEUE)
      i = 0;			/* roll around to zero */
  } while (i != bqueuepos);

  clear_branch_queue();
}

static void case_tag_code_transform(int caseint_reg, exp e, space sp)
{

  long u;
  long l;
  long n;
  exp z=bro(son(e));
  exp zt=z;
  int endlab = new_label();	/* +++ often another jump at endlab */
  int veclab = next_data_lab();
  char *veclabname;
  baseoff zeroveclab;
  int mr = getreg(sp.fixed);	/* no need to guardreg(caseint_reg) as mr not
				 * used until after lase use of caseint_reg */

  l=no(zt);
  while (bro(zt)!=NULL)
  {
    zt=bro(zt);
  }
  u = (son(zt) ==NULL)? no(zt): no(son(zt));


  zeroveclab.offset = 0;
  zeroveclab.base = veclab;

  if (l >= 0 && l <= 4)
  {
    /* between 0 and 4 dummy table entries used to avoid subtract */
    rir_ins(i_sl, caseint_reg, 2, mr);
    n = 0;
  }
  else
  {
    /* subtract to index jump vector */
    rir_ins(i_a, caseint_reg,-l, mr);
    rir_ins(i_sl, mr, 2, mr);
    n = l;
  }
  /* delayed branch */
  set_ins(zeroveclab, R_TMP0);
  ld_rr_ins(i_l, mr, R_TMP0, mr);
  rrr_ins(i_a, mr, R_TMP0, mr);
  mt_ins(i_mtctr, mr);
  z_ins(i_bctr);

  /* .toc entry for veclab */
  fprintf(as_file, "\t.toc\n");
  veclabname = ext_name(veclab);
  fprintf(as_file, "T.%s:\t.tc\t%s[TC],%s\n", veclabname, veclabname, veclabname);
  fprintf(as_file, "\t.csect\t[PR]\n");

  /* build the jump vector, can be to .text or .data */
  fprintf(as_file, "%s:\n", veclabname);
  for (;;)
  {
    for (; no(z)!= n; n++)
    {
      fprintf(as_file, "\t.long\tL.%d-%s\n", endlab, veclabname);
    }
    u = (son(z) == NULL)? n : no(son(z));
    for (; u+1 != n; n++)	/* comparison independent of sign */
    {
	fprintf(as_file, "\t.long\tL.%d-%s\n", no(son(pt(z))), veclabname);
      }
    if (last(z))
    {
      break;
    }
    z = bro(z);
  }
  set_label(endlab);
  return;
}

static void case_tag_code_notransform(int caseint_reg, exp e, space sp)
{
  mm lims;
  exp z = bro(son(e));
  exp zt = z;
  long n;
  long l;
  long u = 0x80000000;
  unsigned long approx_range;	/* max(u-l, 0x7fffffff) avoiding overflow */
  bool use_jump_vector;

  ASSERT(name(e) == case_tag);

  /* calculate crude criterion for using jump vector or branches */
  l = no(zt);
  for (n = 1;; n++)
  {
    if (u + 1 != no(zt) && son(zt)!= NULL)
    {
      n++;
    }
    if (last(zt))
    {
      u = (son(zt)!= NULL)? no(son(zt)): no(zt);
      break;
    }
    if (son(zt)!= NULL)
    {
      u = no(son(zt));
    }
    else
    {
      if (u + 1 == no(zt))
	u += 1;
    }

    zt = bro(zt);
  }


  /* now l is lowest controlling value, u is highest and n is number of cases */

  if (u - l < 0)
    approx_range = 0x7fffffff;	/* u-l overflowed into -ve, use huge */
  else
    approx_range = (unsigned long)(u - l);

  if (approx_range < 16)
  {
    /* small jump vector needed, decide on instuctions executed only */
#define	MTCR_B_DELAY		4	/* fixed point mtctr..bctr delay */
#define	BR_TAKEN_DELAY		3	/* fixed point branch taken delay */
    unsigned jump_vector_cnt = ((l >= 0 && l <= 4)? 8 + MTCR_B_DELAY : 9 + MTCR_B_DELAY);

    unsigned cmp_jmp_step_cnt = 2 + (!IMM_SIZE(l)) + (!IMM_SIZE(u));

    /* cmp & jmp, delay slot filled plus possibly load of large consts */
    /* +++ assume default used as often as case, is this good ??? */
    unsigned default_weight = 1;/* likelyhood of default against single case */
    unsigned total_case_test_chain_cnt =
	((((n + 1)* cmp_jmp_step_cnt)* n) / 2) + BR_TAKEN_DELAY;
    unsigned default_test_chain_cnt =
	(n * cmp_jmp_step_cnt);
    unsigned average_test_chain_cnt =
	(total_case_test_chain_cnt + (default_test_chain_cnt * default_weight)) / (n + default_weight);

    use_jump_vector = jump_vector_cnt <= average_test_chain_cnt;
    FULLCOMMENT2("case_tag small jump vector: jump_vector_cnt=%d average_test_chain_cnt=%d",
		 jump_vector_cnt, average_test_chain_cnt);
  }
  else
  {

    /*
     * space-time product criterion for jump vector instead of tests and
     * branches
     */
    unsigned long range_factor = approx_range + 9;
    unsigned long n_factor = ((unsigned long)n * n) / 2;

    use_jump_vector = range_factor <= n_factor;
  }

  COMMENT4("case_tag: n=%d l,u=%d,%d approx_range=%d", n, l, u, approx_range);
  if (is_signed(sh(son(e)))) {
    ASSERT(l <= u);
  } else {
    ASSERT((unsigned long)l <= (unsigned long)u);
  }
  ASSERT(n >= 0);

  if (use_jump_vector)
  {
    /* use jump vector */
    int endlab = new_label();	/* +++ often another jump at endlab */
    int veclab = next_data_lab();
    char *veclabname;
    baseoff zeroveclab;
    int creg = next_creg();
    int mr = getreg(sp.fixed);	/* no need to guardreg(caseint_reg) as mr not
				 * used until after lase use of caseint_reg */

    zeroveclab.offset = 0;
    zeroveclab.base = veclab;

    if (l >= 0 && l <= 4)
    {
      /* between 0 and 4 dummy table entries used to avoid subtract */
      cmp_ri_ins(i_cmpl, caseint_reg, u + 1, creg);
      /* branch later, to reduce compare..barnch delay */
      rir_ins(i_sl, caseint_reg, 2, mr);
      n = 0;
    }
    else
    {
      /* subtract to index jump vector */
      rir_ins(i_a, caseint_reg,-l, mr);
      cmp_ri_ins(i_cmpl, mr, u - l + 1, creg);
      /* branch later, to reduce compare..branch taken delay */
      rir_ins(i_sl, mr, 2, mr);
      n = l;
    }

    /* delayed branch */
    bc_ins(i_bge, creg, endlab,UNLIKELY_TO_JUMP);

    set_ins(zeroveclab, R_TMP0);

    ld_rr_ins(i_l, mr, R_TMP0, mr);
    rrr_ins(i_a, mr, R_TMP0, mr);

    mt_ins(i_mtctr, mr);
    z_ins(i_bctr);

    /* .toc entry for veclab */
    fprintf(as_file, "\t.toc\n");
    veclabname = ext_name(veclab);
    fprintf(as_file, "T.%s:\t.tc\t%s[TC],%s\n", veclabname, veclabname, veclabname);
    fprintf(as_file, "\t.csect\t[PR]\n");

    /* build the jump vector, can be to .text or .data */
    fprintf(as_file, "%s:\n", veclabname);
    for (;;)
    {
      for (; no(z)!= n; n++)
      {
	fprintf(as_file, "\t.long\tL.%d-%s\n", endlab, veclabname);
      }
      u = (son(z) == NULL)? n : no(son(z));
      for (; u+1 != n; n++)
      {
	fprintf(as_file, "\t.long\tL.%d-%s\n", no(son(pt(z))), veclabname);
      }
      if (last(z))
	break;
      z = bro(z);
    }

    set_label(endlab);
    return;
  }
  else
  if (is_signed(sh(son(e)))) {
    /* use branches - tests are ordered */
    int endlab = 0;
    lims = maxmin(sh(son(e)));
    clear_branch_queue();

    for (;;)
    {
      int lab = no(son(pt(z)));
      int creg = next_creg();

      long l = no(z);
      if (son(z) == NULL)
      {
	/* only single test required */
	cmp_ri_ins(i_cmp, caseint_reg, l, creg);
	queue_bc_ins(i_beq, creg, lab);
	if (l == lims.maxi)
	  lims.maxi -= 1;
	else if (l == lims.mini)
	  lims.mini += 1;
      }
      else if (u = no(son(z)), l > lims.mini)
      {
	if (u >= lims.maxi)
	{
	  /* have already tested lower */
	  cmp_ri_ins(i_cmp, caseint_reg, l, creg);
	  queue_bc_ins(i_bge, creg, lab);
	  lims.maxi = l - 1;
	}
	else
	{
	  int creg2;

	  if (endlab == 0)
	    endlab = new_label();

	  cmp_ri_ins(i_cmp, caseint_reg, l, creg);
	  queue_bc_ins(i_blt, creg, endlab);

	  /*
	   * Note, must queue first bc_ins before second cmp_ins,
	   * which may use a creg already in the queue.
	   */
	  creg2 = next_creg();
	  cmp_ri_ins(i_cmp, caseint_reg, u, creg2);
	  queue_bc_ins(i_ble, creg2, lab);

	  lims.mini = u + 1;
	}
      }
      else if (u < lims.maxi)
      {
	/* lower is <= lower limit of shape */
	cmp_ri_ins(i_cmp, caseint_reg, u, creg);
	queue_bc_ins(i_ble, creg, lab);
	lims.mini = u + 1;
      }
      else
      {
	/* upper is >= upper limit of shape */
	flush_branch_queue();
	uncond_ins(i_b, lab);
      }
      if (last(z))
      {
	flush_branch_queue();
	if (endlab != 0)
	{
	  set_label(endlab);
	}
	return;
      }
      z = bro(z);
    }
  }
  else {
    /* unsigned, use branches - tests are ordered */
    int endlab = 0;
    unsigned long maxi;
    unsigned long mini;
    lims = maxmin(sh(son(e)));
    maxi = (unsigned)lims.maxi;
    mini = (unsigned)lims.mini;
    clear_branch_queue();

    for (;;)
    {
      int lab = no(son(pt(z)));
      int creg = next_creg();

      unsigned long l = no(z);
      if (son(z) == NULL)
      {
	/* only single test required */
	cmp_ri_ins(i_cmpl, caseint_reg, l, creg);
	queue_bc_ins(i_beq, creg, lab);
	if (l == maxi)
	  maxi -= 1;
	else if (l == mini)
	  mini += 1;
      }
      else if (u = no(son(z)), l > mini)
      {
	if (u >= maxi)
	{
	  /* have already tested lower */
	  cmp_ri_ins(i_cmpl, caseint_reg, l, creg);
	  queue_bc_ins(i_bge, creg, lab);
	  maxi = l - 1;
	}
	else
	{
	  int creg2;

	  if (endlab == 0)
	    endlab = new_label();

	  cmp_ri_ins(i_cmpl, caseint_reg, l, creg);
	  queue_bc_ins(i_blt, creg, endlab);

	  /*
	   * Note, must queue first bc_ins before second cmp_ins,
	   * which may use a creg already in the queue.
	   */
	  creg2 = next_creg();
	  cmp_ri_ins(i_cmpl, caseint_reg, u, creg2);
	  queue_bc_ins(i_ble, creg2, lab);

	  mini = u + 1;
	}
      }
      else if (u < maxi)
      {
	/* lower is <= lower limit of shape */
	cmp_ri_ins(i_cmpl, caseint_reg, u, creg);
	queue_bc_ins(i_ble, creg, lab);
	mini = u + 1;
      }
      else
      {
	/* upper is >= upper limit of shape */
	flush_branch_queue();
	uncond_ins(i_b, lab);
      }
      if (last(z))
      {
	flush_branch_queue();
	if (endlab != 0)
	{
	  set_label(endlab);
	}
	return;
      }
      z = bro(z);
    }
  }
}

static void case_tag_code(int caseint_reg, exp e, space sp)
{
	if (optim & OPTIM_CASE) {
		return case_tag_code_transform(caseint_reg, e, sp);
	} else {
		return case_tag_code_notransform(caseint_reg, e, sp);
	}
}

/*
 * Evaluate and generate the compare instruction for a test_tag,
 * and return a bcinfo describing the conditional branch required.
 */
static bc_info make_test_tag_cmp(exp e, space sp)
{
  exp l = son(e);
  exp r = bro(l);
  shape shl = sh(l);
  bc_info bcinfo;

  bcinfo.lab = (ptno(e) < 0)? -ptno(e): no(son(pt(e)));
					/* see frig in cond_tag */
  /* generate compare */
  if (is_floating(name(sh(l))))
  {
    /* float test */
    int a1;
    int a2;
    space nsp;

    if (IsRev(e))
    {
      a2 = freg_operand(r, sp, getfreg(sp.flt));
      nsp = guardfreg(a2, sp);
      a1 = freg_operand(l, nsp, getfreg(nsp.flt));
    }
    else
    {
      a1 = freg_operand(l, sp, getfreg(sp.flt));
      nsp = guardfreg(a1, sp);
      a2 = freg_operand(r, nsp, getfreg(nsp.flt));
    }

    bcinfo.creg = next_creg();
    rrf_cmp_ins(i_fcmpo, a1, a2, bcinfo.creg);
  }
  else
  {
    /* int test */
    bool sgned = is_signed(shl);
    int a1;
    int a2;
    Instruction_P cmp;

    cmp = sgned ? i_cmp : i_cmpl;

    cr0_set = 0;
    /* cr0_set is needed since l could be tracked by reg tracking and there
       fore not coded. In this case cr0_set should remain 0 */
    a1 = reg_operand(l, sp);
    if (record_bit_set(l) && cr0_set==1)
    {
      bcinfo.creg = 0;
      /* no need to generate the compare */
    }
    else
    {
      if (name(r) == val_tag)
      {
	bcinfo.creg = next_creg();
	cmp_ri_ins(cmp, a1, no(r), bcinfo.creg);
      }
      else
      {
	space nsp;
	nsp = guardreg(a1, sp);
	a2 = reg_operand(r, nsp);
	bcinfo.creg = next_creg();
	cmp_rr_ins(cmp, a1, a2, bcinfo.creg);
      }
    }
  }
  cr0_set = 0;
  bcinfo.branch = branches(test_number(e));
  return bcinfo;
}

/*
 * Produce code for expression e, putting its result in dest using t-regs
 * given by sp. If non-zero, exitlab is the label of where the code is to
 * continue.
 */
makeans make_code(exp e, space sp, where dest, int exitlab)
{
  long constval=0;
  makeans mka;
  static long exp_num = 0;  /* count of exps in order of evaluation */

/*
 * A heuristic to estimate if conditional branch is close enough for
 * bc instruction, which can branch +-8k words.  Tests indicate
 * 13500 exp nodes generate 8k words of instructions.
 * We play safe and allow 1 instruction per exp.
 */
#define TEST_TAG_NEAR_BRANCH(e)	(ptno(e) < 0 || absval(ptno(son(pt(e))) -exp_num) < 8192)

 tailrecurse:
  exp_num++;
  mka.lab = exitlab;
  mka.regmove = NOREG;

  switch (name(e))
  {
  /*
   * Procedure related code selection is handled by make_XXX_tag_code()
   * functions in proc.c.
   */
   case proc_tag:		/* procedure definition */
   case general_proc_tag:
    {
      exp_num = 0;
      make_proc_tag_code(e, sp);
      return mka;
    }

   case ident_tag:		/* ident/param definition within proc */
    {
      return make_ident_tag_code(e, sp, dest, exitlab);
    }

   case untidy_return_tag:
   case res_tag:			/* procedure result */
    {
      make_res_tag_code(e, sp);
      return mka;
    }

   case apply_tag:		/* procedure call */
    {
      return make_apply_tag_code(e, sp, dest, exitlab);
    }

   case clear_tag:
    {
      if (dest.answhere.discrim == insomereg)
      {
	/*
	 * Must choose a fixed register to contain answer to clear
	 */
	int *sr = someregalt(dest.answhere);

	if (*sr != -1) {fail("somereg *2");}
	*sr = getreg(sp.fixed);
	setregalt(dest.answhere, *sr);
      }
      else if (dest.answhere.discrim==insomefreg)
      {
	/*
	 * Must choose a float register to contain answer to clear
	 */
	somefreg sfr;
	freg fr;

	sfr = somefregalt(dest.answhere);
	if (*sfr.fr != -1) {fail("somefreg *2");}
	*sfr.fr = getfreg(sp.flt);
	fr.fr = *sfr.fr;
	fr.dble = sfr.dble;
	setfregalt(dest.answhere, fr);
      }

      return mka;
    }

   case seq_tag:
    {
      exp t = son(son(e));

      for (;;)
      {
	exp next = (last(t))?(bro(son(e))): bro(t);

	if (name(next) == goto_tag)	/* gotos end sequences */
	{
	  make_code(t, sp, nowhere, no(son(pt(next))));
	}
	else
	{
	  code_here(t, sp, nowhere);
	}
	if (last(t))
	{
	  exp l = bro(son(e));		/* last exp of sequence */

	  if (name(sh(t)) == bothd && name(l) == res_tag &&
	     (name(son(l)) == clear_tag || name(son(l)) == top_tag))
	  {
	    /*
	     * res_tag that cannot be reached.  Eg an extra one inserted at
	     * end of proc.  Skip it.
	     */
	     COMMENT("make_code seq_tag: unreachable res_tag");
	     return mka;
	  }
	  else
	  {
	    return make_code(l, sp, dest, exitlab);
	  }
	}
	t = bro(t);
      }
    }				/* end seq */

   case cond_tag:
    {
      exp first = son(e);
      exp second = bro(son(e));
      exp test;

      if (dest.answhere.discrim==insomereg)
      {
	/*
	 * Must choose a fixed register to contain answer to cond
	 */
	int *sr = someregalt(dest.answhere);

	if (*sr != -1) {fail("somereg *2");}
	*sr = getreg(sp.fixed);
	setregalt(dest.answhere, *sr);
      }
      else if (dest.answhere.discrim==insomefreg)
      {
	/*
	 * Must choose a float register to contain answer to cond
	 */
	somefreg sfr;
	freg fr;

	sfr = somefregalt(dest.answhere);
	if (*sfr.fr != -1) {fail("somefreg *2");}
	*sfr.fr = getfreg(sp.flt);
	fr.fr = *sfr.fr;
	fr.dble = sfr.dble;
	setfregalt(dest.answhere, fr);
      }

      /*
       * A few optimisations for cond_tag
       */
      if (name(first) == goto_tag && pt(first) == second)
      {
	/* first is goto second */
	no(son(second)) = 0;
	return make_code(second, sp, dest, exitlab);
      }
#if 0 /* could we do this better to prevent long branch problem?*/
      else if (name(second) == labst_tag && name(bro(son(second))) == top_tag)
      {
	/* second is empty */

	int endl = (exitlab == 0)? new_label(): exitlab;

	no(son(second)) = endl;
	make_code(first, sp, dest, endl);
	mka.lab = endl;
	return mka;
      }
#endif
      else if (name(second) == labst_tag && name(bro(son(second))) == goto_tag)
      {
	/* second is goto */
	exp g = bro(son(second));

	no(son(second)) = no(son(pt(g)));
	return make_code(first, sp, dest, exitlab);
      }

      test = testlast(first, second);
      if (test != NULL && TEST_TAG_NEAR_BRANCH(test))
      {
	/* effectively an empty then part */
	int l = (exitlab != 0)? exitlab : new_label();

	ptno(test) = -l;	/* make test jump to exitlab - see test_tag: */
	settest_number(test,obranch(test_number(test)));
	/* settest_number preserves the Rev bit */
	no(son(second)) = new_label();
	make_code(first, sp, dest, l);
	make_code(second, sp, dest, l);
	mka.lab = l;
	return mka;
      }
      else
      {
	int fl;
	int l;

	no(son(second)) = new_label();
	fl = make_code(first, sp, dest, exitlab).lab;
	l = (fl != 0)? fl :((exitlab != 0)? exitlab : new_label());
	if (name(sh(first))!= bothd)
	{
	  uncond_ins(i_b, l);
	}
	make_code(second, sp, dest, l);
	clear_all();
	mka.lab = l;
	return mka;
      }
    }

   case labst_tag:
    {
      ptno(son(e)) = exp_num;	/* update estimate made in scan() */
      if (no(son(e))!= 0)
      {
	clear_all();
	set_label(no(son(e)));

	if (is_loaded_lv(e) && p_save_all_sregs)
	{
	  /* It is long jumpabble to (potentially)*/
	  if (p_has_tp)
	  {
	    /* restore tp */
	    baseoff saved_tp;
	    saved_tp.base = R_FP;
	    saved_tp.offset = 0;
	    ld_ro_ins(i_l,saved_tp,R_TP);comment("restore TP using FP");
	  }
	  if (p_has_saved_sp)
	  {
	    /* Variable frame size */
	    get_sp_from_stack();
	  }
	  else
	  {
	    /* Fixed frame size */
	    rir_ins(i_a,R_FP, - p_frame_size , R_SP);
	  }
	}

      }
      return make_code(bro(son(e)), sp, dest, exitlab);
    }				/* end labst */

  case rep_tag:
    {
      exp first = son(e);
      exp second = bro(first);

      code_here(first,sp,nowhere);
      ASSERT(name(second) ==labst_tag);
      no(son(second)) = new_label();
#if 1
      if (cpu != CPU_POWERPC)
      {
	exp last_test;
	/*
	 * Rearrange test and branch instructions
	 * to reduce RS/6000 branch delays
	 */
	/* look for last test_tag of repeat exp */
	last_test = bro(son(second));	/* under labst_tag */
	/* dive down sequences */
	while (name(last_test) == seq_tag)
	{
	  last_test = bro(son(last_test));
	}


	if (diag == DIAG_NONE && name(last_test) == test_tag)
	{
	  /* we found a test_tag, is it simple and jumps to rep_tag? */

	  if (ptno(last_test) >= 0 && pt(last_test) == second
	      && TEST_TAG_NEAR_BRANCH(last_test))
	  {
	    /*
	     * It jumps to head of repeat. Generate code out of
	     * order to reduce RS/6000 branch delays. RS/6000
	     * assumes fall-through conditional branches are
	     * most common and speculatively executes non-branch
	     * instructions ahaead.  Rearrange as follows:
	     *
	     *		b	start_of_rep_lab
	     *	end_rep_test_lab:
	     *		<reversed test_tag conditonal branch to end_rep_lab>
	     *	start_of_rep_lab:
	     #		<rest of rep>
	     *		<reversed test_tag conditonal test>
	     *		b	end_rep_test_lab
	     *	end_rep_lab:
	     */
	    static int rep_org_labnos = 0;
	    int rep_org_lab = 0;
	    int end_rep_test_lab = new_label();
	    int start_of_rep_lab = no(son(second));	/* labst_tag label */
	    int end_rep_lab = (exitlab == 0)? new_label(): exitlab;
	    bc_info bcinfo;

	    COMMENT("make_code rep_tag: last exp is rep_tag test_tag - evaluate out of order");

	    /* labst_tag label should be in use */
	    ASSERT(start_of_rep_lab!=0);

	    /* allocate new label number for use with .org: L.R%d and L.S%d */
	    rep_org_lab = ++rep_org_labnos;

	    uncond_ins(i_b, start_of_rep_lab);

	    set_label(end_rep_test_lab);

	    /* use .org to leave gap for brought forward bc ins */
	    fprintf(as_file, "L.R%d:\n", rep_org_lab);
	    fprintf(as_file, "\t.org\t$+4\t# loop bc ins\n");

	    /* we will do test_tag ourselves, nuke it out of loop */
	    name(last_test) = top_tag;

	    /* set_label(start_of_rep_lab) done by labst_tag */

	    mka = make_code(second, sp, dest, exitlab);

	    /* reverse test, jump to end_rep_lab */
	    ptno(last_test) = -end_rep_lab;
	    settest_number(last_test,obranch(test_number(last_test)));
	    /* generate compare */
	    bcinfo = make_test_tag_cmp(last_test, sp);

	    uncond_ins(i_b, end_rep_test_lab);

	    if (end_rep_lab != exitlab)
	      set_label(end_rep_lab);

	    /* fill in gap above with bc_ins */
	    fprintf(as_file, "L.S%d:\n", rep_org_lab);
	    fprintf(as_file, ".org\tL.R%d\t# loop bc ins\n", rep_org_lab);
	    bc_ins(bcinfo.branch, bcinfo.creg, bcinfo.lab,UNLIKELY_TO_JUMP);

	    /* .org back */
	    fprintf(as_file, ".org\tL.S%d\n", rep_org_lab);

	    return mka;
	  }

	}
      }
#endif
      /*
       * We could not find last simple test_tag, must be complicated.
       * Don't bother to move tests around.
       * +++ handle cond_tag for more complex terminating condition.
       */
      return make_code(second, sp, dest, exitlab);
    }				/* end rep */

  case goto_tag:
    {
      exp gotodest = pt(e);
      int lab;
#if 0
/* This would be a lovely optimisation, however silly people give me test
   programs with L1:goto L1 so I despair */
      while (name(bro(son(gotodest))) ==goto_tag)
      {
	/* goto to goto optimisation */
	gotodest = pt(bro(son(gotodest)));
      }
#endif
      lab = no(son(gotodest));
      clear_all();
      if (last(e) ==0 || name(bro(e))!=seq_tag || last(bro(e)) ||
	  bro(bro(e))!= gotodest)
      {
	uncond_ins(i_b, lab);
      }/* otherwise dest is next in sequence */
      return mka;
    }				/* end goto */

  case test_tag:
    {
      bc_info bcinfo;
      int branch_prediction=LIKELY_TO_JUMP;

      if (no(e)!=1000 && no(e) >=0 && no(e) <=100)
      {
	branch_prediction = (no(e) >=50)?UNLIKELY_TO_JUMP:LIKELY_TO_JUMP;
      }
      try_record_bit(e);
      if (TEST_TAG_NEAR_BRANCH(e))
      {
	/*
	 * Estimate close enough for bc_ins
	 */
	bcinfo = make_test_tag_cmp(e, sp);
	bc_ins(bcinfo.branch, bcinfo.creg, bcinfo.lab,branch_prediction);
      }
      else
      {
	int newlab = new_label();
	int oldlab = no(son(pt(e)));

	/*
	 * Branch is too far away so we reverse branch to new label
	 * and use an unconditional branch to the target destination
	 */
	ptno(e) = -newlab;
	settest_number(e,obranch(test_number(e)));
	bcinfo = make_test_tag_cmp(e, sp);
	bc_ins(bcinfo.branch, bcinfo.creg, bcinfo.lab,1-branch_prediction);
	uncond_ins(i_b, oldlab);	/* unconditional far jump OK */
	set_label(newlab);
      }
      return mka;
    }				/* end test */

   case ass_tag:
   case assvol_tag:
    {
      exp lhs = son(e);
      exp rhs = bro(lhs);
      where assdest;
      space nsp;
      int contreg = NOREG;
      int hdrhs = name(sh(rhs));
      bool is_float = is_floating(hdrhs);

      /*
       * +++ lose chvar_tag on rhs if no result, remember to invalidate reg
       * remove name(e)==ass_tag tests now assbits_tag has gone
       */

      if (name(e) == assvol_tag)
      {
	/*
	 * Assign to volatile location.
	 * Disable register-location tracing. +++ is this really needed ???
	 * Disable peep-hole optimisation (when implemented)
	 */
	COMMENT("make_code: Assign to volatile");
	clear_all();
      }

      if (name(e) == ass_tag && APPLYLIKE(rhs) &&
	 ((is_float) || valregable(sh(rhs))))
      {
	where apply_res;
	/* This is not an optimisation this is necessary */
	/*
	 * Since if we have a procedure call doing the locate will make a pointer
	 * which will be trashed in the call
	 */
	if (is_float)
	{
	  freg frg;
	  COMMENT("make_code: ass_tag: apply result special handling:float");
	  frg.fr = FR_RESULT;
	  frg.dble = (hdrhs != shrealhd);
	  setfregalt(apply_res.answhere, frg);
	}
	else
	{
	  COMMENT("make_code: ass_tag: apply result special handling:fixed");
	  setregalt(apply_res.answhere, R_RESULT);
	}
	apply_res.ashwhere = ashof(sh(rhs));

	code_here(rhs, sp, apply_res);
	nsp = guard(apply_res, sp);

	assdest = locate(lhs, nsp, sh(rhs), 0);

	move(apply_res.answhere, assdest, nsp.fixed, 1);
	/*
	 * The evaluation of an assignment is the rhs so
	 * we move the rhs to dest as well
     */
	move(apply_res.answhere, dest, nsp.fixed, 1);
	clear_dep_reg(lhs);

#if 0
	/* +++ remember that R_RESULT is lhs */
	if (!is_float)
	{
	  keepcont(lhs,R_RESULT);
	}
#endif
	return mka;
      }

#ifndef NO_REGREG_ST
      /* see if we can use [reg+reg] addressing for this store */
      if (name(lhs) == addptr_tag)
      {
	exp addptr_sons = son(lhs);
	ash a;
	int ashsize;
	a = ashof(sh(rhs));
	ashsize = a.ashsize;

	if (last(bro(addptr_sons))
	    && a.ashalign == ashsize
	    && (ashsize==8 || ashsize==16 || ashsize==32 || is_float))
	{
	  int lhs_addptr_reg;
	  int rhs_addptr_reg;
	  ans aa;

	  COMMENT("make_code ass_tag: store suitable for [reg+reg] addressing");

	  lhs_addptr_reg = reg_operand(addptr_sons, sp);
	  nsp = guardreg(lhs_addptr_reg, sp);
	  rhs_addptr_reg = reg_operand(bro(addptr_sons), nsp);
	  nsp = guardreg(rhs_addptr_reg, nsp);

	  if (is_float)
	  {
	    freg dfreg;

	    dfreg.fr = freg_operand(rhs, nsp, getfreg(nsp.flt));
	    dfreg.dble = ashsize == 64;

	    stf_rr_ins((dfreg.dble?i_stfd:i_stfs),dfreg.fr,lhs_addptr_reg,rhs_addptr_reg);
	    setfregalt(aa, dfreg);
	  }
	  else
	  {
	    int assreg;
	    if (dest.answhere.discrim==inreg &&
	       !IS_R_NO_REG(regalt(dest.answhere)))
	    {
	      assreg = regalt(dest.answhere);
	      reg_operand_here(rhs,nsp,assreg);
	    }
	    else
	    {
	      assreg= reg_operand(rhs, nsp);
	    }
	    st_rr_ins(i_st_sz(ashsize), assreg, lhs_addptr_reg, rhs_addptr_reg);

	    setregalt(aa, assreg);
	  }

	  move(aa, dest, sp.fixed, 1);		/* +++ nsp.fixed ? */

	  clear_dep_reg(lhs);
	  return mka;
	}
      }
#endif

      assdest = locate(lhs, sp, sh(rhs), 0);
      nsp = guard(assdest, sp);
      FULLCOMMENT("make_code: ass_tag: located lhs");
      if (name(e) == ass_tag
	  && assdest.answhere.discrim == notinreg
	  && assdest.ashwhere.ashsize == assdest.ashwhere.ashalign)
      {
	instore is;

	is = insalt(assdest.answhere);
	if (!is.adval)
	{
	 /*
	  * This is an indirect assignment, so make it direct by loading
	  * pointer into reg (and * remember it)
	  */
	  int r = getreg(nsp.fixed);

	  ld_ins(i_l, is.b, r);
	  nsp = guardreg(r, nsp);
	  is.adval = 1;
	  is.b.base = r;
	  is.b.offset = 0;
	  setinsalt(assdest.answhere, is);
	  keepexp(lhs, assdest.answhere);
	}
      }

#if 1
      if (name(e) == ass_tag && is_float && assdest.answhere.discrim == notinreg)
      {
	/*
	 * Ensure floating point values assigned using floating point regs so
	 * floating point reg tracking works better. move() uses fixed regs
	 * for mem to mem, so must pre-load to floating point reg.
	 */
	int f = freg_operand(rhs, nsp, getfreg(nsp.flt));
	freg frg;
	ans aa;

	frg.fr = f;
	frg.dble = (hdrhs != shrealhd);
	setfregalt(aa, frg);
	nsp = guardfreg(f, nsp);
	move(aa, assdest, nsp.fixed, 1);
	move(aa, dest, nsp.fixed, 1);
	clear_dep_reg(lhs);
	/* +++ frg in mka */
	return mka;
      }
#endif

      contreg = code_here(rhs, nsp, assdest);
      /* evaluate source into assignment destination .... */

      /* ... and move it into dest - could use assignment as value */

      switch (assdest.answhere.discrim)
      {
      case inreg:
	{
	  int a = regalt(assdest.answhere);

	  keepreg(rhs, a);
	  /* remember that source has been evaluated into a */
	  clear_dep_reg(lhs);
	  /* forget register dependencies on destination */
	  move(assdest.answhere, dest, nsp.fixed, 1);
	  break;
	}
      case infreg:
	{
	  freg frg;
	  int r;

	  frg = fregalt(assdest.answhere);
	  r = frg.fr + 32;
	  if (frg.dble)
	  {
	    r = -r;
	  };
	  keepreg(rhs, r);
	  /* remember that source has been evaluated into a */
	  clear_dep_reg(lhs);
	  /* forget register dependencies on destination */
	  move(assdest.answhere, dest, nsp.fixed, 1);
	  break;
	}

       case notinreg:
	{
	  if (contreg != NOREG && name(e) == ass_tag)
	  {
	    ans aa;
	    space nnsp;

	    if (contreg > 0 && contreg < 31)
	    {
	      setregalt(aa, contreg);
	      nnsp = guardreg(contreg, sp);
	    }
	    else
	    {
	      freg frg;

	      frg.fr = absval(contreg) - 32;
	      frg.dble = (contreg < 0);
	      nnsp = nsp;
	      setfregalt(aa, frg);
	    }
	    move(aa, dest, nnsp.fixed, 1);
	    clear_dep_reg(lhs);/* Apply fix here from rw 27/10/94*/
	    /* forget register dependencies on destination */
#if 0
	    if (name(lhs) == name_tag || !dependson(lhs, 0, lhs))

	      /*
	       * remember that dest contains source, provided that it is not
	       * dependent on it
	       */
#endif
#if 1
	    if (name(lhs) == name_tag) {
	      exp dc = son(lhs);
	      if (son(dc)!= NULL)dc = son(dc);
	      if (shape_size(sh(dc)) ==
		  shape_size(sh(rhs))) {
		keepcont(lhs, contreg);
	      }
	    } else if (!dependson(lhs, 0, lhs))
#endif
	    {
	      keepcont(lhs, contreg);
	    }
	    return mka;
	  }
	  clear_dep_reg(lhs);
	  /* forget register dependencies on destination */
	  move(assdest.answhere, dest, nsp.fixed, 1);
	  break;
	}
      case insomereg:
	  {
	    clear_dep_reg(lhs);
	    /* forget register dependencies on destination */
	    move(assdest.answhere, dest, guard(assdest, sp).fixed, 1);
	    break;
	  }
      default:
	  fail("make_code: unexpected answhere in assign");
      }				/* end sw on answhere */

      return mka;
    }				/* end ass */

  case compound_tag:
    {
      exp t;
      space nsp;
      instore str;
      int r;

      if (has_bitfield(e))
      {
	/*
	 * Take the easy way out for bitfields, copy a constant.
	 * +++ This only works for C, which does not permit non-const
	 * expressions here.
	 */
	ans aa;

	COMMENT("make_code: compound containing bitfield");

	/* assume struct is small, set up data constant and move */

	fix_nonbitfield(e);		/* ensure all offsets are bit-offsets,
					 * as evaluated_const() expects */

	setinsalt(aa, evaluated_const(e));
	mka.regmove = move(aa, dest, sp.fixed, 0);

	return mka;
      }

      nsp = sp;
      t = son(e);
      switch (dest.answhere.discrim)
      {
      case notinreg:
	{
	  str = insalt(dest.answhere);	/* it should be !! */
	  if (!str.adval)
	  {
	    int r = getreg(sp.fixed);

	    nsp = guardreg(r, sp);
	    ld_ins(i_l, str.b, r);
	    str.adval = 1;
	    str.b.base = r;
	    str.b.offset = 0;
	  }
	  for (;;)
	  {
	    where newdest;
	    instore newis;

	    newis = str;
	    newis.b.offset += no(t);

	    FULLCOMMENT4("make_code compound_tag: name(t) =%d no(t) =%d al2=%d offset=%d",
		name(t), no(t), al2(sh(t)), newis.b.offset);
	    ASSERT(name(t) == val_tag && al2(sh(t)) >= 8);

	    setinsalt(newdest.answhere, newis);
	    newdest.ashwhere = ashof(sh(bro(t)));

	    code_here(bro(t), nsp, newdest);

	    if (last(bro(t)))
	      return mka;

	    t = bro(bro(t));
	  }
	}
#if 1	/* we need in reg compound for ptr type conversion via regs */
      case insomereg:
	{
	  int *sr = someregalt(dest.answhere);

	  if (*sr != -1)
	  {
	    fail("Somereg *2");
	  }
	  *sr = getreg(sp.fixed);
	  setregalt(dest.answhere, *sr);
	  /* FALLTHROUGH */
	}
      case inreg:
	{
	  code_here(bro(t), sp, dest);
	  r = regalt(dest.answhere);
	  ASSERT(name(t) == val_tag);
	  if (no(t)!= 0)
	    rir_ins(i_sl, r,((al2(sh(t)) >= 8)?(no(t) << 3): no(t)), r);
	  nsp = guardreg(r, sp);
	  while (!last(bro(t)))
	  {
	    int z;

	    t = bro(bro(t));
	    ASSERT(name(t) == val_tag);
	    z = reg_operand(bro(t), nsp);
	    if (no(t)!= 0)
	    {
	      rir_ins(i_sl, z,((al2(sh(t)) >= 8)?(no(t) << 3): no(t)), z);
	    }
	    rrr_ins(i_or, r, z, r);
	  }
	  return mka;
	}
#endif
      default:
	fail("no compounds in float reg");
      }
    }				/* end tup */

  case nof_tag:
  case concatnof_tag:
    {
      exp t = son(e);
      space nsp;
      instore str;
      int r, disp = 0;
#if 1
      if (t==NULL)
	return mka;
#endif
      nsp = sp;
      switch (dest.answhere.discrim)
      {
      case notinreg:
	{
	  str = insalt(dest.answhere);	/* it should be !! */
	  if (!str.adval)
	  {
	    int r = getreg(sp.fixed);

	    nsp = guardreg(r, sp);
	    ld_ins(i_l, str.b, r);
	    str.adval = 1;
	    str.b.base = r;
	    str.b.offset = 0;
	  }
	  for (;;)
	  {
	    where newdest;
	    instore newis;

	    newis = str;
	    newis.b.offset += disp;
	    setinsalt(newdest.answhere, newis);
	    newdest.ashwhere = ashof(sh(t));
	    code_here(t, nsp, newdest);
	    if (last(t))
	    {
	      return mka;
	    }
	    disp += (rounder(shape_size(sh(t)), shape_align(sh(bro(t)))) >> 3);
	    t = bro(t);
	  }
	}
      case insomereg:
	{
	  int *sr = someregalt(dest.answhere);

	  if (*sr != -1)
	  {
	    fail("Somereg *2");
	  }
	  *sr = getreg(sp.fixed);
	  setregalt(dest.answhere, *sr);
	  /* ,... */
	}
      case inreg:
	{
	  code_here(t, sp, dest);
	  r = regalt(dest.answhere);
	  nsp = guardreg(r, sp);
	  while (!last(t))
	  {
	    int z;

	    disp += rounder(shape_size(sh(t)), shape_align(sh(bro(t))));
	    t = bro(t);
	    z = reg_operand(t, nsp);
	    if (disp != 0)
	    {
	      rir_ins(i_sl, z, disp, z);
	    }
	    rrr_ins(i_or, r, z, r);
	  }
	  return mka;
	}
      default:
	fail("No Tuples in freg");
      }
    }

  case ncopies_tag:
    {
      exp t = son(e);
      space nsp;
      instore str;
      int i, r, disp = 0;
      int no_of_copies = no(e);
      nsp = sp;

      switch (dest.answhere.discrim)
      {
       case notinreg:
	{
	  str = insalt(dest.answhere);	/* it should be !! */
	  if (!str.adval)
	  {
	    int r = getreg(sp.fixed);

	    nsp = guardreg(r, sp);
	    ld_ins(i_l, str.b, r);
	    str.adval = 1;
	    str.b.base = r;
	    str.b.offset = 0;
	  }
	  for (i = 1; i <= no_of_copies; i++)
	  {
	    where newdest;
	    instore newis;

	    newis = str;
	    newis.b.offset += disp;
	    setinsalt(newdest.answhere, newis);
	    newdest.ashwhere = ashof(sh(t));
	    code_here(t, nsp, newdest);
	    disp += (rounder(shape_size(sh(t)), shape_align(sh(t))) >> 3);
	  }
	  return mka;
	}
      case insomereg:
	{
	  int *sr = someregalt(dest.answhere);

	  if (*sr != -1)
	  {
	    fail("Somereg *2");
	  }
	  *sr = getreg(sp.fixed);
	  setregalt(dest.answhere, *sr);
	  /*FALLTHROUGH*/
	}
      case inreg:
	{
	  code_here(t, sp, dest);
	  r = regalt(dest.answhere);
	  nsp = guardreg(r, sp);
	  for (i = 1; i <= no_of_copies; i++)
	  {
	    int z;

	    disp += rounder(shape_size(sh(t)), shape_align(sh(t)));
	    z = reg_operand(t, nsp);
	    if (disp != 0)
	    {
	      rir_ins(i_sl, z, disp, z);
	    }
	    rrr_ins(i_or, r, z, r);
	  }
	  return mka;
	}
      default:
	fail("no compounds in float reg");
      }
    }

   case diagnose_tag:
    {
      output_diag(dno(e), 0, e);
      mka = make_code(son(e), sp, dest, exitlab);
      output_end_scope(dno(e), e);
      return mka;
    }

  case solve_tag:
    {
      exp m = bro(son(e));
      int l = exitlab;

      if (dest.answhere.discrim==insomereg)
      {
	/* Choose register for fixed result */
	int *sr = someregalt(dest.answhere);
	if (*sr != -1) {fail("somereg *2");}
	*sr = getreg(sp.fixed);
	setregalt(dest.answhere, *sr);
      }
      else if (dest.answhere.discrim==insomefreg)
      {
	/* Choose register for float result */
	somefreg sfr;
	freg fr;
	sfr = somefregalt(dest.answhere);
	if (*sfr.fr != -1) { fail("somefreg *2"); }
	*sfr.fr = getfreg(sp.flt);
	fr.fr = *sfr.fr;
	fr.dble = sfr.dble;
	setfregalt(dest.answhere, fr);
      }

      /* Set up all the labels in the component labst_tags */
      for (;;)
      {
	no(son(m)) = new_label();
	if (last(m))
	{
	  break;
	}
	m = bro(m);
      }
      m = son(e);

      /* Evaluate all the component statements */
      for (;;)
      {
	int fl = make_code(m, sp, dest, l).lab;

	clear_all();
	if (fl != 0)
	{
	  l = fl;
	}
	if (!last(m))
	{
	  /* jump to end of solve */
	  if (l == 0)
	  {
	    l = new_label();
	  }
	  if (name(sh(m))!= bothd)
	  {
	    uncond_ins(i_b, l);
	  }
	}
	if (last(m))
	{
	  mka.lab = l;
	  return mka;
	};
	m = bro(m);
      }
    }				/* end solve */

  case case_tag:
    {
      exp control = son(e);
      int control_reg;
      control_reg = reg_operand(control,sp);
      case_tag_code(control_reg, e, sp);
      return mka;
    }				/* end case */

  case plus_tag:
    {
      if (!optop(e))
      {
	mka.regmove = plus_error_treatment(e,sp,dest);
      }
      else
      {
	if (isrecordbit(e))
	{
	  mka.regmove = comm_op(e, sp, dest, i_a_cr);
	  cr0_set = 1;
	}
	else
	{
	  mka.regmove = comm_op(e, sp, dest, i_a);
	}
      }
      return mka;
    }				/* end plus */

   case chvar_tag:
    {
      exp arg = son(e);			/* source of chvar, adjusted below   */
      int size_e = shape_size(sh(e));	/* size of result                    */
      int to = name(sh(e));		/* to hd                             */
      int from;				/* from hd                           */
      int sreg;			        /* source reg                        */
      int dreg;  			/* dest reg, or temp for memory dest */
      bool inmem_dest;		        /* is dest in memory ? */


      /*
       * For a series of chvar_tags, do large to small in one go
       */
      while (name(arg) == chvar_tag &&
	     ashof(sh(arg)).ashsize >= size_e && NO_ERROR_TREATMENT(arg))
      {
	COMMENT1("make_code chvar_tag: skipping intermediate shape %d",name(sh(arg)));
	arg = son(arg);
      }

      if (ERROR_TREATMENT(e))
      {
	mka.regmove = chvar_error_treatment(e,sp,dest);
	return mka;
      }


      from = name(sh(arg));
      if (from == to ||
	  to == slonghd ||
	  to == ulonghd ||
	 (to == uwordhd && from == ucharhd) ||
	 (to == swordhd && (from == scharhd || from == ucharhd)) ||
	 (to>=slonghd)
	 )
      {
	/*
	 * No changes required, so just move handling dest insomereg well
	 */
	ans aa;

	COMMENT("make_code chvar_tag: no change");
	switch (dest.answhere.discrim)
	{
	 case inreg:
	  sreg = regalt(dest.answhere);
	  if (!IS_R_NO_REG(sreg))
	  {
	    reg_operand_here(arg, sp, sreg);
	    break;
	  }
	  /* result being voided, treat as default */
	  /*FALLTHROUGH*/

	 default:
	  sreg = reg_operand(arg, sp);
	}

	setregalt(aa, sreg);
	mka.regmove = move(aa, dest, sp.fixed, is_signed(sh(e)));
	return mka;
      }

      switch (dest.answhere.discrim)
      {
      case inreg:
	{
	  /* +++
	   * if same size, paste down signed/unsigned to op
	   * paste down and adjust address as per big-endian
	   * for big-endians, use locate()
	   */

	  sreg = reg_operand(arg, sp);
	  dreg = regalt(dest.answhere);
	  if (IS_R_NO_REG(dreg))
	    return mka;		/* dest void */
	  inmem_dest = 0;
	  break;
	}

      case insomereg:
	{
	  int *dr = someregalt(dest.answhere);

	  COMMENT("make_code chvar_tag: dest insomereg");
	  sreg = reg_operand(arg, sp);
	  dreg = getreg(sp.fixed);
	  *dr = dreg;
	  inmem_dest = 0;
	  break;
	}

      default:
	{
	  sreg = reg_operand(arg, sp);
	  dreg = getreg(sp.fixed);
	  inmem_dest = 1;
	}
      }

      COMMENT2("make_code chvar_tag: shape %d to %d", from, to);

      if (inmem_dest && size_e <= shape_size(sh(arg)))
      {
	/* going to smaller sized memory, store will truncate */
	ans aa;

	setregalt(aa, sreg);
	sp=guardreg(sreg,sp);
	move(aa, dest, sp.fixed, 1);
	return mka;
      }

      ASSERT(from != to);		/* done above */

      /* shorten to type if needed */
      adjust_to_size(from,sreg,to,dreg,no_error_jump);
      if (inmem_dest)
      {
	ans aa;

	setregalt(aa, dreg);
	sp=guardreg(dreg,sp);
	move(aa, dest, sp.fixed, 1);
      }
      else
      {
	mka.regmove = dreg;
      }

      return mka;
    }				/* end chvar */

   case minus_tag:
    {
      if (ERROR_TREATMENT(e))
      {
	mka.regmove = minus_error_treatment(e,sp,dest);
      }
      else
      {
	mka.regmove = non_comm_op(e, sp, dest, i_s);
      }
      return mka;
    }				/* end minus */

   case mult_tag:
   case offset_mult_tag:
    {
      bool sgned = is_signed(sh(e));
      if (ERROR_TREATMENT(e))
      {
	mka.regmove = mult_error_treatment(e,sp,dest);
      }
      else
      {
	mka.regmove = do_mul_comm_op(e, sp, dest, sgned);
      }
      return mka;
    }

   case div0_tag:
   case div1_tag:
   case div2_tag:
   case offset_div_by_int_tag:
   case offset_div_tag:
    {
      bool sgned = is_signed(sh(e));

      mka.regmove = do_div_op(e, sp, dest, sgned);
      return mka;
    }				/* end div */

   case mod_tag:
   case rem0_tag:
   case rem2_tag:
    {
      bool sgned = is_signed(sh(e));

      mka.regmove = do_rem_op(e, sp, dest, sgned);
      return mka;
    }				/* end rem */

  case neg_tag:
  case offset_negate_tag:
    {
      if (ERROR_TREATMENT(e))
      {
	mka.regmove = neg_error_treatment(e,sp,dest);
      }
      else
      {
	int r = reg_operand(son(e),sp);
	int destr = regfrmdest(&dest,sp);
	space nsp;
	ans aa;

	nsp = guardreg(destr,sp);
	rr_ins(i_neg, r, destr);
#if 0
  tidyshort(destr, e);
#endif
	setregalt(aa,destr);
	mka.regmove = move(aa,dest,nsp.fixed,1);
	return mka;
      }
      return mka;
    }				/* end neg */

   case abs_tag:
    {
      if (ERROR_TREATMENT(e))
      {
	mka.regmove = abs_error_treatment(e,sp,dest);
        return mka;
      }
      else
      {
	int r = reg_operand(son(e),sp);
	int destr = regfrmdest(&dest,sp);
	space nsp;
	ans aa;

	nsp = guardreg(destr,sp);
	rr_ins(i_abs, r, destr);
#if 0
  tidyshort(destr, e);
#endif
	setregalt(aa,destr);
	mka.regmove = move(aa,dest,nsp.fixed,1);
	return mka;
      }
    }

  case shl_tag:
  case shr_tag:
    {
      exp s = son(e);
      exp b = bro(s);
      int a;
      int d;
      ans aa;
      space nsp;
      bool sgned = is_signed(sh(e));
      Instruction_P shift_ins;
      bool record_bit = isrecordbit(e);


#if 1
      int sz = shape_size(sh(s));
#if 0
      bool lded = ((name(s) == name_tag && regofval(s) >= 100)
		   || (name(s) == cont_tag &&
		      (name(son(s))!= name_tag || regofval(son(s)) > 0)
		      )
		  );
#endif
      bool signok = (sz == 32); /* better safe than sorry for the time being */
      if (name(son(e)) ==shl_tag && shape_size(sh(son(s)))!=32)
      {
	signok=1;
      }
#endif
      if (ERROR_TREATMENT(e))
      {
	fail("Unexpected error treatment for shl");
      }
      if (name(s) ==and_tag && name(b) ==val_tag &&
	 name(bro(son(s))) ==val_tag &&
	 is_a_mask(no(bro(son(s)))) &&
	 shape_size(sh(e)) ==32)
      {
	unsigned int mask= (unsigned int)no(bro(son(s)));
	int mask_left = left_of_mask(mask);
	int rotation_left;
	bool use_rlinm_ins = 0;

	if (name(e) ==shl_tag)
	{
	  int shift_left = no(b);
	  mask = mask<<shift_left;
	  rotation_left = shift_left;
	  use_rlinm_ins = 1;
	}
	else
	{
	  if (mask_left == 31 && is_signed(sh(e)))
	  {
	    use_rlinm_ins = 0;
	    /* sign extension */
	  }
	  else
	  {
	    int shift_right = no(b);
	    mask = mask>>shift_right;
	    rotation_left = 32 - shift_right;
	  }
	}

	if (use_rlinm_ins==1)
	{
	  a = reg_operand(son(s),sp);
	  d = regfrmdest(&dest,sp);

	  if (isrecordbit(e))
	  {
	    rlinm_ins(i_rlinm_cr,a,rotation_left,mask,d);
	  }
	  else
	  {
	    rlinm_ins(i_rlinm,a,rotation_left,mask,d);
	  }
	  setregalt(aa,d);
	  move(aa,dest,sp.fixed,0);
	  return mka;
	}
      }

      a = reg_operand(s, sp);

      if (!signok && name(e) ==shr_tag)
      {
	/*
	 * If doing a shift right we must sign extend
	 * or truncate prior to shifting
	 */
	adjust_to_size(ulonghd,a,name(sh(e)),a,no_error_jump);
      }
      if (name(e) == shr_tag)
      {
	if (record_bit==1)
	{
	  shift_ins = (sgned)? i_sra_cr : i_sr_cr;
	  cr0_set = 1;
	}
	else
	{
	  shift_ins = (sgned)? i_sra : i_sr;
	}
      }
      else
      {
	shift_ins = i_sl;
      }
      nsp = guardreg(a, sp);
      d = regfrmdest(&dest,nsp);

      if (name(b) == val_tag)
      {
	/* Only defined for shifts by 0..31 */
	int n = no(b);
	int n31 = n & 31;

	if (n == 0)
	{
	  mov_rr_ins(a, d);comment("shift by zero so just move");
	}
	else if (n == n31)
	{
	  rir_ins(shift_ins, a, n, d);	/* usual case */
	}
	else
	{			/* Undefined, produce same effect as if */
	  ld_const_ins(0, d);	/* not a constant,0 */
	}
      }
      else
      {
	int ar = reg_operand(b, nsp);
	rrr_ins(shift_ins, a, ar, d);
      }
      if (!signok && name(e) ==shl_tag)
      {
	/*
	 * If doing a shift left we must sign extend
	 * or truncate after the shift
	 */
	adjust_to_size(ulonghd,d,name(sh(e)),d,no_error_jump);
      }
      setregalt(aa, d);
      move(aa, dest, nsp.fixed, 1);
      mka.regmove = d;
      return mka;
    }				/* end shl, shr */

   case minptr_tag:
   case make_stack_limit_tag:
    {
      mka.regmove = non_comm_op(e, sp, dest, i_s);
      return mka;
    }

   case fplus_tag:
    {
      mka.regmove =
	fop(e, sp, dest, is_single_precision(sh(e))? i_fa : i_fa);
      return mka;
    }

  case fminus_tag:
    {
      mka.regmove =
	fop(e, sp, dest, is_single_precision(sh(e))? i_fs : i_fs);
      return mka;
    }

  case fmult_tag:
    {
      mka.regmove =
	fop(e, sp, dest, is_single_precision(sh(e))? i_fm : i_fm);
      return mka;
    }

  case fdiv_tag:
    {
      mka.regmove =
	fop(e, sp, dest, is_single_precision(sh(e))? i_fd : i_fd);
      return mka;
    }

  case fneg_tag:
    {
      mka.regmove =
	fmop(e, sp, dest, is_single_precision(sh(e))? i_fneg : i_fneg);
      return mka;
    }

  case fabs_tag:
    {
      mka.regmove =
	fmop(e, sp, dest, is_single_precision(sh(e))? i_fabs : i_fabs);
      return mka;
    }

  case float_tag:
    {
      exp in = son(e);
      int f = (dest.answhere.discrim == infreg)?
		fregalt(dest.answhere).fr :
		getfreg(sp.flt);
      freg frg;
      ans aa;
      bool from_sgned = is_signed(sh(in));

      frg.fr = f;
      frg.dble = is_double_precision(sh(e));

      if (from_sgned)
      {
	/* signed 32 bit to real */
	int r = reg_operand(in, sp);
	int f1 = getfreg(guardfreg(f, sp).flt);

	ld_const_ins(0x43300000, R_TMP0);
	st_ro_ins(i_st, R_TMP0, mem_temp(0));comment(NIL);
	ld_const_ins(0x80000000, R_TMP0);
	st_ro_ins(i_st, R_TMP0, mem_temp(4));comment(NIL);
	ldf_ro_ins(i_lfd, mem_temp(0), f);
	rir_ins(i_xor, r, 0x80000000, R_TMP0);
	st_ro_ins(i_st, R_TMP0, mem_temp(4));comment(NIL);
	ldf_ro_ins(i_lfd, mem_temp(0), f1);
	rrrf_ins(i_fs, f1, f, f);
      }
      else
      {
	/* unsigned 32 bit to real */
	int r = reg_operand(in, sp);
	int f1 = getfreg(guardfreg(f, sp).flt);

	ld_const_ins(0x43300000, R_TMP0);
	st_ro_ins(i_st, R_TMP0, mem_temp(0));comment(NIL);
	ld_const_ins(0x0, R_TMP0);
	st_ro_ins(i_st, R_TMP0, mem_temp(4));comment(NIL);
	ldf_ro_ins(i_lfd, mem_temp(0), f);
	st_ro_ins(i_st, r, mem_temp(4));comment(NIL);
	ldf_ro_ins(i_lfd, mem_temp(0), f1);
	rrrf_ins(i_fs, f1, f, f);
      }

      setfregalt(aa, frg);
      move(aa, dest, sp.fixed, 1);
      mka.regmove = (frg.dble)? - (f + 32):(f + 32);
      return mka;
    }

  case chfl_tag:
    {
      int to = name(sh(e));
      int from = name(sh(son(e)));
      bool dto = (to != shrealhd);
      bool dfrom = (from != shrealhd);

      if (dto==dfrom)
      {
	/* no change in representation */
	return make_code(son(e), sp, dest, exitlab);
      }
      else
      {
	freg frg;
	ans aa;
	where w;

	frg = fregfrmdest(dfrom,&dest,sp);
	setfregalt(aa, frg);
	w.answhere = aa;
	w.ashwhere = ashof(sh(son(e)));
	code_here(son(e), sp, w);
	if (to==shrealhd)
	{
	  if (ERROR_TREATMENT(e))
	  {
	    chfl_error_treatment(e,frg.fr);
	  }
	  else
	  {
	    rrf_ins(i_frsp,frg.fr,frg.fr);
	  }
	}
	frg.dble = dto;
	setfregalt(aa, frg);
	move(aa, dest, sp.fixed, 1);
	mka.regmove = (frg.dble)? - (frg.fr + 32):(frg.fr + 32);
	return mka;
      }
    }

  case and_tag:
    {
      exp arg1 = son(e);
      exp arg2 = bro(arg1);

      if (name(arg2) ==val_tag &&
	  is_a_mask(no(arg2)) &&
	  shape_size(sh(e)) ==32 &&
	 (name(arg1) ==shl_tag || name(arg1) ==shr_tag) &&
	  name(bro(son(arg1))) ==val_tag)
      {
	unsigned int mask = (unsigned int)no(arg2);
	int mask_left = left_of_mask(mask);
	int mask_right = right_of_mask(mask);
	bool use_rlinm_ins = 0;
	long rotation_left;

	if (name(arg1) ==shl_tag)
	{
	  int shift_left = no(bro(son(arg1)));
	  if (shift_left<=mask_right)
	  {
	    rotation_left = shift_left;
	    use_rlinm_ins=1;
	  }
	}
	else if (name(arg1) ==shr_tag)
	{
	  int shift_right = no(bro(son(arg1)));
	  if (shift_right<= (31-mask_left))
	  {
	    rotation_left = 32 - shift_right;
	    use_rlinm_ins=1;
	  }
	}

	if (use_rlinm_ins==1)
	{
	  int r = reg_operand(son(arg1),sp);
	  int dr = regfrmdest(&dest,sp);
	  ans aa;

	  if (isrecordbit(e))
	  {
	    rlinm_ins(i_rlinm_cr,r,rotation_left,mask,dr);
	  }
	  else
	  {
	    rlinm_ins(i_rlinm,r,rotation_left,mask,dr);
	  }
	  setregalt(aa,dr);
	  move(aa,dest,sp.fixed,0);
	  return mka;
	}
      }



      if (isrecordbit(e))
      {
	mka.regmove = comm_op(e, sp, dest, i_and_cr);
	cr0_set = 1;
      }
      else
      {
	mka.regmove = comm_op(e, sp, dest, i_and);
      }

      return mka;
    }

  case or_tag:
    {
      mka.regmove = comm_op(e, sp, dest, i_or);
      return mka;
    }

  case xor_tag:
    {
      mka.regmove = comm_op(e, sp, dest, i_xor);
      return mka;
    }

  case not_tag:
    {
      /* i_not is a pseudo instruction expanded to sfi dest,-1,src */
      int a1=reg_operand(son(e),sp);
      ans aa;
      int d=regfrmdest(&dest,sp);

      rr_ins(i_not,a1,d);
      adjust_to_size(ulonghd,d,name(sh(e)),d,no_error_jump);
      setregalt(aa,d);
      move(aa,dest,guardreg(d,sp).fixed,1);
      mka.regmove =d;

      return mka;
    }

  case cont_tag:
  case contvol_tag:
    {
      if (name(e) == contvol_tag)
      {
	/*
	 * Load contents of volatile location. Disable register-location
	 * tracing. Disable peep-hole optimisation (not possible with POWER
	 * assembler).
	 */
	COMMENT("make_code: Load volatile");
	clear_all();
      }
      /*
       * Check to see if we can use [reg+reg] addressing for this load
       */
      if (name(son(e)) == addptr_tag)
      {
	shape cont_shape = sh(e);
	int cont_size = shape_size(cont_shape);
	int cont_align = shape_align(cont_shape);
	exp addptr_sons = son(son(e));
	bool is_float = is_floating(name(cont_shape));

	if (last(bro(addptr_sons))
	    && cont_align == cont_size
	    && (cont_size==8 || cont_size==16 || cont_size==32 || is_float))
	{
	  int lhsreg;
	  int rhsreg;
	  bool sgned = (cont_size >= 32) || is_signed(cont_shape);
	  ans aa;

	  COMMENT("make_code: load suitable for [reg+reg] addressing");

	  lhsreg = reg_operand(addptr_sons, sp);
	  rhsreg = reg_operand(bro(addptr_sons), guardreg(lhsreg, sp));

	  if (is_float)
	  {
	    freg dfreg;

	    if (dest.answhere.discrim == infreg)
	    {
	      dfreg = fregalt(dest.answhere);
	    }
	    else
	    {
	      dfreg.fr = getfreg(sp.flt);
	    }
	    dfreg.dble = (cont_size==64);
	    ldf_rr_ins((dfreg.dble?i_lfd :i_lfs), lhsreg, rhsreg, dfreg.fr);
	    setfregalt(aa, dfreg);
	  }
	  else
	  {
	    int dreg = regfrmdest(&dest,sp);

	    ld_rr_ins(i_ld_sz(cont_size,sgned), lhsreg, rhsreg, dreg);
	    if (sgned && cont_size==8)
	    {
	      /* No load signed byte instruction, so propagate sign */
	      adjust_to_size(ulonghd,dreg,scharhd,dreg,no_error_jump);
	    }
	    setregalt(aa, dreg);
	  }

	  mka.regmove = move(aa, dest, sp.fixed, sgned);
	  if (name(e) == contvol_tag)
	    mka.regmove = NOREG;
	  return mka;
	}
      }
    }
    /*FALLTHROUGH*/
   case name_tag:
   case field_tag:
   case reff_tag:
   case addptr_tag:
   case subptr_tag:
    {
      where w;
      bool sgned;
      int dr = (dest.answhere.discrim == inreg)? dest.answhere.val.regans : 0;
      w = locate(e, sp, sh(e), dr);		/* address of arg */
      sgned = (w.ashwhere.ashsize >= 32) || is_signed(sh(e));
      /* +++ load real into float reg, move uses fixed reg */
      mka.regmove = move(w.answhere, dest,(guard(w, sp)).fixed, sgned);
      if (name(e) == contvol_tag)
	mka.regmove = NOREG;
      return mka;
    }				/* end cont */

  case string_tag:
  case real_tag:
    {
      instore isa;
      ans aa;
      bool sgned = ((ashof(sh(e)).ashsize >= 32) || is_signed(sh(e)));

      /*
       * Place constant in appropriate data segment
       */
      isa = evaluated_const(e);
      setinsalt(aa, isa);
      mka.regmove = move(aa, dest, sp.fixed, sgned);
      return mka;
    }				/* end eval */

  case val_tag:
    {
      int size = shape_size(sh(e));


      if (size == 64)
      {
		/*
		 * Could be evaluating into nowhere so check to see it is
		 * trying to evaluate into a genuine place
		 */
        if (dest.answhere.discrim==notinreg)
        {
          flt64 temp;
          int ov;
          int r = getreg(sp.fixed);
          space nsp;
          ans aa;
          if (isbigval(e)) {
	  temp = flt_to_f64(no(e), 0, &ov);
          }
          else {
            temp.big = (is_signed(sh(e)) && no(e) <0)?-1:0;
            temp.small = no(e);
          }
          nsp = guardreg(r, sp);
          ld_const_ins(temp.small,r);
          setregalt(aa,r);
          dest.ashwhere.ashsize = 32;
          dest.ashwhere.ashalign = 32;
          move(aa,dest,nsp.fixed,1);
          ld_const_ins(temp.big,r);
          ASSERT(dest.answhere.discrim==notinreg);
          dest.answhere.val.instoreans.b.offset+=4;
          move(aa,dest,nsp.fixed,1);
        }

	return mka;
      }
      else  if (no(e) == 0)
      {
	goto moveconst_zero;
      }
      else
      {
	if (size == 32 || !is_signed(sh(e)))
	{
	  /* 32 bit size or unsigned */
	  constval = no(e);
	}
	else if (size == 8)
	{
	  constval = no(e) & 255;
	  constval -= (constval & 128) << 1;
	}
	else if (size == 16)
	{
	  constval = no(e) & 65535;
	  constval -= (constval & 32768) << 1;
	}
	else
	{
	  fail("make_code val_tag: unexpected bit size");
	}
	goto moveconst;
      }
    }

    case top_tag:
    case prof_tag:
    {
      return mka;
    }

  case null_tag:
    {
      goto moveconst_zero;
    }

  case round_tag:
    {
      int sfr;
      int destr;
      ans aa;
      bool changed_mode = 0;
      bool call_fctiwz =0;
      int ifr = getfreg(sp.flt);

      sfr = freg_operand(son(e), sp, getfreg(sp.flt));
      /* Doesn't matter if sfr and ifr same */
      switch (round_number(e))
      {
        case R2ZERO:call_fctiwz=1;break;
        case R2NEAR:break;
        case R2PINF:mtfsb1_ins(30);mtfsb0_ins(31);changed_mode=1;break;
        case R2NINF:mtfsb1_ins(30);mtfsb1_ins(31);changed_mode=1;break;
        case 4: break;
        default: fail("Unknown rounding mode");break;
      }
      /* can use fctiw command */

      destr=regfrmdest(&dest,sp);
      rrf_ins(call_fctiwz?i_fctiwz:i_fctiw,sfr,ifr);
      stf_ins(i_stfd,ifr,mem_temp(0));
      ld_ro_ins(i_l,mem_temp(4),destr);comment(NIL);

      if (changed_mode)
      {
	/* put it back to round_to_nearest */
	mtfsb0_ins(30);mtfsb0_ins(31);
      }
      adjust_to_size(ulonghd,destr,name(sh(e)),destr,no_error_jump);
      setregalt(aa, destr);
      mka.regmove = move(aa, dest, sp.fixed, 1);
      return mka;
    }

     case int_to_bitf_tag:
    {
      int r;
      int size_res = shape_size(sh(e));
      int size_op = shape_size(sh(son(e)));
      ans aa;
      space nsp;

      r = reg_operand(son(e), sp);

      COMMENT2("make_code int_to_bitf_tag: size %d -> %d", size_op, size_res);

      /* maybe this not needed if going to memory +++ */
      if (size_res != size_op && size_res != 32)
      {
	int destr;

	switch (dest.answhere.discrim)
	{
	case inreg:
	  {
	    destr = regalt(dest.answhere);
	    break;
	  }
	default:
	  {
	    destr = getreg(sp.fixed);
	  }
	}

	rir_ins(i_and, r,(1 << size_res) - 1, destr);
	r = destr;
      }

      /* r is appropriately truncated operand */

      nsp = guardreg(r, sp);
      setregalt(aa, r);
      move(aa, dest, nsp.fixed, 0);
      return mka;
    }

  case bitf_to_int_tag:
    {
      ash a;
      int r;
      where w;
      bool src_sgned = name(sh(son(e))) & 1;
      bool target_sgned = name(sh(e)) & 1;

      a = ashof(sh(son(e)));
      switch (dest.answhere.discrim)
      {
      case inreg:
	{
	  r = regalt(dest.answhere);
	  break;
	}
      default:
	{
	  r = getreg(sp.fixed);
	}
      }

      setregalt(w.answhere, r);
      w.ashwhere = a;
      code_here(son(e), sp, w);

      COMMENT1("make_code bitsint_tag: size=%d", a.ashsize);

      if (a.ashsize != 32 && src_sgned != target_sgned)
      {
	/* propogate/correct sign bits */
	/* +++ make move() handle this by pasting sign down */
	ash atarget;
	atarget = ashof(sh(e));

	COMMENT4("make_code bitsint_tag: adjusting to sign/size %d/%d -> %d/%d",
		src_sgned, a.ashsize,
		target_sgned, atarget.ashsize);

	if (target_sgned)
	{
	  if (32 - a.ashsize == 16)
	  {
	    rr_ins(i_exts, r, r);
	  }
	  else
	  {
	    rir_ins(i_sl, r, 32 - a.ashsize, r);
	    rir_ins(i_sra, r, 32 - a.ashsize, r);
	  }
	}
	else
	{
	  rir_ins(i_and, r,((1 << a.ashsize) - 1), r);
	}
      }

      move(w.answhere, dest, guardreg(r, sp).fixed, 0);
      keepreg(e, r);
      return mka;
    }

   case movecont_tag:
    {
      exp szarg = bro(bro(son(e)));
      int dr,sr,szr;
      int lout=new_label();
      int creg=next_creg();
      space nsp;
      int bytemove;
      where w;

      sr = getreg(sp.fixed);
      setregalt(w.answhere, sr);
      w.ashwhere = ashof(sh(son(e)));
      make_code(son(e), sp, w, 0);
      nsp = guardreg(sr,sp);
      dr = getreg(nsp.fixed);
      setregalt(w.answhere, dr);
      make_code(bro(son(e)), nsp, w, 0);
      nsp = guardreg(dr,nsp);
      w.ashwhere = ashof(sh(bro(bro(son(e)))));
      szr = getreg(nsp.fixed);
      setregalt(w.answhere, szr);
     (void)make_code(szarg, nsp, w, 0);
      nsp = guardreg(szr, nsp);
      bytemove = (al2(sh(szarg)) >>3);
#if 0
      clear_dep_reg(son(e));
      clear_dep_reg(bro(son(e)));
#else
      clear_all();
#endif
      if (name(szarg)!= val_tag || no(szarg) == 0) {
	cmp_ri_ins(i_cmp, szr, 0, creg);
	bc_ins(i_beq, creg, lout,UNLIKELY_TO_JUMP);
      }
      if (isnooverlap(e)) {
	move_dlts(dr,sr,szr, bytemove);
      }
      else {
	int gtlab = new_label();
	int creg2 = next_creg();
	cmp_rr_ins(i_cmp, dr, sr,creg2);
	bc_ins(i_bgt, creg2, gtlab,LIKELY_TO_JUMP);
	move_dlts(dr,sr,szr, bytemove);
	uncond_ins(i_b, lout);
	set_label(gtlab);
	move_dgts(dr,sr,szr, bytemove);
      }
      set_label(lout);
      return mka;
    }


   case offset_pad_tag:
    {
      int r;
      int v;
      ans aa;
      if (al2(sh(son(e))) >= al2(sh(e)))
      {
	if (al2(sh(e))!= 1 || al2(sh(son(e))) == 1)
	{
	  /*
	   * Is already aligned correctly,
	   * whether as bit or byte-offset
	   */
	  e = son(e); goto tailrecurse;
	}
	r = regfrmdest(&dest, sp);
	v = reg_operand(son(e), sp);
	rir_ins(i_sl,  v, 3 ,r);
      }
      else {
	int al = (al2(sh(son(e))) ==1)?al2(sh(e)):(al2(sh(e)) /8);
	r = regfrmdest(&dest, sp);
	v = reg_operand(son(e), sp);
	rir_ins(i_a, v, al-1, r);
	rir_ins(i_and, r, -al ,r);
	if (al2(sh(son(e))) ==1)
	{ /*
	   * operand is bit-offset,
	   * byte-offset required
	   */
	  rir_ins(i_sra, r, 3 ,r);
	}
      }
      setregalt(aa,r);
      mka.regmove = move(aa, dest, guardreg(r,sp).fixed, 0);
      return mka;
    }


   case min_tag:
   case max_tag:
   case offset_max_tag:
    {
      ans aa;
      int left;
      int right;
      int r=regfrmdest(&dest, sp);
      int creg;
      int lab;

      space nsp;
      if (IsRev(e))
      {
	right=reg_operand(bro(son(e)),sp);
	nsp=guardreg(right,sp);
	left=reg_operand(son(e),nsp);
      }
      else
      {
	left=reg_operand(son(e),sp);
	nsp=guardreg(left,sp);
	right=reg_operand(bro(son(e)),nsp);
      }
      creg = next_creg();
      lab = new_label();
      cmp_rr_ins(i_cmp,left,right,creg);
      mov_rr_ins(left,r);comment(NIL);
      if (name(e) ==min_tag)
      {
	bc_ins(i_blt,creg,lab,LIKELY_TO_JUMP);
      }
      else
      {
	bc_ins(i_bgt,creg,lab,LIKELY_TO_JUMP);
      }
      mov_rr_ins(right,r);comment(NIL);
      set_label(lab);
      setregalt(aa,r);
      move(aa, dest,guardreg(r,sp).fixed , 0);
      mka.regmove = r;
      return mka;
    }

   case offset_add_tag:
    {
      /*
       * byte offset + bit offset
       * All others converted to plus_tag by needscan.
       * The byte offset must be converted into bits for the addition
       */
      exp byte_offset = son(e);
      exp bit_offset = bro(byte_offset);
      int destr = regfrmdest(&dest,sp);
      int byte_offset_reg = reg_operand(byte_offset, sp);
      int bit_offset_reg;
      space nsp;
      ans aa;
      nsp = guardreg(destr, sp);

      rir_ins(i_sl , byte_offset_reg , 3 , destr);
      if (name(bit_offset) ==val_tag)
      {
	if (no(bit_offset)!=0)
	{
	  rir_ins(i_a, destr , no(bit_offset), destr);
	}
      }
      else
      {
	bit_offset_reg = reg_operand(bit_offset, nsp);
	rrr_ins(i_a, destr , bit_offset_reg , destr);
      }
      setregalt(aa, destr);
      mka.regmove = move(aa, dest, nsp.fixed, 0);
      return mka;
    }

   case offset_subtract_tag:
    {
     /*
      * bit offset - byte offset
      * All others converted to minus_tag by needscan.
      */
     exp bit_offset = son(e);
     exp byte_offset = bro(bit_offset);
     int destr = regfrmdest(&dest, sp);
     int byte_offset_reg = reg_operand(byte_offset, sp);
     int bit_offset_reg;
     space nsp;
     ans aa;
     nsp = guardreg(destr,sp);

     rir_ins(i_sl, byte_offset_reg , 3 , destr);
     if (name(bit_offset) ==val_tag)
     {
       if (no(bit_offset)!=0)
       {
	 rir_ins(i_s,destr,no(bit_offset),destr);
       }
     }
     else
     {
       bit_offset_reg = reg_operand(bit_offset, nsp);
       rrr_ins(i_s, destr, bit_offset_reg, destr);
     }
     setregalt(aa, destr);
     mka.regmove = move(aa, dest, nsp.fixed, 0);
     return mka;
   }

   case current_env_tag:
    {
      int r=regfrmdest(&dest, sp);
      ans aa;
      if (p_has_fp)
      {
       	mov_rr_ins(R_FP,r);comment("move FP to register");
      }
      else
      {
	/*
	 * If we don't have a frame pointer we give the location
	 * of where the frame pointer would be anyway.
	 */
	rir_ins(i_a , R_SP, p_frame_size , r);
      }
      setregalt(aa, r);
      mka.regmove = move(aa, dest, sp.fixed, 0);
      return mka;
    }

   case env_offset_tag:
    {
      /* NOTE: env_offset works in conjunction with current_env.
	 So it must be consistent with current env */
      constval = frame_offset(son(e));
      goto moveconst;
    }

   case goto_lv_tag:
    {
      int r = reg_operand(son(e),sp);

      mt_ins(i_mtctr, r);
      z_ins(i_bctr);
      clear_all();
      return mka;
    }

   case make_lv_tag:
    {
      int r = regfrmdest(&dest,sp);
      int next_mlv_number = get_next_mlv_number();
      ans aa;
      long marked_label= no(son(pt(e)));

      fprintf(as_file,"\t.toc\n");
      fprintf(as_file,"T.make_lv%d:\t.tc\tL.%d[TC],L.%d\n",
	      next_mlv_number,(int)marked_label,(int)marked_label);
      fprintf(as_file, "\t.csect\t[PR]\n");
      fprintf(as_file, "\t%s\t%d,T.make_lv%d(2)\n",
	      get_instruction(i_l),r,next_mlv_number);
      clear_reg(r);
      setregalt(aa,r);
      move(aa,dest, guardreg(r,sp).fixed, 0);
      mka.regmove = r;
      return mka;
    }

   case long_jump_tag:
    {
     int fp = reg_operand(son(e), sp);
     int labval = reg_operand(bro(son(e)), sp);
     /*
      * Long jumps are always done through the frame pointer
      * since you cannot tell whether or not you are going in
      * to a proc which needs a frame pointer or not
      * so it is made sure that any procedure that has
      * make_local_lv and current_env is forced to have a
      * frame pointer.
      */
     FULLCOMMENT("long_jump");
     mov_rr_ins(fp,R_FP);comment("move register to FP");
     mt_ins(i_mtctr, labval);
     z_ins(i_bctr);
     clear_all();
     return mka;
   }

   case alloca_tag:
    {
      int dreg = regfrmdest(&dest,sp);
      ans aa;
      int xdreg = (IS_R_TMP(dreg) && checkalloc(e))? getreg(sp.fixed): dreg;

      ASSERT(p_has_alloca);
      ASSERT(p_has_fp);



      if (name(son(e)) ==val_tag)
      {
	/* allocate constant number of bytes on stack*/
	int no_of_bytes = ALLOCA_ALIGNMENT(no(son(e)));
	if (checkalloc(e))
	{
	  rir_ins(i_a,R_SP,- (long)no_of_bytes,xdreg);
	}
	else
	{
	  rir_ins(i_a,R_SP,- (long)no_of_bytes,R_SP);
	}
      }
      else
      {
	int nreg = reg_operand(son(e),sp);
	/* adjust to correct alignment, i.e mul of 8 */
	rir_ins(i_a,nreg,7,R_TMP0);
	rir_ins(i_and,R_TMP0,~7,R_TMP0);
	if (checkalloc(e))
	{
	  rrr_ins(i_sf,R_TMP0,R_SP,xdreg);
	}
	else
	{
	  rrr_ins(i_sf,R_TMP0,R_SP,R_SP);
	}

      }
      if (checkalloc(e))
      {
	baseoff b;
	int cr;
	int slab;
	b = find_tg("__TDFstacklim");
	cr = next_creg();

	slab = get_stack_overflow_lab();
	ld_ins(i_l,b,R_TMP0);
	cmp_rr_ins(i_cmp,xdreg,R_TMP0,cr);
	bc_ins(i_blt,cr,slab,UNLIKELY_TO_JUMP);
	mov_rr_ins(xdreg,R_SP);comment(NIL);
      }



      if (p_args_and_link_size==0)
      {
	mov_rr_ins(R_SP,dreg);comment(NIL);
      }
      else
      {
	rir_ins(i_a, R_SP, p_args_and_link_size,dreg);
      }
      if (p_has_back_chain)
      {
	save_back_chain_using_frame_pointer();
      }
      if (p_has_saved_sp)
      {
	save_sp_on_stack();
      }
      setregalt(aa,dreg);
      move(aa,dest,guardreg(dreg,sp).fixed,0);
      return mka;
    }

   case last_local_tag:
    {
      int r = regfrmdest(&dest, sp);
      ans aa;
      /* The last pointer returned by alloca is placed into r */

      if (p_args_and_link_size !=0)
      {
	rir_ins(i_a, R_SP, p_args_and_link_size, r);
      }
      else
      {
	mov_rr_ins(R_SP , r);comment(NIL);
      }
      setregalt(aa, r);
      mka.regmove = move(aa, dest, sp.fixed, 1);
      return mka;
    }

   case local_free_all_tag:
    {
      if (p_has_alloca)
      {
	/* The stack pointer is returned to how it was before
	   any calls to alloca were made */
	rir_ins(i_a , R_FP,- p_frame_size ,R_SP);
	if (p_has_back_chain)
	{
	  save_back_chain_using_frame_pointer();
	}
	if (p_has_saved_sp)
	{
	  save_sp_on_stack();
	}
      }
      return mka;
    }

   case local_free_tag:
    {
      int r;
      int off;
      space nsp;

      ASSERT(p_has_alloca);
      r = reg_operand(son(e), sp);
      /* r is a pointer returned by alloca
	 off is the number of bytes to free up */
      if (name(bro(son(e))) ==val_tag)
      {
	int displacement=ALLOCA_ALIGNMENT(no(bro(son(e))));
	displacement -= p_args_and_link_size;
	if (displacement!=0)
	{
	  rir_ins(i_a,r,displacement,R_SP);
	}
	else
	{
	  mov_rr_ins(r,R_SP);comment(NIL);
	}
      }
      else
      {
	nsp=guardreg(r,sp);
	off = reg_operand(bro(son(e)),nsp);

	rir_ins(i_a,off,7,off);
	rir_ins(i_and,off,~7,off);
	rrr_ins(i_a,r,off,R_SP);
	if (p_args_and_link_size !=0)
	{
	  rir_ins(i_a , R_SP , - p_args_and_link_size , R_SP);
	}
      }
      if (p_has_back_chain)
      {
	save_back_chain_using_frame_pointer();
      }
      if (p_has_saved_sp)
      {
	save_sp_on_stack();
      }
      return mka;
    }

/*
 * SPEC 3.1 constructions
 */

   case locptr_tag:
    {
      /*
	   * This is the only way of accessing callers in a general proc
       * when calculating general_env_offset using current_env
	   */
      int destr = regfrmdest(&dest,sp);
      int pr = reg_operand(son(e),sp);
      space nsp;
      baseoff b;
      ans aa;
      b.base = pr;
      b.offset = 0; /* R_TP lives where R_FP is for general proc */
      ld_ro_ins(i_l,b,destr);comment("locptr:get TP using FP");
      setregalt(aa,destr);
      nsp = guardreg(destr,sp);
      mka.regmove = move(aa,dest,nsp.fixed,0);
      return mka;
    }

   case apply_general_tag:		/* procedure call */
    {
      return make_apply_general_tag_code(e, sp, dest, exitlab);
    }

   case tail_call_tag:
    {
      make_tail_call_tag_code(e,sp);
      return mka;
    }

   case make_callee_list_tag:
    {
      make_callee_list_tag_code(e,sp);
      return mka;
    }

   case same_callees_tag:
    {
      make_same_callees_tag_code(e,sp);
      return mka;
    }

   case make_dynamic_callee_tag:
    {
      make_dynamic_callee_tag_code(e,sp);
      return mka;
    }

   case caller_name_tag:
    {
      return mka;
    }

   case return_to_label_tag:
    {
      make_return_to_label_tag_code(e,sp);
      return mka;
    }

   case set_stack_limit_tag:
    {
      baseoff b;
      int r = reg_operand(son(e),sp);
      b = find_tg("__TDFstacklim");
      st_ins(i_st,r,b);
      return mka;
    }

   case env_size_tag:
    {
      exp tg = son(son(e));
      procrec * pr = &procrecs[no(son(tg))];
      constval = ((pr->frame_size) >>3) + pr->max_callee_bytes;
      goto moveconst;
    }

   case trap_tag:
    {
      do_trap(e);
      return mka;
    }

   default:
    fail("TDF construct not done yet in make_code");
  }				/* end outer switch */
  ASSERT(0);			/* should have return/goto from switch */
#ifdef DEBUG_POWERTRANS
  showme(e,0,1);
#endif
  /*NOTREACHED*/


 moveconst_zero:
  constval = 0;
 moveconst:
  {
    int r = regfrmdest(&dest,sp);
    ans aa;
    FULLCOMMENT1("load constval = %d",constval);
    ld_const_ins(constval, r);
    setregalt(aa, r);
    move(aa, dest, guardreg(r, sp).fixed, 1);
    mka.regmove = r;
    return mka;
  }
}				/* end make_code */

void move_dlts(int dr, int sr, int szr, int bytemove)
  /* move szr bytes to dr from sr (using R_TMP0)- either nooverlap or dr<=sr */
{
  baseoff sr_baseoff;
  baseoff dr_baseoff;
  int lin = new_label();

  sr_baseoff.base = sr;
  sr_baseoff.offset = 1;
  dr_baseoff.base = dr;
  dr_baseoff.offset = 1;
  /* +++ could do this word at a time? */
  rir_ins(i_a,sr,-1,sr);               /* ai     sr,sr,-1 */
  rir_ins(i_a,dr,-1,dr);               /* ai     dr,dr,-1 */
  mt_ins(i_mtctr,szr);                 /* mtctr  szr      */
  set_label(lin);                      /* L.???           */
  ld_ro_ins(i_lbzu,sr_baseoff,R_TMP0); /* lbzu   0,1(sr)  */comment(NIL);
  st_ro_ins(i_stbu,R_TMP0,dr_baseoff); /* stbu   0,1(dr)  */comment(NIL);
  uncond_ins(i_bdn, lin);              /* bdn    L.???    */
  rrr_ins( i_sf, szr , sr ,sr );       /* sf     sr,szr,sr*/
  rir_ins( i_a , sr , 1, sr);          /* ai     sr,sr,1  */
  rrr_ins( i_sf, szr , dr ,dr );       /* sf     dr,szr,dr*/
  rir_ins( i_a ,dr , 1, dr );          /* ai     dr,dr,1  */
  return;
}

void move_dgts(int dr, int sr, int szr, int bytemove)
	/* move szr bytes to dr from sr (using R_TMP0) with overlap and dr>sr */
{
  baseoff sr_baseoff;
  baseoff dr_baseoff;
  int lin = new_label();

  sr_baseoff.base = sr;
  sr_baseoff.offset = -1;
  dr_baseoff.base = dr;
  dr_baseoff.offset = -1;
  /* +++ could do this word at a time? */
  rrr_ins(i_a,sr,szr,sr);              /* a      sr,szr,sr */
  rrr_ins(i_a,dr,szr,dr);              /* a      dr,szr,dr */
  mt_ins(i_mtctr,szr);                 /* mtctr  szr       */
  set_label(lin);                      /* L.???            */
  ld_ro_ins(i_lbzu,sr_baseoff,R_TMP0); /* lbzu   0,-1(sr)  */comment(NIL);
  st_ro_ins(i_stbu,R_TMP0,dr_baseoff); /* stbu   0,-1(dr)  */comment(NIL);
  uncond_ins(i_bdn, lin);              /* bdn    L.???     */
  return;
}

int regfrmdest(where * dest, space sp)
{
  switch (dest->answhere.discrim) {
   case inreg:
    {
      return regalt(dest->answhere);

    }
   default:
    {
      return getreg(sp.fixed);
    }
  }
}
freg fregfrmdest(bool dble, where * dest, space sp)
{
  freg fr;

  switch (dest->answhere.discrim)
  {
   case infreg:
    {
      fr = fregalt(dest->answhere);
      fr.dble = dble;
      return fr;
    }
   default:
    {
      fr.dble = dble;
      fr.fr = getfreg(sp.flt);
      return fr;
    }

  }
}

static int get_next_mlv_number(void)
{
  static int next_lv_number=0;
  next_lv_number++;
  return next_lv_number;
}



void adjust_to_size(int src_shpe, int sreg, int dest_shpe, int dreg, int trap)
{

/*
 *     0 means nothing to be done
 *
 *                        d   e   s   t
 *
 *                    s   u   s   u   s   u
 *                    c   c   w   w   l   l
 *                    h   h   o   o   o   o
 *                    a   a   r   r   n   n
 *  		  r   r   d   d   g   g
 *
 *        schar       0   X   0   X   0   0
 *        uchar       X   0   0   0   0   0
 *   s    sword       X   X   0   X   0   0
 *   r    uword       X   X   X   0   0   0
 *   c    slong       X   X   X   X   0   0
 *        ulong       X   X   X   X   0   0
 */
  /* Perform the options on the above table */
  if (src_shpe == dest_shpe ||
      dest_shpe == slonghd  ||
      dest_shpe == ulonghd  ||
     (src_shpe == scharhd && dest_shpe == swordhd) ||
     (src_shpe == ucharhd && dest_shpe != scharhd))
  {
    /* Do no adjustment */
    if (sreg!=dreg)
    {
      mov_rr_ins(sreg,dreg);comment(NIL);
    }
    return;
  }


  if (trap==no_error_jump)
  {
    switch (dest_shpe)
    {
     case scharhd:
      if (cpu == CPU_POWERPC)
      {
	rr_ins(i_extsb,sreg,dreg);
      }
      else
      {
	rir_ins(i_sl, sreg, 24, dreg);
	rir_ins(i_sra, dreg, 24, dreg);
      }
      break;
     case ucharhd:
      rir_ins(i_and, sreg, 0x000000ff, dreg);
      break;
     case swordhd:
      rr_ins(i_exts, sreg, dreg);
      break;
     case uwordhd:
      rir_ins(i_and, sreg, 0x0000ffff, dreg);
      break;
     case slonghd:
     case ulonghd:
      mov_rr_ins(sreg,dreg);comment(NIL);
      break;
     default:
      fail("Unexpected integer shape in adjust_to_size");
      break;
    }
  }
  else
  {
    switch (dest_shpe)
    {
     case scharhd:
      testsigned(sreg, -128, 127, trap);
      if (sreg !=dreg) { mov_rr_ins(sreg,dreg);comment(NIL); }
      break;
     case ucharhd:
      testusigned(sreg,255,trap);
      if (sreg !=dreg) { mov_rr_ins(sreg,dreg);comment(NIL); }
      break;
     case swordhd:
      testsigned(sreg,-0x8000,0x7fff,trap);
      if (sreg !=dreg) { mov_rr_ins(sreg,dreg);comment(NIL); }
      break;
     case uwordhd:
      testusigned(sreg,0xffff,trap);
      if (sreg !=dreg) { mov_rr_ins(sreg,dreg);comment(NIL); }
      break;
     case slonghd:
     case ulonghd:
      mov_rr_ins(sreg,dreg);comment(NIL);
      break;
     default:
      fail("Unexpected integer shape in adjust_to_size");
      break;
    }
  }
  return;
}
