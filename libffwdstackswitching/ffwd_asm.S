.text
.globl ffwd_send_request
.type ffwd_send_request, @function

ffwd_send_request:
    # Arguments:
    # rdi = old_sp_slot
    # rsi = new_sp_slot

    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    pushq $1247079108       # Request flag set to magic value (request)

    movq %rdi, %r12         # r12 = old_sp_slot

    movq %rsp, (%r12)       # *old_sp_slot = old rsp
    movq (%rsi), %rsp       # rsp = *new_sp_slot

.Lwait_request_completed:
    movq (%r12), %rax       # Dereference old_sp_slot pointer once
    cmpq $0, (%rax)         # Check if request flag is 0 (**old_sp_slot == 0)
    je .Lrequest_completed
    pause                   # Hint to the CPU that we are in a spin-wait loop
    jmp .Lwait_request_completed

.Lrequest_completed:
    movq (%r12), %rsp       # restore old rsp

    addq $8, %rsp           # skip the request flag
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp

    ret

.size ffwd_send_request, .-ffwd_send_request

.text

.globl context_switch
.type context_switch, @function

/*
 * void context_switch(void *incoming_rsp_ptr,
 *                          void *outgoing_rsp_ptr)
 *
 * rdi = incoming_rsp_ptr
 * rsi = outgoing_rsp_ptr
 *
 * *outgoing_rsp_ptr = current rsp after saving callee-saved regs
 * rsp = *incoming_rsp_ptr
 * restore callee-saved regs from new stack
 * return on new stack
 */
context_switch:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    pushq $0            # Request flag set to 0 (no request / request completed)

    movq %rsp, (%rsi)   # *outgoing_rsp_ptr = rsp
    movq (%rdi), %rsp   # rsp = *incoming_rsp_ptr

    addq $8, %rsp       # skip request flag.
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp

    retq

.size context_switch, .-context_switch
