; ================================================================
; FIRMAWORK Stage 2 Önyükleyici  (v4 — x86-64, COM1 UART, çift çıktı)
;
; Stage 1 → 0x7E00'a yükler.
;
; Adımlar:
;   1. COM1 seri portu başlat (115200-8N1) — erken tanı çıktısı için
;   2. INT 13h uzantı desteği kontrol et (AH=0x41)
;   3. Kernel'ı 0x0000:0x8000 (fiziksel 0x8000) adresine yükle
;        64 sektör × 512 B = 32 KB  (kernel şu an ~25 KB)
;   4. A20 etkinleştir (port 0x92)
;   5. 32-bit GDT kur, CR0.PE = 1
;   6. Far JMP → pm32_start
;   7. PM32: seri port bildirim + 0x8000'den 0x100000'e kopyala
;   8. EAX=0xF12AB007, EBX=0 → 0x100000'e atla
;
; COM1 pinout (NS16550A, port 0x3F8):
;   +0  RBR/THR  (read/write data)
;   +1  IER      (interrupt enable)
;   +2  IIR/FCR
;   +3  LCR      (DLAB @ bit7)
;   +4  MCR
;   +5  LSR      (bit5=THRE, bit0=DR)
; ================================================================

[BITS 16]
[ORG 0x7E00]

start16:
    ; Segment kayıtçılarını sıfırla
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax
    mov  sp, 0x7BFE

    mov  [boot_drive], dl       ; Stage 1'den gelen BIOS sürücü numarası

    ; ================================================================
    ; 1. COM1 Erken Seri Port Başlatma (115200-8N1)
    ;    BIOS çıktısına paralel UART tanı akışı sağlar.
    ; ================================================================
    call com1_init

    mov  si, msg_s2_hello
    call puts_dual              ; BIOS TTY + COM1

    ; ================================================================
    ; 2. INT 13h Uzatılmış Oku desteğini doğrula (AH=0x41)
    ;    Başarısız olursa: cihaz uzatılmış okumayı desteklemiyor.
    ; ================================================================
    mov  ah, 0x41
    mov  bx, 0x55AA
    mov  dl, [boot_drive]
    int  0x13
    jc   no_ext_support         ; CF=1 → desteklenmiyor

    ; ================================================================
    ; 3. Kernel'ı 0x0000:0x8000 = fiziksel 0x8000'e yükle
    ;    64 sektör × 512 = 32 KB
    ;    LBA 17'den başla (disk.img düzeni: LBA0=S1, LBA1-16=S2, LBA17+=kernel)
    ; ================================================================
    mov  ah, 0x42               ; INT 13h: Genişletilmiş Sektör Oku
    mov  dl, [boot_drive]
    mov  si, dap_kernel
    int  0x13
    jc   disk_error

    mov  si, msg_kern_ok
    call puts_dual

    ; ================================================================
    ; 4. A20 Hattını Etkinleştir (port 0x92 Fast A20)
    ; ================================================================
    in   al, 0x92
    or   al, 0x02
    and  al, 0xFE
    out  0x92, al
    times 8 nop

    ; ================================================================
    ; 5. 32-bit GDT kur, Korumalı Moda geç
    ; ================================================================
    mov  si, msg_pm_enter
    call puts_dual
    cli
    lgdt [gdt32_desc]
    mov  eax, cr0
    or   eax, 0x00000001
    mov  cr0, eax
    jmp  0x08:pm32_start

; ================================================================
; Hata işleyicileri (16-bit, çift çıktı)
; ================================================================
no_ext_support:
    mov  si, msg_noext
    call puts_dual
    jmp  .halt
.halt:
    cli
    hlt
    jmp  .halt

disk_error:
    push ax
    mov  si, msg_derr
    call puts_dual
    pop  ax
    mov  al, ah                 ; INT 13h hata kodu → AL
    call print_hex_byte_dual
    mov  si, msg_crlf
    call puts_dual
.halt:
    cli
    hlt
    jmp  .halt

; ================================================================
; 16-bit Yardımcı Rutinler
; ================================================================

; ---- BIOS TTY çıktısı (SI → terminal) ----
puts16:
    lodsb
    test al, al
    jz   .ret
    mov  ah, 0x0E
    xor  bh, bh
    int  0x10
    jmp  puts16
.ret:
    ret

; ---- COM1 başlatma: 115200-8N1 ----
com1_init:
    ; IER: tüm kesmeler devre dışı
    mov  dx, 0x3F9
    xor  al, al
    out  dx, al
    ; LCR: DLAB=1 (baud rate bölücüye eriş)
    mov  dx, 0x3FB
    mov  al, 0x80
    out  dx, al
    ; DLL: bölücü düşük bayt → 115200 baud için 1
    mov  dx, 0x3F8
    mov  al, 0x01
    out  dx, al
    ; DLM: bölücü yüksek bayt → 0
    mov  dx, 0x3F9
    xor  al, al
    out  dx, al
    ; LCR: 8 bit, parite yok, 1 dur biti (DLAB=0)
    mov  dx, 0x3FB
    mov  al, 0x03
    out  dx, al
    ; FCR: FIFO etkin, RX trigger 14 bayt
    mov  dx, 0x3FA
    mov  al, 0xC7
    out  dx, al
    ; MCR: RTS + DTR
    mov  dx, 0x3FC
    mov  al, 0x0B
    out  dx, al
    ret

; ---- COM1 tek karakter gönder (AL, DX bozulur) ----
serial_putc16:
    push ax
.wait_tx:
    mov  dx, 0x3FD              ; LSR
    in   al, dx
    test al, 0x20               ; THRE (bit5): TX tamponu boş mu?
    jz   .wait_tx
    pop  ax
    mov  dx, 0x3F8              ; THR (data)
    out  dx, al
    ret

; ---- COM1 dize gönder (SI) ----
serial_puts16:
    push ax
.lp:
    lodsb
    test al, al
    jz   .done
    ; \n → \r\n dönüşümü
    cmp  al, 0x0A
    jne  .not_lf
    push ax
    mov  al, 0x0D
    call serial_putc16
    pop  ax
.not_lf:
    call serial_putc16
    jmp  .lp
.done:
    pop  ax
    ret

; ---- Çift çıktı: BIOS TTY + COM1  (SI değişmez) ----
puts_dual:
    push si
    call puts16                 ; BIOS TTY
    pop  si
    call serial_puts16          ; COM1
    ret

; ---- AL'yi iki hex karakteri: BIOS TTY + COM1 ----
print_hex_byte_dual:
    push ax
    shr  al, 4
    call print_hex_nibble_dual
    pop  ax
    and  al, 0x0F
    call print_hex_nibble_dual
    ret

print_hex_nibble_dual:
    add  al, '0'
    cmp  al, '9'
    jle  .ok
    add  al, 7
.ok:
    ; BIOS TTY
    push ax
    mov  ah, 0x0E
    xor  bh, bh
    int  0x10
    ; COM1
    pop  ax
    call serial_putc16
    ret

; ================================================================
; Veri bölümü
; ================================================================
boot_drive:  db 0x80

; DAP: 64 sektör, LBA 17, hedef 0x0000:0x8000 = fiziksel 0x8000
dap_kernel:
    db 0x10             ; DAP boyutu
    db 0x00             ; ayrılmış
    dw 64               ; sektör sayısı: 64 × 512 = 32 KB
    dw 0x8000           ; hedef offset
    dw 0x0000           ; hedef segment → fiziksel = 0x8000 ✓
    dq 17               ; LBA başlangıç (64-bit)

msg_s2_hello: db 'FIRMAWORK Stage2 v4 | COM1:115200', 0x0D, 0x0A, 0
msg_kern_ok:  db 'Kernel 32KB @ 0x8000 OK', 0x0D, 0x0A, 0
msg_pm_enter: db 'A20+GDT hazir, PM32 gecisi...', 0x0D, 0x0A, 0
msg_derr:     db 'S2 DiskErr=0x', 0
msg_noext:    db 'S2 NoExtINT13 — hata!', 0x0D, 0x0A, 0
msg_crlf:     db 0x0D, 0x0A, 0

; ================================================================
; 32-bit düz GDT
; ================================================================
align 8
gdt32:
    dq 0x0000000000000000   ; [0x00] Null
    dq 0x00CF9A000000FFFF   ; [0x08] Kod:  P=1 DPL=0 Type=0xA G=1 D=1
    dq 0x00CF92000000FFFF   ; [0x10] Veri: P=1 DPL=0 Type=0x2 G=1 B=1
gdt32_end:

gdt32_desc:
    dw gdt32_end - gdt32 - 1
    dd gdt32

; ================================================================
; 32-bit Korumalı Mod Kodu
; ================================================================
[BITS 32]

pm32_start:
    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax
    mov  esp, 0x7BFE

    ; ================================================================
    ; PM32 giriş bildirimi — COM1 üzerinden
    ; ================================================================
    mov  esi, msg_pm32_ok
    call serial_puts32

    ; ================================================================
    ; 5. Kernel'ı 0x8000'den 0x100000'e kopyala
    ;    32 KB / 4 = 8192 dword
    ; ================================================================
    cld
    mov  esi, 0x00008000    ; kaynak: 0x8000 (gerçek modda yüklendi)
    mov  edi, 0x00100000    ; hedef:  0x100000 (ELF bağlama adresi)
    mov  ecx, 8192          ; 8192 × 4 = 32768 bayt = 32 KB
    rep  movsd

    mov  esi, msg_pm32_jmp
    call serial_puts32

    ; ================================================================
    ; 6. FIRMAWORK çekirdeğine atla: 0x100000
    ;    EAX = 0xF12AB007 ("FIRMAB00T")
    ;    EBX = 0  (MBI işaretçisi yok)
    ;
    ; entry.s _start (0x100000): jmp _start_code (EB XX)
    ;   → 64-bit uzun mod geçişi → main()
    ; ================================================================
    mov  eax, 0xF12AB007
    xor  ebx, ebx
    jmp  dword 0x08:0x00100000

; ================================================================
; 32-bit PM Seri Port Rutinleri (COM1, x86 I/O portları)
;
; serial_putc32: AL'deki karakteri COM1'e gönderir.
;                EAX, EDX değişebilir.
; serial_puts32: ESI'deki null-sonlu dizeyi COM1'e gönderir.
; ================================================================

serial_putc32:
    push eax
    push edx
.wait_tx32:
    mov  dx, 0x3FD          ; LSR
    in   al, dx
    test al, 0x20           ; THRE bit5
    jz   .wait_tx32
    pop  edx
    pop  eax
    ; \n → \r\n
    cmp  al, 0x0A
    jne  .no_lf
    push eax
    push edx
    mov  al, 0x0D
    mov  dx, 0x3F8
    out  dx, al
    pop  edx
    pop  eax
.no_lf:
    mov  dx, 0x3F8
    out  dx, al
    ret

serial_puts32:
    push eax
.sp32_lp:
    lodsb
    test al, al
    jz   .sp32_done
    call serial_putc32
    jmp  .sp32_lp
.sp32_done:
    pop  eax
    ret

; ================================================================
; 32-bit PM mesaj dizileri
; ================================================================
msg_pm32_ok:  db 'FIRMAWORK PM32: gecis tamam, kernel kopyalaniyor...', 0x0D, 0x0A, 0
msg_pm32_jmp: db 'PM32: 0x100000 adresine atlanıyor (EAX=0xF12AB007)', 0x0D, 0x0A, 0
