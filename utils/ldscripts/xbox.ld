/* KallistiOS ##version##

   utils/ldscripts/xbox.ld
   Copyright (C) 2026 Andress Barajas
   Copyright (C) 2026 Cypress

   GNU ld linker script for the original Microsoft Xbox.

   This is the elf32-i386 counterpart of the Dreamcast's shlelf.xc and follows
   its section ordering and idioms wherever the two targets agree, so the two
   scripts can be read side by side. The output is both the artifact the
   network loader uploads and the input to the clean-room ELF->XBE converter
   (elf2xbe). environ_xbox.sh wires it in the same way the Dreamcast
   environment selects its architecture linker script:

       export KOS_LD_SCRIPT="-T${KOS_BASE}/utils/ldscripts/xbox.ld"

   Where this deliberately differs from shlelf.xc:

     - Sections are aligned to 0x20 rather than a page or shlelf.xc's 128.
       environ_xbox.sh passes -z max-page-size=0x20 so ld does not insert
       page-sized gaps between PT_LOADs; elf2xbe derives the native XBE base
       from (first PT_LOAD page - 0x1000), which leaves exactly the one header
       page it reserves.
     - .sub0-.sub9, .ocram and .monitors are Dreamcast hardware features and
       have no Xbox equivalent. .sdata/.sbss/.sdata2/.sbss2 are the SH
       small-data ABI, which i386 does not have.
     - .xbethnk, .stack and .heap below are Xbox-specific; see their comments.
     - .note.gnu.property is discarded. SH never emits it; x86 does, and
       nothing here consumes it.

   Unlike PE, ELF does not prepend an underscore to C identifiers, so the
   object-level names defined here are spelled exactly as C spells them. This
   is why several symbols carry one fewer underscore than in shlelf.xc, where
   sh-elf's leading underscore supplies the difference:
       _bss_start, _bss_end   BSS bounds for arch_main() to zero
       end                    first free byte; base of the KOS heap
       _tdata_start, _tdata_size, _tbss_size,
       _tdata_align, _tbss_align                static TLS geometry
       __EH_FRAME_BEGIN__     start of the DWARF frame list

   Companion pieces are arch/xbox/kernel/startup.S and arch/xbox/kernel/init.c.
*/

OUTPUT_FORMAT("elf32-i386", "elf32-i386", "elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(_start)
STARTUP(_kos_startup.o)
INPUT(crti.o)
INPUT(crtbegin.o)
INPUT(crtend.o)
INPUT(crtn.o)
/* First guest VA above the loader window. Keep the default in sync with
   XBOX_DEFAULT_LOAD_ADDR in kos-tool's mk/memory.mk. Overridable exactly the
   way the Dreamcast overrides its own load address, via
   -Wl,--defsym=LOAD_OFFSET=... */
LOAD_OFFSET = DEFINED(LOAD_OFFSET) ? LOAD_OFFSET : 0x0003c000 ;
SEG_ALIGN = 0x20 ;

SECTIONS
{
  /* Read-only sections, merged into text segment: */
  PROVIDE (__executable_start = LOAD_OFFSET);
  PROVIDE (_executable_start = LOAD_OFFSET);  /* KOS: C "extern char _executable_start" */
  . = LOAD_OFFSET;
  .text           :
  {
    *(.text .stub .text.* .gnu.linkonce.t.*)
    /* .gnu.warning sections are handled specially by elf32.em.  */
    *(.gnu.warning)
  } =0

  .init           :
  {
    KEEP (*(SORT_NONE(.init)))
  } =0
  .fini           :
  {
    KEEP (*(SORT_NONE(.fini)))
  } =0
  .interp         : { *(.interp) }
  .note.gnu.build-id : { *(.note.gnu.build-id) }
  .hash           : { *(.hash) }
  .gnu.hash       : { *(.gnu.hash) }
  .dynsym         : { *(.dynsym) }
  .dynstr         : { *(.dynstr) }
  .gnu.version    : { *(.gnu.version) }
  .gnu.version_d  : { *(.gnu.version_d) }
  .gnu.version_r  : { *(.gnu.version_r) }
  .rel.dyn        :
    {
      *(.rel.init)
      *(.rel.text .rel.text.* .rel.gnu.linkonce.t.*)
      *(.rel.fini)
      *(.rel.rodata .rel.rodata.* .rel.gnu.linkonce.r.*)
      *(.rel.data.rel.ro* .rel.gnu.linkonce.d.rel.ro.*)
      *(.rel.data .rel.data.* .rel.gnu.linkonce.d.*)
      *(.rel.tdata .rel.tdata.* .rel.gnu.linkonce.td.*)
      *(.rel.tbss .rel.tbss.* .rel.gnu.linkonce.tb.*)
      *(.rel.ctors)
      *(.rel.dtors)
      *(.rel.got)
      *(.rel.bss .rel.bss.* .rel.gnu.linkonce.b.*)
    }
  .rela.dyn       :
    {
      *(.rela.init)
      *(.rela.text .rela.text.* .rela.gnu.linkonce.t.*)
      *(.rela.fini)
      *(.rela.rodata .rela.rodata.* .rela.gnu.linkonce.r.*)
      *(.rela.data .rela.data.* .rela.gnu.linkonce.d.*)
      *(.rela.tdata .rela.tdata.* .rela.gnu.linkonce.td.*)
      *(.rela.tbss .rela.tbss.* .rela.gnu.linkonce.tb.*)
      *(.rela.ctors)
      *(.rela.dtors)
      *(.rela.got)
      *(.rela.bss .rela.bss.* .rela.gnu.linkonce.b.*)
    }
  .rel.plt        : { *(.rel.plt) }
  .rela.plt       : { *(.rela.plt) }
  .plt            : { *(.plt) }
  PROVIDE (__etext = .);
  PROVIDE (_etext = .);   /* KOS: C "extern char _etext" */
  PROVIDE (etext = .);
  .rodata         :
  {
    *(.rodata .rodata.* .gnu.linkonce.r.*)
    /* Static TLS alignments, stored as data words because ALIGNOF() is only
       usable at script-evaluation time. arch_tls_setup_data() reads the
       *values* here, but the *addresses* of the size symbols below. */
    . = ALIGN(4);
    _tdata_align = .;
    LONG (ALIGNOF(.tdata));
    . = ALIGN(4);
    _tbss_align = .;
    LONG (ALIGNOF(.tbss));
    . = ALIGN(4);
  }
  .rodata1        : { *(.rodata1) }
  .eh_frame_hdr : { *(.eh_frame_hdr) }
  .eh_frame       : ONLY_IF_RO {
    KEEP (*crtbegin.o(.eh_frame))
    KEEP (*(EXCLUDE_FILE (*crtend.o) .eh_frame))
    KEEP (*crtend.o(.eh_frame))
  }
  .gcc_except_table   : ONLY_IF_RO { *(.gcc_except_table .gcc_except_table.*) }
  /* Adjust the address for the data segment. */
  . = ALIGN(SEG_ALIGN);
  /* Exception handling. crtbegin.o defines __EH_FRAME_BEGIN__ and its
     frame_dummy registers the table from _init(), so the arch startup does
     not call __register_frame() by hand. crtend.o supplies the zero
     terminator that ends the scan.  */
  .eh_frame       : ONLY_IF_RW {
    KEEP (*crtbegin.o(.eh_frame))
    KEEP (*(EXCLUDE_FILE (*crtend.o) .eh_frame))
    KEEP (*crtend.o(.eh_frame))
  }
  .gcc_except_table   : ONLY_IF_RW { *(.gcc_except_table .gcc_except_table.*) }
  /* Thread Local Storage sections. These two are what give the image its
     PT_TLS program header; the linker's @ntpoff values are computed from
     exactly this layout, and arch_tls_setup_data() must reproduce it.  */
  .tdata	  :
  {
    _tdata_start = .;
    *(.tdata .tdata.* .gnu.linkonce.td.*)
  }
  _tdata_size = SIZEOF(.tdata);
  .tbss	(NOLOAD)	  :
  {
    *(.tbss .tbss.* .gnu.linkonce.tb.*)
    *(.tcommon)
  }
  _tbss_size = SIZEOF(.tbss);
  /* GCC emits .ctors rather than .init_array for this target
     (HAVE_INITFINI_ARRAY_SUPPORT is 0), so these three are normally empty.
     They are kept -- with KEEP, or --gc-sections would silently drop them --
     so that a library built against a toolchain which does emit them still
     gets its constructors run instead of losing them without a diagnostic. */
  .preinit_array     :
  {
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  }
  .init_array     :
  {
     PROVIDE_HIDDEN (__init_array_start = .);
     KEEP (*(SORT(.init_array.*)))
     KEEP (*(.init_array))
     PROVIDE_HIDDEN (__init_array_end = .);
  }
  .fini_array     :
  {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(.fini_array))
    KEEP (*(SORT(.fini_array.*)))
    PROVIDE_HIDDEN (__fini_array_end = .);
  }
  .ctors          :
  {
    ___ctors = .;
    /* gcc uses crtbegin.o to find the start of
       the constructors, so we make sure it is
       first.  Because this is a wildcard, it
       doesn't matter if the user does not
       actually link against crtbegin.o; the
       linker won't look for a file to match a
       wildcard.  The wildcard also means that it
       doesn't matter which directory crtbegin.o
       is in.  */
    KEEP (*crtbegin.o(.ctors))
    KEEP (*crtbegin?.o(.ctors))
    /* We don't want to include the .ctor section from
       the crtend.o file until after the sorted ctors.
       The .ctor section from the crtend file contains the
       end of ctors marker and it must be last */
    KEEP (*(EXCLUDE_FILE (*crtend.o *crtend?.o ) .ctors))
    KEEP (*(SORT(.ctors.*)))
    KEEP (*(.ctors))
    ___ctors_end = .;
  }
  .dtors          :
  {
    ___dtors = .;
    KEEP (*crtbegin.o(.dtors))
    KEEP (*crtbegin?.o(.dtors))
    KEEP (*(EXCLUDE_FILE (*crtend.o *crtend?.o ) .dtors))
    KEEP (*(SORT(.dtors.*)))
    KEEP (*(.dtors))
    ___dtors_end = .;
  }
  .jcr            : { KEEP (*(.jcr)) }
  .data.rel.ro : { *(.data.rel.ro.local* .gnu.linkonce.d.rel.ro.local.*) *(.data.rel.ro* .gnu.linkonce.d.rel.ro.*) }
  .dynamic        : { *(.dynamic) }
  .data           :
  {
    __data_start__ = .;
    *(.data .data.* .gnu.linkonce.d.*)
    SORT(CONSTRUCTORS)
    __data_end__ = .;
  }
  .data1          : { *(.data1) }
  .got            : { *(.got.plt) *(.got) }
  /* Standard writable XBE kernel-thunk table. elf2xbe looks this section up
     by name and points the generated XBE header at it, so the Xbox kernel can
     resolve the ordinals before entry. Loader-hosted ELF execution neither
     patches nor uses it.  */
  . = ALIGN(SEG_ALIGN);
  .xbethnk        : { KEEP (*(.xbethnk)) }
  _edata = .; PROVIDE (edata = .);
  . = ALIGN(SEG_ALIGN);
  _bss_start = .;         /* KOS: C "extern uint8_t _bss_start[]" */
  PROVIDE (__bss_start = .);
  .bss            (NOLOAD) :
  {
   *(.dynbss)
   *(.bss .bss.* .gnu.linkonce.b.*)
   *(COMMON)
   /* Align here to ensure that the .bss section occupies space up to
      _end.  Align after .bss to ensure correct alignment even if the
      .bss section disappears because there are no input sections.  */
   . = ALIGN(. != 0 ? 32 / 8 : 1);
  }
  . = ALIGN(32 / 8);
  _bss_end = .;           /* KOS: C "extern uint8_t _bss_end[]"   */
  /*
     Bootstrap stack storage is deliberately outside .bss: arch_main() clears
     the BSS while executing on this stack. startup.S marks the input section
     @nobits, so this reserves virtual address space without storing 256 KiB
     of zeroes in the ELF (and therefore in the XBE).

     Keep this before `end` so the KOS heap begins after the reserved stack.
  */
  . = ALIGN(SEG_ALIGN);
  .stack          (NOLOAD) :
  {
    __stack_start__ = .;
    KEEP (*(.stack))
    KEEP (*(.stack.*))
    __stack_end__ = .;
  }
  /* First free byte after the whole image: base of the KOS heap. Defined
     last, so it sits past every loadable section rather than overlapping.  */
  . = ALIGN(SEG_ALIGN);
  _end = .; PROVIDE (end = .);   /* KOS: C "extern uint8_t end[]" -> heap base */
  __end__ = .;
  __heap_reserve_size__ = DEFINED(__xbox_native_heap_top)
                            ? __xbox_native_heap_top - .
                            : 0;
  /*
     A directly loaded XBE receives mappings only for ranges described by its
     section table. Native links define __xbox_native_heap_top so the XBE
     contains a writable, fileless reservation covering KOS's heap. Loader
     ELF links omit the symbol and this output section stays empty, because
     xbox-load-ip already owns and maps its bounded guest arena.

     Keep `end` above at the real image end: mm_init() must start allocation
     at the beginning of this reserved range, not after it.
  */
  .heap           (NOLOAD) :
  {
    __heap_reserve_start__ = .;
    . += __heap_reserve_size__;
  }
  _heap_reserve_end__ = .;   /* KOS: C "extern uint8_t _heap_reserve_end__[]" */
  /* Stabs debugging sections.  */
  .stab          0 : { *(.stab) }
  .stabstr       0 : { *(.stabstr) }
  .stab.excl     0 : { *(.stab.excl) }
  .stab.exclstr  0 : { *(.stab.exclstr) }
  .stab.index    0 : { *(.stab.index) }
  .stab.indexstr 0 : { *(.stab.indexstr) }
  .comment       0 : { *(.comment) }
  /* DWARF debug sections.
     Symbols in the DWARF debugging sections are relative to the beginning
     of the section so we begin them at 0.  */
  /* DWARF 1 */
  .debug          0 : { *(.debug) }
  .line           0 : { *(.line) }
  /* GNU DWARF 1 extensions */
  .debug_srcinfo  0 : { *(.debug_srcinfo) }
  .debug_sfnames  0 : { *(.debug_sfnames) }
  /* DWARF 1.1 and DWARF 2 */
  .debug_aranges  0 : { *(.debug_aranges) }
  .debug_pubnames 0 : { *(.debug_pubnames) }
  /* DWARF 2 */
  .debug_info     0 : { *(.debug_info .gnu.linkonce.wi.*) }
  .debug_abbrev   0 : { *(.debug_abbrev) }
  .debug_line     0 : { *(.debug_line) }
  .debug_frame    0 : { *(.debug_frame) }
  .debug_str      0 : { *(.debug_str) }
  .debug_loc      0 : { *(.debug_loc) }
  .debug_macinfo  0 : { *(.debug_macinfo) }
  /* SGI/MIPS DWARF 2 extensions */
  .debug_weaknames 0 : { *(.debug_weaknames) }
  .debug_funcnames 0 : { *(.debug_funcnames) }
  .debug_typenames 0 : { *(.debug_typenames) }
  .debug_varnames  0 : { *(.debug_varnames) }
  /* DWARF 3 */
  .debug_pubtypes 0 : { *(.debug_pubtypes) }
  .debug_ranges   0 : { *(.debug_ranges) }
  .gnu.attributes 0 : { KEEP (*(.gnu.attributes)) }
  /DISCARD/ : { *(.note.GNU-stack) *(.gnu_debuglink) *(.note.gnu.property) *(.gnu.lto_*) }
}
