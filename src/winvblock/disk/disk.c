/**
 * Copyright (C) 2009-2010, Shao Miller <shao.miller@yrdsb.edu.on.ca>.
 * Copyright 2006-2008, V.
 * For WinAoE contact information, see http://winaoe.org/
 *
 * This file is part of WinVBlock, derived from WinAoE.
 *
 * WinVBlock is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * WinVBlock is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with WinVBlock.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Disk device specifics.
 */

#include <ntddk.h>

#include "winvblock.h"
#include "wv_stdlib.h"
#include "portable.h"
#include "irp.h"
#include "driver.h"
#include "device.h"
#include "disk.h"
#include "disk_pnp.h"
#include "disk_dev_ctl.h"
#include "disk_scsi.h"
#include "debug.h"

#ifndef _MSC_VER
static long long
__divdi3 (
  long long u,
  long long v
 )
{
  return u / v;
}
#endif

/* Globals. */
static LIST_ENTRY disk_list;
static KSPIN_LOCK disk_list_lock;
winvblock__bool disk__removable[disk__media_count] = { TRUE, FALSE, TRUE };
PWCHAR disk__compat_ids[disk__media_count] =
  { L"GenSFloppy", L"GenDisk", L"GenCdRom" };

/* Forward declarations. */
static device__free_func free_disk;

static
disk__max_xfer_len_decl (
  default_max_xfer_len
 )
{
  return 1024 * 1024;
}

/* Initialize a disk. */
static winvblock__bool STDCALL disk__init_(IN struct device__type * dev) {
    disk__type_ptr disk_ptr = disk__get_ptr(dev);
    return disk_ptr->disk_ops.init(disk_ptr);
  }

disk__init_decl ( default_init )
{
  return TRUE;
}

static void STDCALL disk__close_(IN struct device__type * dev_ptr) {
    disk__type_ptr disk_ptr = disk__get_ptr(dev_ptr);
    disk_ptr->disk_ops.close(disk_ptr);
    return;
  }

disk__close_decl ( default_close )
{
  return;
}

static NTSTATUS STDCALL power(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PIO_STACK_LOCATION Stack,
    IN struct device__type * dev_ptr,
    OUT winvblock__bool_ptr completion_ptr
  )
  {
    PoStartNextPowerIrp ( Irp );
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    IoCompleteRequest ( Irp, IO_NO_INCREMENT );
    *completion_ptr = TRUE;
    return STATUS_NOT_SUPPORTED;
  }

static NTSTATUS STDCALL sys_ctl(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PIO_STACK_LOCATION Stack,
    IN struct device__type * dev_ptr,
    OUT winvblock__bool_ptr completion_ptr
  )
{
  NTSTATUS status = Irp->IoStatus.Status;
  IoCompleteRequest ( Irp, IO_NO_INCREMENT );
  *completion_ptr = TRUE;
  return status;
}

/**
 * Create a disk PDO filled with the given disk parameters.
 *
 * @v dev_ptr           Populate PDO dev. ext. space from these details.
 * @ret dev_obj_ptr     Points to the new PDO, or is NULL upon failure.
 *
 * Returns a Physical Device Object pointer on success, NULL for failure.
 */
static PDEVICE_OBJECT STDCALL create_pdo(IN struct device__type * dev_ptr) {
    /**
     * @v disk_ptr          Used for pointing to disk details
     * @v status            Status of last operation
     * @v dev_obj_ptr       The new node's physical device object (PDO)
     * @v new_ext_size      The extension space size
     * @v disk_types[]      Floppy, hard disk, optical disc specifics
     * @v characteristics[] Floppy, hard disk, optical disc specifics
     */
    disk__type_ptr disk_ptr;
    NTSTATUS status;
    PDEVICE_OBJECT dev_obj_ptr;
    static DEVICE_TYPE disk_types[disk__media_count] =
      { FILE_DEVICE_DISK, FILE_DEVICE_DISK, FILE_DEVICE_CD_ROM };
    static winvblock__uint32 characteristics[disk__media_count] =
      { FILE_REMOVABLE_MEDIA | FILE_FLOPPY_DISKETTE, 0,
      FILE_REMOVABLE_MEDIA | FILE_READ_ONLY_DEVICE
    };
  
    DBG ( "Entry\n" );
    /*
     * Point to the disk details provided
     */
    disk_ptr = disk__get_ptr ( dev_ptr );
    /*
     * Create the disk PDO
     */
    status =
      IoCreateDevice ( driver__obj_ptr, sizeof ( driver__dev_ext ), NULL,
  		     disk_types[disk_ptr->media],
  		     FILE_AUTOGENERATED_DEVICE_NAME | FILE_DEVICE_SECURE_OPEN |
  		     characteristics[disk_ptr->media], FALSE, &dev_obj_ptr );
    if ( !NT_SUCCESS ( status ) )
      {
        Error ( "IoCreateDevice", status );
        return NULL;
      }
    /*
     * Set associations for the PDO, device, disk
     */
    device__set(dev_obj_ptr, dev_ptr);
    dev_ptr->Self = dev_obj_ptr;
    KeInitializeEvent ( &disk_ptr->SearchEvent, SynchronizationEvent, FALSE );
    KeInitializeSpinLock ( &disk_ptr->SpinLock );
    /*
     * Some device parameters
     */
    dev_obj_ptr->Flags |= DO_DIRECT_IO;	/* FIXME? */
    dev_obj_ptr->Flags |= DO_POWER_INRUSH;	/* FIXME? */
    DBG ( "Exit\n" );
    return dev_obj_ptr;
  }

/*
 * fat_extra and fat_super taken from syslinux/memdisk/setup.c by
 * H. Peter Anvin.  Licensed under the terms of the GNU General Public
 * License version 2 or later.
 */
#ifdef _MSC_VER
#  pragma pack(1)
#endif
winvblock__def_struct ( fat_extra )
{
  winvblock__uint8 bs_drvnum;
  winvblock__uint8 bs_resv1;
  winvblock__uint8 bs_bootsig;
  winvblock__uint32 bs_volid;
  char bs_vollab[11];
  char bs_filsystype[8];
} __attribute__ ( ( packed ) );

winvblock__def_struct ( fat_super )
{
  winvblock__uint8 bs_jmpboot[3];
  char bs_oemname[8];
  winvblock__uint16 bpb_bytspersec;
  winvblock__uint8 bpb_secperclus;
  winvblock__uint16 bpb_rsvdseccnt;
  winvblock__uint8 bpb_numfats;
  winvblock__uint16 bpb_rootentcnt;
  winvblock__uint16 bpb_totsec16;
  winvblock__uint8 bpb_media;
  winvblock__uint16 bpb_fatsz16;
  winvblock__uint16 bpb_secpertrk;
  winvblock__uint16 bpb_numheads;
  winvblock__uint32 bpb_hiddsec;
  winvblock__uint32 bpb_totsec32;
  union
  {
    struct
    {
      fat_extra extra;
    } fat16;
    struct
    {
      winvblock__uint32 bpb_fatsz32;
      winvblock__uint16 bpb_extflags;
      winvblock__uint16 bpb_fsver;
      winvblock__uint32 bpb_rootclus;
      winvblock__uint16 bpb_fsinfo;
      winvblock__uint16 bpb_bkbootsec;
      char bpb_reserved[12];
      /*
       * Clever, eh?  Same fields, different offset... 
       */
      fat_extra extra;
    } fat32 __attribute__ ( ( packed ) );
  } x;
} __attribute__ ( ( __packed__ ) );

#ifdef _MSC_VER
#  pragma pack()
#endif

/**
 * Attempt to guess a disk's geometry
 *
 * @v boot_sect_ptr     The MBR or VBR with possible geometry clues
 * @v disk_ptr          The disk to set the geometry for
 */
winvblock__lib_func void
disk__guess_geometry (
  IN disk__boot_sect_ptr boot_sect_ptr,
  IN OUT disk__type_ptr disk_ptr
 )
{
  winvblock__uint16 heads = 0,
    sects_per_track = 0,
    cylinders;
  mbr_ptr as_mbr;

  if ( ( boot_sect_ptr == NULL ) || ( disk_ptr == NULL ) )
    return;

  /*
   * FAT superblock geometry checking taken from syslinux/memdisk/setup.c by
   * H. Peter Anvin.  Licensed under the terms of the GNU General Public
   * License version 2 or later.
   */
  {
    /*
     * Look for a FAT superblock and if we find something that looks
     * enough like one, use geometry from that.  This takes care of
     * megafloppy images and unpartitioned hard disks. 
     */
    fat_extra_ptr extra = NULL;
    fat_super_ptr fs = ( fat_super_ptr ) boot_sect_ptr;

    if ( ( fs->bpb_media == 0xf0 || fs->bpb_media >= 0xf8 )
	 && ( fs->bs_jmpboot[0] == 0xe9 || fs->bs_jmpboot[0] == 0xeb )
	 && fs->bpb_bytspersec == 512 && fs->bpb_numheads >= 1
	 && fs->bpb_numheads <= 256 && fs->bpb_secpertrk >= 1
	 && fs->bpb_secpertrk <= 63 )
      {
	extra = fs->bpb_fatsz16 ? &fs->x.fat16.extra : &fs->x.fat32.extra;
	if ( !
	     ( extra->bs_bootsig == 0x29 && extra->bs_filsystype[0] == 'F'
	       && extra->bs_filsystype[1] == 'A'
	       && extra->bs_filsystype[2] == 'T' ) )
	  extra = NULL;
      }
    if ( extra )
      {
	heads = fs->bpb_numheads;
	sects_per_track = fs->bpb_secpertrk;
      }
  }
  /*
   * If we couldn't parse a FAT superblock, try checking MBR params.
   * Logic derived from syslinux/memdisk/setup.c by H. Peter Anvin
   */
  as_mbr = ( mbr_ptr ) boot_sect_ptr;
  if ( ( heads == 0 ) && ( sects_per_track == 0 )
       && ( as_mbr->mbr_sig == 0xAA55 ) )
    {
      int i;
      for ( i = 0; i < 4; i++ )
	{
	  if ( !( as_mbr->partition[i].status & 0x7f )
	       && as_mbr->partition[i].type )
	    {
	      winvblock__uint8 h,
	       s;

	      h = chs_head ( as_mbr->partition[i].chs_start ) + 1;
	      s = chs_sector ( as_mbr->partition[i].chs_start );

	      if ( heads < h )
		heads = h;
	      if ( sects_per_track < s )
		sects_per_track = s;

	      h = chs_head ( as_mbr->partition[i].chs_end ) + 1;
	      s = chs_sector ( as_mbr->partition[i].chs_end );

	      if ( heads < h )
		heads = h;
	      if ( sects_per_track < s )
		sects_per_track = s;
	    }
	}
    }
  /*
   * If we were unable to guess, use some hopeful defaults
   */
  if ( !heads )
    heads = 255;
  if ( !sects_per_track )
    sects_per_track = 63;
  /*
   * Set params that are not already filled
   */
  if ( !disk_ptr->Heads )
    disk_ptr->Heads = heads;
  if ( !disk_ptr->Sectors )
    disk_ptr->Sectors = sects_per_track;
  if ( !disk_ptr->Cylinders )
    disk_ptr->Cylinders = disk_ptr->LBADiskSize / ( heads * sects_per_track );
}

/* Disk dispatch routine. */
static NTSTATUS STDCALL (disk_dispatch)(
    IN PDEVICE_OBJECT (dev),
    IN PIRP (irp)
  ) {
    NTSTATUS (status);
    winvblock__bool (completion) = FALSE;
    static const irp__handling (handling_table)[] = {
        /*
         * Major, minor, any major?, any minor?, handler
         * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
         * Note that the fall-through case must come FIRST!
         * Why? It sets completion to true, so others won't be called.
         */
        {                     0, 0,  TRUE,  TRUE,  driver__not_supported },
        {          IRP_MJ_CLOSE, 0, FALSE,  TRUE,   driver__create_close },
        {         IRP_MJ_CREATE, 0, FALSE,  TRUE,   driver__create_close },
        { IRP_MJ_DEVICE_CONTROL, 0, FALSE,  TRUE, disk_dev_ctl__dispatch },
        { IRP_MJ_SYSTEM_CONTROL, 0, FALSE,  TRUE,                sys_ctl },
        {          IRP_MJ_POWER, 0, FALSE,  TRUE,                  power },
        {           IRP_MJ_SCSI, 0, FALSE,  TRUE,    disk_scsi__dispatch },
        {            IRP_MJ_PNP, 0, FALSE,  TRUE,       disk_pnp__simple },
        {            IRP_MJ_PNP,
         IRP_MN_QUERY_CAPABILITIES, FALSE, FALSE,
                                            disk_pnp__query_capabilities },
        {            IRP_MJ_PNP,
      IRP_MN_QUERY_BUS_INFORMATION, FALSE, FALSE,
                                                disk_pnp__query_bus_info },
        {            IRP_MJ_PNP,
     IRP_MN_QUERY_DEVICE_RELATIONS, FALSE, FALSE,
                                           disk_pnp__query_dev_relations },
        {            IRP_MJ_PNP,
          IRP_MN_QUERY_DEVICE_TEXT, FALSE, FALSE,
                                                disk_pnp__query_dev_text },
        {            IRP_MJ_PNP,
                   IRP_MN_QUERY_ID, FALSE, FALSE,   device__pnp_query_id },
      };

    status = irp__process_with_table(
        dev,
        irp,
        handling_table,
        sizeof handling_table,
        &completion
      );
    #ifdef DEBUGIRPS
    if (status != STATUS_PENDING)
      Debug_IrpEnd(irp, status);
    #endif

    return status;
  }

/**
 * Create a new disk
 *
 * @ret disk_ptr        The address of a new disk, or NULL for failure
 *
 * See the header file for additional details
 */
winvblock__lib_func disk__type_ptr
disk__create (
  void
 )
{
  struct device__type * dev_ptr;
  disk__type_ptr disk_ptr;

  /*
   * Try to create a device
   */
  dev_ptr = device__create (  );
  if ( dev_ptr == NULL )
    goto err_nodev;
  /*
   * Disk devices might be used for booting and should
   * not be allocated from a paged memory pool
   */
  disk_ptr = wv_mallocz(sizeof *disk_ptr);
  if ( disk_ptr == NULL )
    goto err_nodisk;
  /*
   * Track the new disk in our global list
   */
  ExInterlockedInsertTailList ( &disk_list, &disk_ptr->tracking,
				&disk_list_lock );
  /*
   * Populate non-zero device defaults
   */
  disk_ptr->device = dev_ptr;
  disk_ptr->prev_free = dev_ptr->ops.free;
  disk_ptr->disk_ops.max_xfer_len = default_max_xfer_len;
  disk_ptr->disk_ops.init = default_init;
  disk_ptr->disk_ops.close = default_close;
  dev_ptr->dispatch = disk_dispatch;
  dev_ptr->ops.close = disk__close_;
  dev_ptr->ops.create_pdo = create_pdo;
  dev_ptr->ops.free = free_disk;
  dev_ptr->ops.init = disk__init_;
  dev_ptr->ext = disk_ptr;
  KeInitializeSpinLock ( &disk_ptr->SpinLock );

  return disk_ptr;

err_nodisk:

  device__free ( dev_ptr );
err_nodev:

  return NULL;
}

/**
 * Initialize the global, disk-common environment
 *
 * @ret ntstatus        STATUS_SUCCESS or the NTSTATUS for a failure
 */
NTSTATUS
disk__init (
  void
 )
{
  /*
   * Initialize the global list of disks
   */
  InitializeListHead ( &disk_list );
  KeInitializeSpinLock ( &disk_list_lock );

  return STATUS_SUCCESS;
}

/**
 * Default disk deletion operation.
 *
 * @v dev_ptr           Points to the disk device to delete.
 */
static void STDCALL free_disk(IN struct device__type * dev_ptr)
  {
    disk__type_ptr disk_ptr = disk__get_ptr(dev_ptr);
    /* Free the "inherited class". */
    disk_ptr->prev_free(dev_ptr);
    /*
     * Track the disk deletion in our global list.  Unfortunately,
     * for now we have faith that a disk won't be deleted twice and
     * result in a race condition.  Something to keep in mind...
     */
    ExInterlockedRemoveHeadList(disk_ptr->tracking.Blink, &disk_list_lock);
  
    wv_free(disk_ptr);
  }

/* See header for details */
disk__io_decl ( disk__io )
{
  disk__type_ptr disk_ptr;

  /*
   * Establish a pointer to the disk
   */
  disk_ptr = disk__get_ptr ( dev_ptr );

  return disk_ptr->disk_ops.io ( dev_ptr, mode, start_sector, sector_count,
				 buffer, irp );
}

/* See header for details */
disk__max_xfer_len_decl ( disk__max_xfer_len )
{
  return disk_ptr->disk_ops.max_xfer_len ( disk_ptr );
}
