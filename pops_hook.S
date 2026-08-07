/*
 * Captures the caller's $s0 before any C prologue can touch it, then jumps
 * into the real hook. FUN_0000a250 inside pops_XXg.prx keeps its slot's
 * struct-base pointer in $s0 for the whole function, including across its
 * call into sceCtrlPeekBufferNegative - reading it here (rather than in the
 * C hook itself, where the compiler is free to reuse $s0 for its own locals
 * before we get a chance to look at it) is the only reliable way to recover
 * it. See main.c for how the captured value is used.
 */
#if RJM_ENABLE_POPS_2P

#ifndef RJM_POPS_GATE_SLOT2_RETURN
#define RJM_POPS_GATE_SLOT2_RETURN 4
#endif

	.set noreorder
	.set noat

	.global pops_ctrl_peek_negative_entry
	.ent    pops_ctrl_peek_negative_entry

pops_ctrl_peek_negative_entry:
	lui  $t8, %hi(g_pops_caller_s0)
	j    ctrl_peek_negative_hook
	sw   $s0, %lo(g_pops_caller_s0)($t8)

	.end pops_ctrl_peek_negative_entry

	.global pops_gate_entry
	.ent    pops_gate_entry

pops_gate_entry:
	addiu $t8, $zero, 1
	beq   $a0, $t8, 3f
	nop
	lui   $t8, %hi(g_pops_gate_slot1_hits)
	lw    $t9, %lo(g_pops_gate_slot1_hits)($t8)
	addiu $t9, $t9, 1
	sw    $t9, %lo(g_pops_gate_slot1_hits)($t8)
	lui   $t8, %hi(g_pops_gate_data_base)
	lw    $t8, %lo(g_pops_gate_data_base)($t8)
	lw    $t9, 0x20($t8)
	addiu $v0, $zero, 4
	bne   $t9, $a0, 2f
	nop
	lw    $t9, 0x14($t8)
	addiu $t9, $t9, -6
	sltiu $t8, $t9, 4
	movn  $v0, $t9, $t8
2:
	lui   $t8, %hi(g_pops_gate_result_counts)
	addiu $t8, $t8, %lo(g_pops_gate_result_counts)
	sll   $t9, $v0, 2
	addu  $t8, $t8, $t9
	lw    $t9, 0($t8)
	addiu $t9, $t9, 1
	sw    $t9, 0($t8)
	jr    $ra
	nop
3:
	lui   $t8, %hi(g_pops_gate_slot2_hits)
	lw    $t9, %lo(g_pops_gate_slot2_hits)($t8)
	addiu $t9, $t9, 1
	sw    $t9, %lo(g_pops_gate_slot2_hits)($t8)
	jr    $ra
	addiu $v0, $zero, RJM_POPS_GATE_SLOT2_RETURN

	.end pops_gate_entry

#endif
