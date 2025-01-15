#include <efi.h>
#include <efilib.h>
#include <elf.h>

#define PART_NUM 1


typedef struct {
    EFI_PHYSICAL_ADDRESS framebuffer_base;
    UINTN framebuffer_size;
    UINT32 width;
    UINT32 height;
    UINT32 pixels_per_scanline;
} GRAPHICS;
typedef struct {
    EFI_MEMORY_DESCRIPTOR* map;
    UINTN size;
    UINTN desc_size;
} MEMORY_MAP;
typedef struct {
    GRAPHICS graphics;
    MEMORY_MAP mem_map;
} BOOT_INFO;


VOID error_checker(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop, EFI_STATUS status) { // if an error occurs, a big square appears on the screen
    static int k = 0;

    if (EFI_ERROR(status) != 1) return;
    for (UINT8 i = 0; i < 50; i++) {
        for (UINT8 j = k; j < 50 + k; j++) {
            *((UINT64*)(gop->Mode->FrameBufferBase + (gop->Mode->Info->PixelsPerScanLine * i * 4) + (j * 4))) = 0x00aaaaaa;
        }
    }
    k += 60;
}
BOOLEAN memcmp(VOID* _mem1, VOID* _mem2, UINTN size) {
    UINT8* mem1 = (UINT8*)_mem1;
    UINT8* mem2 = (UINT8*)_mem2;

    for (UINTN i = 0; i < size; i++) {
        if (mem1[i] != mem2[i]) return FALSE;
    }

    return TRUE;
}
EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* SystemTable) {
    InitializeLib(ImageHandle, SystemTable);
    EFI_STATUS status;
    
    // Locate GOP
    status = EFI_SUCCESS;
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = NULL;
    {
        status = uefi_call_wrapper(BS->LocateProtocol, 3, &gEfiGraphicsOutputProtocolGuid, NULL, (VOID**)&gop);

        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* info;
        UINTN info_size, mode_count, native_mode;
        status |= uefi_call_wrapper(gop->QueryMode, 4, gop, gop->Mode == NULL ? 0 : gop->Mode->Mode, &info_size, &info);

        if (status == EFI_NOT_STARTED)
            status |= uefi_call_wrapper(gop->SetMode, 2, gop, 0);

        native_mode = gop->Mode->Mode;
        mode_count = gop->Mode->MaxMode;

        for (UINTN i = 0; i < mode_count; i++) {
            status |= uefi_call_wrapper(gop->QueryMode, 4, gop, i, &info_size, &info);

            if (info->HorizontalResolution == 1920 && info->VerticalResolution == 1080) {
                status |= uefi_call_wrapper(gop->SetMode, 2, gop, i);
                break;
            }
        }
    }
    error_checker(gop, status);

    // Open kernel from partition 2
    // There are only two block_io-s on QEMU, but there are four block_io-s on real machine wtf
    // on QEMU, PART_NUM = 1 / on real machine, PART_NUM = 3
    status = EFI_SUCCESS;
    EFI_FILE_HANDLE kernel;
    {
        EFI_HANDLE* handle_buffer = NULL;
        UINTN handle_count = 0;
        status |= uefi_call_wrapper(BS->LocateHandleBuffer, 5, ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &handle_count, &handle_buffer);
        
        UINT8 part_nums[0xff] = {0, };
        {
            EFI_BLOCK_IO_PROTOCOL* block_io = NULL;
            UINT8 p_i = 0;
            for (UINTN i = 0; i < handle_count; i++) {
                status = uefi_call_wrapper(BS->HandleProtocol, 3, handle_buffer[i], &gEfiBlockIoProtocolGuid, (VOID**)&block_io);
                if (EFI_ERROR(status)) continue;
                if (!block_io->Media->LogicalPartition || !block_io->Media->MediaPresent) continue;
                part_nums[p_i++] = i;
                *((UINT64*)(gop->Mode->FrameBufferBase + (gop->Mode->Info->PixelsPerScanLine * i * 4 * 4) + (0 * 4))) = 0x00aaaaaa;
            }
        }

        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* sfsp = NULL;
        status |= uefi_call_wrapper(BS->HandleProtocol, 3, handle_buffer[part_nums[PART_NUM]], &gEfiSimpleFileSystemProtocolGuid, (VOID**)&sfsp);

        EFI_FILE_HANDLE root;
        status |= uefi_call_wrapper(sfsp->OpenVolume, 2, sfsp, &root);

        status |= uefi_call_wrapper(root->Open, 5, root, &kernel, L"sys\\kernel\\kernel", EFI_FILE_MODE_READ);
    }
    error_checker(gop, status);

    

    // Load kernel
    status = EFI_SUCCESS;
    Elf64_Addr kernel_entry;
    {
        Elf64_Ehdr ehdr;
        UINTN ehdr_size = sizeof(ehdr);
        status |= uefi_call_wrapper(kernel->Read, 3, kernel, &ehdr_size, &ehdr);

        if (
            !memcmp(&ehdr.e_ident[EI_MAG0], ELFMAG, SELFMAG) ||
            ehdr.e_ident[EI_CLASS] != ELFCLASS64 ||
            ehdr.e_ident[EI_DATA] != ELFDATA2LSB ||
            ehdr.e_type != ET_EXEC ||
            ehdr.e_machine != EM_X86_64 ||
            ehdr.e_version != EV_CURRENT
        ) status = !EFI_SUCCESS;
        
        Elf64_Phdr* phdr_buffer;
        status |= uefi_call_wrapper(kernel->SetPosition, 2, kernel, ehdr.e_phoff);

        UINTN phdr_buffer_size = ehdr.e_phnum * ehdr.e_phentsize;
        status |= uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, phdr_buffer_size, (VOID**)&phdr_buffer);
        status |= uefi_call_wrapper(kernel->Read, 3, kernel, &phdr_buffer_size, phdr_buffer);

        for (
            Elf64_Phdr* phdr = phdr_buffer;
            (UINT8*)phdr < (UINT8*)phdr_buffer + phdr_buffer_size;
            phdr = (Elf64_Phdr*)((UINT8*)phdr + ehdr.e_phentsize)
        ) {
            if (phdr->p_type == PT_LOAD) {
                UINTN pages = (phdr->p_memsz + 0x1000 - 1) / 0x1000;
                Elf64_Addr seg = phdr->p_paddr;
                status |= uefi_call_wrapper(BS->AllocatePages, 4, AllocateAddress, EfiLoaderData, pages, &seg);
                status |= uefi_call_wrapper(kernel->SetPosition, 2, kernel, phdr->p_offset);
                
                UINTN phdr_size = phdr->p_filesz;
                status |= uefi_call_wrapper(kernel->Read, 3, kernel, &phdr_size, (VOID*)seg);
            }
        }
        
        kernel_entry = ehdr.e_entry;
    }
    error_checker(gop, status);

    // Get memory map
    status = EFI_SUCCESS;
    EFI_MEMORY_DESCRIPTOR* mem_map = NULL;
    UINTN mem_map_size, mem_map_key, desc_size;
    UINT32 desc_ver;
    {
        uefi_call_wrapper(BS->GetMemoryMap, 5, &mem_map_size, mem_map, &mem_map_key, &desc_size, &desc_ver);
        uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, mem_map_size, (VOID**)&mem_map);
        mem_map_size += sizeof(EFI_MEMORY_DESCRIPTOR) + sizeof(VOID*);
        status = uefi_call_wrapper(BS->GetMemoryMap, 5, &mem_map_size, mem_map, &mem_map_key, &desc_size, &desc_ver);
    }
    error_checker(gop, status);
    
    // Start kernel
    BOOT_INFO boot_info = {
        {
            gop->Mode->FrameBufferBase,
            gop->Mode->FrameBufferSize,
            gop->Mode->Info->HorizontalResolution,
            gop->Mode->Info->VerticalResolution,
            gop->Mode->Info->PixelsPerScanLine
        },
        {
            (VOID*)mem_map,
            mem_map_size,
            desc_size
        }
    };
    void (*kernel_start)(void*) = ((__attribute__((sysv_abi)) void (*)(void*)) kernel_entry);
    
    uefi_call_wrapper(BS->ExitBootServices, 2, ImageHandle, mem_map_key);
    kernel_start(&boot_info);
  
    return EFI_SUCCESS;
}