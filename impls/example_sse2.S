/* Example user implementation in x86-64 assembly (SSE2, 16-byte loop).

   Builds only when the toolchain targets x86-64 (see the Makefile).
   Exports just "memcpy", so running it with ./mb on the other three
   functions will print a "does not provide <symbol>" warning — which
   demonstrates that a shared object may implement any subset of the
   four functions.

   Linux x86-64 SysV ABI: memcpy(void *dst=RDI, const void *src=RSI,
   size_t n=RDX) returns RDI.

     ./mb run memcpy example_sse2
     ./mb check memcpy example_sse2
 */

	.text

	.globl	memcpy
	.type	memcpy, @function
	.p2align	4
memcpy:
	mov	%rdi, %rax		/* return value = dst		*/

	cmp	$16, %rdx
	jb	.Ltail			/* tiny copies: straight to tail */

/* Bulk: 4x16 bytes per iteration (SSE2, unaligned movdqu/movups).  */
	.p2align	4
.Lblk64:
	cmp	$64, %rdx
	jb	.Lblk16
	movdqu	0(%rsi), %xmm0
	movdqu	16(%rsi), %xmm1
	movdqu	32(%rsi), %xmm2
	movdqu	48(%rsi), %xmm3
	movups	%xmm0, 0(%rdi)
	movups	%xmm1, 16(%rdi)
	movups	%xmm2, 32(%rdi)
	movups	%xmm3, 48(%rdi)
	add	$64, %rsi
	add	$64, %rdi
	sub	$64, %rdx
	jmp	.Lblk64

	.p2align	4
.Lblk16:
	cmp	$16, %rdx
	jb	.Ltail
	movdqu	0(%rsi), %xmm0
	movups	%xmm0, 0(%rdi)
	add	$16, %rsi
	add	$16, %rdi
	sub	$16, %rdx
	jmp	.Lblk16

/* Tail: 0..15 bytes remaining.  */
.Ltail:
	test	$8, %edx
	jz	.L4
	mov	0(%rsi), %rcx
	mov	%rcx, 0(%rdi)
	add	$8, %rsi
	add	$8, %rdi
.L4:
	test	$4, %edx
	jz	.L2
	mov	0(%rsi), %ecx
	mov	%ecx, 0(%rdi)
	add	$4, %rsi
	add	$4, %rdi
.L2:
	test	$2, %edx
	jz	.L1
	movzwl	0(%rsi), %ecx
	mov	%cx, 0(%rdi)
	add	$2, %rsi
	add	$2, %rdi
.L1:
	test	$1, %edx
	jz	.Ldone
	movzbl	0(%rsi), %ecx
	mov	%cl, 0(%rdi)
.Ldone:
	ret
	.size	memcpy, .-memcpy
