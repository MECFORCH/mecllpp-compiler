; ================================================================
; FIRMAWORK Stage 1 Önyükleyici  —  MBR  (512 bayt)
;
; BIOS bu 512 baytı diskten 0x7C00'a yükler ve çalıştırır.
;
; Görevler:
;   1. Segment kayıtçılarını sıfırla
;   2. Boot sürücüsünü (DL) sakla
;   3. Hoş geldin mesajı yaz (INT 10h TTY)
;   4. Stage 2'yi diskten 0x7E00'a yükle (INT 13h genişletilmiş okuma)
;   5. Stage 2'ye atla
;
; Disk düzeni:
;   LBA  0       : Stage 1  (bu dosya, 1 sektör = 512 B)
;   LBA  1–16    : Stage 2  (16 sektör = 8 KB)
;   LBA  17+     : Çekirdek ikilisi  (firmware.bin)
; ================================================================

[BITS 16]
[ORG 0x7C00]

; ---- Başlangıç: CS:IP'yi normalleştir ----
    jmp  0x0000:real_start

real_start:
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  sp, 0x7C00         ; yığıt: aşağı doğru büyür, Stage 1'in hemen altı

    mov  [boot_drive], dl   ; BIOS boot sürücüsünü sakla

    ; ---- Hoş geldin mesajı (BIOS TTY) ----
    mov  si, msg_boot
    call puts16

    ; ---- INT 13h genişletilmiş okuma desteği kontrolü ----
    mov  ah, 0x41
    mov  bx, 0x55AA
    mov  dl, [boot_drive]
    int  0x13
    jc   .no_ext            ; genişletilmiş yoksa klasiğe düş

    ; ---- Stage 2'yi yükle: 16 sektör, LBA 1 → 0x7E00 ----
    mov  ah, 0x42
    mov  dl, [boot_drive]
    mov  si, dap_stage2
    int  0x13
    jc   .disk_err

    mov  si, msg_ok
    call puts16

    ; DL'i Stage 2'ye ilet
    mov  dl, [boot_drive]
    jmp  0x0000:0x7E00

.no_ext:
    mov  si, msg_noext
    call puts16
    jmp  .halt

.disk_err:
    mov  si, msg_derr
    call puts16
.halt:
    cli
    hlt
    jmp  .halt

; ---- INT 10h TTY çıktısı ----
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

; ---- Veriler ----
boot_drive: db 0x80

; DAP: 16 sektör, LBA 1'den, 0x0000:0x7E00 = fiziksel 0x7E00
dap_stage2:
    db 0x10             ; DAP boyutu (16 bayt)
    db 0x00             ; ayrılmış
    dw 16               ; okunacak sektör sayısı
    dw 0x7E00           ; hedef offset
    dw 0x0000           ; hedef segment  (0*16+0x7E00 = 0x7E00)
    dq 1                ; LBA başlangıç (sektör 1)

msg_boot:   db 'FIRMAWORK Boot...', 0x0D, 0x0A, 0
msg_ok:     db 'Stage2 Yuklendi.', 0x0D, 0x0A, 0
msg_noext:  db 'INT13 Ext YOK!',   0x0D, 0x0A, 0
msg_derr:   db 'Disk Hatasi!',      0x0D, 0x0A, 0

; ---- MBR dolgusu + önyükleme imzası ----
times 510 - ($ - $$) db 0
dw 0xAA55
