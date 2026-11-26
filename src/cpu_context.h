#ifndef SWR_CPU_CONTEXT_H
#define SWR_CPU_CONTEXT_H

// CONTEXT register members by role, for code that runs on both x86 and x86-64.
#if defined(__x86_64__)
#define SWR_REG_IP Rip
#define SWR_REG_SP Rsp
#define SWR_REG_BP Rbp
#define SWR_REG_AX Rax
#define SWR_REG_BX Rbx
#define SWR_REG_CX Rcx
#define SWR_REG_DX Rdx
#define SWR_REG_SI Rsi
#define SWR_REG_DI Rdi
#else
#define SWR_REG_IP Eip
#define SWR_REG_SP Esp
#define SWR_REG_BP Ebp
#define SWR_REG_AX Eax
#define SWR_REG_BX Ebx
#define SWR_REG_CX Ecx
#define SWR_REG_DX Edx
#define SWR_REG_SI Esi
#define SWR_REG_DI Edi
#endif

#endif
