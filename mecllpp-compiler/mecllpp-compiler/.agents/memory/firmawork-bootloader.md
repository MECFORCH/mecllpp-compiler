---
name: FIRMAWORK Bootloader
description: Özel boot zinciri tasarım kararları ve kısıtlamalar
---

# FIRMAWORK Özel Bootloader Kısıtlamaları

## Kural: SeaBIOS INT 13h ile 1 MB üstüne doğrudan yükleme yapılamaz

SeaBIOS (QEMU varsayılan BIOS), INT 13h AH=0x42 ile segment:offset = 0xFFFF:0x0010
= fiziksel 0x100000 adresine yazma desteği sunmaz. Adres hilesi matematiksel doğru
olsa da BIOS bunu reddeder.

**Why:** A20 hattı etkin olsa bile SeaBIOS disk DMA transferini gerçek mod
adres uzayıyla sınırlar veya bu segment kombinasyonunu doğrulamaz.

**How to apply:** Kernel önce güvenli gerçek mod adresine yükle (0x8000),
ardından 32-bit PM'de REP MOVSD ile 0x100000'e kopyala.
ESI=0x8000, EDI=0x100000, ECX=8192 (32KB = 64 sektor)

## Disk Düzeni

LBA 0 (512B): Stage1 MBR — LBA 1-16 (8KB): Stage2 — LBA 17+: firmware.bin (düz ikili)

## entry.s _start Tasarımı

_start ilk komutu jmp _start_code (EB 0E) olmalı — hem Multiboot hem özel
bootloader 0x100000'e atlar, JMP header'ı atlatır.

## Boot Sihri Değerleri

0x2BADB002 = Multiboot/GRUB, 0xF12AB007 = FIRMAWORK özel ("FIRMAB00T", EBX=0)

## calistir.sh

Varsayılan: Multiboot. Özel: BOOT_MODE=custom sh calistir.sh
