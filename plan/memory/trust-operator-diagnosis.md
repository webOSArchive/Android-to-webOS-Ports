---
name: trust-operator-diagnosis
description: "When the user repeatedly rules out a hypothesis class, pivot away from it immediately — don't keep testing variants"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 7a813961-5d5f-46b3-b86d-ff6f66990032
---

During the WMW Android-shim port (see [[wrapper-spike-progress]]), the user told me 4–5 times, with
increasing firmness ("LISTEN to me", "DO NOT CHASE COORDINATES"), that the in-game touch failure was
**not** a coordinate problem — they'd tapped/dragged the whole screen and nothing registered "even in
the wrong place." I kept testing coordinate variants anyway (raw vs normalized, flips, rotations,
sRealScreenSize). That burned several iterations and the user's patience. They were right.

**Why:** The operator has the live device and is watching the actual behavior — they have signal I
don't. "Nothing happens anywhere, not even in the wrong place" is a precise observation that *rules
out* the whole coordinate-mapping hypothesis class (a wrong mapping still produces a wrong-place
reaction). Their repeated insistence is data, not noise.

**How to apply:** When the user states a diagnostic conclusion — especially more than once, or
emphatically — treat that hypothesis class as eliminated and move to a different one (dispatch path,
state/lifecycle, threading, missing registration), even if I haven't personally proven it false.
Don't re-litigate it with more variants. If I genuinely think they might be wrong, say so explicitly
and ask for one decisive observation rather than silently continuing to test the ruled-out class.
Their "it's not X" plus a behavioral detail is usually a sharper diagnostic than my next experiment.
