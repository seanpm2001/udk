/*++ @file

Copyright (c) 2006 - 2011, Intel Corporation. All rights reserved.<BR>
Portions copyright (c) 2008 - 2011, Apple Inc. All rights reserved.<BR>
This program and the accompanying materials
are licensed and made available under the terms and conditions of the BSD License
which accompanies this distribution.  The full text of the license may be found at
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include "SecMain.h"

#ifdef __APPLE__
#define MAP_ANONYMOUS MAP_ANON
char *gGdbWorkingFileName = NULL;
#endif


//
// Globals
//

EMU_THUNK_PPI mSecEmuThunkPpi = {
  GasketSecUnixPeiAutoScan,
  GasketSecUnixFdAddress,
  GasketSecEmuThunkAddress
};




//
// Default information about where the FD is located.
//  This array gets filled in with information from EFI_FIRMWARE_VOLUMES
//  EFI_FIRMWARE_VOLUMES is a host environment variable set by system.cmd.
//  The number of array elements is allocated base on parsing
//  EFI_FIRMWARE_VOLUMES and the memory is never freed.
//
UINTN       gFdInfoCount = 0;
EMU_FD_INFO *gFdInfo;

//
// Array that supports seperate memory rantes.
//  The memory ranges are set in system.cmd via the EFI_MEMORY_SIZE variable.
//  The number of array elements is allocated base on parsing
//  EFI_MEMORY_SIZE and the memory is never freed.
//
UINTN              gSystemMemoryCount = 0;
EMU_SYSTEM_MEMORY  *gSystemMemory;



UINTN                        mImageContextModHandleArraySize = 0;
IMAGE_CONTEXT_TO_MOD_HANDLE  *mImageContextModHandleArray = NULL;



/*++

Routine Description:
  Main entry point to SEC for Unix. This is a unix program

Arguments:
  Argc - Number of command line arguments
  Argv - Array of command line argument strings
  Envp - Array of environmemt variable strings

Returns:
  0 - Normal exit
  1 - Abnormal exit

**/
int
main (
  IN  int   Argc,
  IN  char  **Argv,
  IN  char  **Envp
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  InitialStackMemory;
  UINT64                InitialStackMemorySize;
  UINTN                 Index;
  UINTN                 Index1;
  UINTN                 Index2;
  UINTN                 PeiIndex;
  CHAR8                 *FileName;
  BOOLEAN               Done;
  EFI_PEI_FILE_HANDLE   FileHandle;
  VOID                  *SecFile;
  CHAR16                *MemorySizeStr;
  CHAR16                *FirmwareVolumesStr;
  UINTN                 *StackPointer;

  setbuf (stdout, 0);
  setbuf (stderr, 0);

  MemorySizeStr      = (CHAR16 *) PcdGetPtr (PcdEmuMemorySize);
  FirmwareVolumesStr = (CHAR16 *) PcdGetPtr (PcdEmuFirmwareVolume);

  printf ("\nEDK II UNIX Emulation Environment from edk2.sourceforge.net\n");

  //
  // PPIs pased into PEI_CORE
  //
  AddThunkPpi (EFI_PEI_PPI_DESCRIPTOR_PPI, &gEmuThunkPpiGuid, &mSecEmuThunkPpi);

  SecInitThunkProtocol ();
  
  //
  // Emulator Bus Driver Thunks
  //
  AddThunkProtocol (&gX11ThunkIo, (CHAR16 *)PcdGetPtr (PcdEmuGop), TRUE); 
  AddThunkProtocol (&gPosixFileSystemThunkIo, (CHAR16 *)PcdGetPtr (PcdEmuFileSystem), TRUE); 
  
  //
  // Emulator other Thunks
  //
  AddThunkProtocol (&gPthreadThunkIo, (CHAR16 *)PcdGetPtr (PcdEmuApCount), FALSE); 

  // EmuSecLibConstructor ();
  
  
  gPpiList = GetThunkPpiList (); 


#ifdef __APPLE__
  //
  // We can't use dlopen on OS X, so we need a scheme to get symboles into gdb
  // We need to create a temp file that contains gdb commands so we can load
  // symbols when we load every PE/COFF image.
  //
  Index = strlen (*Argv);
  gGdbWorkingFileName = malloc (Index + strlen(".gdb") + 1);
  strcpy (gGdbWorkingFileName, *Argv);
  strcat (gGdbWorkingFileName, ".gdb");
#endif


  //
  // Allocate space for gSystemMemory Array
  //
  gSystemMemoryCount  = CountSeperatorsInString (MemorySizeStr, '!') + 1;
  gSystemMemory       = calloc (gSystemMemoryCount, sizeof (EMU_SYSTEM_MEMORY));
  if (gSystemMemory == NULL) {
    printf ("ERROR : Can not allocate memory for system.  Exiting.\n");
    exit (1);
  }
  //
  // Allocate space for gSystemMemory Array
  //
  gFdInfoCount  = CountSeperatorsInString (FirmwareVolumesStr, '!') + 1;
  gFdInfo       = calloc (gFdInfoCount, sizeof (EMU_FD_INFO));
  if (gFdInfo == NULL) {
    printf ("ERROR : Can not allocate memory for fd info.  Exiting.\n");
    exit (1);
  }

  printf ("  BootMode 0x%02x\n", (unsigned int)PcdGet32 (PcdEmuBootMode));

  //
  // Open up a 128K file to emulate temp memory for SEC.
  //  on a real platform this would be SRAM, or using the cache as RAM.
  //  Set InitialStackMemory to zero so UnixOpenFile will allocate a new mapping
  //
  InitialStackMemorySize  = STACK_SIZE;
  InitialStackMemory = (UINTN)MapMemory (
                                0, (UINT32) InitialStackMemorySize,
                                PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANONYMOUS | MAP_PRIVATE
                                );
  if (InitialStackMemory == 0) {
    printf ("ERROR : Can not open SecStack Exiting\n");
    exit (1);
  }

  printf ("  OS Emulator passing in %u KB of temp RAM at 0x%08lx to SEC\n",
    (unsigned int)(InitialStackMemorySize / 1024),
    (unsigned long)InitialStackMemory
    );

  for (StackPointer = (UINTN*) (UINTN) InitialStackMemory;
     StackPointer < (UINTN*)(UINTN)((UINTN) InitialStackMemory + (UINT64) InitialStackMemorySize);
     StackPointer ++) {
    *StackPointer = 0x5AA55AA5;
  }

  //
  // Open All the firmware volumes and remember the info in the gFdInfo global
  //
  FileName = (CHAR8 *)malloc (StrLen (FirmwareVolumesStr) + 1);
  if (FileName == NULL) {
    printf ("ERROR : Can not allocate memory for firmware volume string\n");
    exit (1);
  }

  Index2 = 0;
  for (Done = FALSE, Index = 0, PeiIndex = 0, SecFile = NULL;
       FirmwareVolumesStr[Index2] != 0;
       Index++) {
    for (Index1 = 0; (FirmwareVolumesStr[Index2] != '!') && (FirmwareVolumesStr[Index2] != 0); Index2++) {
      FileName[Index1++] = FirmwareVolumesStr[Index2];
    }
    if (FirmwareVolumesStr[Index2] == '!') {
      Index2++;
    }
    FileName[Index1]  = '\0';

    //
    // Open the FD and remmeber where it got mapped into our processes address space
    //
    Status = MapFile (
              FileName,
              &gFdInfo[Index].Address,
              &gFdInfo[Index].Size
              );
    if (EFI_ERROR (Status)) {
      printf ("ERROR : Can not open Firmware Device File %s (%x).  Exiting.\n", FileName, (unsigned int)Status);
      exit (1);
    }

    printf ("  FD loaded from %s at 0x%08lx",FileName, (unsigned long)gFdInfo[Index].Address);

    if (SecFile == NULL) {
      //
      // Assume the beginning of the FD is an FV and look for the SEC Core.
      // Load the first one we find.
      //
      FileHandle = NULL;
      Status = PeiServicesFfsFindNextFile (
                  EFI_FV_FILETYPE_SECURITY_CORE, 
                  (EFI_PEI_FV_HANDLE)(UINTN)gFdInfo[Index].Address, 
                  &FileHandle
                  );
      if (!EFI_ERROR (Status)) {
        Status = PeiServicesFfsFindSectionData (EFI_SECTION_PE32, FileHandle, &SecFile);
        if (!EFI_ERROR (Status)) {
          PeiIndex = Index;
          printf (" contains SEC Core");
        }
      }
    }

    printf ("\n");
  }
  //
  // Calculate memory regions and store the information in the gSystemMemory
  //  global for later use. The autosizing code will use this data to
  //  map this memory into the SEC process memory space.
  //
  Index1 = 0;
  Index = 0;
  while (1) {
    UINTN val = 0;
    //
    // Save the size of the memory.
    //
    while (MemorySizeStr[Index1] >= '0' && MemorySizeStr[Index1] <= '9') {
      val = val * 10 + MemorySizeStr[Index1] - '0';
      Index1++;
    }
    gSystemMemory[Index++].Size = val * 0x100000;
    if (MemorySizeStr[Index1] == 0) {
      break;
    }
    Index1++;
  }

  printf ("\n");

  //
  // Hand off to SEC
  //
  SecLoadFromCore ((UINTN) InitialStackMemory, (UINTN) InitialStackMemorySize, (UINTN) gFdInfo[0].Address, SecFile);

  //
  // If we get here, then the SEC Core returned. This is an error as SEC should
  //  always hand off to PEI Core and then on to DXE Core.
  //
  printf ("ERROR : SEC returned\n");
  exit (1);
}


EFI_PHYSICAL_ADDRESS *
MapMemory (
  INTN fd,
  UINT64 length,
  INTN   prot,
  INTN   flags)
{
  STATIC UINTN base  = 0x40000000;
  CONST UINTN  align = (1 << 24);
  VOID         *res  = NULL;
  BOOLEAN      isAligned = 0;

  //
  // Try to get an aligned block somewhere in the address space of this
  // process.
  //
  while((!isAligned) && (base != 0)) {
    res = mmap ((void *)base, length, prot, flags, fd, 0);
    if (res == MAP_FAILED) {
      return NULL;
    }
    if ((((UINTN)res) & ~(align-1)) == (UINTN)res) {
      isAligned=1;
    } else {
      munmap(res, length);
      base += align;
    }
  }
  return res;
}


/*++

Routine Description:
  Opens and memory maps a file using Unix services. If BaseAddress is non zero
  the process will try and allocate the memory starting at BaseAddress.

Arguments:
  FileName            - The name of the file to open and map
  MapSize             - The amount of the file to map in bytes
  CreationDisposition - The flags to pass to CreateFile().  Use to create new files for
                        memory emulation, and exiting files for firmware volume emulation
  BaseAddress         - The base address of the mapped file in the user address space.
                         If passed in as NULL the a new memory region is used.
                         If passed in as non NULL the request memory region is used for
                          the mapping of the file into the process space.
  Length              - The size of the mapped region in bytes

Returns:
  EFI_SUCCESS      - The file was opened and mapped.
  EFI_NOT_FOUND    - FileName was not found in the current directory
  EFI_DEVICE_ERROR - An error occured attempting to map the opened file

**/
EFI_STATUS
MapFile (
  IN  CHAR8                     *FileName,
  IN OUT  EFI_PHYSICAL_ADDRESS  *BaseAddress,
  OUT UINT64                    *Length
  )
{
  int fd;
  VOID    *res;
  UINTN   FileSize;

  fd = open (FileName, O_RDONLY);
  if (fd < 0) {
    return EFI_NOT_FOUND;
  }
  FileSize = lseek (fd, 0, SEEK_END);


  res = MapMemory (fd, FileSize, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE);

  close (fd);

  if (res == MAP_FAILED) {
    return EFI_DEVICE_ERROR;
  }
  
  *Length = (UINT64) FileSize;
  *BaseAddress = (EFI_PHYSICAL_ADDRESS) (UINTN) res;

  return EFI_SUCCESS;
}



/*++

Routine Description:
  This is the service to load the SEC Core from the Firmware Volume

Arguments:
  LargestRegion           - Memory to use for SEC.
  LargestRegionSize       - Size of Memory to use for PEI
  BootFirmwareVolumeBase  - Start of the Boot FV
  PeiCorePe32File         - SEC PE32

Returns:
  Success means control is transfered and thus we should never return

**/
VOID
SecLoadFromCore (
  IN  UINTN   LargestRegion,
  IN  UINTN   LargestRegionSize,
  IN  UINTN   BootFirmwareVolumeBase,
  IN  VOID    *PeiCorePe32File
  )
{
  EFI_STATUS                  Status;
  EFI_PHYSICAL_ADDRESS        TopOfMemory;
  VOID                        *TopOfStack;
  EFI_PHYSICAL_ADDRESS        PeiCoreEntryPoint;
  EFI_SEC_PEI_HAND_OFF        *SecCoreData;
  UINTN                       PeiStackSize;

  //
  // Compute Top Of Memory for Stack and PEI Core Allocations
  //
  TopOfMemory  = LargestRegion + LargestRegionSize;
  PeiStackSize = (UINTN)RShiftU64((UINT64)STACK_SIZE,1);

  //
  // |-----------| <---- TemporaryRamBase + TemporaryRamSize
  // |   Heap    |
  // |           |
  // |-----------| <---- StackBase / PeiTemporaryMemoryBase
  // |           |
  // |  Stack    |
  // |-----------| <---- TemporaryRamBase
  //
  TopOfStack  = (VOID *)(LargestRegion + PeiStackSize);
  TopOfMemory = LargestRegion + PeiStackSize;

  //
  // Reservet space for storing PeiCore's parament in stack.
  //
  TopOfStack  = (VOID *)((UINTN)TopOfStack - sizeof (EFI_SEC_PEI_HAND_OFF) - CPU_STACK_ALIGNMENT);
  TopOfStack  = ALIGN_POINTER (TopOfStack, CPU_STACK_ALIGNMENT);


  //
  // Bind this information into the SEC hand-off state
  //
  SecCoreData                         = (EFI_SEC_PEI_HAND_OFF*)(UINTN) TopOfStack;
  SecCoreData->DataSize               = sizeof(EFI_SEC_PEI_HAND_OFF);
  SecCoreData->BootFirmwareVolumeBase = (VOID*)BootFirmwareVolumeBase;
  SecCoreData->BootFirmwareVolumeSize = PcdGet32 (PcdEmuFirmwareFdSize);
  SecCoreData->TemporaryRamBase       = (VOID*)(UINTN)LargestRegion;
  SecCoreData->TemporaryRamSize       = STACK_SIZE;
  SecCoreData->StackBase              = SecCoreData->TemporaryRamBase;
  SecCoreData->StackSize              = PeiStackSize;
  SecCoreData->PeiTemporaryRamBase    = (VOID*) ((UINTN) SecCoreData->TemporaryRamBase + PeiStackSize);
  SecCoreData->PeiTemporaryRamSize    = STACK_SIZE - PeiStackSize;

  //
  // Find the SEC Core Entry Point
  //
  Status = SecPeCoffGetEntryPoint (PeiCorePe32File, (VOID **)&PeiCoreEntryPoint);
  if (EFI_ERROR (Status)) {
    return ;
  }

  //
  // Transfer control to the SEC Core
  //
  PeiSwitchStacks (
    (SWITCH_STACK_ENTRY_POINT) (UINTN) PeiCoreEntryPoint,
    SecCoreData,
    (VOID *)gPpiList,
    NULL,
    TopOfStack
    );
  //
  // If we get here, then the SEC Core returned.  This is an error
  //
  return ;
}


/*++

Routine Description:
  This service is called from Index == 0 until it returns EFI_UNSUPPORTED.
  It allows discontiguous memory regions to be supported by the emulator.
  It uses gSystemMemory[] and gSystemMemoryCount that were created by
  parsing the host environment variable EFI_MEMORY_SIZE.
  The size comes from the varaible and the address comes from the call to
  UnixOpenFile.

Arguments:
  Index      - Which memory region to use
  MemoryBase - Return Base address of memory region
  MemorySize - Return size in bytes of the memory region

Returns:
  EFI_SUCCESS - If memory region was mapped
  EFI_UNSUPPORTED - If Index is not supported

**/
EFI_STATUS
EFIAPI
SecUnixPeiAutoScan (
  IN  UINTN                 Index,
  OUT EFI_PHYSICAL_ADDRESS  *MemoryBase,
  OUT UINT64                *MemorySize
  )
{
  void *res;

  if (Index >= gSystemMemoryCount) {
    return EFI_UNSUPPORTED;
  }

  *MemoryBase = 0;
  res = MapMemory (
          0, gSystemMemory[Index].Size,
          PROT_READ | PROT_WRITE | PROT_EXEC,
          MAP_PRIVATE | MAP_ANONYMOUS
          );
  if (res == MAP_FAILED) {
    return EFI_DEVICE_ERROR;
  }
  *MemorySize = gSystemMemory[Index].Size;
  *MemoryBase = (UINTN)res;
  gSystemMemory[Index].Memory = *MemoryBase;

  return EFI_SUCCESS;
}


/*++

Routine Description:
  Since the SEC is the only Unix program in stack it must export
  an interface to do POSIX calls.  gUnix is initailized in UnixThunk.c.

Arguments:
  InterfaceSize - sizeof (EFI_WIN_NT_THUNK_PROTOCOL);
  InterfaceBase - Address of the gUnix global

Returns:
  EFI_SUCCESS - Data returned

**/
VOID *
EFIAPI
SecEmuThunkAddress (
  VOID
  )
{
  return &gEmuThunkProtocol;
}



RETURN_STATUS
EFIAPI
SecPeCoffGetEntryPoint (
  IN     VOID  *Pe32Data,
  IN OUT VOID  **EntryPoint
  )
{
  EFI_STATUS                    Status;
  PE_COFF_LOADER_IMAGE_CONTEXT  ImageContext;

  ZeroMem (&ImageContext, sizeof (ImageContext));
  ImageContext.Handle     = Pe32Data;
  ImageContext.ImageRead  = (PE_COFF_LOADER_READ_FILE) SecImageRead;

  Status                  = PeCoffLoaderGetImageInfo (&ImageContext);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Relocate image to match the address where it resides
  //
  ImageContext.ImageAddress = Pe32Data;
  Status = PeCoffLoaderLoadImage (&ImageContext);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = PeCoffLoaderRelocateImage (&ImageContext);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // On Unix a dlopen is done that will change the entry point
  SecPeCoffRelocateImageExtraAction (&ImageContext);
  *EntryPoint = (VOID *)(UINTN)ImageContext.EntryPoint;
  
  return Status;
}



/*++

Routine Description:
  Return the FD Size and base address. Since the FD is loaded from a
  file into host memory only the SEC will know it's address.

Arguments:
  Index  - Which FD, starts at zero.
  FdSize - Size of the FD in bytes
  FdBase - Start address of the FD. Assume it points to an FV Header
  FixUp  - Difference between actual FD address and build address

Returns:
  EFI_SUCCESS     - Return the Base address and size of the FV
  EFI_UNSUPPORTED - Index does nto map to an FD in the system

**/
EFI_STATUS
EFIAPI
SecUnixFdAddress (
  IN     UINTN                 Index,
  IN OUT EFI_PHYSICAL_ADDRESS  *FdBase,
  IN OUT UINT64                *FdSize,
  IN OUT EFI_PHYSICAL_ADDRESS  *FixUp
  )
{
  if (Index >= gFdInfoCount) {
    return EFI_UNSUPPORTED;
  }

  *FdBase = gFdInfo[Index].Address;
  *FdSize = gFdInfo[Index].Size;
  *FixUp  = 0;

  if (*FdBase == 0 && *FdSize == 0) {
    return EFI_UNSUPPORTED;
  }

  if (Index == 0) {
    //
    // FD 0 has XIP code and well known PCD values
    // If the memory buffer could not be allocated at the FD build address
    // the Fixup is the difference.
    //
    *FixUp = *FdBase - PcdGet64 (PcdEmuFdBaseAddress);
  }

  return EFI_SUCCESS;
}


/*++

Routine Description:
  Count the number of seperators in String

Arguments:
  String    - String to process
  Seperator - Item to count

Returns:
  Number of Seperator in String

**/
UINTN
CountSeperatorsInString (
  IN  const CHAR16   *String,
  IN  CHAR16         Seperator
  )
{
  UINTN Count;

  for (Count = 0; *String != '\0'; String++) {
    if (*String == Seperator) {
      Count++;
    }
  }

  return Count;
}


EFI_STATUS
EFIAPI
SecImageRead (
  IN     VOID    *FileHandle,
  IN     UINTN   FileOffset,
  IN OUT UINTN   *ReadSize,
  OUT    VOID    *Buffer
  )
/*++

Routine Description:
  Support routine for the PE/COFF Loader that reads a buffer from a PE/COFF file

Arguments:
  FileHandle - The handle to the PE/COFF file
  FileOffset - The offset, in bytes, into the file to read
  ReadSize   - The number of bytes to read from the file starting at FileOffset
  Buffer     - A pointer to the buffer to read the data into.

Returns:
  EFI_SUCCESS - ReadSize bytes of data were read into Buffer from the PE/COFF file starting at FileOffset

**/
{
  CHAR8 *Destination8;
  CHAR8 *Source8;
  UINTN Length;

  Destination8  = Buffer;
  Source8       = (CHAR8 *) ((UINTN) FileHandle + FileOffset);
  Length        = *ReadSize;
  while (Length--) {
    *(Destination8++) = *(Source8++);
  }

  return EFI_SUCCESS;
}


/*++

Routine Description:
  Store the ModHandle in an array indexed by the Pdb File name.
  The ModHandle is needed to unload the image.

Arguments:
  ImageContext - Input data returned from PE Laoder Library. Used to find the
                 .PDB file name of the PE Image.
  ModHandle    - Returned from LoadLibraryEx() and stored for call to
                 FreeLibrary().

Returns:
  EFI_SUCCESS - ModHandle was stored.

**/
EFI_STATUS
AddHandle (
  IN  PE_COFF_LOADER_IMAGE_CONTEXT         *ImageContext,
  IN  VOID                                 *ModHandle
  )
{
  UINTN                       Index;
  IMAGE_CONTEXT_TO_MOD_HANDLE *Array;
  UINTN                       PreviousSize;


  Array = mImageContextModHandleArray;
  for (Index = 0; Index < mImageContextModHandleArraySize; Index++, Array++) {
    if (Array->ImageContext == NULL) {
      //
      // Make a copy of the stirng and store the ModHandle
      //
      Array->ImageContext = ImageContext;
      Array->ModHandle    = ModHandle;
      return EFI_SUCCESS;
    }
  }

  //
  // No free space in mImageContextModHandleArray so grow it by
  // IMAGE_CONTEXT_TO_MOD_HANDLE entires. realloc will
  // copy the old values to the new locaiton. But it does
  // not zero the new memory area.
  //
  PreviousSize = mImageContextModHandleArraySize * sizeof (IMAGE_CONTEXT_TO_MOD_HANDLE);
  mImageContextModHandleArraySize += MAX_IMAGE_CONTEXT_TO_MOD_HANDLE_ARRAY_SIZE;

  mImageContextModHandleArray = realloc (mImageContextModHandleArray, mImageContextModHandleArraySize * sizeof (IMAGE_CONTEXT_TO_MOD_HANDLE));
  if (mImageContextModHandleArray == NULL) {
    ASSERT (FALSE);
    return EFI_OUT_OF_RESOURCES;
  }

  memset (mImageContextModHandleArray + PreviousSize, 0, MAX_IMAGE_CONTEXT_TO_MOD_HANDLE_ARRAY_SIZE * sizeof (IMAGE_CONTEXT_TO_MOD_HANDLE));

  return AddHandle (ImageContext, ModHandle);
}


/*++

Routine Description:
  Return the ModHandle and delete the entry in the array.

Arguments:
  ImageContext - Input data returned from PE Laoder Library. Used to find the
                 .PDB file name of the PE Image.

Returns:
  ModHandle - ModHandle assoicated with ImageContext is returned
  NULL      - No ModHandle associated with ImageContext

**/
VOID *
RemoveHandle (
  IN  PE_COFF_LOADER_IMAGE_CONTEXT         *ImageContext
  )
{
  UINTN                        Index;
  IMAGE_CONTEXT_TO_MOD_HANDLE  *Array;

  if (ImageContext->PdbPointer == NULL) {
    //
    // If no PDB pointer there is no ModHandle so return NULL
    //
    return NULL;
  }

  Array = mImageContextModHandleArray;
  for (Index = 0; Index < mImageContextModHandleArraySize; Index++, Array++) {
    if (Array->ImageContext == ImageContext) {
      //
      // If you find a match return it and delete the entry
      //
      Array->ImageContext = NULL;
      return Array->ModHandle;
    }
  }

  return NULL;
}



//
// Target for gdb breakpoint in a script that uses gGdbWorkingFileName to source a
// add-symbol-file command. Hey what can you say scripting in gdb is not that great....
//
// Put .gdbinit in the CWD where you do gdb SecMain.dll for source level debug
//
// cat .gdbinit
// b SecGdbScriptBreak
// command
// silent
// source SecMain.gdb
// c
// end
//
VOID
SecGdbScriptBreak (
  VOID
  )
{
}

VOID
SecUnixLoaderBreak (
  VOID
  )
{
}

BOOLEAN
IsPdbFile (
  IN  CHAR8   *PdbFileName
  )
{
  UINTN Len;

  if (PdbFileName == NULL) {
    return FALSE;
  }

  Len = strlen (PdbFileName);
  if ((Len < 5)|| (PdbFileName[Len - 4] != '.')) {
    return FALSE;
  }

  if ((PdbFileName[Len - 3] == 'P' || PdbFileName[Len - 3] == 'p') &&
      (PdbFileName[Len - 2] == 'D' || PdbFileName[Len - 2] == 'd') &&
      (PdbFileName[Len - 1] == 'B' || PdbFileName[Len - 1] == 'b')) {
    return TRUE;
  }

  return FALSE;
}


#define MAX_SPRINT_BUFFER_SIZE 0x200

void
PrintLoadAddress (
  IN PE_COFF_LOADER_IMAGE_CONTEXT          *ImageContext
  )
{
  if (ImageContext->PdbPointer == NULL) {
    fprintf (stderr,
      "0x%08lx Loading NO DEBUG with entry point 0x%08lx\n",
      (unsigned long)(ImageContext->ImageAddress),
      (unsigned long)ImageContext->EntryPoint
      );
  } else {
    fprintf (stderr,
      "0x%08lx Loading %s with entry point 0x%08lx\n",
      (unsigned long)(ImageContext->ImageAddress + ImageContext->SizeOfHeaders),
      ImageContext->PdbPointer,
      (unsigned long)ImageContext->EntryPoint
      );
  }
  // Keep output synced up
  fflush (stderr);
}


VOID
EFIAPI
SecPeCoffRelocateImageExtraAction (
  IN OUT PE_COFF_LOADER_IMAGE_CONTEXT         *ImageContext
  )
{

#ifdef __APPLE__
  BOOLEAN EnabledOnEntry;

   //
   // Make sure writting of the file is an atomic operation
   //
   if (SecInterruptEanbled ()) {
     SecDisableInterrupt ();
     EnabledOnEntry = TRUE;
   } else {
     EnabledOnEntry = FALSE;
   }

  PrintLoadAddress (ImageContext);

  //
  // In mach-o (OS X executable) dlopen() can only load files in the MH_DYLIB of MH_BUNDLE format.
  // To convert to PE/COFF we need to construct a mach-o with the MH_PRELOAD format. We create
  // .dSYM files for the PE/COFF images that can be used by gdb for source level debugging.
  //
  FILE  *GdbTempFile;

  //
  // In the Mach-O to PE/COFF conversion the size of the PE/COFF headers is not accounted for.
  // Thus we need to skip over the PE/COFF header when giving load addresses for our symbol table.
  //
  if (ImageContext->PdbPointer != NULL && !IsPdbFile (ImageContext->PdbPointer)) {
    //
    // Now we have a database of the images that are currently loaded
    //

    //
    // 'symbol-file' will clear out currnet symbol mappings in gdb.
    // you can do a 'add-symbol-file filename address' for every image we loaded to get source
    // level debug in gdb. Note Sec, being a true application will work differently.
    //
    // We add the PE/COFF header size into the image as the mach-O does not have a header in
    // loaded into system memory.
    //
    // This gives us a data base of gdb commands and after something is unloaded that entry will be
    // removed. We don't yet have the scheme of how to comunicate with gdb, but we have the
    // data base of info ready to roll.
    //
    // We could use qXfer:libraries:read, but OS X GDB does not currently support it.
    //  <library-list>
    //    <library name="/lib/libc.so.6">   // ImageContext->PdbPointer
    //      <segment address="0x10000000"/> // ImageContext->ImageAddress + ImageContext->SizeOfHeaders
    //    </library>
    //  </library-list>
    //

    //
    // Write the file we need for the gdb script
    //
    GdbTempFile = fopen (gGdbWorkingFileName, "w");
    if (GdbTempFile != NULL) {
      fprintf (GdbTempFile, "add-symbol-file %s 0x%08lx\n", ImageContext->PdbPointer, (long unsigned int)(ImageContext->ImageAddress + ImageContext->SizeOfHeaders));
      fclose (GdbTempFile);

      //
      // Target for gdb breakpoint in a script that uses gGdbWorkingFileName to set a breakpoint.
      // Hey what can you say scripting in gdb is not that great....
      //
      SecGdbScriptBreak ();
    } else {
      ASSERT (FALSE);
    }

    AddHandle (ImageContext, ImageContext->PdbPointer);

    if (EnabledOnEntry) {
      SecEnableInterrupt ();
    }

    
  }

#else

  void        *Handle = NULL;
  void        *Entry = NULL;

  if (ImageContext->PdbPointer == NULL) {
    return;
  }

  if (!IsPdbFile (ImageContext->PdbPointer)) {
    return;
  }

  fprintf (
     stderr,
     "Loading %s 0x%08lx - entry point 0x%08lx\n",
     ImageContext->PdbPointer,
     (unsigned long)ImageContext->ImageAddress,
     (unsigned long)ImageContext->EntryPoint
     );

  Handle = dlopen (ImageContext->PdbPointer, RTLD_NOW);

  if (Handle) {
    Entry = dlsym (Handle, "_ModuleEntryPoint");
  } else {
    printf("%s\n", dlerror());
  }

  if (Entry != NULL) {
    ImageContext->EntryPoint = (UINTN)Entry;
    printf ("Change %s Entrypoint to :0x%08lx\n", ImageContext->PdbPointer, (unsigned long)Entry);
  }

  SecUnixLoaderBreak ();

#endif

  return;
}


VOID
EFIAPI
SecPeCoffUnloadImageExtraAction (
  IN PE_COFF_LOADER_IMAGE_CONTEXT         *ImageContext
  )
{
  VOID *Handle;

  Handle = RemoveHandle (ImageContext);

#ifdef __APPLE__
  FILE  *GdbTempFile;
  BOOLEAN EnabledOnEntry;

  if (Handle != NULL) {
    //
    // Need to skip .PDB files created from VC++
    //
    if (!IsPdbFile (ImageContext->PdbPointer)) {
       if (SecInterruptEanbled ()) {
         SecDisableInterrupt ();
         EnabledOnEntry = TRUE;
       } else {
         EnabledOnEntry = FALSE;
       }
       
      //
      // Write the file we need for the gdb script
      //
      GdbTempFile = fopen (gGdbWorkingFileName, "w");
      if (GdbTempFile != NULL) {
        fprintf (GdbTempFile, "remove-symbol-file %s\n", ImageContext->PdbPointer);
        fclose (GdbTempFile);

        //
        // Target for gdb breakpoint in a script that uses gGdbWorkingFileName to set a breakpoint.
        // Hey what can you say scripting in gdb is not that great....
        //
        SecGdbScriptBreak ();
      } else {
        ASSERT (FALSE);
      }
      
      if (EnabledOnEntry) {
        SecEnableInterrupt ();
      }
    }
  }

#else
  //
  // Don't want to confuse gdb with symbols for something that got unloaded
  //
  if (Handle != NULL) {
    dlclose (Handle);
  }

#endif
  return;
}


