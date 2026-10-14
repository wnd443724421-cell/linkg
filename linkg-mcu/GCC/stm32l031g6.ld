/* STM32L031G6U6: 32 KiB Flash, 8 KiB SRAM, no bootloader offset. */
ENTRY(Reset_Handler)
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 32K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 8K
}
_estack = ORIGIN(RAM) + LENGTH(RAM);
_stack_size = 0x400;
SECTIONS
{
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
    } > FLASH
    .text :
    {
        . = ALIGN(4);
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH
    .ARM.extab : { *(.ARM.extab*) } > FLASH
    .ARM.exidx :
    {
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
    } > FLASH
    _sidata = LOADADDR(.data);
    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH
    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
        _end = .;
        end = .;
    } > RAM
    .stack _estack - _stack_size (NOLOAD) :
    {
        _sstack = .;
        . += _stack_size;
    } > RAM
    .ARM.attributes 0 : { *(.ARM.attributes) }
    /DISCARD/ : { *(.note*) *(.comment*) }
}
ASSERT(_ebss <= _sstack, "RAM data overlaps reserved 1 KiB stack")
ASSERT(SIZEOF(.isr_vector) == 192, "STM32L031 vector table must contain 48 words")
