// aarch64 NEON stp 32 bytes per loop

	.arch armv8-a
	.text

	.globl	memcpy
	.type	memcpy, %function
	.p2align	4
memcpy:
	mov	x3, x0
	sub	x3, x3, 32
loop:
	stp	q0, q1, [x3, 32]!
	subs	x2, x2, 32
	b.hi	loop

	.size	memcpy, .-memcpy
