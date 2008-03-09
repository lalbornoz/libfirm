/*
 * Copyright (C) 1995-2008 University of Karlsruhe.  All right reserved.
 *
 * This file is part of libFirm.
 *
 * This file may be distributed and/or modified under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation and appearing in the file LICENSE.GPL included in the
 * packaging of this file.
 *
 * Licensees holding valid libFirm Professional Edition licenses may use
 * this file in accordance with the libFirm Commercial License.
 * Agreement provided with the Software.
 *
 * This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
 * WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE.
 */

/**
 * @file
 * @brief    Memory disambiguator
 * @author   Michael Beck
 * @date     27.12.2006
 * @version  $Id$
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>

#include "irnode_t.h"
#include "irgraph_t.h"
#include "irprog_t.h"
#include "irmemory.h"
#include "irflag.h"
#include "hashptr.h"
#include "irflag.h"
#include "irouts.h"
#include "irgwalk.h"
#include "irprintf.h"
#include "debug.h"
#include "error.h"

/** The debug handle. */
DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL;)

/** The source language specific language disambiguator function. */
static DISAMBIGUATOR_FUNC language_disambuigator = NULL;

/** The global memory disambiguator options. */
static unsigned global_mem_disamgig_opt = aa_opt_no_opt;

/* Returns a human readable name for an alias relation. */
const char *get_ir_alias_relation_name(ir_alias_relation rel) {
#define X(a) case a: return #a
	switch (rel) {
	X(no_alias);
	X(may_alias);
	X(sure_alias);
	default: assert(0); return "UNKNOWN";
	}
#undef X
}

/* Get the memory disambiguator options for a graph. */
unsigned get_irg_memory_disambiguator_options(ir_graph *irg) {
	unsigned opt = irg->mem_disambig_opt;
	if (opt & aa_opt_inherited)
		return global_mem_disamgig_opt;
	return opt;
}  /* get_irg_memory_disambiguator_options */

/*  Set the memory disambiguator options for a graph. */
void set_irg_memory_disambiguator_options(ir_graph *irg, unsigned options) {
	irg->mem_disambig_opt = options & ~aa_opt_inherited;
}  /* set_irg_memory_disambiguator_options */

/* Set the global disambiguator options for all graphs not having local options. */
void set_irp_memory_disambiguator_options(unsigned options) {
	global_mem_disamgig_opt = options;
}  /* set_irp_memory_disambiguator_options */

/**
 * Find the base address and entity of an Sel node.
 *
 * @param sel  the node
 * @param pEnt after return points to the base entity.
 *
 * @return the base address.
 */
static ir_node *find_base_adr(ir_node *sel, ir_entity **pEnt) {
	ir_node *ptr = get_Sel_ptr(sel);

	while (is_Sel(ptr)) {
		sel = ptr;
		ptr = get_Sel_ptr(sel);
	}
	*pEnt = get_Sel_entity(sel);
	return ptr;
}  /* find_base_adr */

/**
 * Check if a given Const node is greater or equal a given size.
 *
 * @return no_alias if the Const is greater, may_alias else
 */
static ir_alias_relation check_const(ir_node *cns, int size) {
	tarval *tv = get_Const_tarval(cns);
	tarval *tv_size;

	if (size == 0)
		return tarval_is_null(tv) ? may_alias : no_alias;
	tv_size = new_tarval_from_long(size, get_tarval_mode(tv));
	return tarval_cmp(tv_size, tv) & (pn_Cmp_Eq|pn_Cmp_Lt) ? no_alias : may_alias;
}  /* check_const */

/**
 * Treat idx1 and idx2 as integer indexes and check if they differ always more than size.
 *
 * @return sure_alias iff idx1 == idx2
 *         no_alias iff they ALWAYS differ more than size
 *         may_alias else
 */
static ir_alias_relation different_index(ir_node *idx1, ir_node *idx2, int size) {
	if (idx1 == idx2)
		return sure_alias;
	if (is_Const(idx1) && is_Const(idx2)) {
		/* both are const, we can compare them */
		tarval *tv1 = get_Const_tarval(idx1);
		tarval *tv2 = get_Const_tarval(idx2);
		tarval *tv, *tv_size;
		ir_mode *m1, *m2;

		if (size == 0)
			return tv1 == tv2 ? sure_alias : no_alias;

		/* arg, modes may be different */
		m1 = get_tarval_mode(tv1);
		m2 = get_tarval_mode(tv2);
		if (m1 != m2) {
			int size = get_mode_size_bits(m1) - get_mode_size_bits(m2);

			if (size < 0) {
				/* m1 is a small mode, cast up */
				m1 = mode_is_signed(m1) ? find_signed_mode(m2) : find_unsigned_mode(m2);
				if (m1 == NULL) {
					/* should NOT happen, but if it does we give up here */
					return may_alias;
				}
				tv1 = tarval_convert_to(tv1, m1);
			} else if (size > 0) {
				/* m2 is a small mode, cast up */
				m2 = mode_is_signed(m2) ? find_signed_mode(m1) : find_unsigned_mode(m1);
				if (m2 == NULL) {
					/* should NOT happen, but if it does we give up here */
					return may_alias;
				}
				tv2 = tarval_convert_to(tv2, m2);
			}
			/* here the size should be identical, check for signed */
			if (get_mode_sign(m1) != get_mode_sign(m2)) {
				/* find the signed */
				if (mode_is_signed(m2)) {
					tarval *t = tv1;
					ir_mode *tm = m1;
					tv1 = tv2; m1 = m2;
					tv2 = t;   m2 = tm;
				}

				/* m1 is now the signed one */
				if (tarval_cmp(tv1, get_tarval_null(m1)) & (pn_Cmp_Eq|pn_Cmp_Gt)) {
					/* tv1 is signed, but >= 0, simply cast into unsigned */
					tv1 = tarval_convert_to(tv1, m2);
				} else {
					tv_size = new_tarval_from_long(size, m2);

					if (tarval_cmp(tv2, tv_size) & (pn_Cmp_Eq|pn_Cmp_Gt)) {
						/* tv1 is negative and tv2 >= tv_size, so the difference is bigger than size */
						return no_alias;
					}
					/* tv_size > tv2, so we can subtract without overflow */
					tv2 = tarval_sub(tv_size, tv2);

					/* tv1 is < 0, so we can negate it */
					tv1 = tarval_neg(tv1);

					/* cast it into unsigned. for two-complement it does the right thing for MIN_INT */
					tv1 = tarval_convert_to(tv1, m2);

					/* now we can compare without overflow */
					return tarval_cmp(tv1, tv2) & (pn_Cmp_Eq|pn_Cmp_Gt) ? no_alias : may_alias;
				}
			}
		}
		if (tarval_cmp(tv1, tv2) == pn_Cmp_Gt) {
			tarval *t = tv1;
			tv1 = tv2;
			tv2 = t;
		}
		/* tv1 is now the "smaller" one */
		tv      = tarval_sub(tv2, tv1);
		tv_size = new_tarval_from_long(size, get_tarval_mode(tv));
		return tarval_cmp(tv_size, tv) & (pn_Cmp_Eq|pn_Cmp_Lt) ? no_alias : may_alias;
	}

	/* Note: we rely here on the fact that normalization puts constants on the RIGHT side */
	if (is_Add(idx1)) {
		ir_node *l1 = get_Add_left(idx1);
		ir_node *r1 = get_Add_right(idx1);

		if (l1 == idx2) {
			/* x + c == y */
			if (is_Const(r1))
				return check_const(r1, size);
		}
		if (is_Add(idx2)) {
			/* both are Adds, check if they are of x + a == x + b kind */
			ir_node *l2 = get_Add_left(idx2);
			ir_node *r2 = get_Add_right(idx2);

			if (l1 == l2)
				return different_index(r1, r2, size);
			else if (l1 == r2)
				return different_index(r1, l2, size);
			else if (r1 == r2)
				return different_index(l1, l2, size);
			else if (r1 == l2)
				return different_index(l1, r2, size);
		}
	}
	if (is_Add(idx2)) {
		ir_node *l2 = get_Add_left(idx2);
		ir_node *r2 = get_Add_right(idx2);

		if (l2 == idx1) {
			/* x + c == y */
			if (is_Const(r2))
				return check_const(r2, size);
		}
	}

	if (is_Sub(idx1)) {
		ir_node *l1 = get_Sub_left(idx1);
		ir_node *r1 = get_Sub_right(idx1);

		if (l1 == idx2) {
			/* x - c == y */
			if (is_Const(r1))
				return check_const(r1, size);
		}

		if (is_Sub(idx2)) {
			/* both are Subs, check if they are of x - a == x - b kind */
			ir_node *l2 = get_Sub_left(idx2);

			if (l1 == l2) {
				ir_node *r2 = get_Sub_right(idx2);
				return different_index(r1, r2, size);
			}
		}
	}
	if (is_Sub(idx2)) {
		ir_node *l2 = get_Sub_left(idx2);
		ir_node *r2 = get_Sub_right(idx2);

		if (l2 == idx1) {
			/* x - c == y */
			if (is_Const(r2))
				return check_const(r2, size);
		}

	}
	return may_alias;
}  /* different_index */

/**
 * Two Sel addresses have the same base address, check if there offsets are
 * different.
 *
 * @param adr1  The first address.
 * @param adr2  The second address.
 */
static ir_alias_relation different_sel_offsets(ir_node *sel1, ir_node *sel2) {
	/* seems to be broken */
	(void) sel1;
	(void) sel2;
#if 0
	ir_entity *ent1 = get_Sel_entity(sel1);
	ir_entity *ent2 = get_Sel_entity(sel2);
	int i, check_arr = 0;

	if (ent1 == ent2)
		check_arr = 1;
	else {
		ir_type *tp1 = get_entity_type(ent1);
		ir_type *tp2 = get_entity_type(ent2);

		if (tp1 == tp2)
			check_arr = 1;
		else if (get_type_state(tp1) == layout_fixed && get_type_state(tp2) == layout_fixed &&
		         get_type_size_bits(tp1) == get_type_size_bits(tp2))
			check_arr = 1;
	}
	if (check_arr) {
		/* we select an entity of same size, check for indexes */
		int n = get_Sel_n_indexs(sel1);
		int have_no = 0;

		if (n > 0 && n == get_Sel_n_indexs(sel2)) {
			/* same non-zero number of indexes, an array access, check */
			for (i = 0; i < n; ++i) {
				ir_node *idx1 = get_Sel_index(sel1, i);
				ir_node *idx2 = get_Sel_index(sel2, i);
				ir_alias_relation res = different_index(idx1, idx2, 0); /* we can safely IGNORE the size here if it's at least >0 */

				if (res == may_alias)
					return may_alias;
				else if (res == no_alias)
					have_no = 1;
			}
			/* if we have at least one no_alias, there is no alias relation, else we have sure */
			return have_no > 0 ? no_alias : sure_alias;
		}
	}
#endif
	return may_alias;
}  /* different_sel_offsets */

/**
 * Determine the alias relation by checking if adr1 and adr2 are pointer
 * to different type.
 *
 * @param adr1    The first address.
 * @param adr2    The second address.
 */
static ir_alias_relation different_types(ir_node *adr1, ir_node *adr2)
{
	ir_entity *ent1 = NULL, *ent2 = NULL;

	if (is_SymConst_addr_ent(adr1))
		ent1 = get_SymConst_entity(adr1);
	else if (is_Sel(adr1))
		ent1 = get_Sel_entity(adr1);

	if (is_SymConst_addr_ent(adr2))
		ent2 = get_SymConst_entity(adr2);
	else if (is_Sel(adr2))
		ent2 = get_Sel_entity(adr2);

	if (ent1 != NULL && ent2 != NULL) {
		ir_type *tp1 = get_entity_type(ent1);
		ir_type *tp2 = get_entity_type(ent2);

		if (tp1 != tp2) {
			if (is_Pointer_type(tp1) && is_Pointer_type(tp2)) {
				/* do deref until no pointer types are found */
				do {
					tp1 = get_pointer_points_to_type(tp1);
					tp2 = get_pointer_points_to_type(tp2);
				} while (is_Pointer_type(tp1) && is_Pointer_type(tp2));
			}

			if (get_type_tpop(tp1) != get_type_tpop(tp2)) {
				/* different type structure */
				return no_alias;
			}
			if (is_Class_type(tp1)) {
				/* check class hierarchy */
				if (! is_SubClass_of(tp1, tp2) &&
					! is_SubClass_of(tp2, tp1))
					return no_alias;
			} else {
				/* different types */
				return no_alias;
			}
		}
	}
	return may_alias;
}  /* different_types */

/**
 * Returns non-zero if a node is a routine parameter.
 *
 * @param node  the Proj node to test
 */
static int is_arg_Proj(ir_node *node) {
	if (! is_Proj(node))
		return 0;
	node = get_Proj_pred(node);
	if (! is_Proj(node))
		return 0;
	return pn_Start_T_args == get_Proj_proj(node) && is_Start(get_Proj_pred(node));
}  /* is_arg_Proj */

/**
 * Returns non-zero if a node is a result on a malloc-like routine.
 *
 * @param node  the Proj node to test
 */
static int is_malloc_Result(ir_node *node) {
	node = get_Proj_pred(node);
	if (! is_Proj(node))
		return 0;
	node = get_Proj_pred(node);
	if (! is_Call(node))
		return 0;
	node = get_Call_ptr(node);
	if (is_SymConst_addr_ent(node)) {
		ir_entity *ent = get_SymConst_entity(node);

		if (get_entity_additional_properties(ent) & mtp_property_malloc)
			return 1;
		return 0;
	}
	return 0;
}  /* is_malloc_Result */

/**
 * Returns true if an address represents a global variable.
 *
 * @param irn  the node representing the address
 */
static INLINE int is_global_var(ir_node *irn) {
	return is_SymConst_addr_ent(irn);
}  /* is_global_var */

/**
 * classify storage locations.
 * Except STORAGE_CLASS_POINTER they are all disjoint.
 * STORAGE_CLASS_POINTER potentially aliases all classes which don't have a
 * NOTTAKEN modifier.
 */
typedef enum {
	STORAGE_CLASS_POINTER           = 0x0000,
	STORAGE_CLASS_GLOBALVAR         = 0x0001,
	STORAGE_CLASS_LOCALVAR          = 0x0002,
	STORAGE_CLASS_ARGUMENT          = 0x0003,
	STORAGE_CLASS_TLS               = 0x0004,
	STORAGE_CLASS_MALLOCED          = 0x0005,

	STORAGE_CLASS_MODIFIER_NOTTAKEN = 0x1000,
} storage_class_class_t;

static storage_class_class_t classify_pointer(ir_graph *irg, ir_node *irn)
{
	if(is_SymConst_addr_ent(irn)) {
		ir_entity *entity = get_SymConst_entity(irn);
		storage_class_class_t res = STORAGE_CLASS_GLOBALVAR;
		if (get_entity_address_taken(entity) == ir_address_not_taken) {
			res |= STORAGE_CLASS_MODIFIER_NOTTAKEN;
		}
		return res;
	} else if(irn == get_irg_frame(irg)) {
		/* TODO: we already skipped sels so we can't determine address_taken */
		return STORAGE_CLASS_LOCALVAR;
	} else if(is_arg_Proj(irn)) {
		return STORAGE_CLASS_ARGUMENT;
	} else if(irn == get_irg_tls(irg)) {
		/* TODO: we already skipped sels so we can't determine address_taken */
		return STORAGE_CLASS_TLS;
	} else if (is_Proj(irn) && is_malloc_Result(irn)) {
		return STORAGE_CLASS_MALLOCED;
	}

	return STORAGE_CLASS_POINTER;
}

/**
 * Determine the alias relation between two addresses.
 */
static ir_alias_relation _get_alias_relation(
	ir_graph *irg,
	ir_node *adr1, ir_mode *mode1,
	ir_node *adr2, ir_mode *mode2)
{
	ir_entity             *ent1, *ent2;
	unsigned               options;
	long                   offset1 = 0;
	long                   offset2 = 0;
	ir_node               *base1;
	ir_node               *base2;
	ir_node               *orig_adr1 = adr1;
	ir_node               *orig_adr2 = adr2;
	unsigned               mode_size;
	storage_class_class_t  class1;
	storage_class_class_t  class2;

	if (! get_opt_alias_analysis())
		return may_alias;

	if (adr1 == adr2)
		return sure_alias;

	options = get_irg_memory_disambiguator_options(irg);

	/* The Armageddon switch */
	if (options & aa_opt_no_alias)
		return no_alias;

	/* do the addresses have constants offsets?
	 *  Note: nodes are normalized to have constants at right inputs,
	 *        sub X, C is normalized to add X, -C
	 * */
	while (is_Add(adr1)) {
		ir_node *add_right = get_Add_right(adr1);
		if (is_Const(add_right)) {
			tarval *tv  = get_Const_tarval(add_right);
			offset1    += get_tarval_long(tv);
			adr1        = get_Add_left(adr1);
			continue;
		}
		break;
	}
	while (is_Add(adr2)) {
		ir_node *add_right = get_Add_right(adr2);
		if (is_Const(add_right)) {
			tarval *tv  = get_Const_tarval(add_right);
			offset2    += get_tarval_long(tv);
			adr2        = get_Add_left(adr2);
			continue;
		}
		break;
	}

	mode_size = get_mode_size_bytes(mode1);
	if (get_mode_size_bytes(mode2) > mode_size) {
		mode_size = get_mode_size_bytes(mode2);
	}

	/* same base address -> compare offsets */
	if (adr1 == adr2) {
		if(labs(offset2 - offset1) >= mode_size)
			return no_alias;
		else
			return sure_alias;
	}

	/* skip sels */
	base1 = adr1;
	base2 = adr2;
	if (is_Sel(adr1)) {
		adr1 = find_base_adr(adr1, &ent1);
	}
	if (is_Sel(adr2)) {
		adr2 = find_base_adr(adr2, &ent2);
	}

	/* same base address -> compare sel entities*/
	if (adr1 == adr2 && base1 != adr1 && base2 != adr2) {
		if (ent1 != ent2)
			return no_alias;
		else
			return different_sel_offsets(base1, base2);
	}

	class1 = classify_pointer(irg, adr1);
	class2 = classify_pointer(irg, adr2);

	if (class1 == STORAGE_CLASS_POINTER) {
		if (class2 & STORAGE_CLASS_MODIFIER_NOTTAKEN) {
			return no_alias;
		} else {
			return may_alias;
		}
	} else if (class2 == STORAGE_CLASS_POINTER) {
		if (class1 & STORAGE_CLASS_MODIFIER_NOTTAKEN) {
			return no_alias;
		} else {
			return may_alias;
		}
	}

	if (class1 != class2) {
		return no_alias;
	}

	if (class1 == STORAGE_CLASS_GLOBALVAR) {
		ir_entity *entity1 = get_SymConst_entity(adr1);
		ir_entity *entity2 = get_SymConst_entity(adr2);
		if(entity1 != entity2)
			return no_alias;

		/* for some reason CSE didn't work for the 2 symconsts... */
		return may_alias;
	}

   	/* Type based alias analysis */
	if (options & aa_opt_type_based) {
		ir_alias_relation rel;

		if (options & aa_opt_byte_type_may_alias) {
			if (get_mode_size_bits(mode1) == 8 || get_mode_size_bits(mode2) == 8) {
				/* One of the modes address a byte. Assume a may_alias and leave
				   the type based check. */
				goto leave_type_based_alias;
			}
		}
		/* cheap check: If the mode sizes did not match, the types MUST be different */
		if (get_mode_size_bits(mode1) != get_mode_size_bits(mode2))
			return no_alias;

		/* cheap test: if only one is a reference mode, no alias */
		if (mode_is_reference(mode1) != mode_is_reference(mode2))
			return no_alias;

		/* try rule R5 */
		rel = different_types(adr1, adr2);
		if (rel != may_alias)
			return rel;
leave_type_based_alias:;
	}

	/* do we have a language specific memory disambiguator? */
	if (language_disambuigator) {
		ir_alias_relation rel = (*language_disambuigator)(irg, orig_adr1, mode1, orig_adr2, mode2);
		if (rel != may_alias)
			return rel;
	}

	/* access points-to information here */
	return may_alias;
}  /* _get_alias_relation */

/*
 * Determine the alias relation between two addresses.
 */
ir_alias_relation get_alias_relation(
	ir_graph *irg,
	ir_node *adr1, ir_mode *mode1,
	ir_node *adr2, ir_mode *mode2)
{
	ir_alias_relation rel = _get_alias_relation(irg, adr1, mode1, adr2, mode2);
	DB((dbg, LEVEL_1, "alias(%+F, %+F) = %s\n", adr1, adr2, get_ir_alias_relation_name(rel)));
	return rel;
}  /* get_alias_relation */

/* Set a source language specific memory disambiguator function. */
void set_language_memory_disambiguator(DISAMBIGUATOR_FUNC func) {
	language_disambuigator = func;
}  /* set_language_memory_disambiguator */

/** The result cache for the memory disambiguator. */
static set *result_cache = NULL;

/** An entry in the relation cache. */
typedef struct mem_disambig_entry {
	ir_node	          *adr1;    /**< The first address. */
	ir_node	          *adr2;    /**< The second address. */
	ir_alias_relation result;   /**< The alias relation result. */
} mem_disambig_entry;

#define HASH_ENTRY(adr1, adr2)	(HASH_PTR(adr1) ^ HASH_PTR(adr2))

/**
 * Compare two relation cache entries.
 */
static int cmp_mem_disambig_entry(const void *elt, const void *key, size_t size) {
	const mem_disambig_entry *p1 = elt;
	const mem_disambig_entry *p2 = key;
	(void) size;

	return p1->adr1 == p2->adr1 && p1->adr2 == p2->adr2;
}  /* cmp_mem_disambig_entry */

/**
 * Initialize the relation cache.
 */
void mem_disambig_init(void) {
	result_cache = new_set(cmp_mem_disambig_entry, 8);
}  /* mem_disambig_init */

/*
 * Determine the alias relation between two addresses.
 */
ir_alias_relation get_alias_relation_ex(
	ir_graph *irg,
	ir_node *adr1, ir_mode *mode1,
	ir_node *adr2, ir_mode *mode2)
{
	mem_disambig_entry key, *entry;

	ir_fprintf(stderr, "%+F <-> %+F\n", adr1, adr2);

	if (! get_opt_alias_analysis())
		return may_alias;

	if (get_irn_opcode(adr1) > get_irn_opcode(adr2)) {
		ir_node *t = adr1;
		adr1 = adr2;
		adr2 = t;
	}

	key.adr1 = adr1;
	key.adr2 = adr2;
	entry = set_find(result_cache, &key, sizeof(key), HASH_ENTRY(adr1, adr2));
	if (entry)
		return entry->result;

	key.result = get_alias_relation(irg, adr1, mode1, adr2, mode2);

	set_insert(result_cache, &key, sizeof(key), HASH_ENTRY(adr1, adr2));
	return key.result;
}  /* get_alias_relation_ex */

/* Free the relation cache. */
void mem_disambig_term(void) {
	if (result_cache) {
		del_set(result_cache);
		result_cache = NULL;
	}
}  /* mem_disambig_term */

/**
 * Check the mode of a Load/Store with the mode of the entity
 * that is accessed.
 * If the mode of the entity and the Load/Store mode do not match, we
 * have the bad reinterpret case:
 *
 * int i;
 * char b = *(char *)&i;
 *
 * We do NOT count this as one value and return address_taken
 * in that case.
 * However, we support an often used case. If the mode is two-complement
 * we allow casts between signed/unsigned.
 *
 * @param mode     the mode of the Load/Store
 * @param ent_mode the mode of the accessed entity
 *
 * @return non-zero if the Load/Store is a hidden cast, zero else
 */
static int is_hidden_cast(ir_mode *mode, ir_mode *ent_mode) {
	if (ent_mode != mode) {
		if (ent_mode == NULL ||
			get_mode_size_bits(ent_mode) != get_mode_size_bits(mode) ||
			get_mode_sort(ent_mode) != get_mode_sort(mode) ||
			get_mode_arithmetic(ent_mode) != irma_twos_complement ||
			get_mode_arithmetic(mode) != irma_twos_complement)
			return 1;
	}
	return 0;
}  /* is_hidden_cast */

/**
 * Determine the address_taken state of a node (or it's successor Sels).
 *
 * @param irn  the node
 */
static ir_address_taken_state find_address_taken_state(ir_node *irn) {
	int     i, j;
	ir_mode *emode, *mode;
	ir_node *value;
	ir_entity *ent;

	for (i = get_irn_n_outs(irn) - 1; i >= 0; --i) {
		ir_node *succ = get_irn_out(irn, i);

		switch (get_irn_opcode(succ)) {
		case iro_Load:
			/* check if this load is not a hidden conversion */
			mode = get_Load_mode(succ);
			ent = is_SymConst(irn) ? get_SymConst_entity(irn) : get_Sel_entity(irn);
			emode = get_type_mode(get_entity_type(ent));
			if (is_hidden_cast(mode, emode))
				return ir_address_taken;
			break;

		case iro_Store:
			/* check that the node is not the Store's value */
			value = get_Store_value(succ);
			if (value == irn)
				return ir_address_taken;
			/* check if this Store is not a hidden conversion */
			mode = get_irn_mode(value);
			ent = is_SymConst(irn) ? get_SymConst_entity(irn) : get_Sel_entity(irn);
			emode = get_type_mode(get_entity_type(ent));
			if (is_hidden_cast(mode, emode))
				return ir_address_taken;
			break;

		case iro_Sel: {
			/* Check the successor of irn. */
			ir_address_taken_state res = find_address_taken_state(succ);
			if (res != ir_address_not_taken)
				return res;
			break;
		}

		case iro_Call:
			/* Only the call address is not an address taker but
			   this is an uninteresting case, so we ignore it here. */
			for (j = get_Call_n_params(succ) - 1; j >= 0; --j) {
				ir_node *param = get_Call_param(succ, j);
				if (param == irn)
					return ir_address_taken;
			}
			break;

		default:
			/* another op, the address may be taken */
			return ir_address_taken_unknown;
		}
	}
	/* All successors finished, the address is not taken. */
	return ir_address_not_taken;
}  /* find_address_taken_state */

/**
 * Update the "address taken" flag of all frame entities.
 */
static void analyse_irg_address_taken(ir_graph *irg) {
	ir_type *ft = get_irg_frame_type(irg);
	ir_node *irg_frame;
	int i;

	/* set initial state to not_taken, as this is the "smallest" state */
	for (i = get_class_n_members(ft) - 1; i >= 0; --i) {
		ir_entity *ent = get_class_member(ft, i);

		set_entity_address_taken(ent, ir_address_not_taken);
	}

	assure_irg_outs(irg);

	irg_frame = get_irg_frame(irg);

	for (i = get_irn_n_outs(irg_frame) - 1; i >= 0; --i) {
		ir_node *succ = get_irn_out(irg_frame, i);
		ir_address_taken_state state;

	    if (is_Sel(succ)) {
			ir_entity *ent = get_Sel_entity(succ);

			if (get_entity_address_taken(ent) == ir_address_taken)
				continue;

			state = find_address_taken_state(succ);
			if (state > get_entity_address_taken(ent))
				set_entity_address_taken(ent, state);
		}
	}
	/* now computed */
	irg->adr_taken_state = ir_address_taken_computed;
}  /* analyse_address_taken */

/* Returns the current address taken state of the graph. */
ir_address_taken_computed_state get_irg_address_taken_state(const ir_graph *irg) {
	return irg->adr_taken_state;
}  /* get_irg_address_taken_state */

/* Sets the current address taken state of the graph. */
void set_irg_address_taken_state(ir_graph *irg, ir_address_taken_computed_state state) {
	irg->adr_taken_state = state;
}  /* set_irg_address_taken_state */

/* Assure that the address taken flag is computed for the given graph. */
void assure_irg_address_taken_computed(ir_graph *irg) {
	if (irg->adr_taken_state == ir_address_taken_not_computed)
		analyse_irg_address_taken(irg);
}  /* assure_irg_address_taken_computed */


/**
 * Initialize the address_taken flag for a global type like type.
 */
static void init_taken_flag(ir_type * tp) {
	int i;

	/* All external visible entities are at least
	   ir_address_taken_unknown. This is very conservative. */
	for (i = get_compound_n_members(tp) - 1; i >= 0; --i) {
		ir_entity *ent = get_compound_member(tp, i);
		ir_address_taken_state state;

		state = get_entity_visibility(ent) == visibility_external_visible ?
				ir_address_taken_unknown : ir_address_not_taken ;
		set_entity_address_taken(ent, state);
	}
}  /* init_taken_flag */

static void check_initializer_nodes(ir_initializer_t *initializer)
{
	switch(initializer->kind) {
	case IR_INITIALIZER_CONST: {
		ir_node *n = initializer->consti.value;

		/* let's check if it's an address */
		if (is_SymConst_addr_ent(n)) {
			ir_entity *ent = get_SymConst_entity(n);
			set_entity_address_taken(ent, ir_address_taken);
		}
		return;
	}
	case IR_INITIALIZER_TARVAL:
	case IR_INITIALIZER_NULL:
		return;
	case IR_INITIALIZER_COMPOUND: {
		size_t i;

		for(i = 0; i < initializer->compound.n_initializers; ++i) {
			ir_initializer_t *sub_initializer
				= initializer->compound.initializers[i];
			check_initializer_nodes(sub_initializer);
		}
		return;
	}
	}
	panic("invalid initialzier found");
}

/**
 * Mark all entities used in the initializer for the given entity as address taken
 */
static void check_initializer(ir_entity *ent) {
	ir_node *n;
	int i;

	/* do not check uninitialized values */
	if (get_entity_variability(ent) == variability_uninitialized)
		return;

	/* Beware: Methods initialized with "themself". This does not count as a taken
	   address. */
	if (is_Method_type(get_entity_type(ent)))
		return;

	if (ent->has_initializer) {
		check_initializer_nodes(ent->attr.initializer);
	} else if (is_atomic_entity(ent)) {
		/* let's check if it's an address */
		n = get_atomic_ent_value(ent);
		if (is_SymConst_addr_ent(n)) {
			ir_entity *ent = get_SymConst_entity(n);
			set_entity_address_taken(ent, ir_address_taken);
		}
	} else {
		for (i = get_compound_ent_n_values(ent) - 1; i >= 0; --i) {
			n = get_compound_ent_value(ent, i);

			/* let's check if it's an address */
			if (is_SymConst_addr_ent(n)) {
				ir_entity *ent = get_SymConst_entity(n);
				set_entity_address_taken(ent, ir_address_taken);
			}
		}
	}
}  /* check_initializer */


/**
 * Mark all entities used in initializers as address taken
 */
static void check_initializers(ir_type *tp) {
	int i;

	for (i = get_compound_n_members(tp) - 1; i >= 0; --i) {
		ir_entity *ent = get_compound_member(tp, i);

		check_initializer(ent);
	}
}  /* check_initializers */

#ifdef DEBUG_libfirm
/**
 * Print the address taken state of all entities of a given type for debugging.
 */
static void print_address_taken_state(ir_type *tp) {
	int i;
	for (i = get_compound_n_members(tp) - 1; i >= 0; --i) {
		ir_entity *ent = get_compound_member(tp, i);
		ir_address_taken_state state = get_entity_address_taken(ent);

		if (state != ir_address_not_taken) {
			assert(ir_address_not_taken <= (int) state && state <= ir_address_taken);
			ir_printf("%+F: %s\n", ent, get_address_taken_state_name(state));
		}
	}
}  /* print_address_taken_state */
#endif /* DEBUG_libfirm */

/**
 * Post-walker: check for global entity address
 */
static void check_global_address(ir_node *irn, void *env) {
	ir_node *tls = env;
	ir_entity *ent;
	ir_address_taken_state state;

	if (is_SymConst_addr_ent(irn)) {
		/* A global. */
		ent = get_SymConst_entity(irn);
	} else if (is_Sel(irn) && get_Sel_ptr(irn) == tls) {
		/* A TLS variable. */
		ent = get_Sel_entity(irn);
	} else
		return;

	if (get_entity_address_taken(ent) >= ir_address_taken) {
		/* Already at the maximum. */
		return;
	}
	state = find_address_taken_state(irn);
	if (state > get_entity_address_taken(ent))
		set_entity_address_taken(ent, state);
}  /* check_global_address */

/**
 * Update the "address taken" flag of all global entities.
 */
static void analyse_irp_globals_address_taken(void) {
	int i;

	FIRM_DBG_REGISTER(dbg, "firm.ana.irmemory");

	init_taken_flag(get_glob_type());
	init_taken_flag(get_tls_type());

	check_initializers(get_glob_type());
	check_initializers(get_tls_type());

	for (i = get_irp_n_irgs() - 1; i >= 0; --i) {
		ir_graph *irg = get_irp_irg(i);

		assure_irg_outs(irg);
		irg_walk_graph(irg, NULL, check_global_address, get_irg_tls(irg));
	}

#ifdef DEBUG_libfirm
	if (firm_dbg_get_mask(dbg) & LEVEL_1) {
		print_address_taken_state(get_glob_type());
		print_address_taken_state(get_tls_type());
	}
#endif /* DEBUG_libfirm */

	/* now computed */
	irp->globals_adr_taken_state = ir_address_taken_computed;
}  /* analyse_irp_globals_address_taken */

/* Returns the current address taken state of the globals. */
ir_address_taken_computed_state get_irp_globals_address_taken_state(void) {
	return irp->globals_adr_taken_state;
}  /* get_irp_globals_address_taken_state */

/* Sets the current address taken state of the graph. */
void set_irp_globals_address_taken_state(ir_address_taken_computed_state state) {
	irp->globals_adr_taken_state = state;
}  /* set_irg_address_taken_state */

/* Assure that the address taken flag is computed for the globals. */
void assure_irp_globals_address_taken_computed(void) {
	if (irp->globals_adr_taken_state == ir_address_taken_not_computed)
		analyse_irp_globals_address_taken();
}  /* assure_irp_globals_address_taken_computed */


#include <adt/pmap.h>
#include "typerep.h"

DEBUG_ONLY(static firm_dbg_module_t *dbgcall = NULL;)

/** Maps method types to cloned method types. */
static pmap *mtp_map;

/**
 * Clone a method type if not already cloned.
 */
static ir_type *clone_type_and_cache(ir_type *tp) {
	static ident *prefix = NULL;
	ir_type *res;
	pmap_entry *e = pmap_find(mtp_map, tp);

	if (e)
		return e->value;

	if (prefix == NULL)
		prefix = new_id_from_chars("C", 1);

	res = clone_type_method(tp, prefix);
	pmap_insert(mtp_map, tp, res);
	DB((dbgcall, LEVEL_2, "cloned type %+F into %+F\n", tp, res));

	return res;
}  /* clone_type_and_cache */

/**
 * Copy the calling conventions from the entities to the call type.
 */
static void update_calls_to_private(ir_node *call, void *env) {
	(void) env;
	if (is_Call(call)) {
		ir_node *ptr = get_Call_ptr(call);

		if (is_SymConst(ptr)) {
			ir_entity *ent = get_SymConst_entity(ptr);
			ir_type *ctp = get_Call_type(call);

			if (get_entity_additional_properties(ent) & mtp_property_private) {
				if ((get_method_additional_properties(ctp) & mtp_property_private) == 0) {
					ctp = clone_type_and_cache(ctp);
					set_method_additional_property(ctp, mtp_property_private);
					set_Call_type(call, ctp);
					DB((dbgcall, LEVEL_1, "changed call to private method %+F\n", ent));
				}
			}
		}
	}
}  /* update_calls_to_private */

/* Mark all private methods, i.e. those of which all call sites are known. */
void mark_private_methods(void) {
	int i;
	int changed = 0;

	FIRM_DBG_REGISTER(dbgcall, "firm.opt.cc");

	assure_irp_globals_address_taken_computed();

	mtp_map = pmap_create();

	/* first step: change the calling conventions of the local non-escaped entities */
	for (i = get_irp_n_irgs() - 1; i >= 0; --i) {
		ir_graph               *irg = get_irp_irg(i);
		ir_entity              *ent = get_irg_entity(irg);
		ir_address_taken_state state = get_entity_address_taken(ent);

		if (get_entity_visibility(ent) == visibility_local &&
		    state == ir_address_not_taken) {
			ir_type *mtp = get_entity_type(ent);

			set_entity_additional_property(ent, mtp_property_private);
			DB((dbgcall, LEVEL_1, "found private method %+F\n", ent));
			if ((get_method_additional_properties(mtp) & mtp_property_private) == 0) {
				/* need a new type */
				mtp = clone_type_and_cache(mtp);
				set_entity_type(ent, mtp);
				set_method_additional_property(mtp, mtp_property_private);
				changed = 1;
			}
		}
	}

	if (changed)
		all_irg_walk(NULL, update_calls_to_private, NULL);

	pmap_destroy(mtp_map);
}  /* mark_private_methods */
