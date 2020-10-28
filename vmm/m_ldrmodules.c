// m_ldrmodules.c : implementation of the ldrmodules built-in module.
//
// (c) Ulf Frisk, 2018-2020
// Author: Ulf Frisk, pcileech@frizk.net
//
#include "pluginmanager.h"
#include "vmm.h"
#include "vmmwin.h"
#include "util.h"
#include "pe.h"

#define LDRMODULES_CACHE_TP_EAT             1
#define LDRMODULES_CACHE_TP_IAT             2
#define LDRMODULES_NUM_CACHE                8
#define LDRMODULES_LINELENGTH_X86           104ULL
#define LDRMODULES_LINELENGTH_X64           123ULL
#define LDRMODULES_LINELENGTH_DIRECTORIES   54ULL
#define LDRMODULES_LINELENGTH_SECTIONS      70ULL

#define LDRMODULES_MAX_IATEAT               0x10000

typedef struct tdOBLDRMODULES_CACHE_ENTRY {
    OB ObHdr;
    DWORD dwHash;
    DWORD tp;
    DWORD cb;
    BYTE pb[];
} OBLDRMODULES_CACHE_ENTRY, *POBLDRMODULES_CACHE_ENTRY;

/*
* Dynamically generate the file \<modulename>\directories
*/
_Success_(return == 0)
NTSTATUS LdrModules_ReadFile_Directories(_In_ PVMM_PROCESS pProcess, _In_ QWORD vaModuleBase, _Out_ PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbRead, _In_ QWORD cbOffset)
{
    NTSTATUS nt;
    LPSTR sz;
    QWORD i, o = 0, cbMax, cStart, cEnd, cbLINELENGTH;
    IMAGE_DATA_DIRECTORY Directory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
    if(!PE_DirectoryGetAll(pProcess, vaModuleBase, NULL, Directory)) { return VMMDLL_STATUS_FILE_INVALID; }
    cbLINELENGTH = LDRMODULES_LINELENGTH_DIRECTORIES;
    cStart = (DWORD)(cbOffset / cbLINELENGTH);
    cEnd = (DWORD)min(16 - 1, (cb + cbOffset + cbLINELENGTH - 1) / cbLINELENGTH);
    cbMax = 1 + (1 + cEnd - cStart) * cbLINELENGTH;
    if(cStart > 16) { return VMMDLL_STATUS_END_OF_FILE; }
    if(!(sz = LocalAlloc(LMEM_ZEROINIT, cbMax))) { return VMMDLL_STATUS_FILE_INVALID; }
    for(i = cStart; i <= min(15, cEnd); i++) {
        o += Util_snwprintf_u8ln(
            sz + o,
            cbLINELENGTH,
            L"%x %-16.16S %016llx %08x %08x",
            (DWORD)i,
            PE_DATA_DIRECTORIES[i],
            vaModuleBase + Directory[i].VirtualAddress,
            Directory[i].VirtualAddress,
            Directory[i].Size
        );
    }
    nt = Util_VfsReadFile_FromPBYTE(sz, cbMax - 1, pb, cb, pcbRead, cbOffset - cStart * cbLINELENGTH);
    LocalFree(sz);
    return nt;
}

/*
* Dynamically generate the file \<modulename>\sections
*/
_Success_(return == 0)
NTSTATUS LdrModules_ReadFile_Sections(_In_ PVMM_PROCESS pProcess, _In_ QWORD vaModuleBase, _Out_ PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbRead, _In_ QWORD cbOffset)
{
    NTSTATUS nt = VMMDLL_STATUS_FILE_INVALID;
    LPSTR sz;
    QWORD i, o = 0, cbMax, cStart, cEnd, cbLINELENGTH;
    DWORD cSections;
    PIMAGE_SECTION_HEADER pSections = NULL;
    cSections = PE_SectionGetNumberOf(pProcess, vaModuleBase);
    cbLINELENGTH = LDRMODULES_LINELENGTH_SECTIONS;
    cStart = (DWORD)(cbOffset / cbLINELENGTH);
    cEnd = (DWORD)min(cSections - 1, (cb + cbOffset + cbLINELENGTH - 1) / cbLINELENGTH);
    cbMax = 1 + (1 + cEnd - cStart) * cbLINELENGTH;
    if(!cSections || (cStart > cSections)) { return VMMDLL_STATUS_END_OF_FILE; }
    if(!(sz = LocalAlloc(LMEM_ZEROINIT, cbMax))) { goto fail; }
    if(!(pSections = LocalAlloc(LMEM_ZEROINIT, cSections * sizeof(IMAGE_SECTION_HEADER)))) { goto fail; }
    if(!PE_SectionGetAll(pProcess, vaModuleBase, cSections, pSections)) { goto fail; }
    for(i = cStart; i <= cEnd; i++) {
        o += Util_snwprintf_u8ln(
            sz + o,
            cbLINELENGTH,
            L"%02x %-8.8S  %016llx %08x %08x %c%c%c %08x %08x",
            (DWORD)i,
            pSections[i].Name,
            vaModuleBase + pSections[i].VirtualAddress,
            pSections[i].VirtualAddress,
            pSections[i].Misc.VirtualSize,
            (pSections[i].Characteristics & IMAGE_SCN_MEM_READ) ? 'r' : '-',
            (pSections[i].Characteristics & IMAGE_SCN_MEM_WRITE) ? 'w' : '-',
            (pSections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) ? 'x' : '-',
            pSections[i].PointerToRawData,
            pSections[i].SizeOfRawData
        );
    }
    nt = Util_VfsReadFile_FromPBYTE(sz, cbMax - 1, pb, cb, pcbRead, cbOffset - cStart * cbLINELENGTH);
fail:
    LocalFree(pSections);
    LocalFree(sz);
    return nt;
}

/*
* Dynamically generate the file \modules.txt.
* -- pModuleMap
* -- pb
* -- cb
* -- pcbRead
* -- cbOffset
* -- return
*/
_Success_(return == 0)
NTSTATUS LdrModules_ReadModulesFile(_In_ PVMM_PROCESS pProcess, _In_ PVMMOB_MAP_MODULE pModuleMap, _Out_ PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbRead, _In_ QWORD cbOffset)
{
    NTSTATUS nt;
    LPSTR sz;
    QWORD i, o = 0, cbMax, cStart, cEnd, cbLINELENGTH;
    PVMM_MAP_MODULEENTRY pModule;
    cbLINELENGTH = ctxVmm->f32 ? LDRMODULES_LINELENGTH_X86 : LDRMODULES_LINELENGTH_X64;
    cStart = (DWORD)(cbOffset / cbLINELENGTH);
    cEnd = (DWORD)min(pModuleMap->cMap - 1, (cb + cbOffset + cbLINELENGTH - 1) / cbLINELENGTH);
    cbMax = 1 + (1 + cEnd - cStart) * cbLINELENGTH;
    if(!pModuleMap->cMap || (cStart > pModuleMap->cMap)) { return VMMDLL_STATUS_END_OF_FILE; }
    if(!(sz = LocalAlloc(LMEM_ZEROINIT, cbMax))) { return VMMDLL_STATUS_FILE_INVALID; }
    for(i = cStart; i <= cEnd; i++) {
        pModule = pModuleMap->pMap + i;
        if(ctxVmm->f32) {
            o += Util_snwprintf_u8ln(
                sz + o,
                cbLINELENGTH,
                L"%04x%7i %8x %08x-%08x %s",
                (DWORD)i,
                pProcess->dwPID,
                pModule->cbImageSize >> 12,
                (DWORD)pModule->vaBase,
                (DWORD)(pModule->vaBase + pModule->cbImageSize - 1),
                pModule->wszText + pModule->cwszText - min(64, pModule->cwszText)
            );
        } else {
            o += Util_snwprintf_u8ln(
                sz + o,
                cbLINELENGTH,
                L"%04x%7i %8x %016llx-%016llx %s %s",
                (DWORD)i,
                pProcess->dwPID,
                pModule->cbImageSize >> 12,
                pModule->vaBase,
                pModule->vaBase + pModule->cbImageSize - 1,
                pModule->fWoW64 ? L"32" : L"  ",
                pModule->wszText + pModule->cwszText - min(64, pModule->cwszText)
            );
        }
    }
    nt = Util_VfsReadFile_FromPBYTE(sz, cbMax - 1, pb, cb, pcbRead, cbOffset - cStart * cbLINELENGTH);
    LocalFree(sz);
    return nt;
}

/*
* Retrieve a OBLDRMODULES_CACHE_ENTRY object for the Export Address Table (EAT).
* CALLER DECREF: return
* -- ctx
* -- pModule
* -- return
*/
POBLDRMODULES_CACHE_ENTRY LdrModule_GetEAT(_In_ PVMMDLL_PLUGIN_CONTEXT ctx, _In_ PVMM_MAP_MODULEENTRY pModule)
{
    DWORD i, o, cEATs = 0, cbLINELENGTH = 64;
    PVMMPROC_WINDOWS_EAT_ENTRY pEATs = NULL;
    POBLDRMODULES_CACHE_ENTRY pObCacheEntry = NULL;
    PVMM_PROCESS pProcess = (PVMM_PROCESS)ctx->pProcess;
    // 1: retrieve cache
    pObCacheEntry = ObContainer_GetOb(pProcess->Plugin.pObCLdrModulesDisplayCache);
    if(pObCacheEntry && (pObCacheEntry->tp == LDRMODULES_CACHE_TP_EAT) && (pObCacheEntry->dwHash == Util_HashStringUpperW(pModule->wszText))) {
        return pObCacheEntry;
    }
    Ob_DECREF(pObCacheEntry);
    pObCacheEntry = NULL;
    // 2: retrieve exported functions
    pEATs = LocalAlloc(0, LDRMODULES_MAX_IATEAT * sizeof(VMMPROC_WINDOWS_EAT_ENTRY));
    if(!pEATs) { goto fail; }
    VmmWin_PE_LoadEAT_DisplayBuffer(ctx->pProcess, pModule, pEATs, LDRMODULES_MAX_IATEAT, &cEATs);
    if(!cEATs) { goto fail; }
    // 3: fill "display buffer"
    pObCacheEntry = Ob_Alloc('PeEA', LMEM_ZEROINIT, sizeof(OBLDRMODULES_CACHE_ENTRY) + (QWORD)cEATs * cbLINELENGTH + 1, NULL, NULL);
    if(!pObCacheEntry) { goto fail; }
    pObCacheEntry->tp = LDRMODULES_CACHE_TP_EAT;
    pObCacheEntry->cb = cEATs * 64 + 1;
    pObCacheEntry->dwHash = Util_HashStringUpperW(pModule->wszText);
    for(i = 0, o = 0; i < cEATs; i++) {
        o += Util_snwprintf_u8ln(
            pObCacheEntry->pb + o,
            cbLINELENGTH,
            L"%04x %016llx %S",     // 64 bytes (chars) / line (function)
            (WORD)i,
            pModule->vaBase + pEATs[i].vaFunctionOffset,
            pEATs[i].szFunction
        );
    }
    pObCacheEntry->cb = o;
    LocalFree(pEATs);
    ObContainer_SetOb(pProcess->Plugin.pObCLdrModulesDisplayCache, pObCacheEntry);
    return pObCacheEntry;
fail:
    Ob_DECREF(pObCacheEntry);
    LocalFree(pEATs);
    return NULL;
}

/*
* Retrieve a OBLDRMODULES_CACHE_ENTRY object for the Import Address Table (IAT).
* CALLER DECREF: return
* -- ctx
* -- pModule
* -- return
*/
POBLDRMODULES_CACHE_ENTRY LdrModule_GetIAT(_In_ PVMMDLL_PLUGIN_CONTEXT ctx, _In_ PVMM_MAP_MODULEENTRY pModule)
{
    DWORD i, o, cIATs = 0, cbLINELENGTH = 128;
    PVMMWIN_IAT_ENTRY pIATs = NULL;
    POBLDRMODULES_CACHE_ENTRY pObCacheEntry = NULL;
    PVMM_PROCESS pProcess = (PVMM_PROCESS)ctx->pProcess;
    // 1: retrieve cache
    pObCacheEntry = ObContainer_GetOb(pProcess->Plugin.pObCLdrModulesDisplayCache);
    if(pObCacheEntry && (pObCacheEntry->tp == LDRMODULES_CACHE_TP_IAT) && (pObCacheEntry->dwHash == Util_HashStringUpperW(pModule->wszText))) {
        return pObCacheEntry;
    }
    Ob_DECREF(pObCacheEntry);
    pObCacheEntry = NULL;
    // 2: retrieve exported functions
    pIATs = LocalAlloc(0, LDRMODULES_MAX_IATEAT * sizeof(VMMWIN_IAT_ENTRY));
    if(!pIATs) { goto fail; }
    VmmWin_PE_LoadIAT_DisplayBuffer(ctx->pProcess, pModule, pIATs, LDRMODULES_MAX_IATEAT, &cIATs);
    if(!cIATs) { goto fail; }
    // 3: fill "display buffer"
    pObCacheEntry = Ob_Alloc('PeIA', LMEM_ZEROINIT, sizeof(OBLDRMODULES_CACHE_ENTRY) + (QWORD)cIATs * cbLINELENGTH + 1, NULL, NULL);
    if(!pObCacheEntry) { goto fail; }
    pObCacheEntry->tp = LDRMODULES_CACHE_TP_IAT;
    pObCacheEntry->cb = cIATs * 128 + 1;
    pObCacheEntry->dwHash = Util_HashStringUpperW(pModule->wszText);
    for(i = 0, o = 0; i < cIATs; i++) {
        o += Util_snwprintf_u8ln(
            pObCacheEntry->pb + o,
            cbLINELENGTH,
            L"%04x %016llx %-40.40S %S",     // 128 bytes (chars) / line (function)
            (WORD)i,
            pIATs[i].vaFunction,
            pIATs[i].szFunction,
            pIATs[i].szModule
        );
    }
    pObCacheEntry->cb = o;
    LocalFree(pIATs);
    ObContainer_SetOb(pProcess->Plugin.pObCLdrModulesDisplayCache, pObCacheEntry);
    return pObCacheEntry;
fail:
    Ob_DECREF(pObCacheEntry);
    LocalFree(pIATs);
    return NULL;
}

/*
* Helper write function - Write to the requested data directory file.
*/
VOID LdrModules_Write_DirectoriesD(_In_ PVMM_PROCESS pProcess, _In_ PVMM_MAP_MODULEENTRY pModule, _In_ LPWSTR wszDirectory, _In_reads_(cb) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbWrite, _In_ QWORD cbOffset)
{
    DWORD i;
    IMAGE_DATA_DIRECTORY Directory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
    *pcbWrite = 0;
    if(PE_DirectoryGetAll(pProcess, pModule->vaBase, NULL, Directory)) {
        for(i = 0; i < IMAGE_NUMBEROF_DIRECTORY_ENTRIES; i++) {
            if(!Util_wcsstrncmp((LPSTR)PE_DATA_DIRECTORIES[i], wszDirectory, 0)) {
                VmmWriteAsFile(pProcess, pModule->vaBase + Directory[i].VirtualAddress, Directory[i].Size, pb, cb, pcbWrite, cbOffset);
            }
        }
    }
}

/*
* Helper write function - Write to the requested section header file.
*/
VOID LdrModules_Write_SectionsD(_In_ PVMM_PROCESS pProcess, _In_ PVMM_MAP_MODULEENTRY pModule, _In_ LPWSTR wszSection, _In_reads_(cb) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbWrite, _In_ QWORD cbOffset)
{
    IMAGE_SECTION_HEADER SectionHeader;
    CHAR szSection[32];
    CHAR chDefault = '_';
    WideCharToMultiByte(CP_ACP, 0, wszSection, -1, szSection, sizeof(szSection), &chDefault, NULL);
    if(!PE_SectionGetFromName(pProcess, pModule->vaBase, szSection, &SectionHeader)) { *pcbWrite = 0;  return; }
    VmmWriteAsFile(pProcess, pModule->vaBase + SectionHeader.VirtualAddress, SectionHeader.Misc.VirtualSize, pb, cb, pcbWrite, cbOffset);
}

/*
* Write : function as specified by the module manager. The module manager will
* call into this callback function whenever a write shall occur from a "file".
* -- ctx
* -- pb
* -- cb
* -- pcbWrite
* -- cbOffset
* -- return
*/
NTSTATUS LdrModules_Write(_In_ PVMMDLL_PLUGIN_CONTEXT ctx, _In_reads_(cb) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbWrite, _In_ QWORD cbOffset)
{
    PVMM_MAP_MODULEENTRY pModule = NULL;
    PVMMOB_MAP_MODULE pObModuleMap = NULL;
    WCHAR wszModuleName[MAX_PATH];
    LPWSTR wszModuleSubPath;
    PVMM_PROCESS pProcess = (PVMM_PROCESS)ctx->pProcess;
    *pcbWrite = 0;
    wszModuleSubPath = Util_PathSplit2_ExWCHAR(ctx->wszPath, wszModuleName, _countof(wszModuleName));
    if(wszModuleName[0] && wszModuleSubPath[0] && VmmMap_GetModule(pProcess, &pObModuleMap) && (pModule = VmmMap_GetModuleEntry(pObModuleMap, wszModuleName))) {
        if(!_wcsicmp(wszModuleSubPath, L"pefile.dll")) {
            PE_FileRaw_Write(pProcess, pModule->vaBase, pb, cb, pcbWrite, (DWORD)cbOffset);
        }
        if(!_wcsnicmp(wszModuleSubPath, L"sectionsd\\", 10)) {
            LdrModules_Write_SectionsD(pProcess, pModule, wszModuleSubPath + 10, pb, cb, pcbWrite, cbOffset);
        }
        if(!_wcsnicmp(wszModuleSubPath, L"directoriesd\\", 13)) {
            LdrModules_Write_DirectoriesD(pProcess, pModule, wszModuleSubPath + 13, pb, cb, pcbWrite, cbOffset);
        }
    }
    Ob_DECREF(pObModuleMap);
    return VMM_STATUS_SUCCESS;
}

/*
* Helper read function - Read the requested data directory file.
*/
NTSTATUS LdrModules_Read_DirectoriesD(_In_ PVMM_PROCESS pProcess, _In_ PVMM_MAP_MODULEENTRY pModule, _In_ LPWSTR wszDirectory, _Out_writes_to_(cb, *pcbRead) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbRead, _In_ QWORD cbOffset)
{
    DWORD i;
    CHAR chDefault = '_';
    CHAR szDirectory[32];
    IMAGE_DATA_DIRECTORY Directory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
    if(PE_DirectoryGetAll(pProcess, pModule->vaBase, NULL, Directory)) {
        WideCharToMultiByte(CP_ACP, 0, wszDirectory, -1, szDirectory, sizeof(szDirectory), &chDefault, NULL);
        for(i = 0; i < 16; i++) {
            if(!strcmp(szDirectory, PE_DATA_DIRECTORIES[i])) {
                return VmmReadAsFile(pProcess, pModule->vaBase + Directory[i].VirtualAddress, Directory[i].Size, pb, cb, pcbRead, cbOffset);
            }
        }
    }
    return VMMDLL_STATUS_FILE_INVALID;
}

/*
* Helper read function - Read the requested section header file.
*/
NTSTATUS LdrModules_Read_SectionsD(_In_ PVMM_PROCESS pProcess, _In_ PVMM_MAP_MODULEENTRY pModule, _In_ LPWSTR wszSection, _Out_writes_to_(cb, *pcbRead) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbRead, _In_ QWORD cbOffset)
{
    CHAR szSection[32];
    IMAGE_SECTION_HEADER SectionHeader;
    CHAR chDefault = '_';
    WideCharToMultiByte(CP_ACP, 0, wszSection, -1, szSection, sizeof(szSection), &chDefault, NULL);
    if(!PE_SectionGetFromName(pProcess, pModule->vaBase, szSection, &SectionHeader)) { return VMMDLL_STATUS_FILE_INVALID; }
    return VmmReadAsFile(pProcess, pModule->vaBase + SectionHeader.VirtualAddress, SectionHeader.Misc.VirtualSize, pb, cb, pcbRead, cbOffset);
}

NTSTATUS LdrModules_Read_ModuleSubFile(_In_ PVMMDLL_PLUGIN_CONTEXT ctx, _In_ PVMM_MAP_MODULEENTRY pModule, _In_ LPWSTR wszPath, _Out_writes_to_(cb, *pcbRead) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbRead, _In_ QWORD cbOffset)
{
    NTSTATUS nt;
    POBLDRMODULES_CACHE_ENTRY pObCacheEntry = NULL;
    PVMM_PROCESS pProcess = (PVMM_PROCESS)ctx->pProcess;
    if(!_wcsicmp(wszPath, L"base")) {
        return Util_VfsReadFile_FromQWORD(pModule->vaBase, pb, cb, pcbRead, cbOffset, FALSE);
    }
    if(!_wcsicmp(wszPath, L"entry")) {
        return Util_VfsReadFile_FromQWORD(pModule->vaEntry, pb, cb, pcbRead, cbOffset, FALSE);
    }
    if(!_wcsicmp(wszPath, L"fullname.txt")) {
        return Util_VfsReadFile_FromTextWtoU8(pModule->wszFullName, pb, cb, pcbRead, cbOffset);
    }
    if(!_wcsicmp(wszPath, L"size")) {
        return Util_VfsReadFile_FromDWORD(pModule->cbImageSize, pb, cb, pcbRead, cbOffset, FALSE);
    }
    if(!_wcsicmp(wszPath, L"directories")) {
        return LdrModules_ReadFile_Directories(pProcess, pModule->vaBase, pb, cb, pcbRead, cbOffset);
    }
    if(!_wcsicmp(wszPath, L"export")) {
        pObCacheEntry = LdrModule_GetEAT(ctx, pModule);
        if(!pObCacheEntry) { return VMMDLL_STATUS_FILE_INVALID; }
        nt = Util_VfsReadFile_FromPBYTE(pObCacheEntry->pb, pObCacheEntry->cb, pb, cb, pcbRead, cbOffset);
        Ob_DECREF(pObCacheEntry);
        return nt;
    }
    if(!_wcsicmp(wszPath, L"import")) {
        pObCacheEntry = LdrModule_GetIAT(ctx, pModule);
        if(!pObCacheEntry) { return VMMDLL_STATUS_FILE_INVALID; }
        nt = Util_VfsReadFile_FromPBYTE(pObCacheEntry->pb, pObCacheEntry->cb, pb, cb, pcbRead, cbOffset);
        Ob_DECREF(pObCacheEntry);
        return nt;
    }
    if(!_wcsicmp(wszPath, L"pefile.dll")) {
        return PE_FileRaw_Read(pProcess, pModule->vaBase, pb, cb, pcbRead, (DWORD)cbOffset) ? VMMDLL_STATUS_SUCCESS : VMMDLL_STATUS_FILE_INVALID;
    }
    if(!_wcsicmp(wszPath, L"sections")) {
        return LdrModules_ReadFile_Sections(pProcess, pModule->vaBase, pb, cb, pcbRead, cbOffset);
    }
    if(!_wcsnicmp(wszPath, L"sectionsd\\", 10)) {
        return LdrModules_Read_SectionsD(pProcess, pModule, wszPath + 10, pb, cb, pcbRead, cbOffset);
    }
    if(!_wcsnicmp(wszPath, L"directoriesd\\", 13)) {
        return LdrModules_Read_DirectoriesD(pProcess, pModule, wszPath + 13, pb, cb, pcbRead, cbOffset);
    }
    return VMMDLL_STATUS_FILE_INVALID;
}

/*
* Read : function as specified by the module manager. The module manager will
* call into this callback function whenever a read shall occur from a "file".
* -- ctx
* -- pb
* -- cb
* -- pcbRead
* -- cbOffset
* -- return
*/
NTSTATUS LdrModules_Read(_In_ PVMMDLL_PLUGIN_CONTEXT ctx, _Out_writes_to_(cb, *pcbRead) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbRead, _In_ QWORD cbOffset)
{
    NTSTATUS nt = VMMDLL_STATUS_FILE_INVALID;
    WCHAR wszModuleName[MAX_PATH];
    LPWSTR wszModuleSubPath;
    PVMM_MAP_MODULEENTRY pModule = NULL;
    PVMMOB_MAP_MODULE pObModuleMap = NULL;
    if(!_wcsicmp(ctx->wszPath, L"modules.txt")) {
        if(VmmMap_GetModule((PVMM_PROCESS)ctx->pProcess, &pObModuleMap)) {
            nt = LdrModules_ReadModulesFile((PVMM_PROCESS)ctx->pProcess, pObModuleMap, pb, cb, pcbRead, cbOffset);
            Ob_DECREF(pObModuleMap);
        }
        return nt;
    }
    wszModuleSubPath = Util_PathSplit2_ExWCHAR(ctx->wszPath, wszModuleName, _countof(wszModuleName));
    *pcbRead = 0;
    if(wszModuleName[0] && wszModuleSubPath[0] && VmmMap_GetModule((PVMM_PROCESS)ctx->pProcess, &pObModuleMap) && (pModule = VmmMap_GetModuleEntry(pObModuleMap, wszModuleName))) {
        nt = LdrModules_Read_ModuleSubFile(ctx, pModule, wszModuleSubPath, pb, cb, pcbRead, cbOffset);
        Ob_DECREF(pObModuleMap);
        return nt;
    }
    Ob_DECREF(pObModuleMap);
    return VMMDLL_STATUS_FILE_INVALID;
}

/*
* List : function as specified by the module manager. The module manager will
* call into this callback function whenever a list directory shall occur from
* the given module.
* -- ctx
* -- pFileList
* -- return
*/
BOOL LdrModules_List(_In_ PVMMDLL_PLUGIN_CONTEXT ctx, _Inout_ PHANDLE pFileList)
{
    DWORD c, i, j;
    CHAR szSectionName[9] = { 0 };
    WCHAR wszSectionName[9];
    WCHAR wszPath1[MAX_PATH];
    LPWSTR wszPath2;
    PVMMOB_MAP_MODULE pObModuleMap = NULL;
    PVMM_MAP_MODULEENTRY pModule = NULL;
    PIMAGE_SECTION_HEADER pSections = NULL;
    IMAGE_DATA_DIRECTORY pDataDirectories[16];
    PVMM_PROCESS pProcess = (PVMM_PROCESS)ctx->pProcess;
    if(!VmmMap_GetModule(pProcess, &pObModuleMap)) { goto fail; }
    // modules root directory -> add directory per DLL
    if(!ctx->wszPath[0]) {
        for(i = 0; i < pObModuleMap->cMap; i++) {
            VMMDLL_VfsList_AddDirectory(pFileList, pObModuleMap->pMap[i].wszText, NULL);
        }
        VMMDLL_VfsList_AddFile(pFileList, L"modules.txt", pObModuleMap->cMap * (ctxVmm->f32 ? LDRMODULES_LINELENGTH_X86 : LDRMODULES_LINELENGTH_X64), NULL);
        goto success;
    }
    // individual module directory -> list files
    wszPath2 = Util_PathSplit2_ExWCHAR(ctx->wszPath, wszPath1, _countof(wszPath1));
    if(!(pModule = VmmMap_GetModuleEntry(pObModuleMap, wszPath1))) { goto fail; }
    // module-specific 'root' directory
    if(!wszPath2[0]) {
        VmmWin_PE_SetSizeSectionIATEAT_DisplayBuffer(ctx->pProcess, pModule);
        VMMDLL_VfsList_AddFile(pFileList, L"base", 16, NULL);
        VMMDLL_VfsList_AddFile(pFileList, L"entry", 16, NULL);
        VMMDLL_VfsList_AddFile(pFileList, L"fullname.txt",  wcslen_u8(pModule->wszFullName), NULL);
        VMMDLL_VfsList_AddFile(pFileList, L"size", 8, NULL);
        VMMDLL_VfsList_AddFile(pFileList, L"directories", IMAGE_NUMBEROF_DIRECTORY_ENTRIES * LDRMODULES_LINELENGTH_DIRECTORIES, NULL);
        VMMDLL_VfsList_AddFile(pFileList, L"export", pModule->cbDisplayBufferEAT, NULL);
        VMMDLL_VfsList_AddFile(pFileList, L"import", pModule->cbDisplayBufferIAT, NULL);
        VMMDLL_VfsList_AddFile(pFileList, L"sections", pModule->cSection * LDRMODULES_LINELENGTH_SECTIONS, NULL);
        VMMDLL_VfsList_AddFile(pFileList, L"pefile.dll", pModule->cbFileSizeRaw, NULL);
        VMMDLL_VfsList_AddDirectory(pFileList, L"sectionsd", NULL);
        VMMDLL_VfsList_AddDirectory(pFileList, L"directoriesd", NULL);
        goto success;
    }
    // module-specific 'sectiond' directory
    if(wszPath2[0] && !wcscmp(wszPath2, L"sectionsd")) {
        c = PE_SectionGetNumberOf(pProcess, pModule->vaBase);
        if(!(pSections = LocalAlloc(0, c * sizeof(IMAGE_SECTION_HEADER)))) { goto fail; }
        if(!PE_SectionGetAll(pProcess, pModule->vaBase, c, pSections)) { goto fail; }
        for(i = 0; i < c; i++) {
            if(pSections[i].Name[0]) {
                memcpy(szSectionName, pSections[i].Name, 8);
            } else {
                snprintf(szSectionName, 9, "%02x", i);
            }
            for(j = 0; j < 9; j++) {
                wszSectionName[j] = szSectionName[j];
            }
            VMMDLL_VfsList_AddFile(pFileList, wszSectionName, pSections[i].Misc.VirtualSize, NULL);
        }
        LocalFree(pSections);
        goto success;
    }
    // module-specific 'directoriesd' directory
    if(wszPath2[0] && !wcscmp(wszPath2, L"directoriesd")) {
        ZeroMemory(pDataDirectories, IMAGE_NUMBEROF_DIRECTORY_ENTRIES * sizeof(IMAGE_DATA_DIRECTORY));
        if(PE_DirectoryGetAll(pProcess, pModule->vaBase, NULL, pDataDirectories)) {
            for(i = 0; i < IMAGE_NUMBEROF_DIRECTORY_ENTRIES; i++) {
                VMMDLL_VfsList_AddFile(pFileList, (LPWSTR)PE_DATA_DIRECTORIESW[i], pDataDirectories[i].Size, NULL);
            }
        }
        goto success;
    }
fail:
    Ob_DECREF(pObModuleMap);
    return FALSE;
success:
    Ob_DECREF(pObModuleMap);
    return TRUE;
}

/*
* Initialization function. The module manager shall call into this function
* when the module shall be initialized. If the module wish to initialize it
* shall call the supplied pfnPluginManager_Register function.
* NB! the module does not have to register itself - for example if the target
* operating system or architecture is unsupported.
* -- pPluginRegInfo
*/
VOID M_LdrModules_Initialize(_Inout_ PVMMDLL_PLUGIN_REGINFO pRI)
{
    if((pRI->magic != VMMDLL_PLUGIN_REGINFO_MAGIC) || (pRI->wVersion != VMMDLL_PLUGIN_REGINFO_VERSION)) { return; }
    if((pRI->tpSystem != VMM_SYSTEM_WINDOWS_X64) && (pRI->tpSystem != VMM_SYSTEM_WINDOWS_X86)) { return; }
    wcscpy_s(pRI->reg_info.wszPathName, 128, L"\\modules");          // module name
    pRI->reg_info.fProcessModule = TRUE;                             // module shows in process directory
    pRI->reg_fn.pfnList = LdrModules_List;                           // List function supported
    pRI->reg_fn.pfnRead = LdrModules_Read;                           // Read function supported
    if(ctxMain->dev.fWritable) {
        pRI->reg_fn.pfnWrite = LdrModules_Write;                     // Write function supported
    }
    pRI->pfnPluginManager_Register(pRI);
}
