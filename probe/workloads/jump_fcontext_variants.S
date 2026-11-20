/* Two variants of jump_fcontext (src/jump_x86_64_sysv_elf_gas.S, from
   Boost.Context 1.64) for the --yield-jump option of preempt_bench. Each one
   saves and loads the same 64-byte context as jump_fcontext, so jump_fcontext
   can resume a context that a variant saved, and the reverse.

   jump_fcontext_yield_copy: the instructions of jump_fcontext. Only do_yield
   calls it, so its indirect jump (jmp *%r8) always goes to the same place in
   the scheduler. The indirect jump of jump_fcontext goes to four places in each
   run of a filler: the filler entry, the scheduler after the yield, the filler
   after the yield, and the scheduler after the end of the filler.

   jump_fcontext_no_fpu_control_load: jump_fcontext without the loads of the
   MXCSR and of the x87 control word (ldmxcsr and fldcw). It still saves the
   two words, for the jump_fcontext that resumes the context. preempt_bench
   does not change either word, so each context has the same words. */

.text
.globl jump_fcontext_yield_copy
.type jump_fcontext_yield_copy,@function
.align 16
jump_fcontext_yield_copy:
    leaq  -0x38(%rsp), %rsp /* prepare stack */
    stmxcsr  (%rsp)     /* save MMX control- and status-word */
    fnstcw   0x4(%rsp)  /* save x87 control-word */
    movq  %r12, 0x8(%rsp)  /* save R12 */
    movq  %r13, 0x10(%rsp)  /* save R13 */
    movq  %r14, 0x18(%rsp)  /* save R14 */
    movq  %r15, 0x20(%rsp)  /* save R15 */
    movq  %rbx, 0x28(%rsp)  /* save RBX */
    movq  %rbp, 0x30(%rsp)  /* save RBP */
    /* store RSP (pointing to context-data) in RAX */
    movq  %rsp, %rax
    /* restore RSP (pointing to context-data) from RDI */
    movq  %rdi, %rsp
    movq  0x38(%rsp), %r8  /* restore return-address */
    ldmxcsr  (%rsp)     /* restore MMX control- and status-word */
    fldcw    0x4(%rsp)  /* restore x87 control-word */
    movq  0x8(%rsp), %r12  /* restore R12 */
    movq  0x10(%rsp), %r13  /* restore R13 */
    movq  0x18(%rsp), %r14  /* restore R14 */
    movq  0x20(%rsp), %r15  /* restore R15 */
    movq  0x28(%rsp), %rbx  /* restore RBX */
    movq  0x30(%rsp), %rbp  /* restore RBP */
    leaq  0x40(%rsp), %rsp /* prepare stack */
    /* return transfer_t from jump */
    /* RAX == fctx, RDX == data */
    movq  %rsi, %rdx
    /* pass transfer_t as first arg in context function */
    /* RDI == fctx, RSI == data */
    movq  %rax, %rdi
    /* indirect jump to context */
    jmp  *%r8
.size jump_fcontext_yield_copy,.-jump_fcontext_yield_copy

.globl jump_fcontext_no_fpu_control_load
.type jump_fcontext_no_fpu_control_load,@function
.align 16
jump_fcontext_no_fpu_control_load:
    leaq  -0x38(%rsp), %rsp /* prepare stack */
    stmxcsr  (%rsp)     /* save MMX control- and status-word */
    fnstcw   0x4(%rsp)  /* save x87 control-word */
    movq  %r12, 0x8(%rsp)  /* save R12 */
    movq  %r13, 0x10(%rsp)  /* save R13 */
    movq  %r14, 0x18(%rsp)  /* save R14 */
    movq  %r15, 0x20(%rsp)  /* save R15 */
    movq  %rbx, 0x28(%rsp)  /* save RBX */
    movq  %rbp, 0x30(%rsp)  /* save RBP */
    /* store RSP (pointing to context-data) in RAX */
    movq  %rsp, %rax
    /* restore RSP (pointing to context-data) from RDI */
    movq  %rdi, %rsp
    movq  0x38(%rsp), %r8  /* restore return-address */
    /* no ldmxcsr and no fldcw: the words of the context are the same */
    movq  0x8(%rsp), %r12  /* restore R12 */
    movq  0x10(%rsp), %r13  /* restore R13 */
    movq  0x18(%rsp), %r14  /* restore R14 */
    movq  0x20(%rsp), %r15  /* restore R15 */
    movq  0x28(%rsp), %rbx  /* restore RBX */
    movq  0x30(%rsp), %rbp  /* restore RBP */
    leaq  0x40(%rsp), %rsp /* prepare stack */
    /* return transfer_t from jump */
    /* RAX == fctx, RDX == data */
    movq  %rsi, %rdx
    /* pass transfer_t as first arg in context function */
    /* RDI == fctx, RSI == data */
    movq  %rax, %rdi
    /* indirect jump to context */
    jmp  *%r8
.size jump_fcontext_no_fpu_control_load,.-jump_fcontext_no_fpu_control_load

.section .note.GNU-stack,"",%progbits
