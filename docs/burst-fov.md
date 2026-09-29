# Optional burst FOV framing fix

The fix is **disabled by default**. Add these fields to `plugin_config.json`
(alongside your existing settings) to enable it:

```json
"fixBurstFov": true,
"burstFovDelayMs": 1700
```

`unlockFov` must also be enabled. Set `fixBurstFov` to `false` to use the
original FOV handling without the burst heuristic. The normal cursor/focus,
menu restoration and hook-deactivation logic is not replaced.

`burstFovDelayMs` is a whole number from **0 to 10000 milliseconds**, with a
**1700 ms** default. It caps how long native FOV is preserved after the
caller-pattern detector triggers. Return begins sooner if the other learned
camera caller reappears. Delay changes apply to the next detected burst.

The return uses a fixed **200 ms smooth blend**, independent of `fovSmoothing`.
Increasing the delay by 100 delays the timed return by 100 ms; it does not
make the blend slower. For a timer-limited burst, 1700 ms + 200 ms means the
selected FOV is reached approximately 1900 ms after detection.

Older configurations remain valid; missing fields use their defaults.
Invalid values and unknown fields are still rejected. Back up your JSON
before testing. An older released DLL will not recognise these new fields,
so remove them (or restore your backup) when reverting to that DLL.

This is a heuristic, not an exact animation-end signal. It has been tested
by the reporter on several owned characters, not the full roster or every
camera state. Too short a delay can expose cinematic overlay edges again.
There are no new hooks, input polling, screen capture or diagnostic logging.
Preserving the existing hook lifecycle is not a guarantee of account safety.
