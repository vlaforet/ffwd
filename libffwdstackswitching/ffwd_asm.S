.text
.globl call_on_stack
.type call_on_stack, @function

call_on_stack:
    # Arguments:
    # rdi = old_sp_slot
    # rsi = new_sp_slot
    # rdx = fn
    # rcx = arg

    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rdi, %r12        # r12 = old_sp_slot
    movq %rsi, %r13        # r13 = new_sp_slot
    movq %rdx, %r14        # r14 = fn
    movl %ecx, %r15d       # r15d = arg

    movq %rsp, (%r12)      # *old_sp_slot = old rsp
    movq (%r13), %rsp      # rsp = *new_sp_slot

    movl %r15d, %edi       # server_id -> first arg
    call *%r14             # fn(server_id)

    movq %rsp, (%r13)      # *new_sp_slot = current shadow rsp
    movq (%r12), %rsp      # restore old rsp

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp

    ret

.size call_on_stack, .-call_on_stack

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

    movq %rsp, (%rsi)      # *outgoing_rsp_ptr = rsp
    movq (%rdi), %rsp      # rsp = *incoming_rsp_ptr

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp

    retq

.size context_switch, .-context_switch