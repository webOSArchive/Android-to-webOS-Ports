---
name: screenshots-lie
description: "Don't build a rendering theory on a webOS screenshot's brightness/colour — ask what the user sees on the panel; take screenshots with apkenv/tools/grab.sh"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 61d92ecd-d145-4342-9fa7-c94c50c1fc83
  modified: 2026-09-14T16:37:11.620Z
---

During Aralon (2026-09-14) I read a webOS on-device screenshot as "black ground, terrain not drawn"
and ran three device cycles on it. The user corrected me: "its not *black* it just seems darker than on
the android. could just be screen differences -- screenshots lie". It later turned out to be a subtle
lighting difference the user chose to park ("maybe its just the natural brightness of the touchpad
screen vs the android device screen").

**Why:** the on-device screenshot tool does not capture the GL layer faithfully, and two tablets'
panels differ in brightness/warmth — a capture is not a measurement of what the player sees.

**How to apply:** for any visual claim, grab the GL frame with `apkenv/tools/grab.sh` (the user plays
and says "grab"), and before theorising about a brightness/colour gap, ask the user to describe it on
the panel side by side. Treat "slightly darker/less warm" as possibly the screen, and let the user
decide whether it is worth chasing. Related: [[trust-operator-diagnosis]], [[systematic-not-brute-force]].
