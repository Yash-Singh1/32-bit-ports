/*
 * Reserve the recovered executable's complete 32-bit image range before
 * dyld, Rosetta, or libmalloc can claim low pages. game_loader replaces pages
 * directly with the legacy Mach-O segments, without opening an allocation hole.
 */
.globl _lp32_legacy_reservation
.zerofill __LEGACY,__reserve,_lp32_legacy_reservation,2147479552,12
