---
name: inheriting-in-flight-work
description: "Picking up another agent's/session's unfinished work — secure the irreplaceable generated artifacts FIRST, before running any build"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 318ea7b4-ab79-4275-8e97-a5c62dfe2908
  modified: 2026-08-27T22:16:58.021Z
---

When asked to recover work from a crashed or interrupted session, the first job is **not** reading
the diff — it is finding what would be destroyed by the next ordinary command, and making it
reproducible.

2026-08-27: an earlier agent got Temple Run 2's music working, then crashed. The code changes were
uncommitted but safe in the working tree. The thing at risk was the decoded PCM it had built, which
existed in exactly one place — `packaging/stage/…/android/extras/` — and `build-ipk.sh` begins with
`rm -rf "$STAGE"`. **Packaging any game, including an unrelated one, would have deleted it**, and
the recipe to rebuild it lived only in a prose paragraph.

**Why:** generated artifacts are invisible to `git status` (gitignored by design here — they are
copyrighted game content), so the normal "is everything committed?" check reports all-clear while
the irreplaceable part sits in a scratch directory. Losing it costs the session's real discovery,
not just some bytes.

**How to apply, in order:**
1. `git status` **and** hunt gitignored build/stage/scratch dirs for large generated files
   (`find . -newer <last commit> -size +1M`, check `.gitignore` for what is excluded).
2. Move anything irreplaceable somewhere no build wipes, and add an ignore rule for it.
3. **Write the regeneration as a script, then run it and diff against the surviving artifact**
   (md5). Prose instructions are not a backup. `tools/tr2-extract-music.sh` reproduced the PCM
   byte-identically, which is what made the copy expendable.
4. Only then review the diff, and re-run the documented build end-to-end to prove the recipe
   produces the artifact the user already confirmed working.
5. Check the crashed session's device/remote state too — it may have left a debug-instrumented build
   installed or scratch files on the target.

Also worth distrusting: the prose the previous agent left. Its note said "device confirmation on
1.3.7"; the logs showed 1.3.7 was a side-loaded test and the shipped self-contained build was 1.3.8.
Verify claims against the logs before promoting them into the docs.

Related: [[systematic-not-brute-force]], [[static-answer-before-dynamic-probe]].
