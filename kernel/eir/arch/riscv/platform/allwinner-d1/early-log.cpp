#include <eir-internal/arch.hpp>

namespace eir {

typedef unsigned long SbiWord;

void sbiCall1(int ext, int func, SbiWord arg0) {
	register SbiWord rExt asm("a7") = ext;
	register SbiWord rFunc asm("a6") = func;
	register SbiWord rArg0 asm("a0") = arg0;
	register SbiWord rArg1 asm("a1");
	asm volatile("ecall" : "+r"(rArg0), "=r"(rArg1) : "r"(rExt), "r"(rFunc));
	if(rArg0)
		__builtin_trap();
}


void debugPrintChar(char c) {
	sbiCall1(1, 0, c);
}

}
