# Tiny Buffer Lab

This lab is a minimal C buffer-bound example for a SemWeaver generate/evidence/refine smoke test.

Vulnerable behavior:
- `save_tag()` copies `len` bytes into `Record.tag[8]`.
- The original code uses `memcpy(record->tag, input, len)` without proving that `len` fits.

Patch intent:
- Reject input lengths greater than or equal to the destination buffer size before copying.

Files:
- `src/tiny_copy.c`: vulnerable source file
- `patches/tiny_copy_bounds_fix.patch`: bounds-check patch used as `--patch`
- `MANUAL_TEST_STEPS.md`: runnable demo commands
