# airplay_receiver — agent entry point

Read **`AGENTS.md`** in this directory first. It is the authoritative brief: start-here order,
the non-negotiable rules (single entry point, mDNS registration, import alias, build flags,
lwIP sizing, the superseded-slot guard), and the verify-before-declaring-success gate.

Then, before touching the audio or transport path or debugging any artefact, read
**`airplay_receiver/FIELD-NOTES.md`** — findings measured on hardware, including three distinct
bugs that both present as "sender connected, metadata fine, no audio", and a dead-ends list.

`CHEATSHEET.md` is the reference: YAML schema, board pins, the `airplay_audio_*` contract,
build commands, and the pitfalls already handled.
