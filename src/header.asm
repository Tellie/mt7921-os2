; header.asm - MT7921.OS2 device header, strategy entry and DevHlp glue
;
; 16-bit OS/2 physical device driver, assembled with OpenWatcom WASM.
; The DATA group must come first in the file (the kernel finds the
; device header at DS:0000), followed by the CODE group. The link
; order is enforced in mt7921.lnk.

        .386p

; ---- segment declarations (order matters, see mt7921.lnk) ----------
_HEADER segment word public 'DATA' use16
_HEADER ends
CONST   segment word public 'DATA' use16
CONST   ends
CONST2  segment word public 'DATA' use16
CONST2  ends
_DATA   segment word public 'DATA' use16
_DATA   ends
_BSS    segment word public 'BSS' use16
_BSS    ends
ENDDATA segment word public 'BSS' use16
ENDDATA ends

_TEXT   segment byte public 'CODE' use16
_TEXT   ends
ENDCODE segment byte public 'CODE' use16
ENDCODE ends

DGROUP  group _HEADER, CONST, CONST2, _DATA, _BSS, ENDDATA
CGROUP  group _TEXT, ENDCODE

; ---- device header -------------------------------------------------
_HEADER segment
        public  DevHeader
DevHeader:
        dd      0FFFFFFFFh              ; next header (set by kernel)
        dw      8980h                   ; DEV_CHAR_DEV | DEV_30 (open/close)
                                        ; | DEVLEV_3
        dw      offset CGROUP:StratEntry
        dw      0                       ; no IDC entry (yet)
        db      'WRND32$ '              ; name wpa_supplicant-os2 looks for
        dw      0, 0, 0, 0              ; reserved
        dd      00000003h               ; caps: DEV_IOCTL2 | DEV_16MB
_HEADER ends

; ---- globals shared with C -----------------------------------------
_DATA   segment
        public  Device_Help
Device_Help     dd      0               ; DevHlp entry point (from INIT)
        public  MmioResult
MmioResult      dd      0               ; result of the last MmioRd / PciCfgRd
        public  PhysResult
PhysResult      dd      0               ; result of the last DH_AllocPhys
        public  VirtResult
VirtResult      dd      0               ; far pointer from DH_PhysToVirt
_DATA   ends

ENDDATA segment
        public  DataEnd
DataEnd label   byte
ENDDATA ends

; ---- code ----------------------------------------------------------
_TEXT   segment
        assume  cs:CGROUP, ds:DGROUP

        extrn   StrategyC:near          ; C, #pragma aux ... parm [es bx]

; Strategy entry: kernel calls far with ES:BX -> request packet and
; DS = our data segment.
StratEntry proc far
        call    StrategyC
        ret
StratEntry endp

; USHORT DH_VerifyAccess(USHORT sel, USHORT off, USHORT len, USHORT type)
;   #pragma aux: parm [ax] [di] [cx] [dx] value [ax]
;   type: 0 = read, 1 = read/write.  Returns 0 = OK, 0FFFFh = no access.
        public  DH_VerifyAccess
DH_VerifyAccess proc near
        mov     dh, dl                  ; DH = access type
        mov     dl, 27h                 ; DevHlp_VerifyAccess
        call    dword ptr ds:Device_Help
        sbb     ax, ax
        ret
DH_VerifyAccess endp

; USHORT GetCodeEnd(void) / GetDataEnd(void): sizes to keep after INIT
; USHORT DH_AllocGDTSel(USHORT far *sels, USHORT count)
;   #pragma aux: parm [es di] [cx] value [ax]   (INIT time only)
        public  DH_AllocGDTSel
DH_AllocGDTSel proc near
        mov     dl, 2Dh                 ; DevHlp_AllocGDTSelector
        call    dword ptr ds:Device_Help
        sbb     ax, ax
        ret
DH_AllocGDTSel endp

; USHORT DH_PhysToGDTSel(USHORT physHi, USHORT physLo, USHORT len, USHORT sel)
;   #pragma aux: parm [ax] [bx] [cx] [si] value [ax]
;   DevHlp wants AX:BX = physical address, AX = high word.
;   (v0.04: no 32-bit register pairs any more - their word order bit us)
;   len 0 = 64 KB
        public  DH_PhysToGDTSel
DH_PhysToGDTSel proc near
        mov     dl, 2Eh                 ; DevHlp_PhysToGDTSelector
        call    dword ptr ds:Device_Help
        sbb     ax, ax
        ret
DH_PhysToGDTSel endp

; void MmioRd(USHORT sel, USHORT off) - one real 32-bit read,
;   result stored in MmioResult (no register-pair ambiguity)
;   #pragma aux: parm [ax] [bx]
        public  MmioRd
MmioRd  proc near
        push    es
        push    eax
        mov     es, ax
        mov     eax, es:[bx]
        mov     dword ptr ds:MmioResult, eax
        pop     eax
        pop     es
        ret
MmioRd  endp

; void MmioWr(USHORT sel, USHORT off, USHORT lo, USHORT hi)
;   one real 32-bit write of hi:lo
;   #pragma aux: parm [ax] [bx] [cx] [dx]
        public  MmioWr
MmioWr  proc near
        push    es
        push    ecx
        mov     es, ax
        push    dx                      ; high word (higher address)
        push    cx                      ; low word
        pop     ecx                     ; ECX = hi:lo
        mov     es:[bx], ecx
        pop     ecx
        pop     es
        ret
MmioWr  endp

; USHORT DH_AllocPhys(USHORT sizeHi, USHORT sizeLo)
;   #pragma aux: parm [ax] [bx] value [ax]
;   allocates contiguous memory above 1 MB; address -> PhysResult
;   returns 0 = OK, 1 = failed
        public  DH_AllocPhys
DH_AllocPhys proc near
        mov     dh, 0                   ; above 1 MB
        mov     dl, 18h                 ; DevHlp_AllocPhys
        call    dword ptr ds:Device_Help
        jc      short ap_fail
        mov     word ptr ds:PhysResult, bx
        mov     word ptr ds:PhysResult+2, ax
        xor     ax, ax
        ret
ap_fail:
        mov     ax, 1
        ret
DH_AllocPhys endp

; USHORT DH_PhysToVirt(USHORT physHi, USHORT physLo, USHORT len)
;   #pragma aux: parm [ax] [bx] [cx] value [ax]
;   INIT time: gives a pointer usable at ring 3 (GDT selectors are not)
;   result (ES:DI) -> VirtResult; returns 0 = OK, 1 = failed
        public  DH_PhysToVirt
DH_PhysToVirt proc near
        push    es
        mov     dh, 1                   ; result in ES:DI
        mov     dl, 15h                 ; DevHlp_PhysToVirt
        call    dword ptr ds:Device_Help
        jc      short pv_fail
        mov     word ptr ds:VirtResult, di
        mov     word ptr ds:VirtResult+2, es
        pop     es
        xor     ax, ax
        ret
pv_fail:
        pop     es
        mov     ax, 1
        ret
DH_PhysToVirt endp

; void PciCfgRd(USHORT addrHi, USHORT addrLo) - config mechanism #1
;   #pragma aux: parm [ax] [bx]      result -> MmioResult
        public  PciCfgRd
PciCfgRd proc near
        pushf
        push    eax
        push    edx
        push    ax                      ; high word
        push    bx                      ; low word
        pop     eax                     ; EAX = 8000xxxx config address
        cli
        mov     dx, 0CF8h
        out     dx, eax
        mov     dx, 0CFCh
        in      eax, dx
        mov     dword ptr ds:MmioResult, eax
        pop     edx
        pop     eax
        popf
        ret
PciCfgRd endp

; void PciCfgWr(USHORT addrHi, USHORT addrLo, USHORT valLo, USHORT valHi)
;   #pragma aux: parm [ax] [bx] [cx] [dx]
        public  PciCfgWr
PciCfgWr proc near
        pushf
        push    eax
        push    ecx
        push    edx
        push    dx                      ; value high
        push    cx                      ; value low
        pop     ecx                     ; ECX = value
        push    ax
        push    bx
        pop     eax                     ; EAX = config address
        cli
        mov     dx, 0CF8h
        out     dx, eax
        mov     dx, 0CFCh
        mov     eax, ecx
        out     dx, eax
        pop     edx
        pop     ecx
        pop     eax
        popf
        ret
PciCfgWr endp

        public  GetCodeEnd
GetCodeEnd proc near
        mov     ax, offset CGROUP:CodeEnd
        ret
GetCodeEnd endp

        public  GetDataEnd
GetDataEnd proc near
        mov     ax, offset DGROUP:DataEnd
        ret
GetDataEnd endp
_TEXT   ends

ENDCODE segment
        public  CodeEnd
CodeEnd label   byte
ENDCODE ends

        end
