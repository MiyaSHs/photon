/* Photon CPU L3, TRACE mode: a second CLuaInterface vtable whose entries record the slot number in a 256-entry ring, then jump to the
 * real method with every argument register untouched (only rax, r10, r11 are used). l3_ring_dump() prints the last calls. */
#include <stdint.h>
#include <stdio.h>
#include "iface.h"

uint16_t l3_ring[256];
uint32_t l3_ring_pos;

__asm__(".text\n.p2align 4\nl3_common:\n"
        "  movl l3_ring_pos(%rip), %r11d\n"
        "  andl $255, %r11d\n"
        "  leaq l3_ring(%rip), %r10\n"
        "  movw %ax, (%r10,%r11,2)\n"
        "  incl l3_ring_pos(%rip)\n"
        "  leaq photon_iface_vtable(%rip), %r10\n"
        "  jmpq *(%r10,%rax,8)\n");
#define T(n) __asm__(".text\n.p2align 3\nl3_thunk_" #n ":\n  movl $" #n ", %eax\n  jmp l3_common\n"); extern void l3_thunk_##n(void);
#define T10(a) T(a##0) T(a##1) T(a##2) T(a##3) T(a##4) T(a##5) T(a##6) T(a##7) T(a##8) T(a##9)
T(0) T(1) T(2) T(3) T(4) T(5) T(6) T(7) T(8) T(9)
T10(1) T10(2) T10(3) T10(4) T10(5) T10(6) T10(7) T10(8) T10(9) T10(10) T10(11) T10(12)
T(130) T(131) T(132)
#define R(n) (void *)l3_thunk_##n,
#define R10(a) R(a##0) R(a##1) R(a##2) R(a##3) R(a##4) R(a##5) R(a##6) R(a##7) R(a##8) R(a##9)
void *photon_iface_vtable_traced[133] = {
    R(0) R(1) R(2) R(3) R(4) R(5) R(6) R(7) R(8) R(9)
    R10(1) R10(2) R10(3) R10(4) R10(5) R10(6) R10(7) R10(8) R10(9) R10(10) R10(11) R10(12)
    R(130) R(131) R(132)
};

void l3_ring_dump(void) {
    char line[256 * 5 + 1];
    size_t k = 0;
    uint32_t end = l3_ring_pos, start = end > 256 ? end - 256 : 0;
    for (uint32_t i = start; i < end; i++) k += (size_t)snprintf(line + k, sizeof line - k, "%u ", l3_ring[i & 255]);
    l3_trace("last %u interface calls (of %u), oldest first: %s", end - start, end, line);
}
