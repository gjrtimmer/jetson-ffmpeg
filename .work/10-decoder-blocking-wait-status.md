# Issue #10: Decoder Blocking Wait — Execution Status

## Current Branch: `refactor/10-decoder-split`

## Branch 1: refactor/10-decoder-split

| Task | Status | Commit | Notes |
|------|--------|--------|-------|
| 1. Create branch + post issue comment | DONE | — | Branch created, comment posted |
| 2. Extract nvmpictx to nvmpi_dec_internal.h | DONE | 14ac9d4 | Header + struct + macros + fwd decls |
| 3. Extract capture loop to nvmpi_dec_capture.cpp | DONE | 2c35d23, acf52c7 | Capture loop + resolution handler + fwd decl fix |
| 4. Extract V4L2 planes to nvmpi_dec_planes.cpp | DONE | a7faf6e | getNvColorFormat + init/deinitCapturePlane |
| 5. Rename to nvmpi_dec_api.cpp + update CMake | DONE | ca98f3d | git mv + updated 5 comment refs |
| 6. Write docs/ARCHITECTURE.md | DONE | 9569332 | Modular split convention |
| 7. Update docs/DEVELOPMENT.md | DONE | b871361 | Cross-ref ARCHITECTURE.md + file table |
| 8. Build verification | DONE | — | Clean build, 4 decoder files confirmed |
| 9. Push, create MR, set auto-merge | PENDING | — | |

## Branch 2: feat/10-decoder-blocking-wait

Not started. Requires Branch 1 merged to main first.

| Task | Status | Commit | Notes |
|------|--------|--------|-------|
| 10. Create feature branch | PENDING | — | |
| 11. Unit tests for bufpool (TDD) | PENDING | — | |
| 12. HW test suites (TDD) | PENDING | — | |
| 13. NVMPI_bufPool CV support | PENDING | — | |
| 14. Atomic eos + wait_timeout_ms | PENDING | — | |
| 15. Wire shutdown to capture loop | PENDING | — | |
| 16. Implement blocking wait in get_frame | PENDING | — | |
| 17. AVOption wait_timeout in FFmpeg | PENDING | — | |
| 18. Regenerate patches | PENDING | — | |
| 19. docs/THREAD_SAFETY.md | PENDING | — | |
| 20. docs/API_REFERENCE.md | PENDING | — | |
| 21. Update BUILD.md, README, TODO | PENDING | — | |
| 22. Final verification | PENDING | — | |
| 23. Push, create MR, set auto-merge | PENDING | — | |

## Recovery

To continue from any point:
1. Check which branch: `git branch --show-current`
2. Check last commit: `git log --oneline -5`
3. Read this file for task status
4. Resume from next PENDING task in the plan: `.work/10-decoder-blocking-wait-plan.md`
