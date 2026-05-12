	.set noreorder

	.macro IMPORT_START module, flags_ver
	.set push
	.section .rodata.sceResident, "a"
	.word   0
__stub_modulestr_\module:
	.asciz  "\module"
	.align  2

	.section .lib.stub, "a", @progbits
	.global __stub_module_\module
__stub_module_\module:
	.word   __stub_modulestr_\module
	.word   \flags_ver
	.word   0x5
	.word   __executable_start
	.word   __executable_start
	.set pop
	.endm

	.macro IMPORT_FUNC module, funcid, funcname
	.set push
	.set noreorder
	.extern __stub_module_\module
	.section .sceStub.text, "ax", @progbits
	.globl  \funcname
	.type   \funcname, @function
	.ent    \funcname, 0
\funcname:
	.word   __stub_module_\module
	.word   \funcid
	.end    \funcname
	.size   \funcname, .-\funcname

	.section .rodata.sceNid, "a"
	.word   \funcid
	.set pop
	.endm

	IMPORT_START "sceImpose", 0x40010011
	IMPORT_FUNC  "sceImpose", 0x381BD9E7, sceImposeHomeButton

	IMPORT_START "sceImpose_driver", 0x00010011
	IMPORT_FUNC  "sceImpose_driver", 0x1B6E3400, sceImposeGetStatus
	IMPORT_FUNC  "sceImpose_driver", 0x531C9778, sceImposeGetParam
	IMPORT_FUNC  "sceImpose_driver", 0x810FB7FB, sceImposeSetParam
