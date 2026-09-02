/* struct ffwd_request -- must match ffwd_internals.h */
#define REQ_RSP  0
#define REQ_SEQ  8

/* The callee-saved registers are pushed on the switched stack rather than kept
   in a shared slot: the stacks are uncontended (a parked client never touches
   its own stack, and the server owns it for the whole critical section),
   whereas any shared line is being polled and would ping-pong. */
.macro SAVE_CONTEXT
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
.endm

.macro RESTORE_CONTEXT
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
.endm

.text
.globl ffwd_send_request
.type ffwd_send_request, @function

/*
 * void ffwd_send_request(struct ffwd_request *req, void *shadow_sp,
 *                        volatile uint64_t *resp_flags, uint64_t mask,
 *                        void **resume_slot)
 *
 * rdi = req, rsi = shadow stack, rdx = &resp->flags, rcx = mask,
 * r8 = &resp->resume_rsp[slot]
 *
 * Publishes the parked stack pointer in req->rsp, toggles req->seq to post the
 * request, then parks on the shadow stack until the server toggles this
 * client's bit in *resp_flags. The resume stack pointer comes back in the same
 * response line, so the server never has to write to req at all.
 *
 * The response slots arrive as plain pointers so this function needs to know
 * nothing about the group layout.
 */
ffwd_send_request:
    SAVE_CONTEXT

    movq  %rsp, REQ_RSP(%rdi)   # req->rsp = parked rsp
    movq  REQ_SEQ(%rdi), %r9
    xorq  %rcx, %r9             # r9 = new sequence value
    movq  %r9, REQ_SEQ(%rdi)    # post; x86-TSO orders the rsp store first
    movq  %rsi, %rsp            # park on the shadow stack

.Lwait_request_completed:
    movq  (%rdx), %r10
    xorq  %r9, %r10
    testq %rcx, %r10            # released once our response bit matches our sequence bit
    jz    .Lrequest_completed
    pause
    jmp   .Lwait_request_completed

.Lrequest_completed:
    movq  (%r8), %rsp           # resume where the server left us
    RESTORE_CONTEXT

    ret

.size ffwd_send_request, .-ffwd_send_request

.text

.globl context_switch
.type context_switch, @function

/*
 * void context_switch(void **in_rsp_slot, void **out_rsp_slot)
 *
 * rdi = in_rsp_slot  : holds the stack pointer to resume
 * rsi = out_rsp_slot : receives the current stack pointer
 *
 * out_rsp_slot is server-local staging (or the server's own slot), never the
 * outgoing client's line, and it does not release anybody by itself: the
 * release only becomes visible when the server publishes the group.
 */
context_switch:
    SAVE_CONTEXT

    movq  (%rdi), %rax   # incoming rsp
    movq  %rsp, (%rsi)   # hand the outgoing rsp to the staged release
    movq  %rax, %rsp     # switch stacks

    RESTORE_CONTEXT

    retq

.size context_switch, .-context_switch

.text

.globl context_switch_release
.type context_switch_release, @function

/*
 * void context_switch_release(void **in_rsp_slot, void **out_rsp_slot,
 *                             volatile uint64_t *flags, uint64_t flags_value)
 *
 * rdi = in_rsp_slot, rsi = out_rsp_slot, rdx = &resp->flags, rcx = new flags
 *
 * Used for the last handoff of a batch, where out_rsp_slot points straight
 * into the shared response line: the outgoing client is released here rather
 * than after the server has switched back onto its own stack, which is pure
 * added latency when the batch holds only one or two requests.
 *
 * The incoming rsp is loaded first, so no stack access remains after the
 * release becomes visible.
 */
context_switch_release:
    SAVE_CONTEXT

    movq  (%rdi), %rax   # incoming rsp -- read before releasing
    movq  %rsp, (%rsi)   # publish the outgoing resume pointer
    movq  %rcx, (%rdx)   # publish flags -> releases it (x86-TSO orders rsp first)
    movq  %rax, %rsp     # switch stacks

    RESTORE_CONTEXT

    retq

.size context_switch_release, .-context_switch_release
