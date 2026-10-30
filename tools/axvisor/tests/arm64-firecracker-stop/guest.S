/* SPDX-License-Identifier: GPL-2.0 */
/* Linux Image header plus a tiny guest; no guest kernel or disk is needed. */
.section .text, "ax"
.global _start
_start:
	b primary
	.word 0
	.quad 0x80000
	.quad image_end - _start
	.quad 0x8
	.quad 0
	.quad 0
	.quad 0
	.word 0x644d5241
	.word 0

primary:
	msr daifset, #0xf
	adr x19, counter
	mov x0, #3
	movk x0, #0x8400, lsl #16 /* PSCI CPU_ON */
	mov x1, #1
	adr x2, secondary
	mov x3, x19
	hvc #0
	cbnz x0, cpu_on_failed
wait_for_busy:
	ldr x4, [x19]
	cmp x4, #1000
	b.lo wait_for_busy
	adr x5, busy_message
	bl print_uart
	/* A private HVC causes UNKNOWN, not a PSCI system-wide stop request. */
	mov x0, #0xabcd
	movk x0, #0x8200, lsl #16
	hvc #0
	adr x5, resumed_message
	bl print_uart
unexpected_resume:
	b unexpected_resume

cpu_on_failed:
	adr x5, failed_message
	bl print_uart
	b unexpected_resume

secondary:
	msr daifset, #0xf
	mov x19, x0
busy_loop:
	ldr x1, [x19]
	add x1, x1, #1
	str x1, [x19]
	b busy_loop

print_uart:
	/* With no virtio devices, FC v1.10.1 allocates UART first at 0x40000000. */
	mov x6, #0x40000000
1:
	ldrb w7, [x5], #1
	cbz w7, 2f
	strb w7, [x6]
	b 1b
2:
	ret

busy_message: .asciz "FC_STOP_BUSY_COUNTER=PASS\n"
failed_message: .asciz "FC_STOP_GUEST_FAIL CPU_ON\n"
resumed_message: .asciz "FC_STOP_GUEST_FAIL UNKNOWN_RESUMED\n"
.balign 4096
counter: .quad 0
.balign 4096
image_end:
