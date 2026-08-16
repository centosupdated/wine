/*
 * Wine debugger - minidump handling
 *
 * Copyright 2005 Eric Pouech
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "debugger.h"
#include "wingdi.h"
#include "winnt.h"
#include "winuser.h"
#include "tlhelp32.h"
#include "wine/debug.h"
#include "wine/exception.h"

WINE_DEFAULT_DEBUG_CHANNEL(winedbg);

static struct be_process_io be_process_minidump_io;

/* we need this function on 32bit hosts to ensure we zero out the higher DWORD
 * stored in the minidump file (sometimes it's not cleared, or the conversion from
 * 32bit to 64bit wide integers is done as signed, which is wrong)
 * So we clamp on 32bit CPUs (as stored in minidump information) all addresses to
 * keep only the lower 32 bits.
 * FIXME: as of today, since we don't support a backend CPU which is different from
 * CPU this process is running on, casting to (DWORD_PTR) will do just fine.
 */
static inline DWORD64  get_addr64(DWORD64 addr)
{
    return (DWORD_PTR)addr;
}

void minidump_write(const char* file, const EXCEPTION_RECORD* rec)
{
    HANDLE                              hFile;
    MINIDUMP_EXCEPTION_INFORMATION      mei;
    EXCEPTION_POINTERS                  ep;

#ifdef __x86_64__
    if (dbg_curr_process->be_cpu->machine != IMAGE_FILE_MACHINE_AMD64)
    {
        FIXME("Cannot write minidump for 32-bit process using 64-bit winedbg\n");
        return;
    }
#endif

    hFile = CreateFileA(file, GENERIC_READ|GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);

    if (hFile == INVALID_HANDLE_VALUE) return;

    if (rec)
    {
        mei.ThreadId = dbg_curr_thread->tid;
        mei.ExceptionPointers = &ep;
        ep.ExceptionRecord = (EXCEPTION_RECORD*)rec;
        ep.ContextRecord = &dbg_context.ctx;
        mei.ClientPointers = FALSE;
    }
    MiniDumpWriteDump(dbg_curr_process->handle, dbg_curr_process->pid,
                      hFile, MiniDumpNormal/*|MiniDumpWithDataSegs*/,
                      rec ? &mei : NULL, NULL, NULL);
    CloseHandle(hFile);
}

#define Wine_ElfModuleListStream        0xFFF0

struct tgt_process_minidump_data
{
    void*       mapping;
    HANDLE      hFile;
    HANDLE      hMap;
};

static inline struct tgt_process_minidump_data* private_data(struct dbg_process* pcs)
{
    return pcs->pio_data;
}

static BOOL tgt_process_minidump_read(HANDLE hProcess, const void* addr,
                                      void* buffer, SIZE_T len, SIZE_T* rlen)
{
    void*               stream;

    if (!private_data(dbg_curr_process)->mapping) return FALSE;
    if (MiniDumpReadDumpStream(private_data(dbg_curr_process)->mapping,
                               MemoryListStream, NULL, &stream, NULL))
    {
        MINIDUMP_MEMORY_LIST*   mml = stream;
        MINIDUMP_MEMORY_DESCRIPTOR* mmd = mml->MemoryRanges;
        int                     i, found = -1;
        SIZE_T                  ilen, prev_len = 0;

        /* There's no reason that memory ranges inside a minidump do not overlap.
         * So be smart when looking for a given memory range (either grab a
         * range that covers the whole requested area, or if none, the range that
         * has the largest overlap with requested area)
         */
        for (i = 0; i < mml->NumberOfMemoryRanges; i++, mmd++)
        {
            if (get_addr64(mmd->StartOfMemoryRange) <= (DWORD_PTR)addr &&
                (DWORD_PTR)addr < get_addr64(mmd->StartOfMemoryRange) + mmd->Memory.DataSize)
            {
                ilen = min(len,
                           get_addr64(mmd->StartOfMemoryRange) + mmd->Memory.DataSize - (DWORD_PTR)addr);
                if (ilen == len) /* whole range is matched */
                {
                    found = i;
                    prev_len = ilen;
                    break;
                }
                if (found == -1 || ilen > prev_len) /* partial match, keep largest one */
                {
                    found = i;
                    prev_len = ilen;
                }
            }
        }
        if (found != -1)
        {
            mmd = &mml->MemoryRanges[found];
            memcpy(buffer,
                   (char*)private_data(dbg_curr_process)->mapping + mmd->Memory.Rva + (DWORD_PTR)addr - get_addr64(mmd->StartOfMemoryRange),
                   prev_len);
            if (rlen) *rlen = prev_len;
            return TRUE;
        }
    }
    /* The memory isn't present in minidump. Try to fetch read-only area from PE image. */
    {
        IMAGEHLP_MODULEW64 im = {.SizeOfStruct = sizeof(im)};

        if (SymGetModuleInfoW64(dbg_curr_process->handle, (DWORD_PTR)addr, &im))
        {
            WCHAR *image_name;
            HANDLE file, map = 0;
            void *pe_mapping = NULL;
            BOOL found = FALSE;
            const IMAGE_NT_HEADERS *nthdr = NULL;

            image_name = im.LoadedImageName[0] ? im.LoadedImageName : im.ImageName;
            if ((file = CreateFileW(image_name, GENERIC_READ, FILE_SHARE_READ, NULL,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)) != INVALID_HANDLE_VALUE &&
                ((map = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL)) != 0) &&
                ((pe_mapping = MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0)) != NULL) &&
                (nthdr = RtlImageNtHeader(pe_mapping)) != NULL)
            {
                DWORD_PTR rva = (DWORD_PTR)addr - im.BaseOfImage;
                ptrdiff_t size_hdr = (const BYTE*)(IMAGE_FIRST_SECTION(nthdr) + nthdr->FileHeader.NumberOfSections) - (const BYTE*)pe_mapping;

                /* in the PE header ? */
                if (rva < size_hdr)
                {
                    if (rva + len > size_hdr)
                        len = size_hdr - rva;
                    memcpy(buffer, (const BYTE*)pe_mapping + rva, len);
                    if (rlen) *rlen = len;
                    found = TRUE;
                }
                else /* in read only section ? */
                {
                    /* Note: RtlImageRvaToSection checks RVA against raw size, so we won't
                     * get section when rva falls into the (raw size, virtual size( interval.
                     */
                    const IMAGE_SECTION_HEADER *section = RtlImageRvaToSection(nthdr, NULL, rva);
                    if (section && !(section->Characteristics & IMAGE_SCN_MEM_WRITE))
                    {
                        DWORD_PTR offset = rva - section->VirtualAddress;
                        DWORD nw = len;

                        if (offset + nw > section->SizeOfRawData)
                            nw = section->SizeOfRawData - offset;
                        memcpy(buffer, (const char*)pe_mapping + section->PointerToRawData + offset, nw);
                        if (nw < len) /* fill with O? */
                        {
                            if (offset + len > section->Misc.VirtualSize)
                                len = section->Misc.VirtualSize - offset;
                            memset((char*)buffer + nw, 0, len - nw);
                            nw = len;
                        }
                        if (rlen) *rlen = nw;
                        found = TRUE;
                    }
                }
            }
            if (pe_mapping) UnmapViewOfFile(pe_mapping);
            if (map) CloseHandle(map);
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            if (found) return TRUE;
        }
    }

    /* FIXME: this is a dirty hack to let the last frame in a bt to work
     * However, we need to check who's to blame, this code or the current 
     * dbghelp!StackWalk implementation
     */
    if ((DWORD_PTR)addr < 32)
    {
        memset(buffer, 0, len); 
        if (rlen) *rlen = len;
        return TRUE;
    }
    return FALSE;
}

static BOOL tgt_process_minidump_write(HANDLE hProcess, void* addr,
                                       const void* buffer, SIZE_T len, SIZE_T* wlen)
{
    return FALSE;
}

static BOOL CALLBACK validate_file(PCWSTR name, void* user)
{
    return FALSE; /* get the first file we find !! */
}

static BOOL is_pe_module_embedded(struct tgt_process_minidump_data* data,
                                  MINIDUMP_MODULE* pe_mm)
{
    MINIDUMP_MODULE_LIST*       mml;

    if (MiniDumpReadDumpStream(data->mapping, Wine_ElfModuleListStream, NULL,
                               (void**)&mml, NULL))
    {
        MINIDUMP_MODULE*        mm;
        unsigned                i;

        for (i = 0, mm = mml->Modules; i < mml->NumberOfModules; i++, mm++)
        {
            if (get_addr64(mm->BaseOfImage) <= get_addr64(pe_mm->BaseOfImage) &&
                get_addr64(mm->BaseOfImage) + mm->SizeOfImage >= get_addr64(pe_mm->BaseOfImage) + pe_mm->SizeOfImage)
                return TRUE;
        }
    }
    return FALSE;
}

static BOOL copy_context(dbg_ctx_t *ctx, struct tgt_process_minidump_data* data,
                         const MINIDUMP_LOCATION_DESCRIPTOR *loc_desc)
{
    BOOL ret = TRUE;
    unsigned len = loc_desc->DataSize;

    if (len > sizeof(*ctx))
    {
        ERR("Incoming context size is larger than internal structure\n");
        len = sizeof(*ctx);
        ret = FALSE;
    }
    memcpy(ctx, (char*)data->mapping + loc_desc->Rva, len);
    if (len < sizeof(*ctx))
        memset((char*)ctx + len, 0, sizeof(*ctx) - len);
    return ret;
}

static enum dbg_start minidump_do_reload(struct tgt_process_minidump_data* data)
{
    void*                       stream;
    DWORD                       pid = 1; /* by default */
    HANDLE                      hProc = (HANDLE)0x900DBAAD;
    int                         i;
    MINIDUMP_MODULE_LIST*       mml;
    MINIDUMP_MODULE*            mm;
    MINIDUMP_STRING*            mds;
    MINIDUMP_DIRECTORY*         dir;
    WCHAR                       exec_name[1024];
    WCHAR                       nameW[1024];
    unsigned                    len;

    /* fetch PID */
    if (MiniDumpReadDumpStream(data->mapping, MiscInfoStream, NULL, &stream, NULL))
    {
        MINIDUMP_MISC_INFO* mmi = stream;
        if (mmi->Flags1 & MINIDUMP_MISC1_PROCESS_ID)
            pid = mmi->ProcessId;
    }

    /* fetch executable name (it's normally the first one in module list) */
    lstrcpyW(exec_name, L"<minidump-exec>");

    if (MiniDumpReadDumpStream(data->mapping, ModuleListStream, NULL, &stream, NULL))
    {
        mml = stream;
        if (mml->NumberOfModules)
        {
            WCHAR*      ptr;

            mm = mml->Modules;
            mds = (MINIDUMP_STRING*)((char*)data->mapping + mm->ModuleNameRva);
            len = mds->Length / 2;
            memcpy(exec_name, mds->Buffer, mds->Length);
            exec_name[len] = 0;
            for (ptr = exec_name + len - 1; ptr >= exec_name; ptr--)
            {
                if (*ptr == '/' || *ptr == '\\')
                {
                    memmove(exec_name, ptr + 1, (lstrlenW(ptr + 1) + 1) * sizeof(WCHAR));
                    break;
                }
            }
        }
    }

    if (MiniDumpReadDumpStream(data->mapping, SystemInfoStream, &dir, &stream, NULL))
    {
        MINIDUMP_SYSTEM_INFO *msi = stream;

        dbg_printf("WineDbg starting minidump on pid %04lx\n", pid);
        dbg_printf("  %ls was running on #%d CPU%s\n",
                   exec_name, msi->NumberOfProcessors,
                   msi->NumberOfProcessors < 2 ? "" : "s");

        if (msi->ProcessorArchitecture == IMAGE_FILE_MACHINE_UNKNOWN
#ifdef __x86_64__
            || msi->ProcessorArchitecture == IMAGE_FILE_MACHINE_I386
#endif
            )
        {
            dbg_printf("Cannot reload this minidump because of incompatible/unsupported machine %x\n",msi->ProcessorArchitecture);
            return FALSE;
        }
    }

    dbg_curr_process = dbg_add_process(&be_process_minidump_io, pid, hProc);
    dbg_curr_pid = pid;
    dbg_curr_process->pio_data = data;
    dbg_set_process_name(dbg_curr_process, exec_name);
    info_win32_system(FALSE);

    dbg_init(hProc, NULL, FALSE);

    if (MiniDumpReadDumpStream(data->mapping, ThreadListStream, NULL, &stream, NULL))
    {
        MINIDUMP_THREAD_LIST*   mtl = stream;
        ULONG                   i;

        for (i = 0; i < mtl->NumberOfThreads; i++)
        {
            dbg_add_thread(dbg_curr_process, mtl->Threads[i].ThreadId, NULL,
                           (void*)(DWORD_PTR)get_addr64(mtl->Threads[i].Teb));
        }
    }
    /* first load ELF modules, then do the PE ones */
    if (MiniDumpReadDumpStream(data->mapping, Wine_ElfModuleListStream, NULL,
                               &stream, NULL))
    {
        WCHAR   buffer[MAX_PATH];

        mml = stream;
        for (i = 0, mm = mml->Modules; i < mml->NumberOfModules; i++, mm++)
        {
            mds = (MINIDUMP_STRING*)((char*)data->mapping + mm->ModuleNameRva);
            memcpy(nameW, mds->Buffer, mds->Length);
            nameW[mds->Length / sizeof(WCHAR)] = 0;
            if (SymFindFileInPathW(hProc, NULL, nameW, (void*)(DWORD_PTR)mm->CheckSum,
                                   0, 0, SSRVOPT_DWORD, buffer, validate_file, NULL))
                dbg_load_module(hProc, NULL, buffer, get_addr64(mm->BaseOfImage),
                                 mm->SizeOfImage);
            else
                SymLoadModuleExW(hProc, NULL, nameW, NULL, get_addr64(mm->BaseOfImage),
                                 mm->SizeOfImage, NULL, 0);
        }
    }
    if (MiniDumpReadDumpStream(data->mapping, ModuleListStream, NULL, &stream, NULL))
    {
        WCHAR   buffer[MAX_PATH];

        mml = stream;
        for (i = 0, mm = mml->Modules; i < mml->NumberOfModules; i++, mm++)
        {
            mds = (MINIDUMP_STRING*)((char*)data->mapping + mm->ModuleNameRva);
            memcpy(nameW, mds->Buffer, mds->Length);
            nameW[mds->Length / sizeof(WCHAR)] = 0;
            if (SymFindFileInPathW(hProc, NULL, nameW, (void*)(DWORD_PTR)mm->TimeDateStamp,
                                   mm->SizeOfImage, 0, SSRVOPT_DWORD, buffer, validate_file, NULL))
                dbg_load_module(hProc, NULL, buffer, get_addr64(mm->BaseOfImage),
                                 mm->SizeOfImage);
            else if (is_pe_module_embedded(data, mm))
                dbg_load_module(hProc, NULL, nameW, get_addr64(mm->BaseOfImage),
                                 mm->SizeOfImage);
            else
                SymLoadModuleExW(hProc, NULL, nameW, NULL, get_addr64(mm->BaseOfImage),
                                 mm->SizeOfImage, NULL, 0);
        }
    }
    if (MiniDumpReadDumpStream(data->mapping, ExceptionStream, NULL, &stream, NULL))
    {
        MINIDUMP_EXCEPTION_STREAM*      mes = stream;

        if ((dbg_curr_thread = dbg_get_thread(dbg_curr_process, mes->ThreadId)))
        {
            ADDRESS64   addr;

            dbg_curr_tid = mes->ThreadId;
            dbg_curr_thread->in_exception = TRUE;
            dbg_curr_thread->excpt_record.ExceptionCode = mes->ExceptionRecord.ExceptionCode;
            dbg_curr_thread->excpt_record.ExceptionFlags = mes->ExceptionRecord.ExceptionFlags;
            dbg_curr_thread->excpt_record.ExceptionRecord = (void*)(DWORD_PTR)get_addr64(mes->ExceptionRecord.ExceptionRecord);
            dbg_curr_thread->excpt_record.ExceptionAddress = (void*)(DWORD_PTR)get_addr64(mes->ExceptionRecord.ExceptionAddress);
            dbg_curr_thread->excpt_record.NumberParameters = mes->ExceptionRecord.NumberParameters;
            for (i = 0; i < dbg_curr_thread->excpt_record.NumberParameters; i++)
            {
                dbg_curr_thread->excpt_record.ExceptionInformation[i] = mes->ExceptionRecord.ExceptionInformation[i];
            }
            copy_context(&dbg_context, data, &mes->ThreadContext);
            memory_get_current_pc(&addr);
            stack_fetch_frames(&dbg_context);
            dbg_curr_process->be_cpu->print_context(dbg_curr_thread->handle, &dbg_context, 0);
            stack_info(-1);
            dbg_curr_process->be_cpu->print_segment_info(dbg_curr_thread->handle, &dbg_context);
            stack_backtrace(mes->ThreadId);
            source_list_from_addr(&addr, 0);
        }
    }
    return start_ok;
}

static void cleanup(struct tgt_process_minidump_data* data)
{
    if (data->mapping)                          UnmapViewOfFile(data->mapping);
    if (data->hMap)                             CloseHandle(data->hMap);
    if (data->hFile != INVALID_HANDLE_VALUE)    CloseHandle(data->hFile);
    free(data);
}

static struct be_process_io be_process_minidump_io;

enum dbg_start minidump_reload(const char* filename)
{
    struct tgt_process_minidump_data*   data;
    enum dbg_start                      ret = start_error_parse;

    if (dbg_curr_process)
    {
        dbg_printf("Already attached to a process. Use 'detach' or 'kill' before loading a minidump file'\n");
        return start_error_init;
    }
    data = malloc(sizeof(struct tgt_process_minidump_data));
    if (!data) return start_error_init;
    data->mapping = NULL;
    data->hMap    = NULL;
    data->hFile   = INVALID_HANDLE_VALUE;

    if ((data->hFile = CreateFileA(filename, GENERIC_READ, FILE_SHARE_READ, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)) != INVALID_HANDLE_VALUE &&
        ((data->hMap = CreateFileMappingA(data->hFile, NULL, PAGE_READONLY, 0, 0, NULL)) != 0) &&
        ((data->mapping = MapViewOfFile(data->hMap, FILE_MAP_READ, 0, 0, 0)) != NULL))
    {
        __TRY
        {
            if (((MINIDUMP_HEADER*)data->mapping)->Signature == MINIDUMP_SIGNATURE)
            {
                ret = minidump_do_reload(data);
            }
        }
        __EXCEPT_PAGE_FAULT
        {
            dbg_printf("Unexpected fault while reading minidump %s\n", filename);
            dbg_curr_pid = 0;
        }
        __ENDTRY;
    }
    if (ret != start_ok) cleanup(data);
    return ret;
}

enum dbg_start minidump_start(int argc, char* argv[])
{
    /* try the form <myself> minidump-file */
    if (argc != 1) return start_error_parse;

    WINE_TRACE("Processing Minidump file %s\n", argv[0]);

    return minidump_reload(argv[0]);
}

static BOOL tgt_process_minidump_close_process(struct dbg_process* pcs, BOOL kill)
{
    struct tgt_process_minidump_data*    data = private_data(pcs);

    cleanup(data);
    pcs->pio_data = NULL;
    SymCleanup(pcs->handle);
    dbg_del_process(pcs);
    return TRUE;
}

static BOOL tgt_process_minidump_get_selector(HANDLE hThread, DWORD sel, LDT_ENTRY* le)
{
    /* so far, pretend all selectors are valid, and mapped to a 32bit flat address space */
    memset(le, 0, sizeof(*le));
    le->HighWord.Bits.Default_Big = 1;
    return TRUE;
}

static BOOL tgt_process_minidump_fetch_thread_name(const struct dbg_thread *thread, WCHAR **description)
{
    struct tgt_process_minidump_data *data = private_data(thread->process);
    void *stream;

    if (MiniDumpReadDumpStream(data->mapping, ThreadNamesStream, NULL, &stream, NULL))
    {
        MINIDUMP_THREAD_NAME_LIST* mtnl = stream;
        ULONG i;

        for (i = 0; i < mtnl->NumberOfThreadNames; i++)
        {
            if (thread->tid == mtnl->ThreadNames[i].ThreadId)
            {
                MINIDUMP_STRING *mdmp_string = (MINIDUMP_STRING *)((char*)data->mapping + mtnl->ThreadNames[i].RvaOfThreadName);
                WCHAR *ret;
                if (!mdmp_string->Length) return FALSE;
                if (!(ret = malloc(mdmp_string->Length + sizeof(WCHAR)))) return FALSE;
                memcpy(ret, mdmp_string->Buffer, mdmp_string->Length);
                ret[mdmp_string->Length / sizeof(WCHAR)] = L'\0';
                *description = ret;
                return TRUE;
            }
        }
    }
    return FALSE;
}

static BOOL tgt_process_minidump_fetch_thread_context(const struct dbg_thread* thread, dbg_ctx_t *ctx)
{
    struct tgt_process_minidump_data *data = private_data(thread->process);
    void *stream;

    if (MiniDumpReadDumpStream(data->mapping, ThreadListStream, NULL, &stream, NULL))
    {
        MINIDUMP_THREAD_LIST*   mtl = stream;
        ULONG                   i;

        for (i = 0; i < mtl->NumberOfThreads; i++)
        {
            if (thread->tid == mtl->Threads[i].ThreadId)
            {
                copy_context(ctx, data, &mtl->Threads[i].ThreadContext);
                return TRUE;
            }
        }
    }
    return FALSE;
}

static BOOL tgt_process_minidump_fetch_system_info(struct dbg_process *pcs, struct dbg_system_info *sysinfo)
{
    struct tgt_process_minidump_data *data = private_data(pcs);
    MINIDUMP_DIRECTORY *dir;
    MINIDUMP_SYSTEM_INFO *msi;

    if (!MiniDumpReadDumpStream(data->mapping, SystemInfoStream, &dir, (void**)&msi, NULL))
        return FALSE;

    memset(sysinfo, 0, sizeof(*sysinfo));

    switch (msi->ProcessorArchitecture)
    {
    default:
    case PROCESSOR_ARCHITECTURE_UNKNOWN: sysinfo->current_machine = IMAGE_FILE_MACHINE_UNKNOWN; break;
    case PROCESSOR_ARCHITECTURE_INTEL:   sysinfo->current_machine = IMAGE_FILE_MACHINE_I386; break;
    case PROCESSOR_ARCHITECTURE_AMD64:   sysinfo->current_machine = IMAGE_FILE_MACHINE_AMD64; break;
    case PROCESSOR_ARCHITECTURE_ARM:     sysinfo->current_machine = IMAGE_FILE_MACHINE_ARM; break;
    case PROCESSOR_ARCHITECTURE_ARM64:   sysinfo->current_machine = IMAGE_FILE_MACHINE_ARM64; break;
    }
    /* until Wow64 is supported */
    sysinfo->native_machine = sysinfo->current_machine;


    if (sizeof(MINIDUMP_SYSTEM_INFO) + 4 > dir->Location.DataSize &&
        msi->CSDVersionRva >= dir->Location.Rva + sizeof(MINIDUMP_SYSTEM_INFO) + 4)
    {
        const char* code = (const char*)msi + sizeof(MINIDUMP_SYSTEM_INFO);

        if (code[0] == 'W' && code[1] == 'I' && code[2] == 'N' && code[3] == 'E')
        {
            const DWORD* wes = (const DWORD*)(code += 4);
            if (wes[0] >= 3)
            {
                sysinfo->wine_build_id = code + wes[1];
                sysinfo->host_system = code + wes[2];
                sysinfo->host_version = code + wes[3];
            }
        }
    }
    {
        static char windows_version[64];
        snprintf(windows_version, ARRAY_SIZE(windows_version),
                 "Windows Version %d.%d", msi->MajorVersion, msi->MinorVersion);
        sysinfo->windows_version = windows_version;
    }
    sysinfo->guest_machines[0] = IMAGE_FILE_MACHINE_UNKNOWN;

    return TRUE;
}

static struct be_process_io be_process_minidump_io =
{
    tgt_process_minidump_close_process,
    tgt_process_minidump_read,
    tgt_process_minidump_write,
    tgt_process_minidump_get_selector,
    tgt_process_minidump_fetch_thread_name,
    tgt_process_minidump_fetch_thread_context,
    tgt_process_minidump_fetch_system_info,
};
