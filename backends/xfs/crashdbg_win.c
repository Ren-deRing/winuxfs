// SPDX-License-Identifier: GPL-2.0-only
/*
 * Debug aid, Windows only: report an unhandled exception (code, faulting
 * address, unwound frames as module+RVA for addr2line) on stderr before the
 * process dies. WER is off on the test VM, so a crash is otherwise silent.
 */
#ifdef _WIN32
#include <windows.h>
#include <stdio.h>

static LONG WINAPI wfs_crash_filter(EXCEPTION_POINTERS *ep)
{
	CONTEXT c = *ep->ContextRecord;
	int i;

	fprintf(stderr, "[crash] exception %08lx at %p (access addr %p)\n",
		(unsigned long)ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress,
		ep->ExceptionRecord->NumberParameters > 1 ? (void *)ep->ExceptionRecord->ExceptionInformation[1] : NULL);
	for (i = 0; i < 40 && c.Rip; i++) {
		DWORD64 base = 0, hd = 0;
		PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &base, NULL);
		HMODULE hm = NULL;
		char name[MAX_PATH] = "?";

		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				       (LPCSTR)c.Rip, &hm))
			GetModuleFileNameA(hm, name, sizeof(name));
		fprintf(stderr, "[crash]  #%d %s+0x%llx\n", i, name, (unsigned long long)(c.Rip - (DWORD64)hm));
		if (!fe) {
			c.Rip = *(DWORD64 *)c.Rsp;
			c.Rsp += 8;
		} else {
			void *hdata = NULL;

			RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fe, &c, &hdata, &hd, NULL);
		}
	}
	fflush(stderr);
	return EXCEPTION_CONTINUE_SEARCH;
}

__attribute__((constructor)) static void wfs_crash_install(void)
{
	SetUnhandledExceptionFilter(wfs_crash_filter);
}
#endif
