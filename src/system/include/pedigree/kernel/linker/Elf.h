/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef KERNEL_LINKER_ELF_H
#define KERNEL_LINKER_ELF_H

#ifdef IN_PEDIGREE_KERNEL
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/new"
#endif

#include "pedigree/kernel/linker/SymbolTable.h"
#include "pedigree/kernel/utilities/List.h"

/** @addtogroup kernellinker
 * @{ */

#ifdef VERBOSE_LINKER
#define DEBUG NOTICE
#else
#define DEBUG(...)
#endif

// Object file types
#define ET_NONE 0x0
#define ET_REL 0x1
#define ET_EXEC 0x2
#define ET_DYN 0x3
#define ET_CORE 0x4

// Section header types - common to Elf32 and Elf64.
#define SHT_PROGBITS 0x1  // The data is contained in the program file.
#define SHT_SYMTAB 0x2    // Symbol table
#define SHT_STRTAB 0x3    // String table
#define SHT_RELA 0x4
#define SHT_HASH 0x5     // Symbol hash table
#define SHT_DYNAMIC 0x6  // Dynamic linking information
#define SHT_NOTE 0x7
#define SHT_NOBITS 0x8  // The data is not contained in the program file.
#define SHT_REL 0x9
#define SHT_DYNSYM 0xb
#define SHT_INIT_ARRAY 0xe
#define SHT_FINI_ARRAY 0xf
#define SHT_PREINIT_ARRAY 0x10

// Section header flags - common to Elf32 and Elf64.
#define SHF_WRITE 0x1
#define SHF_ALLOC 0x2
#define SHF_EXECINSTR 0x4
#define SHF_MASKPROC 0xf0000000

// Program header flags - common to Elf32 and Elf64.
#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

// Process header flags - common to Elf32 and Elf64.
#define PT_NULL 0    /* Program header table entry unused */
#define PT_LOAD 1    /* Loadable program segment */
#define PT_DYNAMIC 2 /* Dynamic linking information */
#define PT_INTERP 3  /* Program interpreter */
#define PT_NOTE 4    /* Auxiliary information */
#define PT_SHLIB 5   /* Reserved */
#define PT_PHDR 6    /* Entry for header table itself */
#define PT_TLS 7     /* Thread-local storage segment */
#define PT_NUM 8     /* Number of defined types */

// Dynamic table flags - common to Elf32 and Elf64.
#define DT_NULL 0             /* Marks end of dynamic section */
#define DT_NEEDED 1           /* Name of needed library */
#define DT_PLTRELSZ 2         /* Size in bytes of PLT relocs */
#define DT_PLTGOT 3           /* Processor defined value */
#define DT_HASH 4             /* Address of symbol hash table */
#define DT_STRTAB 5           /* Address of string table */
#define DT_SYMTAB 6           /* Address of symbol table */
#define DT_RELA 7             /* Address of Rela relocs */
#define DT_RELASZ 8           /* Total size of Rela relocs */
#define DT_RELAENT 9          /* Size of one Rela reloc */
#define DT_STRSZ 10           /* Size of string table */
#define DT_SYMENT 11          /* Size of one symbol table entry */
#define DT_INIT 12            /* Address of init function */
#define DT_FINI 13            /* Address of termination function */
#define DT_SONAME 14          /* Name of shared object */
#define DT_RPATH 15           /* Library search path (deprecated) */
#define DT_SYMBOLIC 16        /* Start symbol search here */
#define DT_REL 17             /* Address of Rel relocs */
#define DT_RELSZ 18           /* Total size of Rel relocs */
#define DT_RELENT 19          /* Size of one Rel reloc */
#define DT_PLTREL 20          /* Type of reloc in PLT */
#define DT_DEBUG 21           /* For debugging; unspecified */
#define DT_TEXTREL 22         /* Reloc might modify .text */
#define DT_JMPREL 23          /* Address of PLT relocs */
#define DT_BIND_NOW 24        /* Process relocations of object */
#define DT_INIT_ARRAY 25      /* Array with addresses of init fct */
#define DT_FINI_ARRAY 26      /* Array with addresses of fini fct */
#define DT_INIT_ARRAYSZ 27    /* Size in bytes of DT_INIT_ARRAY */
#define DT_FINI_ARRAYSZ 28    /* Size in bytes of DT_FINI_ARRAY */
#define DT_RUNPATH 29         /* Library search path */
#define DT_FLAGS 30           /* Flags for the object being loaded */
#define DT_ENCODING 32        /* Start of encoded range */
#define DT_PREINIT_ARRAY 32   /* Array with addresses of preinit fct*/
#define DT_PREINIT_ARRAYSZ 33 /* size in bytes of DT_PREINIT_ARRAY */

// Symbol types
#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_FUNC 2
#define STT_SECTION 3
#define STT_FILE 4
#define STT_COMMON 5
#define STT_TLS 6

// Symbol bindings
#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2

// Symbol visibilities
#define STV_DEFAULT 0
#define STV_INTERNAL 1
#define STV_HIDDEN 2
#define STV_PROTECTED 3

#if BITS_32

#define R_SYM(val) ((val) >> 8)
#define R_TYPE(val) ((val) &0xff)

#define ST_BIND(i) ((i) >> 4)
#define ST_TYPE(i) ((i) &0xf)
#define ST_INFO(b, t) (((b) << 4) + ((t) &0xf))

typedef uint32_t Elf_Addr;
typedef uint32_t Elf_Off;
typedef uint16_t Elf_Half;
typedef uint32_t Elf_Word;
typedef int32_t Elf_Sword;

// We define the Xword and Sxword types for ELF32 even though they don't exist
// in the spec for forwards compatibility with ELF64.
typedef uint32_t Elf_Xword;
typedef int32_t Elf_Sxword;

#elif BITS_64

#define R_SYM(val) ((val) >> 32)
#define R_TYPE(val) ((val) &0xffffffffUL)

#define ST_BIND(i) ((i) >> 4)
#define ST_TYPE(i) ((i) &0xf)
#define ST_INFO(b, t) (((b) << 4) + ((t) &0xf))

typedef uint64_t Elf_Addr;
typedef uint64_t Elf_Off;
typedef uint16_t Elf_Half;
typedef uint32_t Elf_Word;
typedef int32_t Elf_Sword;
typedef uint64_t Elf_Xword;
typedef int64_t Elf_Sxword;

// Compatibility types for 64-bit kernel, which is actually a 32-bit ELF.
typedef uint32_t Elf32_Addr;
typedef uint32_t Elf32_Off;
typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Word;
typedef int32_t Elf32_Sword;
typedef uint32_t Elf32_Xword;
typedef int32_t Elf32_Sxword;

#endif

// Is the symbol type OK to add to the symbol table?
#define ST_TYPEOK(x) (ST_TYPE((x)) <= STT_FUNC)

#ifndef _NO_ELF_CLASS

/**
 * Provides an implementation of a 32-bit Executable and Linker format file
 * parser. The ELF data can be loaded either by supplying an entire ELF file in
 * a buffer, or by supplying details of each section seperately.
 */
class EXPORTED_PUBLIC Elf
{
    // PosixSubsystem can use memory mapped files to do its own (very basic)
    // ELF loading, which is an improvement on load()'s copies.
    friend class PosixSubsystem;

  protected:
    // Forward declaration of ELF symbol type for lookupSymbol template.
    struct ElfSymbol_t;

  public:
    /** Default constructor - loads no data. */
    Elf();

    /** Destructor.*/
    virtual ~Elf();

    /** The copy-constructor */
    Elf(const Elf &);

    /** Validates the ELF object at the given location. */
    bool validate(uint8_t *pBuffer, size_t length);

    /** Constructs an Elf object, and assumes the given pointer to be
     * to a contiguous region of memory containing an ELF object. */
    bool create(uint8_t *pBuffer, size_t length);

    /** Merely loads "needed libraries" and then returns success or
     *  failure. */
    bool createNeededOnly(uint8_t *pBuffer, size_t length);

    /** Maps memory at a specified address, loads code there and applies
     * relocations only for the .modinfo section. Intended use is for loading
     * kernel modules only, there is no provision for dynamic relocations. */
    bool loadModule(
        uint8_t *pBuffer, size_t length, uintptr_t &loadBase, size_t &loadSize,
        SymbolTable *pSymbolTableCopy = 0);

    /** Finalises a module - applies all relocations except those in the
     * .modinfo section. At this point it is assumed that all of this module's
     * dependencies have been loaded.
     *
     * The load base must be given again for reentrancy reasons. */
    bool finaliseModule(uint8_t *pBuffer, uintptr_t length);

    /** Performs the prerequisite allocation for any normal ELF file - library
     * or executable. For a library, this allocates loadBase, and allocates
     * memory for the entire object - this is not filled however. \note If
     * bAllocate is false, the memory will NOT be allocated. */
    bool allocate(
        uint8_t *pBuffer, size_t length, uintptr_t &loadBase,
        SymbolTable *pSymtab = 0, bool bAllocate = true, size_t *pSize = 0);

    /** Loads (part) of a 'normal' file. This could be an executable or a
     * library. By default the entire file is loaded (memory copied and
     * relocated) but this can be changed using the nStart and nEnd parameters.
     * This allows for lazy loading. \note PLT relocations are not performed
     * here - they are defined in a different section to the standard REL and
     * RELA entries, so must be done specifically (via applySpecificRelocation).
     */
    bool load(
        uint8_t *pBuffer, size_t length, uintptr_t loadBase,
        SymbolTable *pSymtab = 0, uintptr_t nStart = 0, uintptr_t nEnd = ~0,
        bool relocate = true);

    /** Extracts only the entry point from an ELF file at the given buffer. */
    static bool
    extractEntryPoint(uint8_t *pBuffer, size_t length, uintptr_t &entry);

    /** Extracts information about the ELF file at the given buffer. */
    static bool extractInformation(
        uint8_t *pBuffer, size_t length, size_t &phdrCount,
        size_t &phdrEntrySize, uintptr_t &phdrAddress);

    /** Returns a list of required libraries before this object will load. */
    List<char *> &neededLibraries();

    /** Returns the name of the interpreter set aside for this program, or an
     * empty string. */
    String &getInterpreter();

    /** Returns the virtual address of the last byte to be written. Used to
     * calculate the sbrk memory breakpoint. */
    uintptr_t getLastAddress();

    uintptr_t getInitFunc()
    {
        return m_InitFunc;
    }
    uintptr_t getFiniFunc()
    {
        return m_FiniFunc;
    }

    /** Returns the name of the symbol which contains 'addr', and also the
     * starting address of that symbol in 'startAddr' if startAddr != 0.
     * \param[in] addr The address to look up.
     * \param[out] startAddr The starting address of the found symbol
     * (optional). \return The symbol name, as a C string. */
    template <class T = ElfSymbol_t>
    const char *
    lookupSymbol(uintptr_t addr, uintptr_t *startAddr, T *symbolTable);

    /** Default implementation which just uses the normal internal symbol table.
     */
    const char *lookupSymbol(uintptr_t addr, uintptr_t *startAddr);

    /** Returns the start address of the symbol with name 'pName'. */
    uintptr_t lookupSymbol(const char *pName);

    /** Same as lookupSymbol, but acts on the dynamic symbol table instead of
     * the normal one. */
    uintptr_t lookupDynamicSymbolAddress(const char *str, uintptr_t loadBase);

    /** Applies the n'th relocation in the relocation table. Used by PLT
     * entries. */
    uintptr_t applySpecificRelocation(
        uintptr_t off, SymbolTable *pSymtab, uintptr_t loadBase,
        SymbolTable::Policy policy = SymbolTable::LocalFirst);

    /** Gets the address of the global offset table.
     * \return Address of the GOT, or 0 if none was found. */
    uintptr_t getGlobalOffsetTable();

    /** Returns the size of the Procedure Linkage table. */
    size_t getPltSize();

    /**  Adds all the symbols in this Elf into the given symbol table, adjusted
     * by loadBase.
     *
     * \param pSymtab  Symbol table to populate.
     * \param loadBase Offset to adjust each value by. */
    void populateSymbolTable(SymbolTable *pSymtab, uintptr_t loadBase);

    /** Preallocate space for the symbols in this ELF in the symbol table. */
    void preallocateSymbols(
        SymbolTable *pSymtabOverride = nullptr,
        SymbolTable *pAdditionalSymtab = nullptr);

    SymbolTable *getSymbolTable()
    {
        return &m_SymbolTable;
    }

    /** Returns the entry point of the file. */
    uintptr_t getEntryPoint();

    uintptr_t debugFrameTable();
    uintptr_t debugFrameTableLength();

    /** Sets a friendly name for debugging. */
    void setName(const String &s)
    {
        m_Name = s;
    }

    /** Gets the friendly name. */
    const String &getName() const
    {
        return m_Name;
    }

  protected:
#endif
    struct ElfHeader_t
    {
        uint8_t ident[16];
        Elf_Half type;
        Elf_Half machine;
        Elf_Word version;
        Elf_Addr entry;
        Elf_Off phoff;
        Elf_Off shoff;
        Elf_Word flags;
        Elf_Half ehsize;
        Elf_Half phentsize;
        Elf_Half phnum;
        Elf_Half shentsize;
        Elf_Half shnum;
        Elf_Half shstrndx;
    } PACKED;

    struct ElfProgramHeader_t
    {
        Elf_Word type;
#if BITS_64
        Elf_Word flags;
#endif
        Elf_Off offset;
        Elf_Addr vaddr;
        Elf_Addr paddr;
        Elf_Xword filesz;
        Elf_Xword memsz;
#if !BITS_64
        Elf_Word flags;
#endif
        Elf_Xword align;
    } PACKED;

    struct ElfSectionHeader_t
    {
        Elf_Word name;
        Elf_Word type;
        Elf_Xword flags;
        Elf_Addr addr;
        Elf_Off offset;
        Elf_Xword size;
        Elf_Word link;
        Elf_Word info;
        Elf_Xword addralign;
        Elf_Xword entsize;
    } PACKED;

#if BITS_64
    struct Elf32SectionHeader_t
    {
        Elf32_Word name;
        Elf32_Word type;
        Elf32_Xword flags;
        Elf32_Addr addr;
        Elf32_Off offset;
        Elf32_Xword size;
        Elf32_Word link;
        Elf32_Word info;
        Elf32_Xword addralign;
        Elf32_Xword entsize;
    } PACKED;
#else
    typedef ElfSectionHeader_t Elf32SectionHeader_t;
#endif

    struct ElfSymbol_t
    {
        Elf_Word name;
#if BITS_64
        uint8_t info;
        uint8_t other;
        Elf_Half shndx;
#endif
        Elf_Addr value;
        Elf_Xword size;
#if !BITS_64
        uint8_t info;
        uint8_t other;
        Elf_Half shndx;
#endif
    } PACKED;

#if BITS_64
    struct Elf32Symbol_t
    {
        Elf32_Word name;
        Elf32_Addr value;
        Elf32_Xword size;
        uint8_t info;
        uint8_t other;
        Elf32_Half shndx;
    } PACKED;
#else
typedef ElfSymbol_t Elf32Symbol_t;
#endif

    struct ElfHash_t
    {
        Elf_Word nbucket;
        Elf_Word nchain;
        // buckets follow
        // chains follow
    };

    struct ElfDyn_t
    {
        Elf_Sxword tag;
        union
        {
            Elf_Xword val;
            Elf_Addr ptr;
        } un;
    } PACKED;

    struct ElfRel_t
    {
        Elf_Addr offset;
        Elf_Xword info;
    } PACKED;

    struct ElfRela_t
    {
        Elf_Addr offset;
        Elf_Xword info;
        Elf_Sxword addend;
    } PACKED;

#ifndef _NO_ELF_CLASS
  private:
    template <typename T>
    static T *elfCopy(uint8_t *, ElfProgramHeader_t *, size_t, T *, size_t);

    bool relocate(uint8_t *pBuffer, uintptr_t length);
    bool relocateModinfo(uint8_t *pBuffer, uintptr_t length);

    /**
     * Applies one relocation. This overload performs a relocation without
     * addend (REL). \param rel The relocation entry to apply. \param pSh A
     * pointer to the section that the relocation entry refers to. \param
     * pSymtab The symbol table to use for lookups. \param loadBase For a
     * relocatable object, the address at which it is loaded. \param policy
     * Lookup policy. \note Defined in core/processor/.../Elf.cc
     */
    bool applyRelocation(
        ElfRel_t rel, ElfSectionHeader_t *pSh, SymbolTable *pSymtab = 0,
        uintptr_t loadBase = 0,
        SymbolTable::Policy policy = SymbolTable::LocalFirst);

    /**
     * Applies one relocation. This overload performs a relocation with addend
     * (RELA). \param rel The relocation entry to apply. \param pSh A pointer to
     * the section that the relocation entry refers to. \param pSymtab The
     * symbol table to use for lookups. \param loadBase For a relocatable
     * object, the address at which it is loaded. \param policy Lookup policy.
     * \note Defined in core/processor/.../Elf.cc
     */
    bool applyRelocation(
        ElfRela_t rela, ElfSectionHeader_t *pSh, SymbolTable *pSymtab = 0,
        uintptr_t loadBase = 0,
        SymbolTable::Policy policy = SymbolTable::LocalFirst);

    /** Rebase all dynamic section pointers to the m_LoadBase value. */
    void rebaseDynamic();

  protected:
    ElfSymbol_t *m_pSymbolTable;
    size_t m_nSymbolTableSize;
    char *m_pStringTable;
    size_t m_nStringTableSize;
    char *m_pShstrtab;
    size_t m_nShstrtabSize;
    uintptr_t *m_pGotTable;   // Global offset table.
    ElfRel_t *m_pRelTable;    // Dynamic REL relocations.
    ElfRela_t *m_pRelaTable;  // Dynamic RELA relocations.
    size_t m_nRelTableSize;
    size_t m_nRelaTableSize;
    ElfRel_t *m_pPltRelTable;
    ElfRela_t *m_pPltRelaTable;
    bool m_bUsesRela;  // If PltRelaTable is valid, else PltRelTable is.
    uint32_t *m_pDebugTable;
    size_t m_nDebugTableSize;
    ElfSymbol_t *m_pDynamicSymbolTable;
    size_t m_nDynamicSymbolTableSize;
    char *m_pDynamicStringTable;
    size_t m_nDynamicStringTableSize;
    ElfSectionHeader_t *m_pSectionHeaders;
    size_t m_nSectionHeaders;
    ElfProgramHeader_t *m_pProgramHeaders;
    size_t m_nProgramHeaders;
    size_t m_nPltSize;
    uintptr_t m_nEntry;
    List<char *> m_NeededLibraries;
    SymbolTable m_SymbolTable;
    uintptr_t m_InitFunc;
    uintptr_t m_FiniFunc;
    String m_sInterpreter;

    String m_Name;
    uintptr_t m_LoadBase;

  private:
    /** The assignment operator
     *\note currently not implemented */
    Elf &operator=(const Elf &);
};

/** External specializations for ELF symbol types. */
extern template const char *Elf::lookupSymbol<Elf::ElfSymbol_t>(
    uintptr_t addr, uintptr_t *startAddr = 0, ElfSymbol_t *symbolTable = 0);
#if BITS_64
extern template const char *Elf::lookupSymbol<Elf::Elf32Symbol_t>(
    uintptr_t addr, uintptr_t *startAddr = 0, Elf32Symbol_t *symbolTable = 0);
#endif

#endif

/** @} */

#endif
