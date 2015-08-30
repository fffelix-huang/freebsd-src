/*-
 * Copyright (c) 2013 The FreeBSD Foundation
 * All rights reserved.
 *
 * This software was developed by Benno Rice under sponsorship from
 * the FreeBSD Foundation.
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <bootstrap.h>
#include <sys/endian.h>
#include <stand.h>

#include <efi.h>
#include <efilib.h>
#include <efiuga.h>
#include <efipciio.h>
#include <machine/metadata.h>

static EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static EFI_GUID pciio_guid = EFI_PCI_IO_PROTOCOL_GUID;
static EFI_GUID uga_guid = EFI_UGA_DRAW_PROTOCOL_GUID;

static int
efifb_mask_from_pixfmt(struct efi_fb *efifb, EFI_GRAPHICS_PIXEL_FORMAT pixfmt,
    EFI_PIXEL_BITMASK *pixinfo)
{
	int result;

	result = 0;
	switch (pixfmt) {
	case PixelRedGreenBlueReserved8BitPerColor:
		efifb->fb_mask_red = 0x000000ff;
		efifb->fb_mask_green = 0x0000ff00;
		efifb->fb_mask_blue = 0x00ff0000;
		efifb->fb_mask_reserved = 0xff000000;
		break;
	case PixelBlueGreenRedReserved8BitPerColor:
		efifb->fb_mask_red = 0x00ff0000;
		efifb->fb_mask_green = 0x0000ff00;
		efifb->fb_mask_blue = 0x000000ff;
		efifb->fb_mask_reserved = 0xff000000;
		break;
	case PixelBitMask:
		efifb->fb_mask_red = pixinfo->RedMask;
		efifb->fb_mask_green = pixinfo->GreenMask;
		efifb->fb_mask_blue = pixinfo->BlueMask;
		efifb->fb_mask_reserved = pixinfo->ReservedMask;
		break;
	default:
		result = 1;
		break;
	}
	return (result);
}

static int
efifb_from_gop(struct efi_fb *efifb, EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *mode,
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info)
{
	int result;

	efifb->fb_addr = mode->FrameBufferBase;
	efifb->fb_size = mode->FrameBufferSize;
	efifb->fb_height = info->VerticalResolution;
	efifb->fb_width = info->HorizontalResolution;
	efifb->fb_stride = info->PixelsPerScanLine;
	result = efifb_mask_from_pixfmt(efifb, info->PixelFormat,
	    &info->PixelInformation);
	return (result);
}

static int
efifb_from_uga(struct efi_fb *efifb, EFI_UGA_DRAW_PROTOCOL *uga)
{
	uint8_t *buf;
	EFI_PCI_IO_PROTOCOL *pciio;
	EFI_HANDLE handle;
	EFI_STATUS status;
	UINTN bufofs, bufsz;
	uint64_t address, length;
	uint32_t horiz, vert, depth, refresh;
	u_int bar;

	status = uga->GetMode(uga, &horiz, &vert, &depth, &refresh);
        if (EFI_ERROR(status))
		return (1);
	efifb->fb_height = vert;
	efifb->fb_width = horiz;
	efifb_mask_from_pixfmt(efifb, PixelBlueGreenRedReserved8BitPerColor,
	    NULL);
	/* Find all handles that support the UGA protocol. */
	bufsz = 0;
	status = BS->LocateHandle(ByProtocol, &uga_guid, NULL, &bufsz, NULL);
	if (status != EFI_BUFFER_TOO_SMALL)
		return (1);
	buf = malloc(bufsz);
	status = BS->LocateHandle(ByProtocol, &uga_guid, NULL, &bufsz,
	    (EFI_HANDLE *)buf);
	if (status != EFI_SUCCESS) {
		free(buf);
		return (1);
	}
	/* Get the PCI I/O interface of the first handle that supports it. */
	pciio = NULL;
	for (bufofs = 0; bufofs < bufsz; bufofs += sizeof(EFI_HANDLE)) {
		handle = *(EFI_HANDLE *)(buf + bufofs);
		status = BS->HandleProtocol(handle, &pciio_guid,
		    (void **)&pciio);
		if (status == EFI_SUCCESS)
			break;
	}
	free(buf);
	if (pciio == NULL)
		return (1);
	/* Attempt to get the frame buffer address (imprecise). */
	efifb->fb_addr = 0;
	efifb->fb_size = 0;
	for (bar = 0; bar < 6; bar++) {
		status = pciio->GetBarAttributes(pciio, bar, NULL,
		    (EFI_HANDLE *)&buf);
		if (status != EFI_SUCCESS)
			continue;
		/* XXX magic offsets and constants. */
		if (buf[0] == 0x87 && buf[3] == 0) {
			/* 32-bit address space descriptor (MEMIO) */
			address = le32dec(buf + 10);
			length = le32dec(buf + 22);
		} else if (buf[0] == 0x8a && buf[3] == 0) {
			/* 64-bit address space descriptor (MEMIO) */
			address = le64dec(buf + 14);
			length = le64dec(buf + 38);
		} else {
			address = 0;
			length = 0;
		}
		BS->FreePool(buf);
		if (address == 0 || length == 0)
			continue;
		/* We assume the largest BAR is the frame buffer. */
		if (length > efifb->fb_size) {
			efifb->fb_addr = address;
			efifb->fb_size = length;
		}
	}
	if (efifb->fb_addr == 0 || efifb->fb_size == 0)
		return (1);
	/* TODO determine the stride. */
	efifb->fb_stride = efifb->fb_width;	/* XXX */
	return (0);
}

int
efi_find_framebuffer(struct efi_fb *efifb)
{
	EFI_GRAPHICS_OUTPUT *gop;
	EFI_UGA_DRAW_PROTOCOL *uga;
	EFI_STATUS status;

	status = BS->LocateProtocol(&gop_guid, NULL, (VOID **)&gop);
	if (status == EFI_SUCCESS)
		return (efifb_from_gop(efifb, gop->Mode, gop->Mode->Info));

	status = BS->LocateProtocol(&uga_guid, NULL, (VOID **)&uga);
	if (status == EFI_SUCCESS)
		return (efifb_from_uga(efifb, uga));

	return (1);
}

static void
print_efifb(int mode, struct efi_fb *efifb, int verbose)
{
	uint32_t mask;
	u_int depth;

	if (mode >= 0)
		printf("mode %d: ", mode);
	mask = efifb->fb_mask_red | efifb->fb_mask_green |
	    efifb->fb_mask_blue | efifb->fb_mask_reserved;
	if (mask > 0) {
		for (depth = 1; mask != 1; depth++)
			mask >>= 1;
	} else
		depth = 0;
	printf("%ux%ux%u, stride=%u", efifb->fb_width, efifb->fb_height,
	    depth, efifb->fb_stride);
	if (verbose) {
		printf("\n    frame buffer: address=%jx, size=%jx",
		    (uintmax_t)efifb->fb_addr, (uintmax_t)efifb->fb_size);
		printf("\n    color mask: R=%08x, G=%08x, B=%08x\n",
		    efifb->fb_mask_red, efifb->fb_mask_green,
		    efifb->fb_mask_blue);
	}
}

COMMAND_SET(gop, "gop", "graphics output protocol", command_gop);

static int
command_gop(int argc, char *argv[])
{
	struct efi_fb efifb;
	EFI_GRAPHICS_OUTPUT *gop;
	EFI_STATUS status;
	u_int mode;

	status = BS->LocateProtocol(&gop_guid, NULL, (VOID **)&gop);
	if (EFI_ERROR(status)) {
		sprintf(command_errbuf, "%s: Graphics Output Protocol not "
		    "present (error=%lu)", argv[0], status & ~EFI_ERROR_MASK);
		return (CMD_ERROR);
	}

	if (argc < 2)
		goto usage;

	if (!strcmp(argv[1], "set")) {
		char *cp;

		if (argc != 3)
			goto usage;
		mode = strtol(argv[2], &cp, 0);
		if (cp[0] != '\0') {
			sprintf(command_errbuf, "mode is an integer");
			return (CMD_ERROR);
		}
		status = gop->SetMode(gop, mode);
		if (EFI_ERROR(status)) {
			sprintf(command_errbuf, "%s: Unable to set mode to "
			    "%u (error=%lu)", argv[0], mode,
			    status & ~EFI_ERROR_MASK);
			return (CMD_ERROR);
		}
	} else if (!strcmp(argv[1], "get")) {
		if (argc != 2)
			goto usage;
		efifb_from_gop(&efifb, gop->Mode, gop->Mode->Info);
		print_efifb(gop->Mode->Mode, &efifb, 1);
		printf("\n");
	} else if (!strcmp(argv[1], "list")) {
		EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
		UINTN infosz;

		if (argc != 2)
			goto usage;
		pager_open();
		for (mode = 0; mode < gop->Mode->MaxMode; mode++) {
			status = gop->QueryMode(gop, mode, &infosz, &info);
			if (EFI_ERROR(status))
				continue;
			efifb_from_gop(&efifb, gop->Mode, info);
			print_efifb(mode, &efifb, 0);
			if (pager_output("\n"))
				break;
		}
		pager_close();
	}
	return (CMD_OK);

 usage:
	sprintf(command_errbuf, "usage: %s [list | get | set <mode>]",
	    argv[0]);
	return (CMD_ERROR);
}

COMMAND_SET(uga, "uga", "universal graphics adapter", command_uga);

static int
command_uga(int argc, char *argv[])
{
	struct efi_fb efifb;
	EFI_UGA_DRAW_PROTOCOL *uga;
	EFI_STATUS status;

	status = BS->LocateProtocol(&uga_guid, NULL, (VOID **)&uga);
	if (EFI_ERROR(status)) {
		sprintf(command_errbuf, "%s: UGA Protocol not present "
		    "(error=%lu)", argv[0], status & ~EFI_ERROR_MASK);
		return (CMD_ERROR);
	}

	if (argc != 1)
		goto usage;

	if (efifb_from_uga(&efifb, uga) != CMD_OK) {
		sprintf(command_errbuf, "%s: Unable to get UGA information",
		    argv[0]);
		return (CMD_ERROR);
	}

	print_efifb(-1, &efifb, 1);
	printf("\n");
	return (CMD_OK);

 usage:
	sprintf(command_errbuf, "usage: %s", argv[0]);
	return (CMD_ERROR);
}
