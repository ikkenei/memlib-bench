// aarch64 NEON ldp 64 bytes per loop

	.arch armv8-a
	.text

	.globl	memcpy
	.type	memcpy, %function
	.p2align	4
memcpy:
	sub	x1, x1, 32
loop:
	ldp	q0, q1, [x1, 32]
	ldp	q2, q3, [x1, 64]!
	subs	x2, x2, 64
	b.hi	loop

	.size	memcpy, .-memcpy
