/*
 * Copyright (c) 2002, The Tendra Project <http://www.ten15.org/>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *
 *    		 Crown Copyright (c) 1997
 *    
 *    This TenDRA(r) Computer Program is subject to Copyright
 *    owned by the United Kingdom Secretary of State for Defence
 *    acting through the Defence Evaluation and Research Agency
 *    (DERA).  It is made available to Recipients with a
 *    royalty-free licence for its use, reproduction, transfer
 *    to other parties and amendment for any purpose not excluding
 *    product development provided that any such use et cetera
 *    shall be deemed to be acceptance of the following conditions:-
 *    
 *        (1) Its Recipients shall ensure that this Notice is
 *        reproduced upon any copies or amended versions of it;
 *    
 *        (2) Any amended version of it shall be clearly marked to
 *        show both the nature of and the organisation responsible
 *        for the relevant amendment or amendments;
 *    
 *        (3) Its onward transfer from a recipient to another
 *        party shall be deemed to be that party's acceptance of
 *        these conditions;
 *    
 *        (4) DERA gives no warranty or assurance as to its
 *        quality or suitability for any purpose and DERA accepts
 *        no liability whatsoever in relation to any use to which
 *        it may be put.
 *
 * $TenDRA$
 */


#ifndef readglob_key
#define readglob_key 1



extern dec * capsule_tagtab;
/* the table of tags which are available at the capsule level */

extern tok_define * capsule_toktab;
/* the table of tokens which are available at the capsule level */

extern aldef * capsule_altab;
/* the table of alignment tags which are available at the
 *     capsule level */

extern diag_tagdef * capsule_diag_tagtab;	/* OLD DIAGS */
/* the table of diagnostic tags which are available at the
 *     capsule level */

extern dgtag_struct * capsule_dgtab;		/* NEW DIAGS */
/* the table of diagnostic name tags which are available at the
 *     capsule level */

extern int capsule_no_of_tokens;
/* the number of tokens at the capsule level */

extern int capsule_no_of_tags;
/* the number of tags at the capsule level */

extern int capsule_no_of_als;
/* the number of alignment tags at the capsule level */

extern int capsule_no_of_diagtags;	/* OLD DIAGS */
/* the number of diagnostic tags at the capsule level */

extern int capsule_no_of_dgtags;	/* NEW DIAGS */
/* the number of diagnostic tags at the capsule level */


extern dec * unit_tagtab;
/* the table of tags for the current unit */

extern tok_define * unit_toktab;
/* the table of tokens for the current unit */

extern aldef * unit_altab;
/* the table of alignment tags for the current unit */

extern int unit_no_of_tokens;
/* the number of tokens in the current unit */

extern int unit_no_of_tags;
/* the number of tags in the current unit */

extern int unit_no_of_als;
/* the number of alignment tags in the current unit */

extern dec ** unit_ind_tags;
/* table of pointers to the tags used in the current unit */

extern tok_define ** unit_ind_tokens;
/* table of pointers to the tokens used in the current unit */

extern aldef ** unit_ind_als;
/* table of pointers to the alignment tags used in the current unit */


extern exp * unit_labtab;
/* table of labels used in the current unit */

extern int unit_no_of_labels;
/* number of labels used in the current unit */

extern int crt_tagdef_unit_no;
/* number of current tagdef unit. -1 before tagdef units */

extern int unit_index;

extern char * make_local_name(void);

extern int rep_make_proc;

extern BoolT replace_arith_type;

extern int good_trans;

extern char *crt_filename;

extern capsule_frees * capsule_freelist;

extern int newcode;

extern char * add_prefix(char *);

extern char * external_to_string(external);

#ifdef NEWDIAGS
extern int within_diags;
#endif

#endif
