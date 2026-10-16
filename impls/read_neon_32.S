// aarch64 NEON ldp 32 bytes per loop

	.arch armv8-a
	.text

	.globl	memcpy
	.type	memcpy, %function
	.p2align	4
memcpy:
	sub	x1, x1, 32
loop:
	ldp	q0, q1, [x1, 32]!
	subs	x2, x2, 32
	b.hi	loop

	.size	memcpy, .-memcpy
