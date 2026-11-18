cmd_/home/dawn/project/linkg_v2/build/arm/kernel/linkg_fast_nat/linkg_fast_nat_rules_common.o := /home/dawn/t113_tina5.0_v1.2/prebuilt/rootfsbuilt/arm/toolchain-sunxi-glibc-gcc-830/toolchain/bin/arm-openwrt-linux-gcc -Wp,-MD,/home/dawn/project/linkg_v2/build/arm/kernel/linkg_fast_nat/.linkg_fast_nat_rules_common.o.d -nostdinc -isystem /home/dawn/t113_tina5.0_v1.2/prebuilt/rootfsbuilt/arm/toolchain-sunxi-glibc-gcc-830/toolchain/bin/../lib/gcc/arm-openwrt-linux-gnueabi/8.3.0/include -I/home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include -I./arch/arm/include/generated -I/home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include -I./include -I/home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/uapi -I./arch/arm/include/generated/uapi -I/home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi -I./include/generated/uapi -include /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/kconfig.h -include /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/compiler_types.h -D__KERNEL__ -mlittle-endian -Wall -Wundef -Werror=strict-prototypes -Wno-trigraphs -fno-strict-aliasing -fno-common -fshort-wchar -fno-PIE -Werror=implicit-function-declaration -Werror=implicit-int -Wno-format-security -std=gnu89 -fno-dwarf2-cfi-asm -fno-ipa-sra -mabi=aapcs-linux -mfpu=vfp -funwind-tables -marm -Wa,-mno-warn-deprecated -D__LINUX_ARM_ARCH__=7 -march=armv7-a -msoft-float -Uarm -fno-delete-null-pointer-checks -Wno-frame-address -Wno-format-truncation -Wno-format-overflow -Os --param=allow-store-data-races=0 -Wframe-larger-than=2048 -fstack-protector-strong -Wno-unused-but-set-variable -Wimplicit-fallthrough -Wno-unused-const-variable -fomit-frame-pointer -fno-var-tracking-assignments -Wdeclaration-after-statement -Wvla -Wno-pointer-sign -Wno-stringop-truncation -Wno-array-bounds -Wno-stringop-overflow -Wno-restrict -Wno-maybe-uninitialized -fno-strict-overflow -fno-merge-all-constants -fmerge-constants -fno-stack-check -fconserve-stack -Werror=date-time -Werror=incompatible-pointer-types -Werror=designated-init -fmacro-prefix-map=/home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/= -Wno-packed-not-aligned -I/home/dawn/project/linkg_v2/build/arm/kernel/linkg_fast_nat  -DMODULE  -DKBUILD_BASENAME='"linkg_fast_nat_rules_common"' -DKBUILD_MODNAME='"linkg_fast_nat"' -c -o /home/dawn/project/linkg_v2/build/arm/kernel/linkg_fast_nat/linkg_fast_nat_rules_common.o /home/dawn/project/linkg_v2/build/arm/kernel/linkg_fast_nat/linkg_fast_nat_rules_common.c

source_/home/dawn/project/linkg_v2/build/arm/kernel/linkg_fast_nat/linkg_fast_nat_rules_common.o := /home/dawn/project/linkg_v2/build/arm/kernel/linkg_fast_nat/linkg_fast_nat_rules_common.c

deps_/home/dawn/project/linkg_v2/build/arm/kernel/linkg_fast_nat/linkg_fast_nat_rules_common.o := \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/kconfig.h \
    $(wildcard include/config/cc/version/text.h) \
    $(wildcard include/config/cpu/big/endian.h) \
    $(wildcard include/config/booger.h) \
    $(wildcard include/config/foo.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/compiler_types.h \
    $(wildcard include/config/have/arch/compiler/h.h) \
    $(wildcard include/config/enable/must/check.h) \
    $(wildcard include/config/optimize/inlining.h) \
    $(wildcard include/config/cc/has/asm/inline.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/compiler_attributes.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/compiler-gcc.h \
    $(wildcard include/config/retpoline.h) \
    $(wildcard include/config/arch/use/builtin/bswap.h) \
  /home/dawn/project/linkg_v2/build/arm/kernel/linkg_fast_nat/linkg_fast_nat_internal.h \
  /home/dawn/project/linkg_v2/build/arm/kernel/linkg_fast_nat/linkg_fast_nat_uapi.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/ioctl.h \
  arch/arm/include/generated/uapi/asm/ioctl.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/ioctl.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/asm-generic/ioctl.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/types.h \
    $(wildcard include/config/have/uid16.h) \
    $(wildcard include/config/uid16.h) \
    $(wildcard include/config/arch/dma/addr/t/64bit.h) \
    $(wildcard include/config/phys/addr/t/64bit.h) \
    $(wildcard include/config/64bit.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/types.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/uapi/asm/types.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/int-ll64.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/asm-generic/int-ll64.h \
  arch/arm/include/generated/uapi/asm/bitsperlong.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/bitsperlong.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/asm-generic/bitsperlong.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/posix_types.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/stddef.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/stddef.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/compiler_types.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/uapi/asm/posix_types.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/asm-generic/posix_types.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/ip.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/skbuff.h \
    $(wildcard include/config/nf/conntrack.h) \
    $(wildcard include/config/bridge/netfilter.h) \
    $(wildcard include/config/net/tc/skb/ext.h) \
    $(wildcard include/config/highmem.h) \
    $(wildcard include/config/skb/extensions.h) \
    $(wildcard include/config/ipv6/ndisc/nodetype.h) \
    $(wildcard include/config/net/switchdev.h) \
    $(wildcard include/config/net/cls/act.h) \
    $(wildcard include/config/net/redirect.h) \
    $(wildcard include/config/tls/device.h) \
    $(wildcard include/config/net/sched.h) \
    $(wildcard include/config/net/rx/busy/poll.h) \
    $(wildcard include/config/xps.h) \
    $(wildcard include/config/network/secmark.h) \
    $(wildcard include/config/net.h) \
    $(wildcard include/config/have/efficient/unaligned/access.h) \
    $(wildcard include/config/network/phy/timestamping.h) \
    $(wildcard include/config/xfrm.h) \
    $(wildcard include/config/netfilter/xt/target/trace.h) \
    $(wildcard include/config/nf/tables.h) \
    $(wildcard include/config/ip/vs.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/kernel.h \
    $(wildcard include/config/preempt/voluntary.h) \
    $(wildcard include/config/debug/atomic/sleep.h) \
    $(wildcard include/config/mmu.h) \
    $(wildcard include/config/prove/locking.h) \
    $(wildcard include/config/arch/has/refcount.h) \
    $(wildcard include/config/panic/timeout.h) \
    $(wildcard include/config/tracing.h) \
    $(wildcard include/config/ftrace/mcount/record.h) \
  /home/dawn/t113_tina5.0_v1.2/prebuilt/rootfsbuilt/arm/toolchain-sunxi-glibc-gcc-830/toolchain/lib/gcc/arm-openwrt-linux-gnueabi/8.3.0/include/stdarg.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/limits.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/limits.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/vdso/limits.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/linkage.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/stringify.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/export.h \
    $(wildcard include/config/modversions.h) \
    $(wildcard include/config/module/rel/crcs.h) \
    $(wildcard include/config/have/arch/prel32/relocations.h) \
    $(wildcard include/config/modules.h) \
    $(wildcard include/config/trim/unused/ksyms.h) \
    $(wildcard include/config/unused/symbols.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/linkage.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/compiler.h \
    $(wildcard include/config/trace/branch/profiling.h) \
    $(wildcard include/config/profile/all/branches.h) \
    $(wildcard include/config/stack/validation.h) \
    $(wildcard include/config/kasan.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/barrier.h \
    $(wildcard include/config/cpu/32v6k.h) \
    $(wildcard include/config/thumb2/kernel.h) \
    $(wildcard include/config/cpu/xsc3.h) \
    $(wildcard include/config/cpu/fa526.h) \
    $(wildcard include/config/arm/heavy/mb.h) \
    $(wildcard include/config/arm/dma/mem/bufferable.h) \
    $(wildcard include/config/smp.h) \
    $(wildcard include/config/cpu/spectre.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/barrier.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/kasan-checks.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/bitops.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/bits.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/const.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/vdso/const.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/const.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/vdso/bits.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/bitops.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/irqflags.h \
    $(wildcard include/config/trace/irqflags.h) \
    $(wildcard include/config/irqsoff/tracer.h) \
    $(wildcard include/config/preempt/tracer.h) \
    $(wildcard include/config/trace/irqflags/support.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/typecheck.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/irqflags.h \
    $(wildcard include/config/cpu/v7m.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/ptrace.h \
    $(wildcard include/config/arm/thumb.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/uapi/asm/ptrace.h \
    $(wildcard include/config/cpu/endian/be8.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/hwcap.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/uapi/asm/hwcap.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/irqflags.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/bitops/non-atomic.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/bitops/builtin-__fls.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/bitops/builtin-__ffs.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/bitops/builtin-fls.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/bitops/builtin-ffs.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/bitops/ffz.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/bitops/fls64.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/bitops/sched.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/bitops/hweight.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/bitops/arch_hweight.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/bitops/const_hweight.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/bitops/lock.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/atomic.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/atomic.h \
    $(wildcard include/config/generic/atomic64.h) \
    $(wildcard include/config/arm/lpae.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/prefetch.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/processor.h \
    $(wildcard include/config/have/hw/breakpoint.h) \
    $(wildcard include/config/binfmt/elf/fdpic.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/hw_breakpoint.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/unified.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/vdso/processor.h \
    $(wildcard include/config/arm/errata/754327.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/cache.h \
    $(wildcard include/config/arm/l1/cache/shift.h) \
    $(wildcard include/config/aeabi.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/cmpxchg.h \
    $(wildcard include/config/cpu/sa1100.h) \
    $(wildcard include/config/cpu/sa110.h) \
    $(wildcard include/config/cpu/v6.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/cmpxchg-local.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/atomic-fallback.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/atomic-long.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/bitops/find.h \
    $(wildcard include/config/generic/find/first/bit.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/bitops/le.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/uapi/asm/byteorder.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/byteorder/little_endian.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/byteorder/little_endian.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/swab.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/swab.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/swab.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/uapi/asm/swab.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/byteorder/generic.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/bitops/ext2-atomic-setbit.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/log2.h \
    $(wildcard include/config/arch/has/ilog2/u32.h) \
    $(wildcard include/config/arch/has/ilog2/u64.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/printk.h \
    $(wildcard include/config/message/loglevel/default.h) \
    $(wildcard include/config/console/loglevel/default.h) \
    $(wildcard include/config/console/loglevel/quiet.h) \
    $(wildcard include/config/early/printk.h) \
    $(wildcard include/config/printk/nmi.h) \
    $(wildcard include/config/printk.h) \
    $(wildcard include/config/dynamic/debug.h) \
    $(wildcard include/config/dynamic/debug/core.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/init.h \
    $(wildcard include/config/strict/kernel/rwx.h) \
    $(wildcard include/config/strict/module/rwx.h) \
    $(wildcard include/config/lto/clang.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/kern_levels.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/cache.h \
    $(wildcard include/config/arch/has/cache/line/size.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/kernel.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/sysinfo.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/build_bug.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/div64.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/compiler.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/div64.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/time.h \
    $(wildcard include/config/arch/uses/gettimeoffset.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/seqlock.h \
    $(wildcard include/config/debug/lock/alloc.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/spinlock.h \
    $(wildcard include/config/debug/spinlock.h) \
    $(wildcard include/config/preemption.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/preempt.h \
    $(wildcard include/config/preempt/count.h) \
    $(wildcard include/config/debug/preempt.h) \
    $(wildcard include/config/trace/preempt/toggle.h) \
    $(wildcard include/config/preempt/notifiers.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/list.h \
    $(wildcard include/config/debug/list.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/poison.h \
    $(wildcard include/config/illegal/pointer/value.h) \
    $(wildcard include/config/page/poisoning/zero.h) \
  arch/arm/include/generated/asm/preempt.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/preempt.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/thread_info.h \
    $(wildcard include/config/thread/info/in/task.h) \
    $(wildcard include/config/have/arch/within/stack/frames.h) \
    $(wildcard include/config/hardened/usercopy.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/bug.h \
    $(wildcard include/config/generic/bug.h) \
    $(wildcard include/config/bug/on/data/corruption.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/bug.h \
    $(wildcard include/config/debug/bugverbose.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/opcodes.h \
    $(wildcard include/config/cpu/endian/be32.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/bug.h \
    $(wildcard include/config/bug.h) \
    $(wildcard include/config/generic/bug/relative/pointers.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/restart_block.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/time64.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/math64.h \
    $(wildcard include/config/arch/supports/int128.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/vdso/math64.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/vdso/time64.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/time.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/time_types.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/thread_info.h \
    $(wildcard include/config/stackprotector/per/task.h) \
    $(wildcard include/config/crunch.h) \
    $(wildcard include/config/arm/thumbee.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/fpstate.h \
    $(wildcard include/config/vfpv3.h) \
    $(wildcard include/config/iwmmxt.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/page.h \
    $(wildcard include/config/cpu/copy/v4wt.h) \
    $(wildcard include/config/cpu/copy/v4wb.h) \
    $(wildcard include/config/cpu/copy/feroceon.h) \
    $(wildcard include/config/cpu/copy/fa.h) \
    $(wildcard include/config/cpu/xscale.h) \
    $(wildcard include/config/cpu/copy/v6.h) \
    $(wildcard include/config/kuser/helpers.h) \
    $(wildcard include/config/have/arch/pfn/valid.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/glue.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/pgtable-2level-types.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/memory.h \
    $(wildcard include/config/need/mach/memory/h.h) \
    $(wildcard include/config/page/offset.h) \
    $(wildcard include/config/dram/base.h) \
    $(wildcard include/config/dram/size.h) \
    $(wildcard include/config/xip/kernel.h) \
    $(wildcard include/config/have/tcm.h) \
    $(wildcard include/config/arm/patch/phys/virt.h) \
    $(wildcard include/config/phys/offset.h) \
    $(wildcard include/config/xip/phys/addr.h) \
    $(wildcard include/config/debug/virtual.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/sizes.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/memory_model.h \
    $(wildcard include/config/flatmem.h) \
    $(wildcard include/config/discontigmem.h) \
    $(wildcard include/config/sparsemem/vmemmap.h) \
    $(wildcard include/config/sparsemem.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/pfn.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/getorder.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/bottom_half.h \
  arch/arm/include/generated/asm/mmiowb.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/mmiowb.h \
    $(wildcard include/config/mmiowb.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/spinlock_types.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/spinlock_types.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/lockdep.h \
    $(wildcard include/config/lockdep.h) \
    $(wildcard include/config/lock/stat.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/rwlock_types.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/spinlock.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/rwlock.h \
    $(wildcard include/config/preempt.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/spinlock_api_smp.h \
    $(wildcard include/config/inline/spin/lock.h) \
    $(wildcard include/config/inline/spin/lock/bh.h) \
    $(wildcard include/config/inline/spin/lock/irq.h) \
    $(wildcard include/config/inline/spin/lock/irqsave.h) \
    $(wildcard include/config/inline/spin/trylock.h) \
    $(wildcard include/config/inline/spin/trylock/bh.h) \
    $(wildcard include/config/uninline/spin/unlock.h) \
    $(wildcard include/config/inline/spin/unlock/bh.h) \
    $(wildcard include/config/inline/spin/unlock/irq.h) \
    $(wildcard include/config/inline/spin/unlock/irqrestore.h) \
    $(wildcard include/config/generic/lockbreak.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/rwlock_api_smp.h \
    $(wildcard include/config/inline/read/lock.h) \
    $(wildcard include/config/inline/write/lock.h) \
    $(wildcard include/config/inline/read/lock/bh.h) \
    $(wildcard include/config/inline/write/lock/bh.h) \
    $(wildcard include/config/inline/read/lock/irq.h) \
    $(wildcard include/config/inline/write/lock/irq.h) \
    $(wildcard include/config/inline/read/lock/irqsave.h) \
    $(wildcard include/config/inline/write/lock/irqsave.h) \
    $(wildcard include/config/inline/read/trylock.h) \
    $(wildcard include/config/inline/write/trylock.h) \
    $(wildcard include/config/inline/read/unlock.h) \
    $(wildcard include/config/inline/write/unlock.h) \
    $(wildcard include/config/inline/read/unlock/bh.h) \
    $(wildcard include/config/inline/write/unlock/bh.h) \
    $(wildcard include/config/inline/read/unlock/irq.h) \
    $(wildcard include/config/inline/write/unlock/irq.h) \
    $(wildcard include/config/inline/read/unlock/irqrestore.h) \
    $(wildcard include/config/inline/write/unlock/irqrestore.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/time32.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/timex.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/timex.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/param.h \
  arch/arm/include/generated/uapi/asm/param.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/param.h \
    $(wildcard include/config/hz.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/asm-generic/param.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/timex.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/vdso/time32.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/vdso/time.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/bvec.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/errno.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/errno.h \
  arch/arm/include/generated/uapi/asm/errno.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/asm-generic/errno.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/asm-generic/errno-base.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/mm.h \
    $(wildcard include/config/need/multiple/nodes.h) \
    $(wildcard include/config/sysctl.h) \
    $(wildcard include/config/have/arch/mmap/rnd/bits.h) \
    $(wildcard include/config/have/arch/mmap/rnd/compat/bits.h) \
    $(wildcard include/config/mem/soft/dirty.h) \
    $(wildcard include/config/arch/uses/high/vma/flags.h) \
    $(wildcard include/config/arch/has/pkeys.h) \
    $(wildcard include/config/ppc.h) \
    $(wildcard include/config/x86.h) \
    $(wildcard include/config/parisc.h) \
    $(wildcard include/config/ia64.h) \
    $(wildcard include/config/sparc64.h) \
    $(wildcard include/config/x86/intel/mpx.h) \
    $(wildcard include/config/stack/growsup.h) \
    $(wildcard include/config/numa.h) \
    $(wildcard include/config/shmem.h) \
    $(wildcard include/config/arch/has/pte/devmap.h) \
    $(wildcard include/config/transparent/hugepage.h) \
    $(wildcard include/config/hugetlb/page.h) \
    $(wildcard include/config/gki/opt/features.h) \
    $(wildcard include/config/zone/device.h) \
    $(wildcard include/config/dev/pagemap/ops.h) \
    $(wildcard include/config/device/private.h) \
    $(wildcard include/config/pci/p2pdma.h) \
    $(wildcard include/config/numa/balancing.h) \
    $(wildcard include/config/kasan/sw/tags.h) \
    $(wildcard include/config/memcg.h) \
    $(wildcard include/config/have/memblock/node/map.h) \
    $(wildcard include/config/have/arch/early/pfn/to/nid.h) \
    $(wildcard include/config/flat/node/mem/map.h) \
    $(wildcard include/config/debug/vm/rb.h) \
    $(wildcard include/config/page/poisoning.h) \
    $(wildcard include/config/init/on/alloc/default/on.h) \
    $(wildcard include/config/init/on/free/default/on.h) \
    $(wildcard include/config/debug/pagealloc.h) \
    $(wildcard include/config/arch/has/set/direct/map.h) \
    $(wildcard include/config/hibernation.h) \
    $(wildcard include/config/memory/hotplug.h) \
    $(wildcard include/config/hugetlbfs.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/mmdebug.h \
    $(wildcard include/config/debug/vm.h) \
    $(wildcard include/config/debug/vm/pgflags.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/gfp.h \
    $(wildcard include/config/zone/dma.h) \
    $(wildcard include/config/zone/dma32.h) \
    $(wildcard include/config/pm/sleep.h) \
    $(wildcard include/config/contig/alloc.h) \
    $(wildcard include/config/cma.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/mmzone.h \
    $(wildcard include/config/force/max/zoneorder.h) \
    $(wildcard include/config/memory/isolation.h) \
    $(wildcard include/config/shuffle/page/allocator.h) \
    $(wildcard include/config/shadow/call/stack.h) \
    $(wildcard include/config/zsmalloc.h) \
    $(wildcard include/config/compaction.h) \
    $(wildcard include/config/page/extension.h) \
    $(wildcard include/config/deferred/struct/page/init.h) \
    $(wildcard include/config/have/memory/present.h) \
    $(wildcard include/config/have/memoryless/nodes.h) \
    $(wildcard include/config/sparsemem/extreme.h) \
    $(wildcard include/config/memory/hotremove.h) \
    $(wildcard include/config/holes/in/zone.h) \
    $(wildcard include/config/arch/has/holes/memorymodel.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/wait.h \
  arch/arm/include/generated/asm/current.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/current.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/wait.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/threads.h \
    $(wildcard include/config/nr/cpus.h) \
    $(wildcard include/config/base/small.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/numa.h \
    $(wildcard include/config/nodes/shift.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/nodemask.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/bitmap.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/string.h \
    $(wildcard include/config/binary/printf.h) \
    $(wildcard include/config/fortify/source.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/string.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/string.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/pageblock-flags.h \
    $(wildcard include/config/hugetlb/page/size/variable.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/page-flags-layout.h \
  include/generated/bounds.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/mm_types.h \
    $(wildcard include/config/have/aligned/struct/page.h) \
    $(wildcard include/config/userfaultfd.h) \
    $(wildcard include/config/swap.h) \
    $(wildcard include/config/have/arch/compat/mmap/bases.h) \
    $(wildcard include/config/membarrier.h) \
    $(wildcard include/config/aio.h) \
    $(wildcard include/config/mmu/notifier.h) \
    $(wildcard include/config/arch/want/batched/unmap/tlb/flush.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/mm_types_task.h \
    $(wildcard include/config/split/ptlock/cpus.h) \
    $(wildcard include/config/arch/enable/split/pmd/ptlock.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/cpumask.h \
    $(wildcard include/config/cpumask/offstack.h) \
    $(wildcard include/config/hotplug/cpu.h) \
    $(wildcard include/config/debug/per/cpu/maps.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/auxvec.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/auxvec.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/auxvec.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/uapi/asm/auxvec.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/rbtree.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/rcupdate.h \
    $(wildcard include/config/preempt/rcu.h) \
    $(wildcard include/config/rcu/stall/common.h) \
    $(wildcard include/config/no/hz/full.h) \
    $(wildcard include/config/rcu/nocb/cpu.h) \
    $(wildcard include/config/tasks/rcu.h) \
    $(wildcard include/config/tree/rcu.h) \
    $(wildcard include/config/tiny/rcu.h) \
    $(wildcard include/config/debug/objects/rcu/head.h) \
    $(wildcard include/config/prove/rcu.h) \
    $(wildcard include/config/rcu/boost.h) \
    $(wildcard include/config/arch/weak/release/acquire.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/rcutree.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/rwsem.h \
    $(wildcard include/config/rwsem/spin/on/owner.h) \
    $(wildcard include/config/debug/rwsems.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/err.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/osq_lock.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/android_vendor.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/completion.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/uprobes.h \
    $(wildcard include/config/uprobes.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/workqueue.h \
    $(wildcard include/config/debug/objects/work.h) \
    $(wildcard include/config/freezer.h) \
    $(wildcard include/config/sysfs.h) \
    $(wildcard include/config/wq/watchdog.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/timer.h \
    $(wildcard include/config/debug/objects/timers.h) \
    $(wildcard include/config/preempt/rt.h) \
    $(wildcard include/config/no/hz/common.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/ktime.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/jiffies.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/vdso/jiffies.h \
  include/generated/timeconst.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/vdso/ktime.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/timekeeping.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/timekeeping32.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/debugobjects.h \
    $(wildcard include/config/debug/objects.h) \
    $(wildcard include/config/debug/objects/free.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/android_kabi.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/mmu.h \
    $(wildcard include/config/cpu/has/asid.h) \
    $(wildcard include/config/vdso.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/page-flags.h \
    $(wildcard include/config/arch/uses/pg/uncached.h) \
    $(wildcard include/config/memory/failure.h) \
    $(wildcard include/config/idle/page/tracking.h) \
    $(wildcard include/config/thp/swap.h) \
    $(wildcard include/config/ksm.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/memory_hotplug.h \
    $(wildcard include/config/arch/has/add/pages.h) \
    $(wildcard include/config/have/arch/nodedata/extension.h) \
    $(wildcard include/config/have/bootmem/info/node.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/notifier.h \
    $(wildcard include/config/tree/srcu.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/mutex.h \
    $(wildcard include/config/mutex/spin/on/owner.h) \
    $(wildcard include/config/debug/mutexes.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/debug_locks.h \
    $(wildcard include/config/debug/locking/api/selftests.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/srcu.h \
    $(wildcard include/config/tiny/srcu.h) \
    $(wildcard include/config/srcu.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/rcu_segcblist.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/srcutree.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/rcu_node_tree.h \
    $(wildcard include/config/rcu/fanout.h) \
    $(wildcard include/config/rcu/fanout/leaf.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/topology.h \
    $(wildcard include/config/use/percpu/numa/node/id.h) \
    $(wildcard include/config/sched/smt.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/arch_topology.h \
    $(wildcard include/config/generic/arch/topology.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/percpu.h \
    $(wildcard include/config/need/per/cpu/embed/first/chunk.h) \
    $(wildcard include/config/need/per/cpu/page/first/chunk.h) \
    $(wildcard include/config/have/setup/per/cpu/area.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/smp.h \
    $(wildcard include/config/up/late/init.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/llist.h \
    $(wildcard include/config/arch/have/nmi/safe/cmpxchg.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/smp.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/percpu.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/percpu.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/percpu-defs.h \
    $(wildcard include/config/debug/force/weak/per/cpu.h) \
    $(wildcard include/config/amd/mem/encrypt.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/topology.h \
    $(wildcard include/config/arm/cpu/topology.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/topology.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/range.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/percpu-refcount.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/bit_spinlock.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/shrinker.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/resource.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/resource.h \
  arch/arm/include/generated/uapi/asm/resource.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/resource.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/asm-generic/resource.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/page_ext.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/stacktrace.h \
    $(wildcard include/config/stacktrace.h) \
    $(wildcard include/config/arch/stackwalk.h) \
    $(wildcard include/config/have/reliable/stacktrace.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/stackdepot.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/page_ref.h \
    $(wildcard include/config/debug/page/ref.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/tracepoint-defs.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/static_key.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/jump_label.h \
    $(wildcard include/config/jump/label.h) \
    $(wildcard include/config/have/arch/jump/label/relative.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/jump_label.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/memremap.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/ioport.h \
    $(wildcard include/config/io/strict/devmem.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/overflow.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/pgtable.h \
    $(wildcard include/config/highpte.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/proc-fns.h \
    $(wildcard include/config/big/little.h) \
    $(wildcard include/config/harden/branch/predictor.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/glue-proc.h \
    $(wildcard include/config/cpu/arm7tdmi.h) \
    $(wildcard include/config/cpu/arm720t.h) \
    $(wildcard include/config/cpu/arm740t.h) \
    $(wildcard include/config/cpu/arm9tdmi.h) \
    $(wildcard include/config/cpu/arm920t.h) \
    $(wildcard include/config/cpu/arm922t.h) \
    $(wildcard include/config/cpu/arm925t.h) \
    $(wildcard include/config/cpu/arm926t.h) \
    $(wildcard include/config/cpu/arm940t.h) \
    $(wildcard include/config/cpu/arm946e.h) \
    $(wildcard include/config/cpu/arm1020.h) \
    $(wildcard include/config/cpu/arm1020e.h) \
    $(wildcard include/config/cpu/arm1022.h) \
    $(wildcard include/config/cpu/arm1026.h) \
    $(wildcard include/config/cpu/mohawk.h) \
    $(wildcard include/config/cpu/feroceon.h) \
    $(wildcard include/config/cpu/v6k.h) \
    $(wildcard include/config/cpu/pj4b.h) \
    $(wildcard include/config/cpu/v7.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/pgtable-nopud.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/pgtable-nop4d-hack.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/5level-fixup.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/pgtable-hwdef.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/pgtable-2level-hwdef.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/tlbflush.h \
    $(wildcard include/config/smp/on/up.h) \
    $(wildcard include/config/cpu/tlb/v4wt.h) \
    $(wildcard include/config/cpu/tlb/fa.h) \
    $(wildcard include/config/cpu/tlb/v4wbi.h) \
    $(wildcard include/config/cpu/tlb/feroceon.h) \
    $(wildcard include/config/cpu/tlb/v4wb.h) \
    $(wildcard include/config/cpu/tlb/v6.h) \
    $(wildcard include/config/cpu/tlb/v7.h) \
    $(wildcard include/config/arm/errata/720789.h) \
    $(wildcard include/config/arm/errata/798181.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/sched.h \
    $(wildcard include/config/virt/cpu/accounting/native.h) \
    $(wildcard include/config/sched/info.h) \
    $(wildcard include/config/schedstats.h) \
    $(wildcard include/config/fair/group/sched.h) \
    $(wildcard include/config/rt/group/sched.h) \
    $(wildcard include/config/uclamp/task.h) \
    $(wildcard include/config/uclamp/buckets/count.h) \
    $(wildcard include/config/cgroup/sched.h) \
    $(wildcard include/config/blk/dev/io/trace.h) \
    $(wildcard include/config/psi.h) \
    $(wildcard include/config/compat/brk.h) \
    $(wildcard include/config/cgroups.h) \
    $(wildcard include/config/blk/cgroup.h) \
    $(wildcard include/config/stackprotector.h) \
    $(wildcard include/config/arch/has/scaled/cputime.h) \
    $(wildcard include/config/cpu/freq/times.h) \
    $(wildcard include/config/virt/cpu/accounting/gen.h) \
    $(wildcard include/config/posix/cputimers.h) \
    $(wildcard include/config/keys.h) \
    $(wildcard include/config/sysvipc.h) \
    $(wildcard include/config/detect/hung/task.h) \
    $(wildcard include/config/audit.h) \
    $(wildcard include/config/auditsyscall.h) \
    $(wildcard include/config/rt/mutexes.h) \
    $(wildcard include/config/ubsan.h) \
    $(wildcard include/config/block.h) \
    $(wildcard include/config/task/xacct.h) \
    $(wildcard include/config/cpusets.h) \
    $(wildcard include/config/x86/cpu/resctrl.h) \
    $(wildcard include/config/futex.h) \
    $(wildcard include/config/compat.h) \
    $(wildcard include/config/perf/events.h) \
    $(wildcard include/config/rseq.h) \
    $(wildcard include/config/task/delay/acct.h) \
    $(wildcard include/config/fault/injection.h) \
    $(wildcard include/config/latencytop.h) \
    $(wildcard include/config/function/graph/tracer.h) \
    $(wildcard include/config/kcov.h) \
    $(wildcard include/config/bcache.h) \
    $(wildcard include/config/vmap/stack.h) \
    $(wildcard include/config/livepatch.h) \
    $(wildcard include/config/security.h) \
    $(wildcard include/config/gcc/plugin/stackleak.h) \
    $(wildcard include/config/arch/task/struct/on/stack.h) \
    $(wildcard include/config/debug/rseq.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/sched.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/pid.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/rculist.h \
    $(wildcard include/config/prove/rcu/list.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/refcount.h \
    $(wildcard include/config/refcount/full.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/sem.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/sem.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/ipc.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/uidgid.h \
    $(wildcard include/config/multiuser.h) \
    $(wildcard include/config/user/ns.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/highuid.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/rhashtable-types.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/ipc.h \
  arch/arm/include/generated/uapi/asm/ipcbuf.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/asm-generic/ipcbuf.h \
  arch/arm/include/generated/uapi/asm/sembuf.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/asm-generic/sembuf.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/shm.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/shm.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/asm-generic/hugetlb_encode.h \
  arch/arm/include/generated/uapi/asm/shmbuf.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/asm-generic/shmbuf.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/shmparam.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/kcov.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/kcov.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/plist.h \
    $(wildcard include/config/debug/plist.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/hrtimer.h \
    $(wildcard include/config/high/res/timers.h) \
    $(wildcard include/config/time/low/res.h) \
    $(wildcard include/config/timerfd.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/hrtimer_defs.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/timerqueue.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/seccomp.h \
    $(wildcard include/config/seccomp.h) \
    $(wildcard include/config/have/arch/seccomp/filter.h) \
    $(wildcard include/config/seccomp/filter.h) \
    $(wildcard include/config/checkpoint/restore.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/seccomp.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/latencytop.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/sched/prio.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/sched/types.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/signal_types.h \
    $(wildcard include/config/old/sigaction.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/signal.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/signal.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/uapi/asm/signal.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/asm-generic/signal-defs.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/uapi/asm/sigcontext.h \
  arch/arm/include/generated/uapi/asm/siginfo.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/asm-generic/siginfo.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/task_io_accounting.h \
    $(wildcard include/config/task/io/accounting.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/posix-timers.h \
    $(wildcard include/config/posix/timers.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/alarmtimer.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/rseq.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/pgtable-2level.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/pgtable.h \
    $(wildcard include/config/pgtable/levels.h) \
    $(wildcard include/config/have/arch/transparent/hugepage/pud.h) \
    $(wildcard include/config/have/arch/soft/dirty.h) \
    $(wildcard include/config/arch/enable/thp/migration.h) \
    $(wildcard include/config/have/arch/huge/vmap.h) \
    $(wildcard include/config/x86/espfix64.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/huge_mm.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/sched/coredump.h \
    $(wildcard include/config/core/dump/default/elf/headers.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/fs.h \
    $(wildcard include/config/read/only/thp/for/fs.h) \
    $(wildcard include/config/fs/posix/acl.h) \
    $(wildcard include/config/cgroup/writeback.h) \
    $(wildcard include/config/ima.h) \
    $(wildcard include/config/file/locking.h) \
    $(wildcard include/config/fsnotify.h) \
    $(wildcard include/config/fs/encryption.h) \
    $(wildcard include/config/fs/verity.h) \
    $(wildcard include/config/epoll.h) \
    $(wildcard include/config/unicode.h) \
    $(wildcard include/config/quota.h) \
    $(wildcard include/config/fs/dax.h) \
    $(wildcard include/config/mandatory/file/locking.h) \
    $(wildcard include/config/migration.h) \
    $(wildcard include/config/io/uring.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/wait_bit.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/kdev_t.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/kdev_t.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/dcache.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/rculist_bl.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/list_bl.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/lockref.h \
    $(wildcard include/config/arch/use/cmpxchg/lockref.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/stringhash.h \
    $(wildcard include/config/dcache/word/access.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/hash.h \
    $(wildcard include/config/have/arch/hash.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/path.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/stat.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/uapi/asm/stat.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/stat.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/list_lru.h \
    $(wildcard include/config/memcg/kmem.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/radix-tree.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/xarray.h \
    $(wildcard include/config/xarray/multi.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/kconfig.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/capability.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/capability.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/semaphore.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/fcntl.h \
    $(wildcard include/config/arch/32bit/off/t.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/fcntl.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/uapi/asm/fcntl.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/asm-generic/fcntl.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/fiemap.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/migrate_mode.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/percpu-rwsem.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/rcuwait.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/rcu_sync.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/delayed_call.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/uuid.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/uuid.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/errseq.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/ioprio.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/sched/rt.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/iocontext.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/fs_types.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/fs.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/quota.h \
    $(wildcard include/config/quota/netlink/interface.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/percpu_counter.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/dqblk_xfs.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/dqblk_v1.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/dqblk_v2.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/dqblk_qtree.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/projid.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/quota.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/nfs_fs_i.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/vmstat.h \
    $(wildcard include/config/vm/event/counters.h) \
    $(wildcard include/config/debug/tlbflush.h) \
    $(wildcard include/config/debug/vm/vmacache.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/vm_event_item.h \
    $(wildcard include/config/memory/balloon.h) \
    $(wildcard include/config/balloon/compaction.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/socket.h \
    $(wildcard include/config/proc/fs.h) \
  arch/arm/include/generated/uapi/asm/socket.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/asm-generic/socket.h \
  arch/arm/include/generated/uapi/asm/sockios.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/asm-generic/sockios.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/sockios.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/uio.h \
    $(wildcard include/config/arch/has/uaccess/flushcache.h) \
    $(wildcard include/config/arch/has/uaccess/mcsafe.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/crypto/hash.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/crypto.h \
    $(wildcard include/config/crypto/stats.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/slab.h \
    $(wildcard include/config/debug/slab.h) \
    $(wildcard include/config/failslab.h) \
    $(wildcard include/config/have/hardened/usercopy/allocator.h) \
    $(wildcard include/config/slab.h) \
    $(wildcard include/config/slub.h) \
    $(wildcard include/config/slob.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/kasan.h \
    $(wildcard include/config/kasan/generic.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/uaccess.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/uaccess.h \
    $(wildcard include/config/cpu/sw/domain/pan.h) \
    $(wildcard include/config/cpu/use/domains.h) \
    $(wildcard include/config/uaccess/with/memcpy.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/domain.h \
    $(wildcard include/config/io/36.h) \
    $(wildcard include/config/cpu/cp15/mmu.h) \
  arch/arm/include/generated/asm/extable.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/extable.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/uio.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/socket.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/net.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/random.h \
    $(wildcard include/config/arch/random.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/once.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/random.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/irqnr.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/irqnr.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/prandom.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/net.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/textsearch.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/checksum.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/checksum.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/in6.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/in6.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/libc-compat.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/dma-mapping.h \
    $(wildcard include/config/dma/declare/coherent.h) \
    $(wildcard include/config/arch/has/sync/dma/for/device.h) \
    $(wildcard include/config/swiotlb.h) \
    $(wildcard include/config/arch/has/sync/dma/for/cpu.h) \
    $(wildcard include/config/arch/has/sync/dma/for/cpu/all.h) \
    $(wildcard include/config/has/dma.h) \
    $(wildcard include/config/arch/has/setup/dma/ops.h) \
    $(wildcard include/config/arch/has/teardown/dma/ops.h) \
    $(wildcard include/config/need/dma/map/state.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/device.h \
    $(wildcard include/config/acpi.h) \
    $(wildcard include/config/debug/devres.h) \
    $(wildcard include/config/generic/msi/irq/domain.h) \
    $(wildcard include/config/pinctrl.h) \
    $(wildcard include/config/generic/msi/irq.h) \
    $(wildcard include/config/dma/cma.h) \
    $(wildcard include/config/of.h) \
    $(wildcard include/config/devtmpfs.h) \
    $(wildcard include/config/sysfs/deprecated.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/kobject.h \
    $(wildcard include/config/uevent/helper.h) \
    $(wildcard include/config/debug/kobject/release.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/sysfs.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/kernfs.h \
    $(wildcard include/config/kernfs.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/idr.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/kobject_ns.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/kref.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/klist.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/pm.h \
    $(wildcard include/config/vt/console/sleep.h) \
    $(wildcard include/config/pm.h) \
    $(wildcard include/config/pm/clk.h) \
    $(wildcard include/config/pm/generic/domains.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/ratelimit.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/device.h \
    $(wildcard include/config/dmabounce.h) \
    $(wildcard include/config/iommu/api.h) \
    $(wildcard include/config/arm/dma/use/iommu.h) \
    $(wildcard include/config/arch/omap.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/pm_wakeup.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/dma-debug.h \
    $(wildcard include/config/dma/api/debug.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/dma-direction.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/scatterlist.h \
    $(wildcard include/config/need/sg/dma/length.h) \
    $(wildcard include/config/debug/sg.h) \
    $(wildcard include/config/sgl/alloc.h) \
    $(wildcard include/config/arch/no/sg/chain.h) \
    $(wildcard include/config/sg/pool.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/io.h \
    $(wildcard include/config/pci.h) \
    $(wildcard include/config/need/mach/io/h.h) \
    $(wildcard include/config/pcmcia/soc/common.h) \
    $(wildcard include/config/isa.h) \
    $(wildcard include/config/pccard.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/pci_iomap.h \
    $(wildcard include/config/no/generic/pci/ioport/map.h) \
    $(wildcard include/config/generic/pci/iomap.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/io.h \
    $(wildcard include/config/generic/iomap.h) \
    $(wildcard include/config/has/ioport/map.h) \
    $(wildcard include/config/virt/to/bus.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/logic_pio.h \
    $(wildcard include/config/indirect/pio.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/fwnode.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/vmalloc.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/mem_encrypt.h \
    $(wildcard include/config/arch/has/mem/encrypt.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/dma-mapping.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/xen/xen.h \
    $(wildcard include/config/xen.h) \
    $(wildcard include/config/xen/pvh.h) \
    $(wildcard include/config/xen/dom0.h) \
    $(wildcard include/config/xen/balloon.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/xen/interface/hvm/start_info.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/xen/hypervisor.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/xen/arm/hypervisor.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/netdev_features.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/sched/clock.h \
    $(wildcard include/config/have/unstable/sched/clock.h) \
    $(wildcard include/config/irq/time/accounting.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/flow_dissector.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/siphash.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/if_ether.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/splice.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/pipe_fs_i.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/if_packet.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/flow.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/netfilter/nf_conntrack_common.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/netfilter/nf_conntrack_common.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/ip.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/netfilter.h \
    $(wildcard include/config/netfilter.h) \
    $(wildcard include/config/netfilter/family/arp.h) \
    $(wildcard include/config/netfilter/family/bridge.h) \
    $(wildcard include/config/decnet.h) \
    $(wildcard include/config/nf/nat.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/if.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/hdlc/ioctl.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/in.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/in.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/netfilter_defs.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/netfilter.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/netdevice.h \
    $(wildcard include/config/dcb.h) \
    $(wildcard include/config/hyperv/net.h) \
    $(wildcard include/config/wlan.h) \
    $(wildcard include/config/ax25.h) \
    $(wildcard include/config/mac80211/mesh.h) \
    $(wildcard include/config/net/ipip.h) \
    $(wildcard include/config/net/ipgre.h) \
    $(wildcard include/config/ipv6/sit.h) \
    $(wildcard include/config/ipv6/tunnel.h) \
    $(wildcard include/config/rps.h) \
    $(wildcard include/config/netpoll.h) \
    $(wildcard include/config/xdp/sockets.h) \
    $(wildcard include/config/bql.h) \
    $(wildcard include/config/rfs/accel.h) \
    $(wildcard include/config/fcoe.h) \
    $(wildcard include/config/xfrm/offload.h) \
    $(wildcard include/config/net/poll/controller.h) \
    $(wildcard include/config/libfcoe.h) \
    $(wildcard include/config/wireless/ext.h) \
    $(wildcard include/config/net/l3/master/dev.h) \
    $(wildcard include/config/ipv6.h) \
    $(wildcard include/config/vlan/8021q.h) \
    $(wildcard include/config/net/dsa.h) \
    $(wildcard include/config/tipc.h) \
    $(wildcard include/config/irda.h) \
    $(wildcard include/config/atalk.h) \
    $(wildcard include/config/mpls/routing.h) \
    $(wildcard include/config/netfilter/ingress.h) \
    $(wildcard include/config/garp.h) \
    $(wildcard include/config/mrp.h) \
    $(wildcard include/config/cgroup/net/prio.h) \
    $(wildcard include/config/net/flow/limit.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/delay.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/delay.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/dynamic_queue_limits.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/ethtool.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/compat.h \
    $(wildcard include/config/arch/has/syscall/wrapper.h) \
    $(wildcard include/config/x86/x32/abi.h) \
    $(wildcard include/config/compat/old/sigaction.h) \
    $(wildcard include/config/odd/rt/sigaction.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/aio_abi.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/unistd.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/unistd.h \
    $(wildcard include/config/oabi/compat.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/uapi/asm/unistd.h \
  arch/arm/include/generated/uapi/asm/unistd-eabi.h \
  arch/arm/include/generated/uapi/asm/unistd-common.h \
  arch/arm/include/generated/asm/unistd-nr.h \
  arch/arm/include/generated/asm/compat.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/compat.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/ethtool.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/if_ether.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/net_namespace.h \
    $(wildcard include/config/ieee802154/6lowpan.h) \
    $(wildcard include/config/ip/sctp.h) \
    $(wildcard include/config/ip/dccp.h) \
    $(wildcard include/config/nf/defrag/ipv6.h) \
    $(wildcard include/config/netfilter/netlink/acct.h) \
    $(wildcard include/config/nf/ct/netlink/timeout.h) \
    $(wildcard include/config/wext/core.h) \
    $(wildcard include/config/mpls.h) \
    $(wildcard include/config/can.h) \
    $(wildcard include/config/crypto/user.h) \
    $(wildcard include/config/net/ns.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/sysctl.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/sysctl.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/netns/core.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/netns/mib.h \
    $(wildcard include/config/xfrm/statistics.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/snmp.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/snmp.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/u64_stats_sync.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/netns/unix.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/netns/packet.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/netns/ipv4.h \
    $(wildcard include/config/ip/multiple/tables.h) \
    $(wildcard include/config/ip/route/classid.h) \
    $(wildcard include/config/ip/mroute.h) \
    $(wildcard include/config/ip/mroute/multiple/tables.h) \
    $(wildcard include/config/ip/route/multipath.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/inet_frag.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/netns/ipv6.h \
    $(wildcard include/config/ipv6/multiple/tables.h) \
    $(wildcard include/config/ipv6/mroute.h) \
    $(wildcard include/config/ipv6/mroute/multiple/tables.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/dst_ops.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/icmpv6.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/netns/nexthop.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/netns/ieee802154_6lowpan.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/netns/sctp.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/netns/dccp.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/netns/netfilter.h \
    $(wildcard include/config/nf/defrag/ipv4.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/netns/x_tables.h \
    $(wildcard include/config/bridge/nf/ebtables.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/netns/conntrack.h \
    $(wildcard include/config/nf/ct/proto/dccp.h) \
    $(wildcard include/config/nf/ct/proto/sctp.h) \
    $(wildcard include/config/nf/ct/proto/gre.h) \
    $(wildcard include/config/nf/conntrack/events.h) \
    $(wildcard include/config/nf/conntrack/labels.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/list_nulls.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/netfilter/nf_conntrack_tcp.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/netfilter/nf_conntrack_tcp.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/netfilter/nf_conntrack_dccp.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/netfilter/nf_conntrack_tuple_common.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/netfilter/nf_conntrack_sctp.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/netfilter/nf_conntrack_sctp.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/netns/nftables.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/netns/xfrm.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/xfrm.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/netns/mpls.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/netns/can.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/netns/xdp.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/ns_common.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/seq_file_net.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/seq_file.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/cred.h \
    $(wildcard include/config/debug/credentials.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/key.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/assoc_array.h \
    $(wildcard include/config/associative/array.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/sched/user.h \
    $(wildcard include/config/fanotify.h) \
    $(wildcard include/config/posix/mqueue.h) \
    $(wildcard include/config/bpf/syscall.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/nsproxy.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/netprio_cgroup.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/cgroup.h \
    $(wildcard include/config/cgroup/cpuacct.h) \
    $(wildcard include/config/sock/cgroup/data.h) \
    $(wildcard include/config/cgroup/net/classid.h) \
    $(wildcard include/config/cgroup/data.h) \
    $(wildcard include/config/cgroup/bpf.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/cgroupstats.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/taskstats.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/user_namespace.h \
    $(wildcard include/config/inotify/user.h) \
    $(wildcard include/config/persistent/keyrings.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/kernel_stat.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/interrupt.h \
    $(wildcard include/config/irq/forced/threading.h) \
    $(wildcard include/config/generic/irq/probe.h) \
    $(wildcard include/config/irq/timings.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/irqreturn.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/hardirq.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/ftrace_irq.h \
    $(wildcard include/config/ftrace/nmi/enter.h) \
    $(wildcard include/config/hwlat/tracer.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/vtime.h \
    $(wildcard include/config/virt/cpu/accounting.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/context_tracking_state.h \
    $(wildcard include/config/context/tracking.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/hardirq.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/irq.h \
    $(wildcard include/config/sparse/irq.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/irq_cpustat.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/sections.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/sections.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/cgroup-defs.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/bpf-cgroup.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/bpf.h \
    $(wildcard include/config/bpf/stream/parser.h) \
    $(wildcard include/config/inet.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/bpf.h \
    $(wildcard include/config/efficient/unaligned/access.h) \
    $(wildcard include/config/bpf/kprobe/override.h) \
    $(wildcard include/config/function/error/injection.h) \
    $(wildcard include/config/bpf/lirc/mode2.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/bpf_common.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/file.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/rbtree_latch.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/psi_types.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/kthread.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/xdp.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/neighbour.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/netlink.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/scm.h \
    $(wildcard include/config/security/network.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/security.h \
    $(wildcard include/config/security/infiniband.h) \
    $(wildcard include/config/security/network/xfrm.h) \
    $(wildcard include/config/security/path.h) \
    $(wildcard include/config/securityfs.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/sched/signal.h \
    $(wildcard include/config/sched/autogroup.h) \
    $(wildcard include/config/bsd/process/acct.h) \
    $(wildcard include/config/taskstats.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/signal.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/sched/jobctl.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/sched/task.h \
    $(wildcard include/config/have/copy/thread/tls.h) \
    $(wildcard include/config/have/exit/thread.h) \
    $(wildcard include/config/arch/wants/dynamic/task/struct.h) \
    $(wildcard include/config/have/arch/thread/struct/whitelist.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/netlink.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/netdevice.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/if_link.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/if_link.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/if_bonding.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/pkt_cls.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/pkt_sched.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/hashtable.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/netfilter/nf_conntrack_zones_common.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/netfilter/nf_conntrack_tuple_common.h \
  /home/dawn/project/linkg_v2/build/arm/kernel/linkg_fast_nat/linkg_fast_nat_utils.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/inetdevice.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/rtnetlink.h \
    $(wildcard include/config/net/ingress.h) \
    $(wildcard include/config/net/egress.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/rtnetlink.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/if_addr.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/tcp.h \
    $(wildcard include/config/smc.h) \
    $(wildcard include/config/bpf.h) \
    $(wildcard include/config/tcp/md5sig.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/win_minmax.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/sock.h \
    $(wildcard include/config/sock/validate/xmit.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/page_counter.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/memcontrol.h \
    $(wildcard include/config/memcg/swap.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/vmpressure.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/eventfd.h \
    $(wildcard include/config/eventfd.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/writeback.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/flex_proportions.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/backing-dev-defs.h \
    $(wildcard include/config/debug/fs.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/blk_types.h \
    $(wildcard include/config/alpha.h) \
    $(wildcard include/config/blk/cgroup/iocost.h) \
    $(wildcard include/config/blk/inline/encryption.h) \
    $(wildcard include/config/dm/default/key.h) \
    $(wildcard include/config/blk/dev/integrity.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/blk-cgroup.h \
    $(wildcard include/config/blk/dev/throttling.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/blkdev.h \
    $(wildcard include/config/blk/rq/alloc/time.h) \
    $(wildcard include/config/blk/wbt.h) \
    $(wildcard include/config/blk/dev/zoned.h) \
    $(wildcard include/config/blk/dev/bsg.h) \
    $(wildcard include/config/blk/debug/fs.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/major.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/genhd.h \
    $(wildcard include/config/fail/make/request.h) \
    $(wildcard include/config/solaris/x86/partition.h) \
    $(wildcard include/config/bsd/disklabel.h) \
    $(wildcard include/config/unixware/disklabel.h) \
    $(wildcard include/config/minix/subpartition.h) \
  arch/arm/include/generated/asm/local.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/local.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/pagemap.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/highmem.h \
    $(wildcard include/config/x86/32.h) \
    $(wildcard include/config/debug/highmem.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/cacheflush.h \
    $(wildcard include/config/arm/errata/411920.h) \
    $(wildcard include/config/cpu/cache/vipt.h) \
    $(wildcard include/config/outer/cache.h) \
    $(wildcard include/config/frame/pointer.h) \
    $(wildcard include/config/cpu/icache/mismatch/workaround.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/glue-cache.h \
    $(wildcard include/config/cpu/cache/v4.h) \
    $(wildcard include/config/cpu/cache/v4wb.h) \
    $(wildcard include/config/cache/b15/rac.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/cachetype.h \
    $(wildcard include/config/cpu/cache/vivt.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/outercache.h \
    $(wildcard include/config/outer/cache/sync.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/kmap_types.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/highmem.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/hugetlb_inline.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/mempool.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/bio.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/bio-crypt-ctx.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/bsg.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/bsg.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/blkzoned.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/elevator.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/filter.h \
    $(wildcard include/config/cfi/clang.h) \
    $(wildcard include/config/bpf/jit.h) \
    $(wildcard include/config/bpf/jit/always/on.h) \
    $(wildcard include/config/have/ebpf/jit.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/cryptohash.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/set_memory.h \
    $(wildcard include/config/arch/has/set/memory.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/set_memory.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/kallsyms.h \
    $(wildcard include/config/kallsyms/all.h) \
    $(wildcard include/config/kallsyms.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/module.h \
    $(wildcard include/config/modules/tree/lookup.h) \
    $(wildcard include/config/tracepoints.h) \
    $(wildcard include/config/bpf/events.h) \
    $(wildcard include/config/event/tracing.h) \
    $(wildcard include/config/module/unload.h) \
    $(wildcard include/config/constructors.h) \
    $(wildcard include/config/module/sig.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/kmod.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/umh.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/elf.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/elf.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/vdso_datapage.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/user.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/elf.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/elf-em.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/moduleparam.h \
    $(wildcard include/config/ppc64.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/error-injection.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/error-injection.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/cfi.h \
    $(wildcard include/config/cfi/clang/shadow.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/module.h \
    $(wildcard include/config/arm/unwind.h) \
    $(wildcard include/config/arm/module/plts.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/asm-generic/module.h \
    $(wildcard include/config/have/mod/arch/specific.h) \
    $(wildcard include/config/modules/use/elf/rel.h) \
    $(wildcard include/config/modules/use/elf/rela.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/if_vlan.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/etherdevice.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/arch/arm/include/asm/unaligned.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/unaligned/le_struct.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/unaligned/packed_struct.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/unaligned/be_byteshift.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/unaligned/generic.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/if_vlan.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/sch_generic.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/pkt_cls.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/gen_stats.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/gen_stats.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/rtnetlink.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/netlink.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/flow_offload.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/rhashtable.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/jhash.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/filter.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/rculist_nulls.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/poll.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/poll.h \
  arch/arm/include/generated/uapi/asm/poll.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/asm-generic/poll.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/eventpoll.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/dst.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/neighbour.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/tcp_states.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/net_tstamp.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/smc.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/l3mdev.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/fib_rules.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/fib_rules.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/fib_notifier.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/inet_connection_sock.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/inet_sock.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/request_sock.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/netns/hash.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/inet_timewait_sock.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/timewait_sock.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/tcp.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/ip.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/route.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/inetpeer.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/ipv6.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/ipv6.h \
    $(wildcard include/config/ipv6/router/pref.h) \
    $(wildcard include/config/ipv6/route/info.h) \
    $(wildcard include/config/ipv6/optimistic/dad.h) \
    $(wildcard include/config/ipv6/seg6/hmac.h) \
    $(wildcard include/config/ipv6/mip6.h) \
    $(wildcard include/config/ipv6/subtrees.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/ipv6.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/icmpv6.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/udp.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/udp.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/jump_label_ratelimit.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/if_inet6.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/ndisc.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/ipv6_stubs.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/linux/if_arp.h \
    $(wildcard include/config/firewire/net.h) \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/if_arp.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/ip_fib.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/net/arp.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/in_route.h \
  /home/dawn/t113_tina5.0_v1.2/kernel/linux-5.4/include/uapi/linux/route.h \

/home/dawn/project/linkg_v2/build/arm/kernel/linkg_fast_nat/linkg_fast_nat_rules_common.o: $(deps_/home/dawn/project/linkg_v2/build/arm/kernel/linkg_fast_nat/linkg_fast_nat_rules_common.o)

$(deps_/home/dawn/project/linkg_v2/build/arm/kernel/linkg_fast_nat/linkg_fast_nat_rules_common.o):
