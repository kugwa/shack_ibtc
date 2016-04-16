/*
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 */

#include <stdio.h>
#include <stdlib.h>
#include "exec-all.h"
#include "tcg-op.h"
#include "helper.h"
#define GEN_HELPER 1
#include "helper.h"
#include "optimization.h"

extern uint8_t *optimization_ret_addr;

/*
 * Shadow Stack
 */
// list_t *shadow_hash_list;

static int hash(target_ulong guest_eip)
{
    return guest_eip % HASH_TABLE_SIZE;
}

static void hash_init(CPUState *env)
{
    shadow_pair **list = malloc(HASH_TABLE_SIZE * sizeof(shadow_pair*));
    if (list == NULL) {
        fprintf(stderr, "hash_init() GG\n");
        exit(1);
    }

    int i;
    for (i = 0; i < HASH_TABLE_SIZE; i++) list[i] = NULL;
    env->shadow_hash_list = list;
}

static shadow_pair *hash_insert(CPUState *env, target_ulong guest_eip, uint8_t *shadow_slot)
{
    shadow_pair *entry = malloc(sizeof(shadow_pair));
    if (entry == NULL) {
        fprintf(stderr, "hash_insert() GG\n");
        exit(1);
    }
    entry->guest_eip = guest_eip;
    entry->shadow_slot = shadow_slot;

    int key = hash(guest_eip);
    entry->l.next = (list_t*)(((shadow_pair**)env->shadow_hash_list)[key]);
    ((shadow_pair**)env->shadow_hash_list)[key] = entry;
    return entry;
}

static shadow_pair *hash_retrieve(CPUState *env, target_ulong guest_eip)
{
    int key = hash(guest_eip);
    shadow_pair *entry;
    for (entry = ((shadow_pair**)env->shadow_hash_list)[key]; entry != NULL; entry = (shadow_pair*)(entry->l.next)) {
        if (entry->guest_eip == guest_eip) return entry;
    }
    return NULL;
}

static inline void shack_init(CPUState *env)
{
    hash_init(env);
    env->shack = malloc(SHACK_SIZE * sizeof(shadow_pair*));
    if (env->shack == NULL) {
        fprintf(stderr, "shack_init() GG\n");
        exit(1);
    }
    env->shack_top = env->shack;
}

/*
 * shack_set_shadow()
 *  Insert a guest eip to host eip pair if it is not yet created.
 */
void shack_set_shadow(CPUState *env, target_ulong guest_eip, unsigned long *host_eip)
{
    shadow_pair *entry = hash_retrieve(env, guest_eip);
    if (entry == NULL) hash_insert(env, guest_eip, (uint8_t*)host_eip);
    else entry->shadow_slot = (uint8_t*)host_eip;
}

/*
 * helper_shack_flush()
 *  Reset shadow stack.
 */
void helper_shack_flush(CPUState *env)
{
}

/*
 * push_shack()
 *  Push next guest eip into shadow stack.
 */
void push_shack(CPUState *env, TCGv_ptr cpu_env, target_ulong next_eip)
{
    shadow_pair *entry = hash_retrieve(env, next_eip);
    if (entry == NULL) entry = hash_insert(env, next_eip, (uint8_t*)NULL);

    TCGv_ptr shack_top = tcg_temp_new_ptr();
    tcg_gen_ld_ptr(shack_top, cpu_env, offsetof(CPUState, shack_top));
    tcg_gen_st_tl(tcg_const_tl((target_ulong)entry), shack_top, 0);
    tcg_gen_addi_ptr(shack_top, shack_top, sizeof(shadow_pair*));
    tcg_gen_st_ptr(shack_top, cpu_env, offsetof(CPUState, shack_top));
    tcg_temp_free_ptr(shack_top);
}

/*
 * pop_shack()
 *  Pop next host eip from shadow stack.
 */
void pop_shack(TCGv_ptr cpu_env, TCGv next_eip)
{
    TCGv_ptr temp_shack_top                 = tcg_temp_local_new_ptr();
    TCGv_ptr temp_shack                     = tcg_temp_new_ptr();
    TCGv_ptr temp_pair                      = tcg_temp_local_new_ptr();
    TCGv_ptr temp_pair_arrow_guest_eip      = tcg_temp_new_ptr();
    TCGv_ptr temp_pair_arrow_shadow_slot    = tcg_temp_local_new_ptr();
    int end = gen_new_label();

    // if (shack_top == shack) goto end;
    tcg_gen_ld_ptr(temp_shack_top, cpu_env, offsetof(CPUState, shack_top));
    tcg_gen_ld_ptr(temp_shack, cpu_env, offsetof(CPUState, shack));
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_shack_top, temp_shack, end);
    
    // if ((*shack_top)->guest_eip != next_eip) goto end;
    tcg_gen_addi_ptr(temp_shack_top, temp_shack_top, -sizeof(shadow_pair*));
    tcg_gen_st_ptr(temp_shack_top, cpu_env, offsetof(CPUState, shack_top));
    tcg_gen_ld_ptr(temp_pair, temp_shack_top, 0);
    tcg_gen_ld_ptr(temp_pair_arrow_guest_eip, temp_pair, offsetof(shadow_pair, guest_eip));
    tcg_gen_brcond_tl(TCG_COND_NE, temp_pair_arrow_guest_eip, next_eip, end);

    // if ((*shack_top)->shadow_slot == NULL) goto end;
    tcg_gen_ld_ptr(temp_pair_arrow_shadow_slot, temp_pair, offsetof(shadow_pair, shadow_slot));
    tcg_gen_brcondi_tl(TCG_COND_EQ, temp_pair_arrow_shadow_slot, (int32_t)NULL, end);

    // goto shadow_slot;
    *gen_opc_ptr++ = INDEX_op_jmp;
    *gen_opparam_ptr++ = temp_pair_arrow_shadow_slot;

    gen_set_label(end);
    tcg_temp_free_ptr(temp_shack_top);
    tcg_temp_free_ptr(temp_shack);
    tcg_temp_free_ptr(temp_pair);
    tcg_temp_free_ptr(temp_pair_arrow_guest_eip);
    tcg_temp_free_ptr(temp_pair_arrow_shadow_slot);
}

/*
 * Indirect Branch Target Cache
 */
__thread int update_ibtc;
static ibtc_table* ibtc_table_v;
static target_ulong being_lookup_eip;

/*
 * helper_lookup_ibtc()
 *  Look up IBTC. Return next host eip if cache hit or
 *  back-to-dispatcher stub address if cache miss.
 */
void *helper_lookup_ibtc(target_ulong guest_eip)
{
    target_ulong hash = (guest_eip & IBTC_CACHE_MASK);
    if (ibtc_table_v->htable[hash].guest_eip == guest_eip) {
        return ibtc_table_v->htable[hash].tb->tc_ptr;
    } else {
        update_ibtc = 1;
        being_lookup_eip = guest_eip;
        return optimization_ret_addr;
    }
}

/*
 * update_ibtc_entry()
 *  Populate eip and tb pair in IBTC entry.
 */
void update_ibtc_entry(TranslationBlock *tb)
{
    target_ulong hash = (being_lookup_eip & IBTC_CACHE_MASK);
    ibtc_table_v->htable[hash].guest_eip = being_lookup_eip;
    ibtc_table_v->htable[hash].tb = tb;
    update_ibtc = 0;
}

/*
 * ibtc_init()
 *  Create and initialize indirect branch target cache.
 */
static inline void ibtc_init(CPUState *env)
{
    ibtc_table_v = (ibtc_table*)malloc(sizeof(ibtc_table));
    memset(ibtc_table_v, 0, sizeof(ibtc_table));
}

/*
 * init_optimizations()
 *  Initialize optimization subsystem.
 */
int init_optimizations(CPUState *env)
{
    shack_init(env);
    ibtc_init(env);

    return 0;
}

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */
