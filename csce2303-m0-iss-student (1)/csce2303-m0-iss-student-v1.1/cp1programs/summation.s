@ main(void) is not used: this image enters at _start.
.syntax unified
.cpu cortex-m0
.thumb


.section .vectors,"a"
.word 0x20040000
.word _start


.section .text
.global _start
.thumb_func
_start:
    @ Test 1: mixed even and odd numbers
    ldr r0, =array1
    movs r1, #6
    bl sum_even
    bl print_result


    @ Test 2: positive and negative numbers
    ldr r0, =array2
    movs r1, #5
    bl sum_even
    bl print_result


    @ Test 3: length = 0
    ldr r0, =array1
    movs r1, #0
    bl sum_even
    bl print_result


    @ Test 4: length = -1
    ldr r0, =array1
    movs r1, #0
    mvns r1, r1
    bl sum_even
    bl print_result


    @ Exit
    movs r0, #0
    svc #0x00


.global sum_even
.thumb_func
sum_even:
    push {r4}
    movs r2, #0
    cmp r1, #0
    ble done
    movs r4, #1


loop:
    ldr r3, [r0]
    tst r3, r4
    bne skip
    adds r2, r2, r3


skip:
    adds r0, #4
    subs r1, #1
    bne loop


done:
    movs r0, r2
    pop {r4}
    bx lr


.thumb_func
print_result:
    svc #0x12
    movs r0, #10
    svc #0x10
    bx lr


.ltorg


.section .rodata
.align 2


array1:
    .word 1, 2, 3, 4, 5, 6


array2:
    .word -4, -3, 2, 7, 8
