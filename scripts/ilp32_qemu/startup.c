/*
 * Reset handler, vector table, heap and fault reporting for the Cortex-M3 of
 * QEMU's mps2-an385 board. Copies .data from flash, clears .bss, then enters
 * newlib's rdimon crt0 (_start), which fetches argv over semihosting, runs
 * main and reports its return value to QEMU as the exit status.
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

extern uint32_t __etext, __data_start__, __data_end__, __bss_start__, __bss_end__;
extern uint32_t __StackTop;
extern uint8_t __moq_heap_start[], __moq_heap_end[];
extern void _start(void);

void Reset_Handler(void);
void Default_Handler(void);
void moq_ilp32_fault(const uint32_t *frame);
void *_sbrk(ptrdiff_t incr);

void Reset_Handler(void)
{
    const uint32_t *src = &__etext;
    uint32_t *dst = &__data_start__;
    while (dst < &__data_end__) *dst++ = *src++;
    for (dst = &__bss_start__; dst < &__bss_end__;) *dst++ = 0;
    _start();
    for (;;) {}
}

/* The heap is the PSRAM window the linker script reserves. Exhaustion is
 * an ordinary allocation failure (malloc returns NULL), never a write past
 * the window. */
void *_sbrk(ptrdiff_t incr)
{
    static uint8_t *brk = __moq_heap_start;
    if (incr < 0 ? (size_t)-incr > (size_t)(brk - __moq_heap_start)
                 : (size_t)incr > (size_t)(__moq_heap_end - brk)) {
        errno = ENOMEM;
        return (void *)-1;
    }
    uint8_t *prev = brk;
    brk += incr;
    return prev;
}

static uint32_t semihost(uint32_t op, const void *arg)
{
    register uint32_t r0 __asm__("r0") = op;
    register const void *r1 __asm__("r1") = arg;
    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

static void put_hex(char *p, uint32_t v)
{
    for (int i = 7; i >= 0; i--, v >>= 4) p[i] = "0123456789abcdef"[v & 0xFu];
}

/* Reports the fault (exception number, CFSR, faulting PC) on the semihosting
 * console and ends the run with a failing status instead of parking the
 * core until the timeout. */
void moq_ilp32_fault(const uint32_t *frame)
{
    uint32_t ipsr;
    __asm__ volatile("mrs %0, ipsr" : "=r"(ipsr));
    char msg[] = "ILP32 fault: exception 00000000 cfsr 00000000 pc 00000000\n";
    put_hex(msg + 23, ipsr & 0x1FFu);
    put_hex(msg + 37, *(volatile const uint32_t *)0xE000ED28u);
    put_hex(msg + 49, frame[6]);
    (void)semihost(0x04u /* SYS_WRITE0 */, msg);
    (void)semihost(0x18u /* SYS_EXIT */, (const void *)0x20023u /* RunTimeErrorUnknown */);
    for (;;) {}
}

/* Any exception without its own handler is a fault for a test run. */
__attribute__((naked)) void Default_Handler(void)
{
    __asm__ volatile("tst lr, #4\n"
                     "ite eq\n"
                     "mrseq r0, msp\n"
                     "mrsne r0, psp\n"
                     "b moq_ilp32_fault\n");
}

/* Initial stack pointer, then the reset and system exception handlers. */
__attribute__((section(".vectors"), used))
const uintptr_t moq_ilp32_vectors[16] = {
    (uintptr_t)&__StackTop,
    (uintptr_t)Reset_Handler,
    (uintptr_t)Default_Handler,     /* NMI */
    (uintptr_t)Default_Handler,     /* HardFault */
    (uintptr_t)Default_Handler,     /* MemManage */
    (uintptr_t)Default_Handler,     /* BusFault */
    (uintptr_t)Default_Handler,     /* UsageFault */
    0, 0, 0, 0,
    (uintptr_t)Default_Handler,     /* SVCall */
    (uintptr_t)Default_Handler,     /* DebugMonitor */
    0,
    (uintptr_t)Default_Handler,     /* PendSV */
    (uintptr_t)Default_Handler,     /* SysTick */
};
