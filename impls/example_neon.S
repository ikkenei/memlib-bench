/* Example user implementation in AArch64 assembly (NEON 16-byte loop).

   Builds only when the toolchain targets aarch64 (see the Makefile).
   Exports just "memcpy", so running it with ./mb on the other three
   functions will print a "does not provide <symbol>" warning — which
   demonstrates that a shared object may implement any subset of the
   four functions.

     ./mb run memcpy example_neon
     ./mb check memcpy example_neon
 */

	.arch armv8-a
	.text

	.globl	memcpy
	.type	memcpy, %function
	.p2align	4
memcpy:
	mov	x3, x0			/* keep return value in x0 */

	cmp	x2, #16
	b.lo	.Ltail			/* tiny copies: straight to tail */

/* Bulk: 4x16 bytes per iteration, unaligned ld1/st1 (fine on aarch64).  */
	.p2align	4
.Lloop16:
	cmp	x2, #64
	b.lo	.Lloop1
	ld1	{v0.16b, v1.16b, v2.16b, v3.16b}, [x1], #64
	st1	{v0.16b, v1.16b, v2.16b, v3.16b}, [x3], #64
	sub	x2, x2, #64
	b	.Lloop16

	.p2align	4
.Lloop1:
	ld1	{v0.16b}, [x1], #16
	st1	{v0.16b}, [x3], #16
	subs	x2, x2, #16
	b.hs	.Lloop1

/* Tail: 0..15 bytes remaining.  */
.Ltail:
	tbz	x2, #3, .L4
	ldr	x4, [x1], #8
	str	x4, [x3], #8
.L4:
	tbz	x2, #2, .L2
	ldr	w4, [x1], #4
	str	w4, [x3], #4
.L2:
	tbz	x2, #1, .L1
	ldrh	w4, [x1], #2
	strh	w4, [x3], #2
.L1:
	tbz	x2, #0, .Ldone
	ldrb	w4, [x1]
	strb	w4, [x3]
.Ldone:
	ret
	.size	memcpy, .-memcpy
