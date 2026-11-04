# Teensy2048 — Ableton bridge (ENCABLETONMODE)
#
# Companion control-surface script for the Teensy 4.1 performance controller.
# It speaks the firmware's SysEx protocol (Link.h/Link.cpp) and does three
# things:
#   * mirrors track selection both ways,
#   * turns Teensy patterns into clip notes and fires/stops clips,
#   * maps the ENCABLETONMODE knobs (CC64-71 echoed as SysEx) onto the
#     selected device's parameters and sends names/values back for the OLED.
#
# The Tap script is untouched: this lives in its own control-surface slot on
# the Teensy's own MIDI port.
#
# Install: copy this folder into
#   <User Library>/Remote Scripts/Teensy2048/
# then pick "Teensy2048" as a Control Surface in Settings > Tempo & MIDI with
# the Teensy port as Input AND Output.  Tick the port's Track switch as usual
# and Sync if the firmware follows Ableton's clock.

from __future__ import absolute_import, print_function, unicode_literals

import Live
from Live.Clip import MidiNoteSpecification
from _Framework.ControlSurface import ControlSurface

MFR = 0x7D
TAG = (0x37, 0x32, 0x34, 0x38)   # '7248'
PROTO = 1

CMD_HELLO = 0x01
CMD_ACK = 0x02
CMD_TRACK_SEL = 0x10
CMD_TRACK = 0x11
CMD_CLIP_ACT = 0x12
CMD_CLIP_STATE = 0x13
CMD_PAGE_STEP = 0x14
CMD_DEV_STEP = 0x15
CMD_STOP_TRACK = 0x16
CMD_STOP_ALL = 0x17
CMD_SEL_ROW = 0x18
CMD_KNOB = 0x19
CMD_PAT_STEP = 0x20
CMD_FILL_DONE = 0x21
CMD_PARAM = 0x30
CMD_PARAM_SHORT = 0x31
CMD_PAGE = 0x32
CMD_BANK = 0x33

PARAM_SLOTS = 8
STEP_BEATS = 0.25          # one Teensy step = a 16th note
NAME_CHARS = 8
TEXT_CHARS = 7

ACT_LAUNCH = 0
ACT_STOP = 1
ACT_FILL = 2


def _ascii(value, limit):
    """7-bit, printable, truncated - the wire is a SysEx payload."""
    text = u"{}".format(value if value is not None else u"")
    text = text.replace(u"\n", u" ")
    out = []
    for ch in text:
        code = ord(ch)
        if 0x20 <= code < 0x7F:
            out.append(ch)
        if len(out) >= limit:
            break
    return u"".join(out)


class Teensy2048(ControlSurface):

    def __init__(self, c_instance):
        ControlSurface.__init__(self, c_instance)
        self._page = 0
        self._track = None
        self._device = None
        self._clip_bits = [0] * 16
        self._last_row = None
        self._pending = {}            # ch -> {"len": n, "steps": {}, "row": r}
        self._param_cache = {}
        self._tick_count = 0
        self._killed = False
        with self.component_guard():
            self.song().view.add_selected_track_listener(self._on_track_changed)
            self._on_track_changed()
        self._schedule_tick()

    # ---------------------------------------------------------------- wire --
    def _send(self, payload):
        if self._killed:
            return
        data = (0xF0, MFR) + TAG + tuple(int(b) & 0x7F for b in payload) + (0xF7,)
        try:
            self._send_midi(data)
        except Exception:
            pass

    def handle_sysex(self, message):
        data = tuple(message)
        start = None
        for i in range(len(data) - 5):
            base = i + 1 if data[i] == 0xF0 else i
            if base + 5 >= len(data):
                break
            if data[base] != MFR:
                continue
            if tuple(data[base + 1:base + 5]) != TAG:
                continue
            start = base
            break
        if start is None:
            return
        try:
            self._on_command(data[start + 5], data[start + 6:])
        except Exception as error:
            self.log_message("sysex error: {}".format(error))

    def _on_command(self, cmd, args):
        if cmd == CMD_HELLO:
            self._send([CMD_ACK, PROTO, 0x03])
            self._send_track()
            self._send_params()
            self._send_page()
            self._send_clip_states()
            self._send_highlight()
        elif cmd == CMD_TRACK_SEL:
            if len(args) >= 1:
                self._select_track(args[0])
        elif cmd == CMD_CLIP_ACT:
            if len(args) >= 2:
                self._clip_action(args[0], args[1])
        elif cmd == CMD_PAGE_STEP:
            if len(args) >= 1:
                self._step_page(1 if args[0] else -1)
        elif cmd == CMD_DEV_STEP:
            if len(args) >= 1:
                self._step_device(1 if args[0] else -1)
        elif cmd == CMD_STOP_TRACK:
            self._stop_track_clips()
        elif cmd == CMD_STOP_ALL:
            try:
                self.song().stop_all_clips()
            except Exception:
                pass
        elif cmd == CMD_KNOB:
            if len(args) >= 2:
                self._write_param(args[0], args[1])
        elif cmd == CMD_PAT_STEP:
            self._pattern_chunk(args)

    # ------------------------------------------------------------- helpers --
    def _selected_track(self):
        return self.song().view.selected_track

    def _selected_device(self):
        track = self._selected_track()
        if track is None:
            return None
        try:
            return track.view.selected_device
        except Exception:
            return None

    def _schedule_tick(self):
        if self._killed:
            return
        try:
            self.schedule_message(1, self._tick)
        except Exception:
            pass

    def _tick(self):
        if self._killed:
            return
        self._schedule_tick()
        self._tick_count += 1
        if self._tick_count % 4 == 0:
            self._send_clip_states()
            self._send_highlight()
        if self._tick_count % 16 == 0:
            self._send_params()

    # -------------------------------------------------------------- tracks --
    def _on_track_changed(self):
        self._page = 0
        self._track = self._selected_track()
        self._device = self._selected_device()
        self._clip_bits = [0] * 16
        self._send_track()
        self._send_params()
        self._send_page()
        self._send_clip_states()

    def _select_track(self, index):
        try:
            tracks = list(self.song().tracks)
            if 0 <= index < len(tracks):
                self.song().view.selected_track = tracks[index]
        except Exception:
            pass

    def _send_track(self):
        track = self._selected_track()
        if track is None:
            return
        try:
            index = list(self.song().tracks).index(track)
        except ValueError:
            index = 0
        self._send([CMD_TRACK, index] + self._codes(track.name, 15))

    def _codes(self, text, limit):
        return [ord(ch) for ch in _ascii(text, limit)]

    # -------------------------------------------------------------- params --
    def _parameters(self):
        track = self._selected_track()
        if track is None:
            return [], u""
        device = self._selected_device()
        if device is not None:
            try:
                return list(device.parameters)[1:], _ascii(device.name, NAME_CHARS)
            except Exception:
                return [], u""
        # no device selected: the track's own mixer fills the row
        try:
            mixer = track.mixer_device
            params = [mixer.volume, mixer.panning]
            params.extend(list(mixer.sends)[:6])
            return params, u"Mixer"
        except Exception:
            return [], u""

    def _page_parameters(self):
        params, bank = self._parameters()
        start = self._page * PARAM_SLOTS
        page = params[start:start + PARAM_SLOTS]
        while len(page) < PARAM_SLOTS:
            page.append(None)
        return page, bank

    def _param_bounds(self, param):
        try:
            low = float(param.min)
            high = float(param.max)
        except Exception:
            low, high = 0.0, 1.0
        if high <= low:
            high = low + 1.0
        return low, high

    def _param_to_127(self, param):
        low, high = self._param_bounds(param)
        try:
            value = float(param.value)
        except Exception:
            value = low
        scaled = int(round((value - low) / (high - low) * 127.0))
        return max(0, min(127, scaled))

    def _send_params(self):
        page, bank = self._page_parameters()
        for slot in range(PARAM_SLOTS):
            param = page[slot]
            if param is None:
                self._send([CMD_PARAM, slot, self._page, 0, 0] +
                           self._codes(u"", NAME_CHARS) +
                           self._codes(u"", TEXT_CHARS))
                continue
            name = _ascii(param.name, NAME_CHARS)
            try:
                text = _ascii(param.display_value, TEXT_CHARS)
            except Exception:
                text = u""
            flags = 1 if getattr(param, "is_quantized", False) else 0
            self._send([CMD_PARAM, slot, self._page, self._param_to_127(param),
                        flags] + self._codes(name, NAME_CHARS) +
                       self._codes(text, TEXT_CHARS))
        self._send([CMD_BANK] + self._codes(bank, NAME_CHARS))

    def _send_page(self):
        params, _ = self._parameters()
        count = max(1, (len(params) + PARAM_SLOTS - 1) // PARAM_SLOTS)
        self._send([CMD_PAGE, self._page, min(count, 127)])

    def _step_page(self, direction):
        params, _ = self._parameters()
        count = max(1, (len(params) + PARAM_SLOTS - 1) // PARAM_SLOTS)
        self._page = max(0, min(count - 1, self._page + direction))
        self._send_page()
        self._send_params()

    def _write_param(self, slot, value):
        page, _ = self._page_parameters()
        param = page[slot] if 0 <= slot < len(page) else None
        if param is None:
            return
        low, high = self._param_bounds(param)
        try:
            param.value = low + (float(value) / 127.0) * (high - low)
        except Exception:
            return
        self._send([CMD_PARAM_SHORT, slot, self._param_to_127(param)])

    def _step_device(self, direction):
        track = self._selected_track()
        if track is None:
            return
        try:
            devices = list(track.devices)
            if not devices:
                return
            current = self._selected_device()
            index = devices.index(current) if current in devices else -1
            index = max(0, min(len(devices) - 1, index + direction))
            track.view.selected_device = devices[index]
        except Exception:
            pass
        self._page = 0
        self._send_page()
        self._send_params()

    # --------------------------------------------------------------- clips --
    def _send_clip_states(self):
        track = self._selected_track()
        if track is None:
            return
        bits = []
        for row in range(16):
            state = 0
            try:
                slot = track.clip_slots[row]
                if slot.has_clip:
                    state |= 0x01
                    if slot.is_playing:
                        state |= 0x02
                    if getattr(slot, "is_triggered", False):
                        state |= 0x04
                if getattr(track, "arm", False):
                    state |= 0x08
            except Exception:
                pass
            bits.append(state)
        for base in range(0, 16, 4):
            self._send([CMD_CLIP_STATE, base] + bits[base:base + 4])

    def _send_highlight(self):
        try:
            row = self.song().view.highlighted_clip_slot
            track = row.canonical_parent
            slots = list(track.clip_slots)
            index = slots.index(row)
        except Exception:
            return
        if index == self._last_row:
            return
        self._last_row = index
        self._send([CMD_SEL_ROW, index & 0x0F])

    def _clip_action(self, row, action):
        track = self._selected_track()
        if track is None:
            return
        try:
            slot = track.clip_slots[row]
        except Exception:
            return
        if action == ACT_FILL:
            channel = self._track_index(track)
            self._pending[channel] = {"len": 0, "steps": {}, "row": row}
            return
        try:
            if action == ACT_LAUNCH:
                slot.fire()
            elif action == ACT_STOP:
                slot.stop()
        except Exception as error:
            self.log_message("clip action error: {}".format(error))

    def _track_index(self, track):
        try:
            return list(self.song().tracks).index(track)
        except ValueError:
            return 0

    def _stop_track_clips(self):
        track = self._selected_track()
        if track is None:
            return
        for slot in list(track.clip_slots)[:16]:
            try:
                if slot.is_playing or getattr(slot, "is_triggered", False):
                    slot.stop()
            except Exception:
                pass

    # ------------------------------------------------------------- patterns --
    def _pattern_chunk(self, args):
        if len(args) < 4:
            return
        channel = args[0] & 0x0F
        pat_len = max(1, min(64, args[1]))
        first = args[2]
        count = args[3]
        state = self._pending.get(channel)
        if state is None:
            state = {"len": pat_len, "steps": {}, "row": self._last_row or 0}
            self._pending[channel] = state
        state["len"] = pat_len

        index = 4
        for offset in range(count):
            if index + 1 >= len(args):
                break
            note_count = args[index]
            index += 1
            notes = list(args[index:index + note_count])
            index += note_count
            if index + 2 >= len(args):
                break
            attr = args[index]
            prob = args[index + 1]
            velocity = args[index + 2]
            index += 3
            state["steps"][first + offset] = {
                "notes": notes,
                "attr": attr,
                "prob": prob,
                "velocity": velocity,
            }

        if len(state["steps"]) >= state["len"]:
            self._write_clip(channel, state)
            self._pending.pop(channel, None)

    def _write_clip(self, channel, state):
        tracks = list(self.song().tracks)
        if not 0 <= channel < len(tracks):
            return
        track = tracks[channel]
        row = state["row"]
        if not 0 <= row < len(track.clip_slots):
            return
        slot = track.clip_slots[row]
        length = state["len"] * STEP_BEATS
        written = 0
        try:
            if not slot.has_clip:
                slot.create_clip(length)
            clip = slot.clip
            if clip is None:
                return
            try:
                clip.remove_notes_extended(0, 128, 0, max(clip.length, length))
            except AttributeError:
                clip.clear_all_notes()

            specs = []
            steps = state["steps"]
            for step in sorted(steps.keys()):
                data = steps[step]
                start = step * STEP_BEATS
                duration = STEP_BEATS * 0.9
                if data["attr"] & 0x01:          # TIE: ring into the next notes
                    cursor = step + 1
                    while cursor < state["len"] and cursor not in steps:
                        duration += STEP_BEATS
                        cursor += 1
                if data["attr"] & 0x02:          # GLIDE: overlap a hair
                    duration += 0.02
                probability = 1.0
                if data["attr"] & 0x08:
                    probability = max(0.0, min(1.0, data["prob"] / 100.0))
                velocity = max(1, min(127, int(data["velocity"])))
                for note in data["notes"]:
                    pitch = max(0, min(127, int(note)))
                    specs.append(MidiNoteSpecification(
                        pitch=pitch,
                        start_time=start,
                        duration=duration,
                        velocity=velocity,
                        mute=False,
                        probability=probability))
                    written += 1

            if specs:
                try:
                    clip.add_new_notes(tuple(specs))
                except AttributeError:
                    clip.set_notes(tuple(
                        (spec.pitch, spec.start_time, spec.duration,
                         spec.velocity, False) for spec in specs))
            try:
                clip.loop_start = 0.0
                clip.loop_end = length
                clip.looping = True
            except Exception:
                pass
        except Exception as error:
            self.log_message("clip write error: {}".format(error))
            self._send([CMD_FILL_DONE, channel, 0, written & 0x7F,
                        (written >> 7) & 0x7F])
            return
        self._send([CMD_FILL_DONE, channel, 1, written & 0x7F,
                    (written >> 7) & 0x7F])

    # ------------------------------------------------------------ teardown --
    def disconnect(self):
        self._killed = True
        try:
            self.song().view.remove_selected_track_listener(self._on_track_changed)
        except Exception:
            pass
        ControlSurface.disconnect(self)
