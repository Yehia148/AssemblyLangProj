@ main(void) is not used: this image enters at _start.
@ CSCE 2303 assembly template. Keep the vector table; write your program after _start.
        .syntax unified
        .cpu    cortex-m0
        .thumb

        .section .vectors, "a"
        .word   0x20040000        @ initial sp: top of RAM
        .word   _start            @ reset vector

        .text
        .thumb_func
        .global _start
_start:
        ldr r0, =firstmsg
    bl readinteger
    movs r4, r0

    ldr r0, =lastmsg
    bl readinteger
    movs r5, r0

    ldr r0, =stepmsg
    bl readinteger
    movs r6, r0

    cmp r6, #0
    ble invalid

    movs r0, r4
    movs r1, r5
    movs r2, r6
    bl calculaterange

    movs r4, r0
    movs r5, r1

    ldr r0, =countmsg
    svc #0x11
    movs r0, r4
    svc #0x12

    ldr r0, =summsg
    svc #0x11
    movs r0, r5
    svc #0x12
    b exit

invalid:
    ldr r0, =errormsg
    svc #0x11

exit:
    movs r0, #0
    svc #0x00

.thumb_func
readinteger:
    svc #0x11
    svc #0x15
    bx lr

.thumb_func
calculaterange:
    push {r4, r5, r6, r7}

    movs r3, r0
    movs r4, #0
    movs r5, #0

rangeloop:
    cmp r3, r1
    bgt rangedone

    adds r4, #1
    adds r5, r5, r3

    adds r6, r3, r2
    cmp r6, r3
    blt rangedone

    movs r3, r6
    b rangeloop

rangedone:
    movs r0, r4
    movs r1, r5

    pop {r4, r5, r6, r7}
    bx lr

.ltorg

.section .rodata
firstmsg: .asciz "Enter first value: "
lastmsg:  .asciz "Enter last value: "
stepmsg:  .asciz "Enter step: "
countmsg: .asciz "Count = "
summsg:   .asciz "\nSum = "
errormsg: .asciz "Step must be positive."
