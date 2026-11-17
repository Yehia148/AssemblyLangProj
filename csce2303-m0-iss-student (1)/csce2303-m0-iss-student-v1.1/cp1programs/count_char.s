@ main(void) is not used: this image enters at _start.

.syntax unified
.cpu cortex-m0
.thumb

.section .vectors, "a"
.word 0x20040000
.word _start

.text
.thumb_func
.global _start
_start:
    ldr r0, =str1
    movs r1, #97 @ target 'a'
    bl count_char
    svc #0x12 @ PUTINT
    movs r0, #10
    svc #0x10 @ PUTCHAR newline

    ldr r0, =str2
    movs r1, #97 @ target 'a'
    bl count_char
    svc #0x12
    movs r0, #10
    svc #0x10

    ldr r0, =str1
    movs r1,#122 @ target 'z'
    bl count_char
    svc #0x12
    movs r0, #10
    svc #0x10

    movs r0, #0
    svc #0x00 @ EXIT
.thumb_func
.global count_char
count_char:
    movs r2,#0 @ count = 0
count_loop:
    ldrb r3,[r0] @ r3 = *str (unsigned byte)
    cmp r3,#0
    beq done  @ stop at NUL
    cmp r3, r1
    bne skip @ not the target
    adds r2, #1 @ count++
skip:
    adds r0, #1 @ str++
    b count_loop
done:
    movs r0, r2 @ return count
    bx lr

.ltorg
.section .rodata
str1:.asciz "banana"
str2:.asciz ""
