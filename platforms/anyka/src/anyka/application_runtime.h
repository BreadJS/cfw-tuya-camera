/* SPDX-License-Identifier: MIT */
#ifndef ANYKA_APPLICATION_RUNTIME_H
#define ANYKA_APPLICATION_RUNTIME_H
/* Executable/cwd/application maps must be RAM backed. The sole exception is
 * canonical /lib regular files on preserved readonly SquashFS mtdblock5; the
 * installer independently pins the full ROOTFS hash before any APP erase.
 * Does not stop DMA and never claims hardware shutdown. */
int anyka_application_ram_safe(void);
/* 0: no matching owned APP source, 1: readonly APP mtd7, 2: readonly RAM-loop
 * APP test. Running SDK host and module must match the mounted APP bytes.
 * Only 1 permits installation/installed-boot confirmation. */
int anyka_application_app_source(void);
int anyka_application_source_receipt(const char *directory,int source);
/* Root-owned RAM directory, exact current kernel boot UUID + newline,0600.
 * Caller must first stop transport and acknowledge executor pause. */
int anyka_application_quiesce_receipt(const char *directory);
#endif
