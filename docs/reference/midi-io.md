<!-- section moved from the old README; edit freely -->
_Docs home: [Teensy2048](../README.md) · [Cheat sheet](../cheatsheet.md) · [Glossary](../glossary.md)_

# Transport
- **PLAY** button (SEQMODE): Start (or Stop if INT transport running)
- **PLAY** button (**ENCMODE3**): toggle clock **INT/EXT** — it never starts the transport there (frees SEQMODE3 ENC4)
- **SYNC=INT**: Teensy generates MIDI Clock (0xF8) + Start (0xFA) → DAW follows (**default**)
- **SYNC=EXT**: Follows incoming USB-MIDI Clock/Start/Stop/Continue

## MIDI I/O
- **USB-MIDI Device**: Notes, CC, Clock, Start/Stop/Continue
- **5-pin DIN MIDI input** (Serial2, pin 7 RX / pin 8 TX): **notes only** — clock/transport deliberately stays USB-only so two clock sources can never double-advance the sequencer; per track gated by SEQMODE4 **EXT IN / SRC** (see [Swing + External In](../modes/timing.md))
- **Keyboard output channel**: Default ch2 (ch1 reserved for DRUM)
- **Sequencer output**: Each sequence plays on its own MIDI channel (1–16)
- **External MIDI In**: `extNoteOn/Off` feeds pending chords of sequences whose input channel matches **and whose SRC (USB/DIN/BOTH) allows the port the note arrived on**; a THRU track forwards them to its output channel
