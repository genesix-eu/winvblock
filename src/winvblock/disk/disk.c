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
 * Disk device specifics
 *
 */

#include <ntddk.h>

#include "winvblock.h"
#include "portable.h"
#include "irp.h"
#include "driver.h"
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

static winvblock__uint32 next_disk = 0;
winvblock__bool disk__removable[disk__media_count] = { TRUE, FALSE, TRUE };
PWCHAR disk__compat_ids[disk__media_count] =
  { L"GenSFloppy", L"GenDisk", L"GenCdRom" };

disk__max_xfer_len_decl ( disk__default_max_xfer_len )
{
  return 1024 * 1024;
}

static
driver__dev_init_decl (
  init
 )
{
  disk__type_ptr disk_ptr = get_disk_ptr ( dev_ptr );
  return disk_ptr->ops->init ( disk_ptr );
}

disk__init_decl ( disk__default_init )
{
  return TRUE;
}

static
driver__dev_close_decl (
  close
 )
{
  disk__type_ptr disk_ptr = get_disk_ptr ( dev_ptr );
  disk_ptr->ops->close ( disk_ptr );
  return;
}

disk__close_decl ( disk__default_close )
{
  return;
}

static
irp__handler_decl (
  power
 )
{
  PoStartNextPowerIrp ( Irp );
  Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
  IoCompleteRequest ( Irp, IO_NO_INCREMENT );
  *completion_ptr = TRUE;
  return STATUS_NOT_SUPPORTED;
}

static
irp__handler_decl (
  sys_ctl
 )
{
  NTSTATUS status = Irp->IoStatus.Status;
  IoCompleteRequest ( Irp, IO_NO_INCREMENT );
  *completion_ptr = TRUE;
  return status;
}

static irp__handling handling_table[] = {
  /*
   * Major, minor, any major?, any minor?, handler
   */
  {IRP_MJ_DEVICE_CONTROL, 0, FALSE, TRUE, disk_dev_ctl__dispatch}
  ,
  {IRP_MJ_SYSTEM_CONTROL, 0, FALSE, TRUE, sys_ctl}
  ,
  {IRP_MJ_POWER, 0, FALSE, TRUE, power}
  ,
  {IRP_MJ_SCSI, 0, FALSE, TRUE, disk_scsi__dispatch}
  ,
  {IRP_MJ_PNP, 0, FALSE, TRUE, disk_pnp__simple}
  ,
  {IRP_MJ_PNP, IRP_MN_QUERY_CAPABILITIES, FALSE, FALSE,
   disk_pnp__query_capabilities}
  ,
  {IRP_MJ_PNP, IRP_MN_QUERY_BUS_INFORMATION, FALSE, FALSE,
   disk_pnp__query_bus_info}
  ,
  {IRP_MJ_PNP, IRP_MN_QUERY_DEVICE_RELATIONS, FALSE, FALSE,
   disk_pnp__query_dev_relations}
  ,
  {IRP_MJ_PNP, IRP_MN_QUERY_DEVICE_TEXT, FALSE, FALSE, disk_pnp__query_dev_text}
  ,
  {IRP_MJ_PNP, IRP_MN_QUERY_ID, FALSE, FALSE, disk_pnp__query_id}
};

/**
 * Create a disk PDO filled with the given disk parameters
 *
 * @v dev_ptr           Populate PDO dev. ext. space from these details
 *
 * Returns a Physical Device Object pointer on success, NULL for failure.
 */
static
driver__dev_create_pdo_decl (
  create_pdo
 )
{
  /**
   * @v disk_ptr          Used for pointing to disk details
   * @v status            Status of last operation
   * @v dev_obj_ptr       The new node's device object
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
  disk_ptr = get_disk_ptr ( dev_ptr );
  /*
   * Create the disk device.  Whoever calls us should have set
   * the device extension space size requirement appropriately
   */
  status =
    IoCreateDevice ( driver__obj_ptr, disk_ptr->device.size, NULL,
		     disk_types[disk_ptr->media],
		     FILE_AUTOGENERATED_DEVICE_NAME | FILE_DEVICE_SECURE_OPEN |
		     characteristics[disk_ptr->media], FALSE, &dev_obj_ptr );
  if ( !NT_SUCCESS ( status ) )
    {
      Error ( "IoCreateDevice", status );
      return NULL;
    }
  /*
   * Re-purpose dev_ptr to point into the PDO's device
   * extension space.  We have disk_ptr still for original details
   */
  dev_ptr = dev_obj_ptr->DeviceExtension;
  /*
   * Clear the extension space and establish parameters
   */
  RtlZeroMemory ( dev_ptr, disk_ptr->device.size );
  /*
   * Copy the provided disk parameters into the disk extension space
   */
  RtlCopyMemory ( dev_ptr, &disk_ptr->device, disk_ptr->device.size );
  /*
   * Universal disk properties the caller needn't bother with
   */
  dev_ptr->IsBus = FALSE;
  dev_ptr->Self = dev_obj_ptr;
  dev_ptr->DriverObject = driver__obj_ptr;
  dev_ptr->State = NotStarted;
  dev_ptr->OldState = NotStarted;
  dev_ptr->irp_handler_chain = NULL;
  /*
   * Register the default driver IRP handling table
   */
  irp__reg_table_s ( &dev_ptr->irp_handler_chain, driver__handling_table,
		     driver__handling_table_size );
  /*
   * Register the default disk IRP handling table
   */
  irp__reg_table ( &dev_ptr->irp_handler_chain, handling_table );
  /*
   * Establish a pointer to the disk
   */
  disk_ptr = get_disk_ptr ( dev_ptr );
  KeInitializeEvent ( &disk_ptr->SearchEvent, SynchronizationEvent, FALSE );
  KeInitializeSpinLock ( &disk_ptr->SpinLock );
  disk_ptr->Unmount = FALSE;
  disk_ptr->DiskNumber = InterlockedIncrement ( &next_disk ) - 1;
  /*
   * Some device parameters
   */
  dev_obj_ptr->Flags |= DO_DIRECT_IO;	/* FIXME? */
  dev_obj_ptr->Flags |= DO_POWER_INRUSH;	/* FIXME? */
  DBG ( "Exit\n" );
  return dev_obj_ptr;
}

/* Device operations for disks */
driver__dev_ops disk__dev_ops = {
  create_pdo,
  init,
  close
};

winvblock__lib_func driver__dev_ops_ptr
disk__get_ops (
  void
 )
{
  return &disk__dev_ops;
}

/* An MBR C/H/S address and ways to access its components */
typedef winvblock__uint8 chs[3];

#define     chs_head( chs ) chs[0]
#define   chs_sector( chs ) ( chs[1] & 0x3F )
#define chs_cyl_high( chs ) ( ( ( winvblock__uint16 ) ( chs[1] & 0xC0 ) ) << 2 )
#define  chs_cyl_low( chs ) ( ( winvblock__uint16 ) chs[2] )
#define chs_cylinder( chs ) ( chs_cyl_high ( chs ) | chs_cyl_low ( chs ) )

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

winvblock__def_struct ( mbr )
{
  winvblock__uint8 code[440];
  winvblock__uint32 disk_sig;
  winvblock__uint16 pad;
  struct
  {
    winvblock__uint8 status;
    chs chs_start;
    winvblock__uint8 type;
    chs chs_end;
    winvblock__uint32 lba_start;
    winvblock__uint32 lba_count;
  } partition[4] __attribute__ ( ( packed ) );
  winvblock__uint16 mbr_sig;
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
