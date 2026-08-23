;****************************************************************************
; SCPTRACE.ASM - I/O port trace VxD for the Roland SCP-55 wave-path capture.
;
; Installs ring-3 I/O trap handlers (Install_IO_Handler) on the card's
; 16-byte window 0x330-0x33F (MPU-401 at +0/+1, proprietary glue block at
; +2..+7, CS4231A at +8..+B) plus the PCIC index/data pair 0x3E0/0x3E1,
; logs every trapped access (port, direction, value, ms timestamp) into a
; locked ring buffer, and passes the access through to real hardware.
;
; Retargeted from YTRACE.ASM (pcc10xg).  KNOWN LIMIT, learned there: this
; traps I/O from VMs (ring-3 / V86) only.  A VxD doing its own ring-0 port
; I/O is invisible, and Install_IO_Handler cannot place a second handler on
; a port another VxD already owns - on the Yamaha the whole card window came
; back unhooked for exactly that reason.  So the ARM hook mask is itself the
; first measurement: it says whether SCP95.VXD owns 0x330-0x33F.
; If it does, the escape hatches are (a) the Win3.1 ring-3-only driver
; (OEMSETUP.INF: scp55.drv, "MIDI,WAVE,AUX", no VxD) installed as the Win98
; wave driver, or (b) CR4.DE debug-register I/O breakpoints, which do catch
; ring-0 accesses.  The 0x3E0/0x3E1 half needs none of that: PCIC tracing
; worked on this bench, and a memory-window setup (regs 0x10-0x17) appearing
; during card init would confirm the 512-byte function-specific common
; memory the CIS declares as the wave channel.
;
; ARM takes an optional dword input = requested port mask (bit n = portlist
; entry n); absent/zero-length input means "all".  Lets the PCIC pair be
; traced on its own without MPU/codec chatter flooding the buffer.
; The buffer is capture-FIRST-N, not circular: the init sequence survives
; even when a wave feed overruns it.
;
; Dynamically loadable, same pattern as CFU1.VXD (rex-cfu1):
;   CreateFile("\\\\.\\<path>\\SCPTRACE.VXD", ..., FILE_FLAG_DELETE_ON_CLOSE)
; ioctls: 1=ARM 2=DISARM 3=STATUS 4=READ 5=RESET (see SCPTRACE.C).
;
; Build (Open Watcom wlink + JWasm, Win95-stamped like CFU1): ./build.sh
;****************************************************************************
        .386p
        include vmm.inc
        include vwin32.inc

;----------------------------------------------------------------------------
; ioctl codes (match SCPTRACE.C)
;----------------------------------------------------------------------------
ST_ARM          equ 1           ; in: dword requested mask (optional)
                                ; out: dword mask of ports actually hooked
ST_DISARM       equ 2
ST_STATUS       equ 3           ; out: dword evcount | (overflow << 31)
ST_READ         equ 4           ; out: evcount dwords (bounded by cbOutBuffer)
ST_RESET        equ 5

STERR_BADIOCTL  equ 6

NPORTS          equ 18          ; 330-33F, 3E0, 3E1
MAXEV           equ 4096        ; 16KB of events

; event dword: [7:0]=value [15:8]=port low byte [16]=dir(1=out) [31:17]=ms
;----------------------------------------------------------------------------
DECLARE_VIRTUAL_DEVICE SCPTRACE, 1, 0, SCPTRACE_Control, UNDEFINED_DEVICE_ID, \
                       UNDEFINED_INIT_ORDER

VxD_LOCKED_DATA_SEG
armed           db      0
                db      0
portlist        dw      330h, 331h, 332h, 333h, 334h, 335h, 336h, 337h
                dw      338h, 339h, 33Ah, 33Bh, 33Ch, 33Dh, 33Eh, 33Fh
                dw      3E0h, 3E1h
reqmask         dd      0               ; bit n set = portlist[n] requested
hookmask        dd      0               ; bit n set = portlist[n] hooked
evcount         dd      0
evover          dd      0
evbuf           dd      MAXEV dup (0)
VxD_LOCKED_DATA_ENDS

VxD_LOCKED_CODE_SEG
;----------------------------------------------------------------------------
; trap handler: EBX=VM, ECX=IO type, EDX=port, EAX=data (for OUT)
; must return EAX = data for IN.  Non-byte ops decomposed via Simulate_IO.
;----------------------------------------------------------------------------
BeginProc trace_io
        Emulate_Non_Byte_IO
        cmp     ecx, Byte_Output
        je      short tio_out
; ---- byte IN: perform real read, log, return in AL ----
        push    edx
        in      al, dx
        movzx   eax, al
        call    tio_log_in              ; logs EAX value, EDX port, dir=0
        pop     edx
        ret
tio_out:
; ---- byte OUT: log, perform real write ----
        push    eax
        push    edx
        call    tio_log_out
        pop     edx
        pop     eax
        out     dx, al
        ret
EndProc trace_io

; log helpers: value in AL (zero-extended EAX), port in EDX
BeginProc tio_log_in
        push    ecx
        xor     ecx, ecx                ; dir = 0
        call    tio_log
        pop     ecx
        ret
EndProc tio_log_in

BeginProc tio_log_out
        push    ecx
        mov     ecx, 1                  ; dir = 1
        call    tio_log
        pop     ecx
        ret
EndProc tio_log_out

; tio_log: AL=value DL=port-low CL(bit0)=dir; preserves EAX EDX
BeginProc tio_log
        push    eax
        push    ebx
        push    edx
        push    esi
        mov     esi, [evcount]
        cmp     esi, MAXEV
        jb      short @F
        inc     [evover]
        jmp     short tl_done
@@:
        ; build event dword in EBX
        movzx   ebx, al                 ; value
        movzx   eax, dl
        shl     eax, 8
        or      ebx, eax                ; port low byte
        test    cl, 1
        jz      short @F
        or      ebx, 10000h             ; dir=out
@@:
        push    ebx
        push    ecx
        push    edx
        VMMCall Get_System_Time         ; EAX = ms
        pop     edx
        pop     ecx
        pop     ebx
        shl     eax, 17
        or      ebx, eax                ; [31:17] = ms low 15 bits
        mov     [evbuf+esi*4], ebx
        inc     [evcount]
tl_done:
        pop     esi
        pop     edx
        pop     ebx
        pop     eax
        ret
EndProc tio_log
VxD_LOCKED_CODE_ENDS

VxD_CODE_SEG
;----------------------------------------------------------------------------
; arm/disarm
;----------------------------------------------------------------------------
BeginProc st_do_arm                     ; in: EAX = requested mask
                                        ; out: EAX = hook mask
        cmp     [armed], 0
        jne     short da_ret
        mov     [reqmask], eax
        xor     edi, edi                ; port index
        xor     ebx, ebx                ; mask
da_next:
        cmp     edi, NPORTS
        jae     short da_fin
        bt      [reqmask], edi
        jnc     short da_skip
        movzx   edx, [portlist+edi*2]
        mov     esi, OFFSET32 trace_io
        VMMCall Install_IO_Handler
        jc      short da_skip
        bts     ebx, edi
da_skip:
        inc     edi
        jmp     short da_next
da_fin:
        mov     [hookmask], ebx
        mov     [armed], 1
da_ret:
        mov     eax, [hookmask]
        ret
EndProc st_do_arm

BeginProc st_do_disarm
        cmp     [armed], 0
        je      short dd_ret
        xor     edi, edi
dd_next:
        cmp     edi, NPORTS
        jae     short dd_fin
        bt      [hookmask], edi
        jnc     short dd_skip
        movzx   edx, [portlist+edi*2]
        VMMCall Remove_IO_Handler
dd_skip:
        inc     edi
        jmp     short dd_next
dd_fin:
        mov     [hookmask], 0
        mov     [armed], 0
dd_ret:
        ret
EndProc st_do_disarm

;----------------------------------------------------------------------------
; control dispatch
;----------------------------------------------------------------------------
BeginProc SCPTRACE_Control
        Control_Dispatch SYS_DYNAMIC_DEVICE_INIT, SCPTRACE_Init
        Control_Dispatch SYS_DYNAMIC_DEVICE_EXIT, SCPTRACE_Exit
        Control_Dispatch W32_DEVICEIOCONTROL,     SCPTRACE_Ioctl
        clc
        ret
EndProc SCPTRACE_Control

BeginProc SCPTRACE_Init
        clc
        ret
EndProc SCPTRACE_Init

BeginProc SCPTRACE_Exit
        call    st_do_disarm            ; never leave dangling traps
        clc
        ret
EndProc SCPTRACE_Exit

;----------------------------------------------------------------------------
; W32_DEVICEIOCONTROL: ESI -> DIOCParams (preserve ESI across helpers!)
;----------------------------------------------------------------------------
BeginProc SCPTRACE_Ioctl
        mov     eax, [esi.dwIoControlCode]
        cmp     eax, DIOC_OPEN
        jne     @F
        xor     eax, eax
        ret
@@:     cmp     eax, DIOC_CLOSEHANDLE
        jne     @F
        push    esi
        call    st_do_disarm
        pop     esi
        xor     eax, eax
        ret
@@:     cmp     eax, ST_ARM
        je      ioc_arm
        cmp     eax, ST_DISARM
        je      ioc_disarm
        cmp     eax, ST_STATUS
        je      ioc_status
        cmp     eax, ST_READ
        je      ioc_read
        cmp     eax, ST_RESET
        je      ioc_reset
        mov     eax, STERR_BADIOCTL
        ret
EndProc SCPTRACE_Ioctl

; --- return dword in EAX via lpvOutBuffer (CFU1 ret_dword pattern) --------
BeginProc ret_dword
        mov     edx, [esi.lpvOutBuffer]
        test    edx, edx
        jz      @F
        cmp     [esi.cbOutBuffer], 4
        jb      @F
        mov     [edx], eax
        mov     edx, [esi.lpcbBytesReturned]
        test    edx, edx
        jz      @F
        mov     dword ptr [edx], 4
@@:     xor     eax, eax
        ret
EndProc ret_dword

ioc_arm:
        mov     eax, 0FFFFFFFFh         ; no input buffer => hook everything
        mov     edx, [esi.lpvInBuffer]
        test    edx, edx
        jz      short @F
        cmp     [esi.cbInBuffer], 4
        jb      short @F
        mov     eax, [edx]
        test    eax, eax                ; a zero mask means "all", not "none"
        jnz     short @F
        mov     eax, 0FFFFFFFFh
@@:
        push    esi
        call    st_do_arm
        pop     esi
        jmp     ret_dword

ioc_disarm:
        push    esi
        call    st_do_disarm
        pop     esi
        xor     eax, eax
        jmp     ret_dword

ioc_status:
        mov     eax, [evcount]
        cmp     [evover], 0
        je      @F
        or      eax, 80000000h
@@:     jmp     ret_dword

ioc_reset:
        mov     [evcount], 0
        mov     [evover], 0
        xor     eax, eax
        jmp     ret_dword

ioc_read:
        mov     edx, [esi.lpvOutBuffer]
        test    edx, edx
        jz      ir_bad
        mov     ecx, [esi.cbOutBuffer]
        shr     ecx, 2                  ; dwords that fit
        cmp     ecx, [evcount]
        jbe     @F
        mov     ecx, [evcount]
@@:
        push    ecx                     ; dwords copied
        push    esi
        mov     esi, OFFSET32 evbuf
        mov     edi, edx
        cld
        rep     movsd
        pop     esi
        pop     ecx
        shl     ecx, 2                  ; bytes copied
        mov     edx, [esi.lpcbBytesReturned]
        test    edx, edx
        jz      @F
        mov     [edx], ecx
@@:     xor     eax, eax
        ret
ir_bad:
        mov     eax, STERR_BADIOCTL
        ret

VxD_CODE_ENDS
        end
