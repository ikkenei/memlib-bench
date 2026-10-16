// aarch64 NEON stp 64 bytes per loop

	.arch armv8-a
	.text

	.globl	memcpy
	.type	memcpy, %function
	.p2align	4
memcpy:
	mov	x3, x0
	sub	x3, x3, 32
loop:
	stp	q0, q1, [x3, 32]
	stp	q2, q3, [x3, 64]!
	subs	x2, x2, 64
	b.hi	loop

	.size	memcpy, .-memcpy
